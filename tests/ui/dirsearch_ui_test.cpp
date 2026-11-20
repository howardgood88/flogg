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

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>

#include "test_utils.h"

#include "crawlerwidget.h"
#include "dirsearchpanel.h"
#include "editview.h"
#include "filetreepanel.h"
#include "mainwindow.h"
#include "session.h"
#include "tabbedcrawlerwidget.h"

namespace {

void writeAll( const QString& path, const QByteArray& content )
{
    QFile file( path );
    REQUIRE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    file.write( content );
}

QByteArray readAll( const QString& path )
{
    QFile file( path );
    REQUIRE( file.open( QIODevice::ReadOnly ) );
    return file.readAll();
}

// A folder of logs: two with matches, one without, one not included by its name
QString makeLogs( const QTemporaryDir& dir )
{
    const auto root = QFileInfo( dir.path() ).canonicalFilePath();
    REQUIRE( QDir( root ).mkpath( "sub" ) );
    writeAll( root + "/a.log", "start\nERROR one\nok\nerror two error\n" );
    writeAll( root + "/sub/b.log", "error three\n" );
    writeAll( root + "/c.log", "nothing\n" );
    writeAll( root + "/d.txt", "error in a text file\n" );
    return root;
}

void search( DirSearchPanel* panel, const QString& text, bool regex = false )
{
    dirsearch::Query query;
    query.text = text;
    query.regex = regex;
    panel->setQuery( query );
    panel->startSearch();
    REQUIRE( waitUiState( [ panel ] { return !panel->isBusy(); } ) );
}

QTreeWidgetItem* fileItem( DirSearchPanel* panel, const QString& name )
{
    auto* results = panel->resultsView();
    for ( int i = 0; i < results->topLevelItemCount(); ++i ) {
        if ( results->topLevelItem( i )->text( 0 ) == name ) {
            return results->topLevelItem( i );
        }
    }
    return nullptr;
}

} // namespace

