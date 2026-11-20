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

#include <catch2/catch.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextCodec>

#include "editdocument.h"
#include "fileholder.h"

namespace {

QByteArray readAll( const QString& path )
{
    QFile file( path );
    REQUIRE( file.open( QIODevice::ReadOnly ) );
    return file.readAll();
}

void writeAll( const QString& path, const QByteArray& content )
{
    QFile file( path );
    REQUIRE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    file.write( content );
}

// The text in UTF-8 as the editor gets it, and the format of the file
QByteArray load( const QString& path, const char* codecName, edit::FileFormat* format )
{
    QByteArray text;
    QString error;
    const auto ok = edit::readFile(
        path, QTextCodec::codecForName( codecName ),
        [ &text ]( const char* data, std::size_t length ) {
            text.append( data, static_cast<qsizetype>( length ) );
            return true;
        },
        format, &error );
    INFO( error.toStdString() );
    REQUIRE( ok );
    return text;
}

bool save( const QString& path, const QByteArray& text, const edit::FileFormat& format,
           QString* error = nullptr )
{
    QString message;
    const auto ok = edit::writeFile( path, text.constData(), static_cast<std::size_t>( text.size() ),
                                     format, error ? error : &message );
    return ok;
}

} // namespace

TEST_CASE( "Edit mode files are written back byte for byte", "[edit]" )
{
    QTemporaryDir dir;
    const auto path = dir.filePath( "file.txt" );

    struct Case {
        const char* name;
        const char* codec;
        QByteArray bytes;
        QByteArray utf8;
        QByteArray endOfLine;
    };
    const auto gbk = QTextCodec::codecForName( "GBK" );
    const auto big5 = QTextCodec::codecForName( "Big5" );
    const auto chinese = QStringLiteral( "日誌第一行\r\n第二行\r\n" );
    const std::vector<Case> cases{
        { "UTF-8", "UTF-8", "one\ntwo \xC3\xA9\n", "one\ntwo \xC3\xA9\n", "\n" },
        { "UTF-8 BOM CRLF", "UTF-8", "\xEF\xBB\xBFone\r\ntwo\r\n", "one\r\ntwo\r\n", "\r\n" },
        { "ASCII", "US-ASCII", "plain\rold mac\r", "plain\rold mac\r", "\r" },
        // Bytes that are not UTF-8 are kept as they are
        { "Invalid UTF-8", "UTF-8", "bad \xFF\xFE byte\n", "bad \xFF\xFE byte\n", "\n" },
        { "GBK", "GBK", gbk->fromUnicode( QStringLiteral( "日志第一行\r\n第二行\r\n" ) ),
          QStringLiteral( "日志第一行\r\n第二行\r\n" ).toUtf8(), "\r\n" },
        { "Big5", "Big5", big5->fromUnicode( chinese ), chinese.toUtf8(), "\r\n" },
        { "UTF-16LE BOM", "UTF-16LE", QByteArray( "\xFF\xFE" "a\0\n\0b\0\n\0", 10 ), "a\nb\n",
          "\n" },
    };

    for ( const auto& testCase : cases ) {
        INFO( testCase.name );
        writeAll( path, testCase.bytes );

        edit::FileFormat format;
        const auto text = load( path, testCase.codec, &format );
        CHECK( text == testCase.utf8 );
        CHECK( format.endOfLine == testCase.endOfLine );
        CHECK_FALSE( format.lossy );

        // Not modified: the same bytes
        REQUIRE( save( path, text, format ) );
        CHECK( readAll( path ) == testCase.bytes );
    }
}

TEST_CASE( "Edit mode changes are written in the encoding of the file", "[edit]" )
{
    QTemporaryDir dir;
    const auto path = dir.filePath( "file.txt" );
    auto* gbk = QTextCodec::codecForName( "GBK" );
    writeAll( path, gbk->fromUnicode( QStringLiteral( "第一行\n" ) ) );

    edit::FileFormat format;
    auto text = load( path, "GBK", &format );
    CHECK( format.codec == "GBK" );
    text.append( QStringLiteral( "新增的一行\n" ).toUtf8() );
    REQUIRE( save( path, text, format ) );
    CHECK( readAll( path ) == gbk->fromUnicode( QStringLiteral( "第一行\n新增的一行\n" ) ) );

    // A character GBK has not: nothing written
    const auto before = readAll( path );
    QString error;
    CHECK_FALSE( save( path, text + QStringLiteral( "\U0001F600\n" ).toUtf8(), format, &error ) );
    CHECK( error.contains( "GBK" ) );
    CHECK( readAll( path ) == before );
}

