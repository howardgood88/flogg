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

#include <QFile>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "filemerge.h"

namespace {
QString write( const QTemporaryDir& dir, const QString& name, const QByteArray& content )
{
    const auto path = dir.filePath( name );
    QFile file( path );
    REQUIRE( file.open( QIODevice::WriteOnly ) );
    file.write( content );
    return path;
}

QByteArray read( const QString& path )
{
    QFile file( path );
    REQUIRE( file.open( QIODevice::ReadOnly ) );
    return file.readAll();
}
} // namespace

TEST_CASE( "Files merged one after the other", "[filemerge]" )
{
    QTemporaryDir dir;
    const auto a = write( dir, "a.log", "a1\na2\n" );
    const auto b = write( dir, "b.log", "b1\nb2" );                  // no line end at the end
    const auto c = write( dir, "c.log", "\xEF\xBB\xBF" "c1\r\nc2" ); // BOM, CRLF
    const auto d = write( dir, "d.log", "" );
    const auto e = write( dir, "e.log", "e1\n" );
    AtomicFlag cancel;

    const auto output = dir.filePath( "out.log" );
    int lastProgress = -1;
    CHECK( filemerge::merge( { a, b, c, d, e }, output, {}, cancel,
                             [ &lastProgress ]( int p ) { lastProgress = p; } )
               .isEmpty() );
    CHECK( read( output ) == "a1\na2\nb1\nb2\nc1\r\nc2\r\ne1\n" );
    CHECK( lastProgress == 100 );

    // In another order
    CHECK( filemerge::merge( { e, a }, output, {}, cancel ).isEmpty() );
    CHECK( read( output ) == "e1\na1\na2\n" );

    // The BOM of the first file stays
    CHECK( filemerge::merge( { c, a }, output, {}, cancel ).isEmpty() );
    CHECK( read( output ) == "\xEF\xBB\xBF" "c1\r\nc2\r\na1\na2\n" );

    // With the file names
    filemerge::Options options;
    options.fileNameLines = true;
    CHECK( filemerge::merge( { a, b }, output, options, cancel ).isEmpty() );
    CHECK( read( output ) == "==> a.log <==\na1\na2\n\n==> b.log <==\nb1\nb2" );

    // Mistakes leave the output as it was
    CHECK( filemerge::merge( { a, output }, output, {}, cancel ).contains( "can not be one" ) );
    CHECK( filemerge::merge( { a, dir.filePath( "missing.log" ) }, output, {}, cancel )
               .contains( "Can not read" ) );
    CHECK( read( output ) == "==> a.log <==\na1\na2\n\n==> b.log <==\nb1\nb2" );

    cancel.set();
    const auto cancelled = dir.filePath( "cancelled.log" );
    CHECK( filemerge::merge( { a, b }, cancelled, {}, cancel ) == "Stopped" );
    CHECK( !QFile::exists( cancelled ) );
}

TEST_CASE( "Merged file name", "[filemerge]" )
{
    QTemporaryDir dir;
    const auto a = write( dir, "a.log", "a" );
    CHECK( filemerge::defaultOutput( { a } ) == dir.filePath( "merged.log" ) );
    write( dir, "merged.log", "" );
    CHECK( filemerge::defaultOutput( { a } ) == dir.filePath( "merged-2.log" ) );
}

TEST_CASE( "Merge files dialog", "[filemerge]" )
{
    QTemporaryDir dir;
    const auto a = write( dir, "a.log", "a\n" );
    const auto b = write( dir, "b.log", "b\n" );
    const auto c = write( dir, "c.log", "c\n" );

    MergeFilesDialog dialog( { a, b, c } );
    CHECK( dialog.files() == QStringList{ a, b, c } );
    CHECK( dialog.output() == dir.filePath( "merged.log" ) );

    // c moved to the top
    auto* list = dialog.findChild<QListWidget*>();
    REQUIRE( list );
    list->setCurrentRow( 2 );
    auto buttons = dialog.findChildren<QPushButton*>();
    const auto button = [ &buttons ]( const QString& text ) {
        return *std::find_if( buttons.begin(), buttons.end(),
                              [ &text ]( auto* b ) { return b->text() == text; } );
    };
    button( "Up" )->click();
    button( "Up" )->click();
    CHECK( !button( "Up" )->isEnabled() );
    CHECK( dialog.files() == QStringList{ c, a, b } );

    const auto renderDir = qEnvironmentVariable( "FLOGG_RENDER_DIR" );
    if ( !renderDir.isEmpty() ) {
        dialog.grab().save( renderDir + "/merge_files.png" );
    }

    QSignalSpy merged( &dialog, &MergeFilesDialog::merged );
    dialog.merge();
    REQUIRE( merged.wait( 5000 ) );
    CHECK( merged.front().front().toString() == dir.filePath( "merged.log" ) );
    CHECK( read( dir.filePath( "merged.log" ) ) == "c\na\nb\n" );
}
