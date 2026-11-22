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

#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPlainTextEdit>

#include "loganalysis.h"
#include "ruleseditor.h"
#include "selectiontimelabel.h"

TEST_CASE( "Rules written and read back", "[rules]" )
{
    const auto set = analysis::parseRules( analysis::exampleRules() );
    REQUIRE( set.error.isEmpty() );
    const auto again = analysis::parseRules( analysis::rulesToJson( set.rules ) );
    REQUIRE( again.error.isEmpty() );
    REQUIRE( again.rules.size() == set.rules.size() );
    for ( size_t r = 0; r < set.rules.size(); ++r ) {
        CHECK( again.rules[ r ].name == set.rules[ r ].name );
        CHECK( again.rules[ r ].type == set.rules[ r ].type );
        CHECK( again.rules[ r ].patterns == set.rules[ r ].patterns );
        CHECK( again.rules[ r ].withinSeconds == set.rules[ r ].withinSeconds );
        CHECK( again.rules[ r ].note == set.rules[ r ].note );
    }
    analysis::Rule absence;
    absence.type = analysis::Rule::Type::Absence;
    absence.patterns = { "only one" };
    CHECK( !analysis::checkRule( absence ).isEmpty() );
    absence.patterns.append( "(" );
    CHECK( analysis::checkRule( absence ).contains( "Invalid" ) );
    absence.regex = false;
    CHECK( analysis::checkRule( absence ).isEmpty() );
    absence.patterns.last() = "two";
    absence.regex = true;
    CHECK( analysis::checkRule( absence ).isEmpty() );

    // Plain text rules: written as "regex": false, read back the same
    analysis::Rule text;
    text.name = "text";
    text.patterns = { "a (b)" };
    text.regex = false;
    const auto textBack = analysis::parseRules( analysis::rulesToJson( { text } ) );
    REQUIRE( textBack.rules.size() == 1 );
    CHECK( !textBack.rules[ 0 ].regex );
    CHECK( textBack.rules[ 0 ].patterns == text.patterns );
    CHECK( !analysis::rulesToJson( { text } ).contains( "\\\\(" ) );
    CHECK( analysis::parseRules( R"({"rules":[{"type":"match","regex":false,"patterns":["("]}]})" )
               .error.isEmpty() );
    CHECK( again.rules[ 0 ].regex );
}

TEST_CASE( "Notes of rules with links", "[rules]" )
{
    CHECK( RulesEditor::noteHtml( "See https://wiki.example.com/runbook, then ask <ops>." )
           == "See <a href=\"https://wiki.example.com/runbook\">https://wiki.example.com/runbook</a>"
              ", then ask &lt;ops&gt;." );
    CHECK( RulesEditor::noteHtml( "(www.example.com/a?b=1)\nline 2" )
           == "(<a href=\"https://www.example.com/a?b=1\">www.example.com/a?b=1</a>)<br>line 2" );
    CHECK( RulesEditor::noteHtml( "no link" ) == "no link" );
    // The older name of the field is read too
    const auto set
        = analysis::parseRules( R"json({"rules":[{"name":"a","pattern":"x","link":"http://y"}]})json" );
    REQUIRE( set.rules.size() == 1 );
    CHECK( set.rules[ 0 ].note == "http://y" );
}

TEST_CASE( "Rules editor", "[rules]" )
{
    QTemporaryDir dir;
    const auto file = dir.filePath( "rules.json" );
    RulesEditor editor( {} );
    CHECK( editor.rules().empty() );

    // A rule from a selected text: the text as it is, plain text
    editor.addRuleFor( "connect failed (code 17)" );
    REQUIRE( editor.rules().size() == 1 );
    CHECK( editor.rules()[ 0 ].patterns == QStringList{ "connect failed (code 17)" } );
    CHECK( !editor.rules()[ 0 ].regex );

    // Edited in the form
    auto* name = editor.findChildren<QLineEdit*>().at( 0 );
    name->setText( "Connect failed" );
    Q_EMIT name->textEdited( name->text() );
    auto* type = editor.findChildren<QComboBox*>().at( 0 );
    type->setCurrentIndex( 2 ); // absence
    auto* patterns = editor.findChild<QPlainTextEdit*>();
    patterns->setPlainText( "connect start id=(\\d+)\nconnected id=(\\d+)" );
    const auto checkBox = [ &editor ]( const QString& text ) {
        for ( auto* box : editor.findChildren<QCheckBox*>() ) {
            if ( box->text() == text ) {
                return box;
            }
        }
        return static_cast<QCheckBox*>( nullptr );
    };
    checkBox( "Regular expression" )->setChecked( true );
    checkBox( "Within" )->setChecked( true );

    const auto& rule = editor.rules()[ 0 ];
    CHECK( rule.name == "Connect failed" );
    CHECK( rule.type == analysis::Rule::Type::Absence );
    CHECK( rule.patterns.size() == 2 );
    CHECK( rule.regex );
    CHECK( rule.withinSeconds == 5.0 );

    const auto renderDir = qEnvironmentVariable( "FLOGG_RENDER_DIR" );
    if ( !renderDir.isEmpty() ) {
        RulesEditor example( {} );
        QFile rules( renderDir + "/example_rules.json" );
        rules.open( QIODevice::WriteOnly );
        rules.write( analysis::exampleRules() );
        rules.close();
        RulesEditor shown( renderDir + "/example_rules.json" );
        shown.show();
        shown.grab().save( renderDir + "/rules_editor.png" );
    }

    REQUIRE( editor.save( file ) );
    RulesEditor reopened( file );
    REQUIRE( reopened.rules().size() == 1 );
    CHECK( reopened.rules()[ 0 ].patterns == rule.patterns );
}

#include <QHBoxLayout>
#include <QToolButton>
#include "styles.h"

TEST_CASE( "Time between lines label", "[timestamp]" )
{
    const auto renderDir = qEnvironmentVariable( "FLOGG_RENDER_DIR" );
    if ( !renderDir.isEmpty() ) {
        SelectionTimeLabel shown;
        const auto save = [ & ]( const QString& name ) {
            shown.resize( shown.sizeHint() );
            shown.grab().save( renderDir + "/" + name + ".png" );
        };
        shown.setTime( "Δ +1.234 s", {} );
        save( "time_rest" );
        shown.setTime( "Δ -29.179 s ⚠", {} );
        save( "time_warning" );
        shown.setTime( "Δ: No time stamp in the last line", {} );
        save( "time_none" );

        // Next to a checked tool button in the flogg styles: the badge at rest uses its colours
        for ( const auto& styleName : { StyleManager::LightStyleKey, StyleManager::DarkStyleKey } ) {
            StyleManager::applyStyle( styleName );
            QWidget bar;
            auto* layout = new QHBoxLayout( &bar );
            auto* badge = new SelectionTimeLabel;
            badge->setTime( "Δ +32 µs", {} );
            auto* button = new QToolButton;
            button->setAutoRaise( true );
            button->setCheckable( true );
            button->setChecked( true );
            button->setText( "Tool" );
            layout->addWidget( badge );
            layout->addWidget( button );
            bar.show();
            bar.grab().save( renderDir + "/time_beside_checked_" + QString( styleName ) + ".png" );
        }
        StyleManager::applyStyle( StyleManager::FusionKey );
    }

    SelectionTimeLabel label;
    label.setTime( "Δ +1.234 s ⚠", "Line 1: ..." );
    CHECK( label.durationText() == "+1.234 s" );
    CHECK( label.toolTip().startsWith( "Line 1: ..." ) );
    CHECK( label.toolTip().contains( "Right click" ) );
    label.setTime( "Δ: No time stamp in the last line", {} );
    CHECK( label.durationText() == "No time stamp in the last line" );
}

#include "timeformatsdialog.h"
#include "timestamp.h"

TEST_CASE( "Time formats window", "[timestamp]" )
{
    const QString line = "<I> 22.11.2026 at 14:32:05 . 120 main started";
    TimeFormatsDialog dialog( line );
    CHECK( dialog.result().contains( "time of day" ) );

    // A format of the user, tried on the line as it is typed
    dialog.setFormats( { "dd.MM.yyyy at HH:mm:ss . fff", "yyyy-MM-dd" } );
    CHECK( dialog.result().contains( "dd.MM.yyyy at HH:mm:ss . fff" ) );
    CHECK( dialog.result().contains( "2026-11-22 14:32:05.120" ) );
    CHECK( timestamps::customFormats() == QStringList{ "dd.MM.yyyy at HH:mm:ss . fff" } );

    const auto renderDir = qEnvironmentVariable( "FLOGG_RENDER_DIR" );
    if ( !renderDir.isEmpty() ) {
        dialog.show();
        dialog.grab().save( renderDir + "/time_formats.png" );
    }

    // Cancel puts the previous formats back
    dialog.reject();
    CHECK( timestamps::customFormats().isEmpty() );
}