SCENARIO( "Search and replace in a folder", "[ui][dirsearch]" )
{
    QTemporaryDir dir;
    const auto root = makeLogs( dir );

    DirSearchPanel panel;
    QStringList warnings;
    panel.setWarningHandler( [ &warnings ]( const QString& text ) { warnings.append( text ); } );
    panel.setFolder( root, root );
    panel.setIncludeText( "*.log" );
    QSignalSpy changed( &panel, &DirSearchPanel::filesChanged );

    search( &panel, "error" );
    CHECK( panel.fileCount() == 2 );
    CHECK( panel.matchCount() == 4 );
    auto* a = fileItem( &panel, "a.log" );
    REQUIRE( a );
    REQUIRE( a->childCount() == 2 );
    CHECK( a->child( 0 )->text( 0 ) == "ERROR one" );

    GIVEN( "a line selected" )
    {
        panel.setReplacement( "warning" );
        panel.resultsView()->setCurrentItem( a->child( 1 ) );
        panel.replaceSelected();
        REQUIRE( waitUiState( [ & ] { return !panel.isBusy(); } ) );
        CHECK( readAll( root + "/a.log" ) == "start\nERROR one\nok\nwarning two warning\n" );
        CHECK( readAll( root + "/sub/b.log" ) == "error three\n" );
        CHECK( changed.size() == 1 );
        // Searched again
        CHECK( panel.matchCount() == 2 );
        CHECK( panel.statusText().contains( "Replaced 2 matches in 1 file" ) );
    }

    GIVEN( "replace all, asked first" )
    {
        panel.setReplacement( "E" );
        QString question;
        panel.setConfirmHandler( [ &question ]( const QString& text ) {
            question = text;
            return false;
        } );
        panel.replaceAll();
        CHECK( question.contains( "4 matches in 2 files" ) );
        CHECK( readAll( root + "/sub/b.log" ) == "error three\n" );

        panel.setConfirmHandler( []( const QString& ) { return true; } );
        panel.replaceAll();
        REQUIRE( waitUiState( [ & ] { return !panel.isBusy(); } ) );
        CHECK( readAll( root + "/a.log" ) == "start\nE one\nok\nE two E\n" );
        CHECK( readAll( root + "/sub/b.log" ) == "E three\n" );
        CHECK( readAll( root + "/d.txt" ) == "error in a text file\n" );
        CHECK( panel.fileCount() == 0 );
    }

    GIVEN( "a file with changes not saved in the edit mode" )
    {
        panel.setModifiedFilesProvider( [ root ] { return QStringList{ root + "/a.log" }; } );
        panel.setConfirmHandler( []( const QString& ) { return true; } );
        panel.setReplacement( "x" );
        panel.replaceAll();
        REQUIRE( waitUiState( [ & ] { return !panel.isBusy(); } ) );
        REQUIRE( warnings.size() == 1 );
        CHECK( warnings.front().contains( "a.log" ) );
        CHECK( readAll( root + "/a.log" ) == "start\nERROR one\nok\nerror two error\n" );
        CHECK( readAll( root + "/sub/b.log" ) == "x three\n" );
    }

    GIVEN( "a file changed since the search" )
    {
        writeAll( root + "/a.log", "error moved\nstart\nERROR one\nok\nerror two error\n" );
        panel.resultsView()->setCurrentItem( a->child( 1 ) );
        panel.setReplacement( "x" );
        panel.replaceSelected();
        REQUIRE( waitUiState( [ & ] { return !panel.isBusy(); } ) );
        REQUIRE( warnings.size() == 1 );
        CHECK( warnings.front().contains( "search again" ) );
        CHECK( readAll( root + "/a.log" ).startsWith( "error moved\n" ) );
    }

    GIVEN( "a regular expression with groups" )
    {
        search( &panel, "(error) (\\w+)", true );
        // Not matching case
        CHECK( panel.matchCount() == 3 );
        panel.setConfirmHandler( []( const QString& ) { return true; } );
        panel.setReplacement( "\\2=\\1" );
        panel.replaceAll();
        REQUIRE( waitUiState( [ & ] { return !panel.isBusy(); } ) );
        CHECK( readAll( root + "/a.log" ) == "start\none=ERROR\nok\ntwo=error error\n" );
        CHECK( readAll( root + "/sub/b.log" ) == "three=error\n" );
    }

    GIVEN( "an invalid regular expression" )
    {
        search( &panel, "(", true );
        CHECK( panel.fileCount() == 0 );
        CHECK( panel.statusText().startsWith( "Invalid regular expression" ) );
    }

    GIVEN( "a sub folder" )
    {
        panel.setFolder( root + "/sub", root );
        search( &panel, "error" );
        CHECK( panel.fileCount() == 1 );
        CHECK( panel.matchCount() == 1 );
    }
}

