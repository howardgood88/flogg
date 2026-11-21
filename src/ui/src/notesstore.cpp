/*
 * Copyright (C) 2026 flogg contributors
 *
 * This file is part of flogg.
 *
 * flogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * flogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with flogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "notesstore.h"

#include <algorithm>
#include <memory>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include "filedigest.h"
#include "log.h"
#include "persistentinfo.h"

namespace {
QString hex( uint64_t value )
{
    return QString::number( value, 16 );
}

uint64_t fromHex( const QJsonValue& value )
{
    return value.toString().toULongLong( nullptr, 16 );
}
} // namespace

namespace {
std::unique_ptr<NotesStore>& instance()
{
    static std::unique_ptr<NotesStore> store;
    return store;
}
} // namespace

NotesStore& NotesStore::get()
{
    auto& store = instance();
    if ( !store ) {
        // Next to the settings when they are kept in a file of their own, like the filters
        auto dir = PersistentInfo::portableDirectory();
        if ( dir.isEmpty() ) {
            dir = QStandardPaths::writableLocation( QStandardPaths::GenericDataLocation )
                  + "/flogg";
        }
        store = std::make_unique<NotesStore>( dir + "/notes.json" );
    }
    return *store;
}

void NotesStore::useFile( const QString& path )
{
    instance() = std::make_unique<NotesStore>( path );
}

NotesStore::NotesStore( QString path )
    : path_( std::move( path ) )
{
}

QString NotesStore::key( const QString& fileName )
{
    const QFileInfo info( fileName );
    const auto canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

std::optional<NotesStore::Head> NotesStore::readHead( const QString& fileName, qint64 size )
{
    QFile file( fileName );
    if ( !file.open( QIODevice::ReadOnly ) ) {
        return {};
    }
    const auto data = file.read( size );
    FileDigest digest;
    digest.addData( data );
    return Head{ digest.digest(), data.size() };
}

uint64_t NotesStore::lineHash( const QString& line )
{
    FileDigest digest;
    digest.addData( line.toUtf8() );
    return digest.digest();
}

void NotesStore::load()
{
    if ( loaded_ ) {
        return;
    }
    loaded_ = true;

    QFile file( path_ );
    if ( !file.open( QIODevice::ReadOnly ) ) {
        return;
    }
    const auto document = QJsonDocument::fromJson( file.readAll() );
    for ( const auto& fileValue : document.object().value( "files" ).toArray() ) {
        const auto object = fileValue.toObject();
        FileNotes entry;
        entry.path = object.value( "path" ).toString();
        entry.head.digest = fromHex( object.value( "head" ) );
        entry.head.size = static_cast<qint64>( object.value( "headSize" ).toDouble() );
        entry.used = static_cast<int64_t>( object.value( "used" ).toDouble() );
        for ( const auto& noteValue : object.value( "notes" ).toArray() ) {
            const auto noteObject = noteValue.toObject();
            Note note;
            note.line = static_cast<uint64_t>( noteObject.value( "line" ).toDouble() );
            note.slots = static_cast<uint32_t>( noteObject.value( "bookmarks" ).toInt() );
            note.text = noteObject.value( "text" ).toString();
            note.lineHash = fromHex( noteObject.value( "hash" ) );
            if ( !note.text.isEmpty() ) {
                entry.notes.push_back( std::move( note ) );
            }
        }
        if ( !entry.path.isEmpty() && !entry.notes.empty() ) {
            files_.push_back( std::move( entry ) );
        }
    }
}

void NotesStore::save() const
{
    QJsonArray files;
    for ( const auto& entry : files_ ) {
        QJsonArray notes;
        for ( const auto& note : entry.notes ) {
            notes.append( QJsonObject{ { "line", static_cast<double>( note.line ) },
                                       { "bookmarks", static_cast<int>( note.slots ) },
                                       { "hash", hex( note.lineHash ) },
                                       { "text", note.text } } );
        }
        files.append( QJsonObject{ { "path", entry.path },
                                   { "head", hex( entry.head.digest ) },
                                   { "headSize", static_cast<double>( entry.head.size ) },
                                   { "used", static_cast<double>( entry.used ) },
                                   { "notes", notes } } );
    }

    QDir{}.mkpath( QFileInfo( path_ ).absolutePath() );
    QSaveFile file( path_ );
    if ( !file.open( QIODevice::WriteOnly ) ) {
        LOG_WARNING << "Can not write the notes to " << path_;
        return;
    }
    file.write( QJsonDocument( QJsonObject{ { "version", 1 }, { "files", files } } ).toJson() );
    if ( !file.commit() ) {
        LOG_WARNING << "Can not write the notes to " << path_;
    }
}

bool NotesStore::hasNotes( const QString& fileName )
{
    load();
    const auto path = key( fileName );
    return std::any_of( files_.begin(), files_.end(),
                        [ &path ]( const auto& entry ) { return entry.path == path; } );
}

std::vector<NotesStore::Note> NotesStore::notes( const QString& fileName, bool* changed )
{
    load();
    if ( changed ) {
        *changed = false;
    }
    const auto path = key( fileName );
    const auto entry = std::find_if( files_.begin(), files_.end(),
                                     [ &path ]( const auto& e ) { return e.path == path; } );
    if ( entry == files_.end() ) {
        return {};
    }
    const auto head = readHead( fileName, entry->head.size );
    if ( !head || head->size != entry->head.size || head->digest != entry->head.digest ) {
        // Another file now (rotated, overwritten): its notes are no use
        if ( changed ) {
            *changed = true;
        }
        files_.erase( entry );
        save();
        return {};
    }
    // Only kept in memory: the time of use is written with the next change
    entry->used = QDateTime::currentSecsSinceEpoch();
    return entry->notes;
}

void NotesStore::setNotes( const QString& fileName, const std::vector<Note>& notes )
{
    load();
    const auto head = notes.empty() ? std::optional<Head>{} : readHead( fileName );
    if ( !notes.empty() && !head ) {
        return;
    }
    const auto path = key( fileName );
    const auto entry = std::find_if( files_.begin(), files_.end(),
                                     [ &path ]( const auto& e ) { return e.path == path; } );
    if ( notes.empty() ) {
        if ( entry != files_.end() ) {
            files_.erase( entry );
            save();
        }
        return;
    }

    if ( entry != files_.end() ) {
        entry->head = *head;
        entry->notes = notes;
        entry->used = QDateTime::currentSecsSinceEpoch();
    }
    else {
        files_.push_back( { path, *head, QDateTime::currentSecsSinceEpoch(), notes } );
    }

    // The least recently used files go first
    if ( files_.size() > static_cast<size_t>( MaxFiles ) ) {
        std::sort( files_.begin(), files_.end(),
                   []( const auto& a, const auto& b ) { return a.used > b.used; } );
        files_.resize( MaxFiles );
    }
    save();
}
