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

#include <catch2/catch.hpp>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "test_utils.h"

#include "crawlerwidget.h"
#include "logdata.h"
#include "logfiltereddata.h"
#include "notesstore.h"
#include "session.h"

namespace {
void writeLines( const QString& fileName, int count, const QByteArray& prefix = "line " )
{
    QFile file( fileName );
    REQUIRE( file.open( QIODevice::WriteOnly ) );
    for ( int i = 0; i < count; ++i ) {
        file.write( prefix + QByteArray::number( i ) + "\n" );
    }
}

void appendLine( const QString& fileName, const QByteArray& line )
{
    QFile file( fileName );
    REQUIRE( file.open( QIODevice::Append ) );
    file.write( line + "\n" );
}

std::unique_ptr<CrawlerWidget> openCrawler( Session& session, const QString& fileName )
{
    std::unique_ptr<CrawlerWidget> crawler( static_cast<CrawlerWidget*>(
        session.open( fileName, []() { return new CrawlerWidget(); } ) ) );
    REQUIRE( waitUiState( [ & ]() { return !crawler->isLoading(); } ) );
    return crawler;
}
} // namespace

SCENARIO( "Notes on lines", "[logdata][notes]" )
{
    QTemporaryDir dir;
    const auto fileName = dir.filePath( "notes.log" );
    writeLines( fileName, 100 );

    LogData logData;
    SafeQSignalSpy loadEndSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    logData.attachFile( fileName );
    REQUIRE( loadEndSpy.safeWait( 10000 ) );
    auto filteredData = logData.getNewFilteredData();
    const auto isMarked = [ &filteredData ]( LineNumber line ) {
        return filteredData->lineTypeByLine( line ).testFlag(
            AbstractLogData::LineTypeFlags::Mark );
    };

    filteredData->setNote( 10_lnum, "first" );
    CHECK( filteredData->getNote( 10_lnum ) == "first" );
    CHECK( filteredData->getBookmarkSlots( 10_lnum ) == LogFilteredData::NoteFlag );
    CHECK( isMarked( 10_lnum ) );
    CHECK( filteredData->hasBookmarks() );

    // A bookmark on the line keeps the note, and removing it too
    filteredData->toggleBookmark( 1, 10_lnum );
    CHECK( filteredData->getBookmarkSlots( 10_lnum ) == ( LogFilteredData::NoteFlag | 0b10u ) );
    filteredData->toggleBookmark( 1, 10_lnum );
    CHECK( filteredData->getNote( 10_lnum ) == "first" );
    CHECK( isMarked( 10_lnum ) );

    // Unmarking the line removes its bookmarks, not its note
    filteredData->toggleBookmark( 0, 10_lnum );
    filteredData->toggleMark( 10_lnum );
    CHECK( !isMarked( 10_lnum ) );
    CHECK( filteredData->getBookmarkSlots( 10_lnum ) == LogFilteredData::NoteFlag );
    CHECK( filteredData->getNote( 10_lnum ) == "first" );

    // Removing the note of a line it marked removes the mark
    filteredData->setNote( 20_lnum, "second" );
    filteredData->setNote( 20_lnum, {} );
    CHECK( !isMarked( 20_lnum ) );
    CHECK( filteredData->getBookmarkSlots( 20_lnum ) == 0u );

    // A line marked before keeps its mark
    filteredData->addMark( 30_lnum );
    filteredData->setNote( 30_lnum, "third" );
    filteredData->setNote( 30_lnum, {} );
    CHECK( isMarked( 30_lnum ) );

    filteredData->toggleBookmark( 2, 40_lnum );
    const auto all = filteredData->getBookmarksAndNotes();
    REQUIRE( all.size() == 2 );
    CHECK( all[ 0 ].line == 10_lnum );
    CHECK( all[ 1 ].line == 40_lnum );
    CHECK( filteredData->getNotes().size() == 1 );

    // Clearing the marks keeps the notes, clearing the notes removes them
    filteredData->clearMarks();
    CHECK( filteredData->getNotes().size() == 1 );
    CHECK( filteredData->getBookmarkSlots( 40_lnum ) == 0u );
    filteredData->clearNotes();
    CHECK( !filteredData->hasBookmarks() );
}