SCENARIO( "Search from the file tree of a window", "[ui][dirsearch]" )
{
    QTemporaryDir dir;
    const auto root = makeLogs( dir );

    auto appSession = std::make_shared<Session>();
    WindowSession windowSession{ appSession, "DirSearch", 0 };
    MainWindow mainWindow( windowSession );
    mainWindow.resize( 1000, 600 );
    mainWindow.show();
    mainWindow.setRootFolder( root );
    auto* tree = mainWindow.fileTreePanel();
    auto* tabs = mainWindow.findChild<TabbedCrawlerWidget*>();
    const auto currentCrawler = [ tabs ] {
        return qobject_cast<CrawlerWidget*>( tabs->currentWidget() );
    };

    GIVEN( "the root searched" )
    {
        CHECK( tree->searchPanel() == nullptr );
        tree->searchInFolder( {} );
        auto* panel = tree->searchPanel();
        REQUIRE( panel );
        CHECK( panel->isVisible() );
        CHECK( panel->folder() == root );
        search( panel, "error" );
        CHECK( panel->fileCount() == 3 );

        WHEN( "a result is opened" )
        {
            auto* a = fileItem( panel, "a.log" );
            REQUIRE( a );
            Q_EMIT panel->resultsView()->itemActivated( a->child( 1 ), 0 );
            REQUIRE( waitUiState( [ & ] { return tabs->count() == 1; } ) );
            auto* crawler = currentCrawler();
            REQUIRE( waitUiState( [ & ] { return !crawler->isLoading(); } ) );
            CHECK_FALSE( crawler->isEditMode() );
        }

        WHEN( "the results are opened as a log" )
        {
            panel->exportResults();
            REQUIRE( waitUiState( [ & ] { return tabs->count() == 1; } ) );
            auto* crawler = currentCrawler();
            REQUIRE( waitUiState( [ & ] { return !crawler->isLoading(); } ) );
            const auto path = appSession->openFileNames().value( 0 );
            CHECK( QFileInfo( path ).fileName().startsWith( "search_error_" ) );
            CHECK( readAll( path ).startsWith( "a.log:2:ERROR one\na.log:4:error two error\n" ) );
        }

        WHEN( "the root changes to a folder out of the search" )
        {
            QTemporaryDir other;
            mainWindow.setRootFolder( other.path() );
            CHECK( panel->isHidden() );
        }
    }

    GIVEN( "the search button of the tree" )
    {
        QToolButton* searchButton = nullptr;
        for ( auto* button : tree->findChildren<QToolButton*>() ) {
            if ( button->toolTip() == "Search in Folder" ) {
                searchButton = button;
            }
        }
        REQUIRE( searchButton );
        searchButton->click();
        REQUIRE( tree->searchPanel() );
        CHECK( tree->searchPanel()->isVisible() );
        CHECK( tree->searchPanel()->folder() == root );
    }

    GIVEN( "a file searched" )
    {
        dirsearch::Query query;
        query.text = "two";
        Q_EMIT tree->searchInFileRequested( root + "/a.log", query );
        REQUIRE( waitUiState( [ & ] { return tabs->count() == 1; } ) );
        auto* crawler = currentCrawler();
        REQUIRE( crawler );
        CHECK( crawler->isEditMode() );
        auto* view = crawler->editView();
        REQUIRE( waitUiState( [ & ] { return view->isLoaded(); } ) );
        auto* findEdit = view->findChild<QLineEdit*>();
        REQUIRE( findEdit );
        REQUIRE( waitUiState( [ & ] { return findEdit->isVisible(); } ) );
        CHECK( findEdit->text() == "two" );
    }
}

// Hidden, saves a picture of the search: FLOGG_RENDER_DIR=<dir> klogg_itests "[.render]"
SCENARIO( "Search in folder rendering", "[.render]" )
{
    const auto renderDir = qEnvironmentVariable( "FLOGG_RENDER_DIR" );
    if ( renderDir.isEmpty() ) {
        WARN( "FLOGG_RENDER_DIR is not set" );
        return;
    }
    QTemporaryDir dir;
    const auto root = makeLogs( dir );
    auto appSession = std::make_shared<Session>();
    WindowSession windowSession{ appSession, "DirSearchRender", 0 };
    MainWindow mainWindow( windowSession );
    mainWindow.resize( 1000, 600 );
    mainWindow.show();
    mainWindow.setRootFolder( root );
    auto* tree = mainWindow.fileTreePanel();
    tree->searchInFolder( {} );
    search( tree->searchPanel(), "error" );
    tree->searchPanel()->setReplacement( "warning" );
    QTest::qWait( 300 );
    REQUIRE( tree->grab().save( renderDir + "/dir_search.png" ) );

    Q_EMIT tree->searchInFileRequested( root + "/a.log", tree->searchPanel()->query() );
    auto* tabs = mainWindow.findChild<TabbedCrawlerWidget*>();
    REQUIRE( waitUiState( [ & ] { return tabs->count() == 1; } ) );
    auto* crawler = qobject_cast<CrawlerWidget*>( tabs->currentWidget() );
    REQUIRE( waitUiState( [ & ] { return crawler->editView() && crawler->editView()->isLoaded(); } ) );
    QTest::qWait( 300 );
    REQUIRE( mainWindow.grab().save( renderDir + "/dir_search_file.png" ) );
}
