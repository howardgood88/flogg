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

#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextCodec>

#include "dirsearch.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
// A leading dot hides a file on macOS and Linux, Windows has an attribute for it
void markHidden( const QString& path )
{
#ifdef Q_OS_WIN
    REQUIRE( SetFileAttributesW( reinterpret_cast<LPCWSTR>( QDir::toNativeSeparators( path ).utf16() ),
                                 FILE_ATTRIBUTE_HIDDEN ) );
#else
    Q_UNUSED( path );
#endif
}


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

dirsearch::FileResult search( const QString& path, const dirsearch::Query& query,
                              qint64 maxLines = dirsearch::MaxResultLines )
{
    dirsearch::FileResult result;
    QString error;
    const auto ok = dirsearch::searchFile( path, query.pattern(), maxLines, &result, &error );
    INFO( error.toStdString() );
    REQUIRE( ok );
    return result;
}

qint64 replace( const QString& path, const dirsearch::Query& query, const QString& replacement,
                const QSet<qint64>* lines = nullptr, QString* error = nullptr )
{
    QString message;
    return dirsearch::replaceInFile( path, query.pattern(), replacement, query.regex, lines,
                                     error ? error : &message );
}

dirsearch::Query plain( const QString& text, bool caseSensitive = false )
{
    dirsearch::Query query;
    query.text = text;
    query.caseSensitive = caseSensitive;
    return query;
}

dirsearch::Query regex( const QString& text )
{
    dirsearch::Query query;
    query.text = text;
    query.regex = true;
    query.caseSensitive = true;
    return query;
}

} // namespace

TEST_CASE( "Search in a folder lists the files", "[dirsearch]" )
{
    QTemporaryDir dir;
    QDir( dir.path() ).mkpath( "sub/deep" );
    QDir( dir.path() ).mkpath( ".hidden" );
    writeAll( dir.filePath( "a.log" ), "x\n" );
    writeAll( dir.filePath( "b.txt" ), "x\n" );
    writeAll( dir.filePath( "sub/c.log" ), "x\n" );
    writeAll( dir.filePath( "sub/deep/d.log" ), "x\n" );
    writeAll( dir.filePath( ".hidden/e.log" ), "x\n" );
    writeAll( dir.filePath( ".f.log" ), "x\n" );
    markHidden( dir.filePath( ".hidden" ) );
    markHidden( dir.filePath( ".f.log" ) );

    const auto names = []( const QStringList& files, const QString& root ) {
        QStringList relative;
        for ( const auto& file : files ) {
            relative.append( QDir( root ).relativeFilePath( file ) );
        }
        return relative;
    };

    const auto all = dirsearch::listFiles( dir.path(), {}, false );
    CHECK( names( all, dir.path() )
           == QStringList{ "a.log", "b.txt", "sub/c.log", "sub/deep/d.log" } );

    const QRegularExpression logs( "^.*\\.log$", QRegularExpression::CaseInsensitiveOption );
    CHECK( names( dirsearch::listFiles( dir.path(), logs, false ), dir.path() )
           == QStringList{ "a.log", "sub/c.log", "sub/deep/d.log" } );

    const auto hidden = names( dirsearch::listFiles( dir.path(), logs, true ), dir.path() );
    CHECK( hidden.contains( ".hidden/e.log" ) );
    CHECK( hidden.contains( ".f.log" ) );

    CHECK( names( dirsearch::listFiles( dir.filePath( "sub" ), {}, false ), dir.filePath( "sub" ) )
           == QStringList{ "c.log", "deep/d.log" } );
}

