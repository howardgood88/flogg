/*
 * Copyright (C) 2026 flogg contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * klogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "editdocument.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTextCodec>

#include <memory>

#if defined( Q_OS_MAC )
#include <mach/mach.h>
#elif defined( Q_OS_WIN )
#include <windows.h>
#elif defined( Q_OS_LINUX )
#include <fstream>
#include <string>
#endif

namespace edit {

namespace {

constexpr int MibUtf8 = 106;
constexpr int MibAscii = 3;

bool isCancelled( const std::atomic_bool* cancelled )
{
    return cancelled && cancelled->load( std::memory_order_relaxed );
}

void setProgress( std::atomic_int* progress, qint64 done, qint64 total )
{
    if ( progress ) {
        progress->store( total > 0 ? static_cast<int>( done * 100 / total ) : 100 );
    }
}

QByteArray bomOf( const QByteArray& head )
{
    for ( const auto* bom : { "\xEF\xBB\xBF", "\xFF\xFE\x00\x00", "\x00\x00\xFE\xFF" } ) {
        const auto mark = QByteArray( bom, bom[ 0 ] == '\xEF' ? 3 : 4 );
        if ( head.startsWith( mark ) ) {
            return mark;
        }
    }
    for ( const auto* bom : { "\xFF\xFE", "\xFE\xFF" } ) {
        const auto mark = QByteArray( bom, 2 );
        if ( head.startsWith( mark ) ) {
            return mark;
        }
    }
    return {};
}

// First end of line of the UTF-8 text, empty while none
QByteArray endOfLineOf( const char* data, std::size_t length )
{
    for ( std::size_t i = 0; i < length; ++i ) {
        if ( data[ i ] == '\n' ) {
            return "\n";
        }
        if ( data[ i ] == '\r' ) {
            return i + 1 < length && data[ i + 1 ] == '\n' ? QByteArray( "\r\n" )
                                                           : QByteArray( "\r" );
        }
    }
    return {};
}

} // namespace

bool readFile( const QString& fileName, QTextCodec* codec,
               const std::function<bool( const char*, std::size_t )>& sink, FileFormat* format,
               QString* error, const std::atomic_bool* cancelled, std::atomic_int* progress )
{
    QFile file( fileName );
    if ( !file.open( QIODevice::ReadOnly ) ) {
        *error = QCoreApplication::translate( "EditView", "The file can not be read: %1" )
                     .arg( file.errorString() );
        return false;
    }
    const auto total = file.size();

    // The byte order mark is kept apart, written back when saving. A mark of UTF-16 or 32
    // gives the exact codec (UTF-16 alone would read big endian without it).
    const auto head = file.peek( 4 );
    *format = FileFormat{};
    format->bom = bomOf( head );
    if ( format->bom.size() > 3 || format->bom.size() == 2 ) {
        codec = QTextCodec::codecForUtfText( head, codec );
    }
    // Not converted: UTF-8, ASCII
    const auto mib = codec ? codec->mibEnum() : MibUtf8;
    const auto convert = mib != MibUtf8 && mib != MibAscii;
    if ( convert ) {
        format->codec = codec->name();
    }
    // The mark of UTF-8 in a file read in another encoding is a part of its text
    if ( format->bom.size() == 3 && convert ) {
        format->bom.clear();
    }
    if ( !format->bom.isEmpty() ) {
        file.read( format->bom.size() );
    }

    std::unique_ptr<QTextDecoder> decoder;
    // The text is encoded again and compared with the bytes read: the codecs do not all tell
    // the bytes they replace or drop (GBK), saving would change them
    std::unique_ptr<QTextEncoder> checker;
    QByteArray unchecked;
    if ( convert ) {
        decoder.reset( codec->makeDecoder( QTextCodec::IgnoreHeader ) );
        checker.reset( codec->makeEncoder( QTextCodec::IgnoreHeader ) );
    }

    qint64 done = format->bom.size();
    bool endOfLineFound = false;
    while ( !file.atEnd() ) {
        if ( isCancelled( cancelled ) ) {
            return false;
        }
        const auto block = file.read( BlockSize );
        if ( block.isEmpty() && file.error() != QFileDevice::NoError ) {
            *error = QCoreApplication::translate( "EditView", "The file can not be read: %1" )
                         .arg( file.errorString() );
            return false;
        }
        done += block.size();

        QByteArray utf8;
        const char* data = block.constData();
        auto length = static_cast<std::size_t>( block.size() );
        if ( convert ) {
            const auto text = decoder->toUnicode( block );
            if ( decoder->hasFailure() ) {
                format->lossy = true;
            }
            if ( !format->lossy ) {
                // The bytes of a character cut by the end of the block come with the next one
                unchecked.append( block );
                const auto encoded = checker->fromUnicode( text );
                if ( !unchecked.startsWith( encoded ) || checker->hasFailure() ) {
                    format->lossy = true;
                    unchecked.clear();
                }
                else {
                    unchecked.remove( 0, encoded.size() );
                }
            }
            utf8 = text.toUtf8();
            data = utf8.constData();
            length = static_cast<std::size_t>( utf8.size() );
        }

        if ( !endOfLineFound ) {
            const auto endOfLine = endOfLineOf( data, length );
            if ( !endOfLine.isEmpty() ) {
                format->endOfLine = endOfLine;
                endOfLineFound = true;
            }
        }

        if ( length > 0 && !sink( data, length ) ) {
            *error = QCoreApplication::translate( "EditView", "Not enough memory for the file." );
            return false;
        }
        setProgress( progress, done, total );
    }
    // Bytes left at the end: a character cut, dropped by the decoder
    if ( !unchecked.isEmpty() ) {
        format->lossy = true;
    }
    setProgress( progress, 1, 1 );
    return true;
}

bool writeFile( const QString& fileName, const char* utf8, std::size_t length,
                const FileFormat& format, QString* error, const std::atomic_bool* cancelled,
                std::atomic_int* progress )
{
    std::unique_ptr<QTextEncoder> encoder;
    if ( !format.isUtf8() ) {
        auto* codec = QTextCodec::codecForName( format.codec );
        if ( codec == nullptr ) {
            *error = QCoreApplication::translate( "EditView", "Unknown encoding %1." )
                         .arg( QString::fromLatin1( format.codec ) );
            return false;
        }
        encoder.reset( codec->makeEncoder( QTextCodec::IgnoreHeader ) );
    }

    // Written next to the file, then renamed over it: an error leaves the file as it was
    QSaveFile file( fileName );
    if ( !file.open( QIODevice::WriteOnly ) ) {
        *error = QCoreApplication::translate( "EditView", "The file can not be written: %1" )
                     .arg( file.errorString() );
        return false;
    }

    if ( !format.bom.isEmpty() ) {
        file.write( format.bom );
    }

    std::size_t done = 0;
    while ( done < length ) {
        if ( isCancelled( cancelled ) ) {
            file.cancelWriting();
            return false;
        }
        auto end = std::min( length, done + static_cast<std::size_t>( BlockSize ) );
        if ( encoder ) {
            // Not inside a character: continuation bytes are 10xxxxxx
            while ( end < length && ( static_cast<unsigned char>( utf8[ end ] ) & 0xC0 ) == 0x80 ) {
                ++end;
            }
            const auto text = QString::fromUtf8( utf8 + done, static_cast<qsizetype>( end - done ) );
            const auto encoded = encoder->fromUnicode( text );
            if ( encoder->hasFailure() ) {
                file.cancelWriting();
                *error = QCoreApplication::translate(
                             "EditView",
                             "Some characters can not be written in %1, the file was not saved." )
                             .arg( QString::fromLatin1( format.codec ) );
                return false;
            }
            file.write( encoded );
        }
        else {
            file.write( utf8 + done, static_cast<qint64>( end - done ) );
        }
        done = end;
        setProgress( progress, static_cast<qint64>( done ), static_cast<qint64>( length ) );
    }

    if ( !file.commit() ) {
        *error = QCoreApplication::translate( "EditView", "The file can not be written: %1" )
                     .arg( file.errorString() );
        return false;
    }
    setProgress( progress, 1, 1 );
    return true;
}

std::optional<quint64> availableMemory()
{
#if defined( Q_OS_MAC )
    vm_statistics64_data_t stats;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t pageSize = 0;
    if ( host_page_size( mach_host_self(), &pageSize ) != KERN_SUCCESS
         || host_statistics64( mach_host_self(), HOST_VM_INFO64,
                               reinterpret_cast<host_info64_t>( &stats ), &count )
                != KERN_SUCCESS ) {
        return std::nullopt;
    }
    // Free, and the pages the system gives back when asked
    return static_cast<quint64>( stats.free_count + stats.inactive_count
                                 + stats.purgeable_count )
           * pageSize;
#elif defined( Q_OS_WIN )
    MEMORYSTATUSEX status;
    status.dwLength = sizeof( status );
    if ( !GlobalMemoryStatusEx( &status ) ) {
        return std::nullopt;
    }
    return static_cast<quint64>( status.ullAvailPhys );
#elif defined( Q_OS_LINUX )
    std::ifstream meminfo( "/proc/meminfo" );
    std::string key;
    quint64 value = 0;
    std::string unit;
    while ( meminfo >> key >> value >> unit ) {
        if ( key == "MemAvailable:" ) {
            return value * 1024;
        }
    }
    return std::nullopt;
#else
    return std::nullopt;
#endif
}

bool isTemporaryCopy( const QString& fileName )
{
    // The temporary folder of the window (QTemporaryDir): <temp>/<application>-XXXXXX
    const auto prefix = QDir::cleanPath( QDir::tempPath() ) + QLatin1Char( '/' )
                        + QCoreApplication::applicationName() + QLatin1Char( '-' );
    const auto path = QDir::cleanPath( QFileInfo( fileName ).absoluteFilePath() );
    const auto canonical = QFileInfo( fileName ).canonicalFilePath();
    const auto canonicalPrefix
        = QFileInfo( QDir::tempPath() ).canonicalFilePath() + QLatin1Char( '/' )
          + QCoreApplication::applicationName() + QLatin1Char( '-' );
    return path.startsWith( prefix ) || ( !canonical.isEmpty() && canonical.startsWith( canonicalPrefix ) );
}

} // namespace edit