SCENARIO( "Notes store", "[notes]" )
{
    QTemporaryDir dir;
    const auto storeFile = dir.filePath( "notes.json" );
    const auto log = dir.filePath( "a.log" );
    writeLines( log, 10 );

    NotesStore store( storeFile );
    CHECK( store.notes( log ).empty() );
    // Nothing written while there is nothing to keep
    store.setNotes( log, {} );
    CHECK( !QFile::exists( storeFile ) );

    store.setNotes( log, { { 3, 0b101u, "a note", NotesStore::lineHash( "line 3" ) } } );
    REQUIRE( QFile::exists( storeFile ) );

    // Read back by another store (another start of flogg)
    {
        NotesStore again( storeFile );
        const auto notes = again.notes( log );
        REQUIRE( notes.size() == 1 );
        CHECK( notes[ 0 ].line == 3 );
        CHECK( notes[ 0 ].slots == 0b101u );
        CHECK( notes[ 0 ].text == "a note" );
        CHECK( notes[ 0 ].lineHash == NotesStore::lineHash( "line 3" ) );
    }

    // A file that grew still has its notes
    appendLine( log, "line 10" );
    CHECK( NotesStore( storeFile ).notes( log ).size() == 1 );

    // The last note removed, the file is no longer in the store
    store.setNotes( log, {} );
    CHECK( !NotesStore( storeFile ).hasNotes( log ) );

    // A file that does not start the same way any more loses its notes
    store.setNotes( log, { { 3, 0, "a note", 0 } } );
    writeLines( log, 10, "other " );
    {
        NotesStore again( storeFile );
        bool changed = false;
        CHECK( again.notes( log, &changed ).empty() );
        CHECK( changed );
        CHECK( !NotesStore( storeFile ).hasNotes( log ) );
    }

    // The least recently used files go past the limit
    NotesStore many( dir.filePath( "many.json" ) );
    for ( int i = 0; i < NotesStore::MaxFiles + 5; ++i ) {
        const auto name = dir.filePath( QString( "f%1.log" ).arg( i ) );
        writeLines( name, 1 );
        many.setNotes( name, { { 0, 0, "n", 0 } } );
    }
    QFile json( dir.filePath( "many.json" ) );
    REQUIRE( json.open( QIODevice::ReadOnly ) );
    const auto files = QJsonDocument::fromJson( json.readAll() ).object().value( "files" );
    CHECK( files.toArray().size() == NotesStore::MaxFiles );
}

SCENARIO( "Notes restored when the file is opened again", "[ui][notes]" )
{
    QTemporaryDir dir;
    NotesStore::useFile( dir.filePath( "notes.json" ) );
    const auto log = dir.filePath( "restore.log" );
    writeLines( log, 50 );

    Session session;
    {
        auto crawler = openCrawler( session, log );
        crawler->setNote( 5_lnum, "five" );
        crawler->setNote( 7_lnum, "seven\nsecond line" );
        crawler->selectLineWhenLoaded( 7_lnum );
        crawler->toggleBookmark( 2 );
        const auto markdown = crawler->bookmarksAndNotesMarkdown();
        CHECK( markdown.endsWith( "\n\n// five\n6: line 5\n// seven\n// second line\n"
                                  "8: line 7\n" ) );
        CHECK( markdown.startsWith( QDir::toNativeSeparators( log ) ) );
    }

    {
        auto crawler = openCrawler( session, log );
        CHECK( crawler->note( 5_lnum ) == "five" );
        CHECK( crawler->note( 7_lnum ) == "seven\nsecond line" );
        CHECK( crawler->bookmarksAndNotesMarkdown().contains( "// seven\n// second line\n8: line 7" ) );
        // Removed notes are gone from the store
        crawler->setNote( 5_lnum, {} );
    }

    // A line that changed loses its note, the others stay
    {
        QFile file( log );
        REQUIRE( file.open( QIODevice::ReadOnly ) );
        auto content = file.readAll();
        file.close();
        content.replace( "line 7\n", "LINE 7\n" );
        REQUIRE( file.open( QIODevice::WriteOnly ) );
        file.write( content );
    }
    {
        auto crawler = openCrawler( session, log );
        CHECK( crawler->note( 5_lnum ).isEmpty() );
        CHECK( crawler->note( 7_lnum ).isEmpty() );
    }
    CHECK( !NotesStore::get().hasNotes( log ) );

    NotesStore::useFile( dir.filePath( "unused.json" ) );
}