TEST_CASE( "Edit mode blocks keep the characters whole", "[edit]" )
{
    QTemporaryDir dir;
    const auto path = dir.filePath( "big.txt" );
    auto* gbk = QTextCodec::codecForName( "GBK" );

    // More than a block of two byte characters, a character across the end of a block
    QString content;
    while ( content.size() * 2 < edit::BlockSize * 2 + 3 ) {
        content.append( QStringLiteral( "中文日誌記錄行\n" ) );
    }
    const auto bytes = gbk->fromUnicode( QStringLiteral( "x" ) + content );
    writeAll( path, bytes );

    edit::FileFormat format;
    const auto text = load( path, "GBK", &format );
    CHECK_FALSE( format.lossy );
    CHECK( QString::fromUtf8( text ) == QStringLiteral( "x" ) + content );
    REQUIRE( save( path, text, format ) );
    CHECK( readAll( path ) == bytes );
}

TEST_CASE( "Edit mode marks bytes it can not read", "[edit]" )
{
    QTemporaryDir dir;
    const auto path = dir.filePath( "file.txt" );
    // Not GBK: replaced, or cut at the end (dropped)
    for ( const auto* bytes : { "ok \x81\n", "ok \x81" } ) {
        INFO( bytes );
        writeAll( path, bytes );
        edit::FileFormat format;
        load( path, "GBK", &format );
        CHECK( format.lossy );
    }
    writeAll( path, QTextCodec::codecForName( "GBK" )->fromUnicode( QStringLiteral( "好\n" ) ) );
    edit::FileFormat format;
    load( path, "GBK", &format );
    CHECK_FALSE( format.lossy );

    // Mapped to a private character by ICU, written back as it was; marked as not read by
    // the codecs of Qt without ICU (Windows, the Qt of the installer): told before a save
    writeAll( path, "ok \xFF\n" );
    const auto text = load( path, "GBK", &format );
    if ( !format.lossy ) {
        REQUIRE( save( path, text, format ) );
        CHECK( readAll( path ) == "ok \xFF\n" );
    }
}

TEST_CASE( "Edit mode keeps the permissions of the file", "[edit]" )
{
    QTemporaryDir dir;
    const auto path = dir.filePath( "file.txt" );
    writeAll( path, "a\n" );
    const auto permissions = QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup;
    REQUIRE( QFile::setPermissions( path, permissions ) );

    edit::FileFormat format;
    const auto text = load( path, "UTF-8", &format );
    REQUIRE( save( path, text + "b\n", format ) );
    CHECK( readAll( path ) == "a\nb\n" );
#ifdef Q_OS_WIN
    // Only the read only attribute on Windows: the other permissions are made up by Qt
    CHECK( QFileInfo( path ).isWritable() );
#else
    const auto mask = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup
                      | QFile::WriteGroup | QFile::ExeGroup | QFile::ReadOther
                      | QFile::WriteOther | QFile::ExeOther;
    CHECK( ( QFile::permissions( path ) & mask ) == permissions );
#endif
}

TEST_CASE( "Edit mode reading can be cancelled", "[edit]" )
{
    QTemporaryDir dir;
    const auto path = dir.filePath( "file.txt" );
    writeAll( path, QByteArray( static_cast<qsizetype>( edit::BlockSize * 3 ), 'x' ) );

    const std::atomic_bool cancelled{ true };
    edit::FileFormat format;
    QString error;
    std::size_t received = 0;
    CHECK_FALSE( edit::readFile(
        path, nullptr,
        [ &received ]( const char*, std::size_t length ) {
            received += length;
            return true;
        },
        &format, &error, &cancelled ) );
    CHECK( received == 0 );
}

TEST_CASE( "Edit mode helpers", "[edit]" )
{
    CHECK( edit::isTemporaryCopy( QDir::tempPath() + "/" + QCoreApplication::applicationName()
                                  + "-abc123/archive.log" ) );
    CHECK_FALSE( edit::isTemporaryCopy( QDir::homePath() + "/logs/app.log" ) );
#if defined( Q_OS_MAC ) || defined( Q_OS_LINUX ) || defined( Q_OS_WIN )
    const auto memory = edit::availableMemory();
    REQUIRE( memory );
    CHECK( *memory > 0 );
#endif
}

TEST_CASE( "Edit mode save: the log file is closed meanwhile", "[edit]" )
{
    QTemporaryDir dir;
    const auto path = dir.filePath( "file.txt" );
    writeAll( path, "a\n" );
    for ( const auto keepClosed : { false, true } ) {
        INFO( keepClosed );
        FileHolder holder( keepClosed );
        holder.open( path );
        CHECK( holder.isOpen() != keepClosed );
        holder.attachReader();
        CHECK( holder.isOpen() );

        // Not opened again by a reader nor a change of the file until restored
        holder.release();
        CHECK_FALSE( holder.isOpen() );
        holder.detachReader();
        holder.attachReader();
        CHECK_FALSE( holder.isOpen() );
        holder.reOpenFile();
        CHECK_FALSE( holder.isOpen() );
        // The file can be replaced
        REQUIRE( QFile::remove( path ) );
        writeAll( path, "a\nb\n" );

        holder.restore();
        CHECK( holder.isOpen() );
        CHECK( holder.size() == 4 );
        holder.detachReader();
        CHECK( holder.isOpen() != keepClosed );
    }
}