TEST_CASE( "Search in a file finds the lines", "[dirsearch]" )
{
    QTemporaryDir dir;
    const auto path = dir.filePath( "file.log" );

    SECTION( "Plain text, every match of a line" )
    {
        writeAll( path, "one Error\r\ntwo\r\nerror and ERROR\r\nlast error" );
        const auto result = search( path, plain( "error" ) );
        CHECK( result.matchCount == 4 );
        CHECK( result.lineCount == 4 );
        REQUIRE( result.lines.size() == 3 );
        CHECK( result.lines[ 0 ].line == 0 );
        CHECK( result.lines[ 0 ].preview == "one Error" );
        CHECK( result.lines[ 0 ].ranges == std::vector<std::pair<int, int>>{ { 4, 5 } } );
        CHECK( result.lines[ 1 ].line == 2 );
        CHECK( result.lines[ 1 ].ranges == std::vector<std::pair<int, int>>{ { 0, 5 }, { 10, 5 } } );
        CHECK( result.lines[ 2 ].line == 3 );

        CHECK( search( path, plain( "error", true ) ).matchCount == 2 );
        // A plain text is not a regular expression
        CHECK( search( path, plain( "e.ror" ) ).matchCount == 0 );
    }

    SECTION( "A regular expression does not cross the end of a line" )
    {
        writeAll( path, "a1\nb\na22\n" );
        auto result = search( path, regex( "^a\\d+$" ) );
        CHECK( result.matchCount == 2 );
        // \s would take the line feed: the line is matched alone
        result = search( path, regex( "a\\d+\\s*" ) );
        CHECK( result.matchCount == 2 );
        REQUIRE( result.lines.size() == 2 );
        CHECK( result.lines[ 0 ].ranges == std::vector<std::pair<int, int>>{ { 0, 2 } } );
        CHECK( result.lines[ 1 ].line == 2 );
        // An empty match is not one
        CHECK( search( path, regex( "x*" ) ).matchCount == 0 );
        CHECK( search( path, regex( "\\n" ) ).matchCount == 0 );
    }

    SECTION( "Lines across blocks" )
    {
        // More than a block of 1 MiB: the line numbers go on
        QByteArray content;
        for ( int i = 0; i < 100'000; ++i ) {
            content += QByteArray::number( i ) + ( i % 1000 == 999 ? " mark" : " filler text" ) + '\n';
        }
        writeAll( path, content );
        const auto result = search( path, plain( "mark" ) );
        CHECK( result.matchCount == 100 );
        REQUIRE( result.lines.size() == 100 );
        CHECK( result.lines.back().line == 99'999 );
        CHECK( result.lineCount == 100'000 );

        // Only counted past the limit
        const auto limited = search( path, plain( "mark" ), 10 );
        CHECK( limited.matchCount == 100 );
        CHECK( limited.lines.size() == 10 );
    }

    SECTION( "A long line is shown around its match" )
    {
        writeAll( path, QByteArray( 1000, 'a' ) + "needle" + QByteArray( 1000, 'b' ) + "\n" );
        const auto result = search( path, plain( "needle" ) );
        REQUIRE( result.lines.size() == 1 );
        const auto& line = result.lines[ 0 ];
        CHECK( line.preview.size() <= dirsearch::MaxPreviewLength + 2 );
        REQUIRE( line.ranges.size() == 1 );
        CHECK( line.preview.mid( line.ranges[ 0 ].first, line.ranges[ 0 ].second ) == "needle" );
    }

    SECTION( "Binary files are skipped" )
    {
        writeAll( path, QByteArray( "needle\0\x01\x02", 9 ) );
        dirsearch::FileResult result;
        QString error;
        CHECK_FALSE( dirsearch::searchFile( path, plain( "needle" ).pattern(), 10, &result, &error ) );
        CHECK( error.isEmpty() );
    }

    SECTION( "UTF-16 with its byte order mark" )
    {
        auto* codec = QTextCodec::codecForName( "UTF-16LE" );
        writeAll( path, QByteArray( "\xFF\xFE", 2 ) + codec->fromUnicode( u"first\nsecond 中文\n"_qs ) );
        const auto result = search( path, plain( u"中文"_qs ) );
        CHECK( result.matchCount == 1 );
        REQUIRE( result.lines.size() == 1 );
        CHECK( result.lines[ 0 ].line == 1 );
    }
}

TEST_CASE( "Replace in a file", "[dirsearch]" )
{
    QTemporaryDir dir;
    const auto path = dir.filePath( "file.log" );

    SECTION( "Every match, the other bytes kept" )
    {
        writeAll( path, "\xEF\xBB\xBFkeep\r\nfoo and foo\r\nFOO\r\nno end foo" );
        CHECK( replace( path, plain( "foo" ), "bar" ) == 4 );
        CHECK( readAll( path ) == "\xEF\xBB\xBFkeep\r\nbar and bar\r\nbar\r\nno end bar" );
    }

    SECTION( "Only the lines given" )
    {
        writeAll( path, "foo\nfoo\nfoo\n" );
        const QSet<qint64> lines{ 1 };
        CHECK( replace( path, plain( "foo" ), "bar", &lines ) == 1 );
        CHECK( readAll( path ) == "foo\nbar\nfoo\n" );
    }

    SECTION( "Groups of a regular expression" )
    {
        writeAll( path, "id=12 name=a\nid=7 name=b\n" );
        CHECK( replace( path, regex( "id=(\\d+) name=(\\w)" ), "\\2:\\1 \\\\" ) == 2 );
        CHECK( readAll( path ) == "a:12 \\\nb:7 \\\n" );
        // A plain text replacement keeps its backslashes
        CHECK( replace( path, plain( "a:12" ), "\\1" ) == 1 );
        CHECK( readAll( path ).startsWith( "\\1 " ) );
    }

    SECTION( "Nothing to replace leaves the file" )
    {
        writeAll( path, "abc\n" );
        const auto before = QFileInfo( path ).lastModified();
        CHECK( replace( path, plain( "zzz" ), "y" ) == 0 );
        CHECK( readAll( path ) == "abc\n" );
        CHECK( QFileInfo( path ).lastModified() == before );
    }

    SECTION( "A line with bytes its encoding can not read is not changed" )
    {
        // UTF-8 by its mark, with a byte UTF-8 does not have
        const QByteArray bom( "\xEF\xBB\xBF" );
        writeAll( path, bom + "ok foo\nbad \xFF foo\n" );
        QString error;
        CHECK( replace( path, plain( "foo" ), "bar", nullptr, &error ) == -1 );
        CHECK_FALSE( error.isEmpty() );
        CHECK( readAll( path ) == bom + "ok foo\nbad \xFF foo\n" );
        // Lines without such bytes are replaced
        const QSet<qint64> lines{ 0 };
        CHECK( replace( path, plain( "foo" ), "bar", &lines ) == 1 );
        CHECK( readAll( path ) == bom + "ok bar\nbad \xFF foo\n" );
    }

    SECTION( "A file in a single byte encoding keeps its bytes" )
    {
        writeAll( path, "caf\xE9 foo\n" );
        CHECK( replace( path, plain( "foo" ), "bar" ) == 1 );
        CHECK( readAll( path ) == "caf\xE9 bar\n" );
    }

    SECTION( "In the encoding of the file" )
    {
        auto* codec = QTextCodec::codecForName( "UTF-16LE" );
        const auto bom = QByteArray( "\xFF\xFE", 2 );
        writeAll( path, bom + codec->fromUnicode( u"日誌 one\r\nother\r\n"_qs ) );
        CHECK( replace( path, plain( "one" ), u"二"_qs ) == 1 );
        CHECK( readAll( path ) == bom + codec->fromUnicode( u"日誌 二\r\nother\r\n"_qs ) );
    }

    SECTION( "Large files, across blocks" )
    {
        QByteArray content;
        for ( int i = 0; i < 200'000; ++i ) {
            content += "line " + QByteArray::number( i ) + " value\n";
        }
        writeAll( path, content );
        CHECK( replace( path, plain( "value" ), "v" ) == 200'000 );
        auto expected = content;
        expected.replace( " value\n", " v\n" );
        CHECK( readAll( path ) == expected );
    }
}

TEST_CASE( "Search results written as a log", "[dirsearch]" )
{
    QTemporaryDir dir;
    QDir( dir.path() ).mkpath( "sub" );
    writeAll( dir.filePath( "a.log" ), "error one\r\nok\r\nerror two" );
    writeAll( dir.filePath( "sub/b.log" ), "x\nerror three\n" );
    QBuffer output;
    output.open( QIODevice::WriteOnly );
    QString error;
    std::atomic_int done{ 0 };
    REQUIRE( dirsearch::exportMatches( { dir.filePath( "a.log" ), dir.filePath( "sub/b.log" ) },
                                       dir.path(), plain( "error" ).pattern(), &output, &error,
                                       nullptr, &done ) );
    CHECK( done == 2 );
    const auto separator = QDir::separator().toLatin1();
    CHECK( output.data()
           == "a.log:1:error one\na.log:3:error two\nsub" + QByteArray( 1, separator )
                  + "b.log:2:error three\n" );
}

// Hidden, times a search and a replace of a large log:
// FLOGG_BENCH_FILE=<log> klogg_tests "[.benchmark]" "*Search in folder timing*"
TEST_CASE( "Search in folder timing", "[.benchmark]" )
{
    const auto file = qEnvironmentVariable( "FLOGG_BENCH_FILE" );
    if ( file.isEmpty() ) {
        WARN( "FLOGG_BENCH_FILE is not set" );
        return;
    }
    const auto size = static_cast<double>( QFileInfo( file ).size() ) / ( 1024 * 1024 );
    const auto time = [ & ]( const char* name, const dirsearch::Query& query ) {
        QElapsedTimer timer;
        timer.start();
        const auto result = search( file, query );
        const auto ms = static_cast<double>( timer.elapsed() );
        WARN( name << ": " << result.matchCount << " matches in " << result.lineCount
                   << " lines, " << ms << " ms, " << size / ( ms / 1000 ) << " MiB/s" );
    };
    time( "rare text", plain( "zzzz_not_there" ) );
    time( "text, case", plain( "ERROR", true ) );
    time( "text", plain( "error" ) );
    time( "regex", regex( "key=\\d+" ) );

    // A copy replaced, written next to it
    QTemporaryDir dir;
    const auto copy = dir.filePath( "copy.log" );
    REQUIRE( QFile::copy( file, copy ) );
    QElapsedTimer timer;
    timer.start();
    const auto replaced = replace( copy, plain( "ERROR", true ), "FAULT" );
    const auto ms = static_cast<double>( timer.elapsed() );
    WARN( "replace: " << replaced << " matches, " << ms << " ms, " << size / ( ms / 1000 )
                      << " MiB/s" );
}
