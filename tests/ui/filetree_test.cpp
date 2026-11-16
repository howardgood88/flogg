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

#include <QApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QScreen>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileSystemModel>
#include <QLineEdit>
#include <QMenu>
#include <QLabel>
#include <QMimeData>
#include <QToolTip>
#include <QToolButton>
#include <QMenuBar>
#include <QToolBar>
#include <QSortFilterProxyModel>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>
#include <QUrl>

#include "test_utils.h"

#include "filetreepanel.h"
#include "mainwindow.h"
#include "session.h"
#include "sessioninfo.h"
#include "styles.h"
#include "tabbedcrawlerwidget.h"
#include "tooltipplacement.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
void writeFile( const QString& path )
{
    QFile file( path );
    REQUIRE( file.open( QIODevice::WriteOnly ) );
    file.write( "line one\nline two\n" );
}

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

// A folder with app/main.log, app/worker.log, sys.log.gz and notes.txt
QString makeFolder( QTemporaryDir& dir )
{
    REQUIRE( QDir( dir.path() ).mkpath( "app" ) );
    writeFile( dir.filePath( "app/main.log" ) );
    writeFile( dir.filePath( "app/worker.log" ) );
    writeFile( dir.filePath( "sys.log.gz" ) );
    writeFile( dir.filePath( "notes.txt" ) );
    return QDir::cleanPath( QFileInfo( dir.path() ).canonicalFilePath() );
}

QFileSystemModel* fileSystem( FileTreePanel* panel )
{
    auto* proxy = qobject_cast<QSortFilterProxyModel*>( panel->treeView()->model() );
    return qobject_cast<QFileSystemModel*>( proxy->sourceModel() );
}

// Names of the rows shown under the folder once it is read and the folders are filtered
QStringList childNames( FileTreePanel* panel, const QString& folder )
{
    auto* model = fileSystem( panel );
    const auto source = model->index( folder );
    if ( model->canFetchMore( source ) ) {
        model->fetchMore( source );
    }
    waitUiState( [ & ] { return model->rowCount( source ) > 0; } );
    waitUiState( [ & ] { return !panel->isIndexing(); } );
    QTest::qWait( 50 );
    auto* view = panel->treeView()->model();
    const auto parent = panel->indexOf( folder );
    QStringList names;
    for ( int row = 0; row < view->rowCount( parent ); ++row ) {
        names.append( view->index( row, 0, parent ).data().toString() );
    }
    names.sort();
    return names;
}

// Names of the rows shown under the folder, in the order of the tree
QStringList orderedNames( FileTreePanel* panel, const QString& folder )
{
    childNames( panel, folder );
    auto* view = panel->treeView()->model();
    const auto parent = panel->indexOf( folder );
    QStringList names;
    for ( int row = 0; row < view->rowCount( parent ); ++row ) {
        names.append( view->index( row, 0, parent ).data().toString() );
    }
    return names;
}

// Text of the actions of the menu, "-" for a separator
QStringList menuTexts( QMenu* menu )
{
    QStringList texts;
    for ( const auto* action : menu->actions() ) {
        texts.append( action->isSeparator() ? QString( "-" ) : action->text() );
    }
    return texts;
}

bool isBold( FileTreePanel* panel, const QString& path )
{
    return panel->indexOf( path ).data( Qt::FontRole ).value<QFont>().bold();
}
} // namespace

TEST_CASE( "File tree name filters", "[ui]" )
{
    CHECK( FileTreePanel::nameFilters( "" ).isEmpty() );
    CHECK( FileTreePanel::nameFilters( "*.log" ) == QStringList{ "*.log" } );
    CHECK( FileTreePanel::nameFilters( "main; *.gz  worker" )
           == QStringList{ "*main*", "*.gz", "*worker*" } );

    const auto pattern = FileTreePanel::namePattern( "*.log main" );
    CHECK( pattern.match( "worker.LOG" ).hasMatch() );
    CHECK( pattern.match( "the_main.txt" ).hasMatch() );
    CHECK_FALSE( pattern.match( "worker.log.gz" ).hasMatch() );
    CHECK( FileTreePanel::namePattern( "" ).pattern().isEmpty() );
}

SCENARIO( "File tree panel shows a folder", "[ui]" )
{
    QTemporaryDir dir;
    const auto folder = makeFolder( dir );

    FileTreePanel panel;
    auto* stack = panel.findChild<QStackedWidget*>();
    // Nothing is read before a folder is set
    CHECK( stack->currentIndex() == 0 );
    CHECK( panel.treeView()->model() == nullptr );

    QSignalSpy rootChanged( &panel, &FileTreePanel::rootFolderChanged );
    panel.setRootFolder( folder );
    CHECK( rootChanged.count() == 1 );
    CHECK( stack->currentIndex() == 1 );
    CHECK( panel.rootFolder() == folder );
    CHECK( FileTreePanel::recentFolders().value( 0 ) == folder );

    REQUIRE( childNames( &panel, folder )
             == QStringList{ "app", "notes.txt", "sys.log.gz" } );
    // Indexed only once the name filter is used
    CHECK_FALSE( panel.isIndexed() );
    CHECK_FALSE( panel.isIndexing() );

    // The filter hides the files and the folders without a matching file
    REQUIRE( QDir( folder ).mkpath( "empty/deeper" ) );
    REQUIRE( QDir( folder ).mkpath( "other/deep/deeper" ) );
    writeFile( folder + "/other/readme.txt" );
    writeFile( folder + "/other/deep/deeper/old.LOG" );
    // The folders made after the folder was read come from the file watcher
    REQUIRE( waitUiState( [ & ] { return childNames( &panel, folder ).size() == 5; } ) );
    panel.setNameFilter( "*.log" );
    CHECK( childNames( &panel, folder ) == QStringList{ "app", "other" } );
    CHECK( panel.isIndexed() );
    CHECK( childNames( &panel, folder + "/app" ) == QStringList{ "main.log", "worker.log" } );
    CHECK( childNames( &panel, folder + "/other" ) == QStringList{ "deep" } );
    CHECK( childNames( &panel, folder + "/other/deep" ) == QStringList{ "deeper" } );

    panel.setNameFilter( "notes" );
    CHECK( childNames( &panel, folder ) == QStringList{ "notes.txt" } );

    // A matching file new in a hidden folder shows it
    childNames( &panel, folder + "/empty" );
    writeFile( folder + "/empty/notes.md" );
    REQUIRE( waitUiState( [ & ] { return childNames( &panel, folder ).contains( "empty" ); } ) );

    // A new folder with a matching file, a folder removed: the index follows the disk
    REQUIRE( QDir( folder ).mkpath( "rotated/2026-11-15" ) );
    writeFile( folder + "/rotated/2026-11-15/notes-1.log" );
    REQUIRE( waitUiState( [ & ] { return childNames( &panel, folder ).contains( "rotated" ); } ) );
    CHECK( childNames( &panel, folder + "/rotated" ) == QStringList{ "2026-11-15" } );
    REQUIRE( QDir( folder + "/rotated" ).removeRecursively() );
    REQUIRE( waitUiState( [ & ] { return !childNames( &panel, folder ).contains( "rotated" ); } ) );
    // The matching file renamed away hides its folder
    REQUIRE( QFile::rename( folder + "/empty/notes.md", folder + "/empty/readme.md" ) );
    REQUIRE( waitUiState( [ & ] { return !childNames( &panel, folder ).contains( "empty" ); } ) );

    panel.setNameFilter( "" );
    CHECK( childNames( &panel, folder )
           == QStringList{ "app", "empty", "notes.txt", "other", "sys.log.gz" } );

    // Open files are in bold
    const auto mainLog = folder + "/app/main.log";
    panel.setOpenFiles( { mainLog } );
    CHECK( isBold( &panel, mainLog ) );
    CHECK_FALSE( isBold( &panel, folder + "/app/worker.log" ) );

    // The current file is selected, its folder expanded
    panel.setCurrentFile( mainLog );
    CHECK( panel.pathAt( panel.treeView()->currentIndex() ) == mainLog );
    CHECK( panel.treeView()->isExpanded( panel.indexOf( folder + "/app" ) ) );

    // Enter opens the selected files, a double click a file
    QSignalSpy openRequested( &panel, &FileTreePanel::openFilesRequested );
    QTest::keyClick( panel.treeView(), Qt::Key_Return );
    REQUIRE( openRequested.count() == 1 );
    CHECK( openRequested.at( 0 ).at( 0 ).toStringList() == QStringList{ mainLog } );

    // Closing the folder drops the model
    panel.setRootFolder( {} );
    CHECK( stack->currentIndex() == 0 );
    CHECK( panel.treeView()->model() == nullptr );
}

SCENARIO( "File tree panel hidden files", "[ui]" )
{
    QTemporaryDir dir;
    const auto folder = makeFolder( dir );
    REQUIRE( QDir( folder ).mkpath( ".cache/deep" ) );
    writeFile( folder + "/.cache/deep/old.log" );
    writeFile( folder + "/.secret.log" );
    markHidden( folder + "/.cache" );
    markHidden( folder + "/.secret.log" );

    FileTreePanel panel;
    panel.setShowHidden( false );
    panel.setRootFolder( folder );
    CHECK( childNames( &panel, folder ) == QStringList{ "app", "notes.txt", "sys.log.gz" } );

    panel.setShowHidden( true );
    CHECK( FileTreePanel( nullptr ).showHidden() );
    CHECK( childNames( &panel, folder )
           == QStringList{ ".cache", ".secret.log", "app", "notes.txt", "sys.log.gz" } );

    // The index follows the choice
    panel.setNameFilter( "old" );
    CHECK( childNames( &panel, folder ) == QStringList{ ".cache" } );
    panel.setShowHidden( false );
    CHECK( childNames( &panel, folder ).isEmpty() );
    panel.setNameFilter( "" );

    // Cmd+Shift+. (Ctrl+Shift+. elsewhere) in the tree
    QTest::keyClick( panel.treeView(), Qt::Key_Period, Qt::ControlModifier | Qt::ShiftModifier );
    CHECK( panel.showHidden() );
    panel.setShowHidden( false );
}

SCENARIO( "File tree panel rename", "[ui]" )
{
    QTemporaryDir dir;
    const auto folder = makeFolder( dir );

    FileTreePanel panel;
    panel.setShowHidden( false );
    panel.resize( 300, 400 );
    panel.show();
    panel.setRootFolder( folder );
    childNames( &panel, folder );
    childNames( &panel, folder + "/app" );
    auto* view = panel.treeView()->model();
    QSignalSpy failed( &panel, &FileTreePanel::renameFailed );

    // Dragged out files are copied, never moved by the drop target
    CHECK( view->supportedDragActions() == Qt::CopyAction );

    // A file
    CHECK( view->setData( panel.indexOf( folder + "/notes.txt" ), "renamed.txt" ) );
    CHECK( QFile::exists( folder + "/renamed.txt" ) );
    CHECK_FALSE( QFile::exists( folder + "/notes.txt" ) );
    CHECK( failed.isEmpty() );

    // A change of case only
    CHECK( view->setData( panel.indexOf( folder + "/renamed.txt" ), "Renamed.txt" ) );
    CHECK( QDir( folder ).entryList( QDir::Files ).contains( "Renamed.txt" ) );

    // Refused: an existing name, an invalid one
    CHECK_FALSE( view->setData( panel.indexOf( folder + "/Renamed.txt" ), "sys.log.gz" ) );
    CHECK_FALSE( view->setData( panel.indexOf( folder + "/Renamed.txt" ), "a/b.txt" ) );
    CHECK_FALSE( view->setData( panel.indexOf( folder + "/Renamed.txt" ), " " ) );
    CHECK( failed.count() == 3 );
    CHECK( QFile::exists( folder + "/Renamed.txt" ) );

    // Refused: a file open in a tab, a folder holding one
    panel.setOpenFilesProvider( [ & ] { return QStringList{ folder + "/app/main.log" }; } );
    CHECK( panel.isInUse( folder + "/app/main.log" ) );
    CHECK( panel.isInUse( folder + "/app" ) );
    CHECK_FALSE( panel.isInUse( folder + "/app/worker.log" ) );
    CHECK_FALSE( panel.isInUse( folder + "/ap" ) );
    CHECK_FALSE( view->setData( panel.indexOf( folder + "/app/main.log" ), "x.log" ) );
    CHECK_FALSE( view->setData( panel.indexOf( folder + "/app" ), "x" ) );
    CHECK( failed.count() == 5 );
    CHECK( QFile::exists( folder + "/app/main.log" ) );
    panel.treeView()->setCurrentIndex( panel.indexOf( folder + "/app/main.log" ) );
    panel.renameCurrent();
    CHECK( failed.count() == 6 );
    CHECK( panel.treeView()->findChild<QLineEdit*>() == nullptr );

    // A folder holding no open file
    CHECK( view->setData( panel.indexOf( folder + "/app/worker.log" ), "worker-1.log" ) );
    REQUIRE( QDir( folder ).mkpath( "old" ) );
    REQUIRE( waitUiState( [ & ] { return childNames( &panel, folder ).contains( "old" ); } ) );
    CHECK( view->setData( panel.indexOf( folder + "/old" ), "older" ) );
    CHECK( QFileInfo( folder + "/older" ).isDir() );

    // F2 edits the name without its extension
    panel.treeView()->setCurrentIndex( panel.indexOf( folder + "/sys.log.gz" ) );
    QTest::keyClick( panel.treeView(), Qt::Key_F2 );
    auto* editor = panel.treeView()->findChild<QLineEdit*>();
    REQUIRE( editor );
    QTest::qWait( 50 );
    CHECK( editor->selectedText() == "sys.log" );
    editor->setText( "system.log.gz" );
    // Enter saves the name and does not open the file
    QSignalSpy openRequested( &panel, &FileTreePanel::openFilesRequested );
    QTest::keyClick( editor, Qt::Key_Return );
    REQUIRE( waitUiState( [ & ] { return QFile::exists( folder + "/system.log.gz" ); } ) );
    QTest::qWait( 100 );
    CHECK( openRequested.isEmpty() );

    // Escape leaves the name
    panel.treeView()->setCurrentIndex( panel.indexOf( folder + "/system.log.gz" ) );
    QTest::keyClick( panel.treeView(), Qt::Key_F2 );
    editor = panel.treeView()->findChild<QLineEdit*>();
    REQUIRE( editor );
    editor->setText( "other.log.gz" );
    QTest::keyClick( editor, Qt::Key_Escape );
    QTest::qWait( 100 );
    CHECK( QFile::exists( folder + "/system.log.gz" ) );
    CHECK( openRequested.isEmpty() );
}

SCENARIO( "File tree panel sort", "[ui]" )
{
    QTemporaryDir dir;
    const auto folder = QFileInfo( dir.path() ).canonicalFilePath();
    REQUIRE( QDir( folder ).mkpath( "zeta" ) );
    REQUIRE( QDir( folder ).mkpath( "alpha" ) );
    const auto now = QDateTime::currentDateTime();
    // name, bytes, age in hours
    const std::tuple<const char*, int, int> files[] = {
        { "app.log.10", 300, 1 }, { "app.log.2", 100, 3 }, { "b.txt", 200, 2 } };
    for ( const auto& [ name, bytes, hours ] : files ) {
        QFile file( folder + "/" + name );
        REQUIRE( file.open( QIODevice::WriteOnly ) );
        file.write( QByteArray( bytes, 'x' ) );
        // Written before the time is set, not when closed after
        REQUIRE( file.flush() );
        REQUIRE( file.setFileTime( now.addSecs( -3600 * hours ),
                                   QFileDevice::FileModificationTime ) );
    }

    FileTreePanel panel;
    panel.setShowHidden( false );
    panel.setSortOrder( FileTreeSort::NameAscending );
    panel.setRootFolder( folder );

    // The folders first, natural order of the numbers
    CHECK( orderedNames( &panel, folder )
           == QStringList{ "alpha", "zeta", "app.log.2", "app.log.10", "b.txt" } );
    panel.setSortOrder( FileTreeSort::NameDescending );
    CHECK( orderedNames( &panel, folder )
           == QStringList{ "zeta", "alpha", "b.txt", "app.log.10", "app.log.2" } );
    panel.setSortOrder( FileTreeSort::NewestFirst );
    CHECK( orderedNames( &panel, folder ).mid( 2 )
           == QStringList{ "app.log.10", "b.txt", "app.log.2" } );
    panel.setSortOrder( FileTreeSort::OldestFirst );
    CHECK( orderedNames( &panel, folder ).mid( 2 )
           == QStringList{ "app.log.2", "b.txt", "app.log.10" } );
    panel.setSortOrder( FileTreeSort::LargestFirst );
    CHECK( orderedNames( &panel, folder ).mid( 2 )
           == QStringList{ "app.log.10", "b.txt", "app.log.2" } );
    panel.setSortOrder( FileTreeSort::SmallestFirst );
    CHECK( orderedNames( &panel, folder ).mid( 2 )
           == QStringList{ "app.log.2", "b.txt", "app.log.10" } );
    panel.setSortOrder( FileTreeSort::Type );
    CHECK( orderedNames( &panel, folder ).mid( 2 )
           == QStringList{ "app.log.2", "app.log.10", "b.txt" } );

    // Kept in the settings
    CHECK( FileTreePanel().sortOrder() == FileTreeSort::Type );
    panel.setSortOrder( FileTreeSort::NameAscending );

    // The tool tip: size and time of change
    const auto tip = panel.indexOf( folder + "/app.log.10" ).data( Qt::ToolTipRole ).toString();
    // On one line, placed as the other tool tips
    CHECK_FALSE( tip.contains( '\n' ) );
    CHECK_FALSE( tip.contains( "app.log.10" ) );
    CHECK( tip.startsWith( QLocale().formattedDataSize( 300 ) ) );
    CHECK( tip.contains( QLocale().formattedDataSize( 300 ) ) );
    CHECK( tip.contains( now.addSecs( -3600 ).toString( "yyyy-MM-dd HH:mm" ) ) );
}

TEST_CASE( "File tree copy texts", "[ui]" )
{
    const QString root = "/logs/run";
    const QStringList paths{ "/logs/run/app/main.log", "/logs/run/sys.log" };
    CHECK( FileTreePanel::copyText( FileTreeCopy::Name, root, paths ) == "main.log\nsys.log" );
    CHECK( FileTreePanel::copyText( FileTreeCopy::RelativePath, root, paths )
           == QDir::toNativeSeparators( "app/main.log" ) + "\nsys.log" );
    CHECK( FileTreePanel::copyText( FileTreeCopy::Path, root, paths.mid( 1 ) )
           == QDir::toNativeSeparators( "/logs/run/sys.log" ) );
}

SCENARIO( "File tree panel move to trash", "[ui]" )
{
    QTemporaryDir dir;
    const auto folder = makeFolder( dir );

    FileTreePanel panel;
    panel.setShowHidden( false );
    panel.setRootFolder( folder );
    childNames( &panel, folder );
    // Not the trash of the user: removed for the test
    QStringList asked;
    auto answer = true;
    panel.setTrashHandlers(
        [ & ]( const QStringList& paths ) {
            asked = paths;
            return answer;
        },
        []( const QString& path ) {
            return QFileInfo( path ).isDir() ? QDir( path ).removeRecursively()
                                             : QFile::remove( path );
        } );
    QSignalSpy failed( &panel, &FileTreePanel::trashFailed );

    // Cancelled
    answer = false;
    panel.moveToTrash( { folder + "/notes.txt" } );
    CHECK( asked == QStringList{ folder + "/notes.txt" } );
    CHECK( QFile::exists( folder + "/notes.txt" ) );

    // Moved, gone from the tree at once
    answer = true;
    panel.moveToTrash( { folder + "/notes.txt", folder + "/sys.log.gz" } );
    CHECK_FALSE( QFile::exists( folder + "/notes.txt" ) );
    CHECK_FALSE( panel.indexOf( folder + "/notes.txt" ).isValid() );
    CHECK( orderedNames( &panel, folder ) == QStringList{ "app" } );

    // Refused: a file open in a tab, its folder
    panel.setOpenFilesProvider( [ & ] { return QStringList{ folder + "/app/main.log" }; } );
    asked.clear();
    panel.moveToTrash( { folder + "/app" } );
    CHECK( asked.isEmpty() );
    CHECK( failed.count() == 1 );
    CHECK( QFile::exists( folder + "/app/main.log" ) );

    // A failure is told
    panel.setTrashHandlers( []( const QStringList& ) { return true; },
                            []( const QString& ) { return false; } );
    panel.moveToTrash( { folder + "/app/worker.log" } );
    CHECK( failed.count() == 2 );
    CHECK( panel.indexOf( folder + "/app/worker.log" ).isValid() );

    // The shortcut on the current row
    panel.setTrashHandlers( []( const QStringList& ) { return true; },
                            []( const QString& path ) { return QFile::remove( path ); } );
    childNames( &panel, folder + "/app" );
    panel.treeView()->setCurrentIndex( panel.indexOf( folder + "/app/worker.log" ) );
#ifdef Q_OS_MAC
    QTest::keyClick( panel.treeView(), Qt::Key_Backspace, Qt::ControlModifier );
#else
    QTest::keyClick( panel.treeView(), Qt::Key_Delete );
#endif
    CHECK_FALSE( QFile::exists( folder + "/app/worker.log" ) );
}

SCENARIO( "File tree folder menu", "[ui]" )
{
    QTemporaryDir dir;
    const auto folder = makeFolder( dir );
    QTemporaryDir other;
    const auto otherFolder = QFileInfo( other.path() ).canonicalFilePath();

    FileTreePanel panel;
    panel.setRootFolder( otherFolder );
    panel.setRootFolder( folder );
    QMenu* menu = nullptr;
    for ( auto* button : panel.findChildren<QToolButton*>() ) {
        if ( button->toolTip() == QDir::toNativeSeparators( folder ) ) {
            menu = button->menu();
        }
    }
    REQUIRE( menu );
    Q_EMIT menu->aboutToShow();
    const auto texts = menuTexts( menu );
    // Recent folders, then open, close and view groups
    const auto open = texts.indexOf( "Open Folder..." );
    REQUIRE( open > 0 );
    CHECK( texts.at( open - 1 ) == "-" );
    CHECK( texts.mid( open ) == QStringList{ "Open Folder...", "Reveal in File Manager", "-",
                                             "Close Folder", "Clear Recent Folders", "-",
                                             "Show Hidden Files" } );
    CHECK( texts.contains( QDir::toNativeSeparators( otherFolder ) ) );
}

SCENARIO( "File tree panel drop", "[ui]" )
{
    QTemporaryDir dir;
    const auto folder = makeFolder( dir );

    FileTreePanel panel;
    QSignalSpy openRequested( &panel, &FileTreePanel::openFilesRequested );

    QMimeData mime;
    mime.setUrls( { QUrl::fromLocalFile( folder + "/app" ), QUrl::fromLocalFile( folder + "/notes.txt" ) } );
    // Drops are only delivered once the drag enter is accepted
    QDragEnterEvent enter( QPoint( 10, 10 ), Qt::CopyAction, &mime, Qt::LeftButton,
                           Qt::NoModifier );
    QApplication::sendEvent( &panel, &enter );
    REQUIRE( enter.isAccepted() );
    QDropEvent drop( QPointF( 10, 10 ), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier );
    QApplication::sendEvent( &panel, &drop );

    CHECK( panel.rootFolder() == folder + "/app" );
    REQUIRE( openRequested.count() == 1 );
    CHECK( openRequested.at( 0 ).at( 0 ).toStringList() == QStringList{ folder + "/notes.txt" } );
}

SCENARIO( "Main window opens a folder in the file tree", "[ui]" )
{
    QTemporaryDir dir;
    const auto folder = makeFolder( dir );
    const auto mainLog = folder + "/app/main.log";

    auto appSession = std::make_shared<Session>();
    WindowSession windowSession{ appSession, "FileTree", 0 };
    {
        MainWindow mainWindow( windowSession );
        auto* dock = mainWindow.findChild<QDockWidget*>( "fileTreeDock" );
        REQUIRE( dock );
        CHECK( mainWindow.dockWidgetArea( dock ) == Qt::LeftDockWidgetArea );
        CHECK( dock->isHidden() );

        // A folder from the command line (or dropped) goes to the tree, not to a tab
        WindowCommand command;
        command.files = { folder };
        mainWindow.runCommand( command );
        auto* tabArea = mainWindow.findChild<TabbedCrawlerWidget*>();
        CHECK( tabArea->count() == 0 );
        CHECK( mainWindow.rootFolder() == folder );
        CHECK_FALSE( dock->isHidden() );

        // Files of the tree open in tabs, once
        auto* panel = mainWindow.fileTreePanel();
        Q_EMIT panel->openFilesRequested( { mainLog, folder + "/notes.txt" } );
        REQUIRE( waitUiState( [ & ] { return tabArea->count() == 2; } ) );
        Q_EMIT panel->openFilesRequested( { mainLog } );
        QTest::qWait( 100 );
        CHECK( tabArea->count() == 2 );
        CHECK( isBold( panel, mainLog ) );
        // Open in a tab: not renamed, nor its folder
        CHECK( panel->isInUse( mainLog ) );
        CHECK( panel->isInUse( folder + "/app" ) );
        CHECK_FALSE( panel->isInUse( folder + "/sys.log.gz" ) );
        CHECK( panel->pathAt( panel->treeView()->currentIndex() ) == mainLog );

        // loadFile of a folder also sets the root
        mainWindow.loadInitialFile( folder + "/app", false );
        CHECK( mainWindow.rootFolder() == folder + "/app" );
        CHECK( tabArea->count() == 2 );

    }

    // The session keeps the folder of the window and restores it with its panel
    windowSession.save( {}, {}, {}, {}, folder + "/app", true );
    CHECK( SessionInfo::getSynced().folder( "FileTree" ).first == folder + "/app" );
    {
        MainWindow restored( windowSession );
        restored.reloadSession();
        CHECK( restored.rootFolder() == folder + "/app" );
        CHECK_FALSE( restored.findChild<QDockWidget*>( "fileTreeDock" )->isHidden() );
    }
    windowSession.save( {}, {}, {}, {}, folder, false );
    {
        MainWindow restored( windowSession );
        restored.reloadSession();
        CHECK( restored.rootFolder() == folder );
        CHECK( restored.findChild<QDockWidget*>( "fileTreeDock" )->isHidden() );
    }
}

SCENARIO( "Side panels moved in a window open there in the next one", "[ui]" )
{
    auto appSession = std::make_shared<Session>();
    WindowSession firstSession{ appSession, "PanelsFirst", 0 };
    WindowSession secondSession{ appSession, "PanelsSecond", 1 };
    WindowSession thirdSession{ appSession, "PanelsThird", 2 };

    MainWindow first( firstSession );
    auto* fileTree = first.findChild<QDockWidget*>( "fileTreeDock" );
    auto* filters = first.findChild<QDockWidget*>( "filterDock" );
    REQUIRE( fileTree );
    REQUIRE( filters );

    // Their title bars and toggle buttons show the panel icons
    CHECK_FALSE( fileTree->toggleViewAction()->icon().isNull() );
    CHECK_FALSE( filters->toggleViewAction()->icon().isNull() );
    CHECK( fileTree->titleBarWidget() );
    CHECK( filters->titleBarWidget() );

    // Dropped on the other side while the first window stays open
    first.addDockWidget( Qt::RightDockWidgetArea, fileTree );
    first.addDockWidget( Qt::LeftDockWidgetArea, filters );
    {
        MainWindow second( secondSession );
        CHECK( second.dockWidgetArea( second.findChild<QDockWidget*>( "fileTreeDock" ) )
               == Qt::RightDockWidgetArea );
        CHECK( second.dockWidgetArea( second.findChild<QDockWidget*>( "filterDock" ) )
               == Qt::LeftDockWidgetArea );
    }

    // Back to the default places for the other tests
    first.addDockWidget( Qt::LeftDockWidgetArea, fileTree );
    first.addDockWidget( Qt::RightDockWidgetArea, filters );
    MainWindow third( thirdSession );
    CHECK( third.dockWidgetArea( third.findChild<QDockWidget*>( "fileTreeDock" ) )
           == Qt::LeftDockWidgetArea );
}

// Hidden, saves a picture of the panel: FLOGG_RENDER_DIR=<dir> klogg_itests "[.render]"
SCENARIO( "File tree panel rendering", "[.render]" )
{
    const auto renderDir = qEnvironmentVariable( "FLOGG_RENDER_DIR" );
    if ( renderDir.isEmpty() ) {
        WARN( "FLOGG_RENDER_DIR is not set" );
        return;
    }

    QTemporaryDir dir;
    const auto folder = makeFolder( dir );
    REQUIRE( QDir( folder ).mkpath( "archive/2026-11" ) );
    writeFile( folder + "/archive/2026-11/old.log" );

    auto appSession = std::make_shared<Session>();
    WindowSession windowSession{ appSession, "FileTreeRender", 0 };
    MainWindow mainWindow( windowSession );
    mainWindow.resize( 1000, 500 );

    auto* panel = mainWindow.fileTreePanel();
    panel->resize( 260, 400 );
    REQUIRE( panel->grab().save( renderDir + "/file_tree_empty.png" ) );

    mainWindow.setRootFolder( folder );
    mainWindow.loadInitialFile( folder + "/app/main.log", false );
    auto* tabArea = mainWindow.findChild<TabbedCrawlerWidget*>();
    REQUIRE( waitUiState( [ & ] { return tabArea->count() == 1; } ) );
    childNames( panel, folder );
    childNames( panel, folder + "/app" );
    QTest::qWait( 300 );
    REQUIRE( panel->grab().save( renderDir + "/file_tree.png" ) );
    for ( auto* button : panel->findChildren<QToolButton*>() ) {
        if ( auto* menu = button->menu() ) {
            Q_EMIT menu->aboutToShow();
            menu->adjustSize();
            const auto name = button->toolTip() == "Sort" ? "sort" : "folder";
            REQUIRE( menu->grab().save( renderDir + "/file_tree_menu_" + name + ".png" ) );
        }
    }

    mainWindow.show();
    QTest::qWait( 300 );
    REQUIRE( mainWindow.grab().save( renderDir + "/file_tree_window.png" ) );

    // The panel buttons at the end of the tool bar, and the Tools menu
    mainWindow.resize( 1400, 500 );
    QTest::qWait( 300 );
    auto* toolBar = mainWindow.findChild<QToolBar*>();
    REQUIRE( toolBar->grab().save( renderDir + "/tool_bar.png" ) );
    for ( auto* menu : mainWindow.menuBar()->findChildren<QMenu*>( Qt::FindDirectChildrenOnly ) ) {
        if ( menu->title().remove( '&' ) == "Tools" ) {
            menu->adjustSize();
            REQUIRE( menu->grab().save( renderDir + "/tools_menu.png" ) );
        }
    }
}

// Hidden, times the name filter on a folder of a few thousand files:
// klogg_itests "[.benchmark]" "*File tree filter timing*"
SCENARIO( "File tree filter timing", "[.benchmark]" )
{
    QTemporaryDir dir;
    const auto root = QFileInfo( dir.path() ).canonicalFilePath();
    // 60 folders of 50 files (3000), with a few nested folders
    for ( int d = 0; d < 60; ++d ) {
        const auto sub = QString( "%1/svc%2/%3" ).arg( root ).arg( d % 20 ).arg( d );
        REQUIRE( QDir().mkpath( sub ) );
        for ( int f = 0; f < 50; ++f ) {
            QFile file( QString( "%1/%2-%3.%4" ).arg( sub ).arg( d ).arg( f ).arg( f % 5 ? "log" : "txt" ) );
            REQUIRE( file.open( QIODevice::WriteOnly ) );
        }
    }

    FileTreePanel panel;
    panel.resize( 300, 600 );
    panel.setRootFolder( root );
    childNames( &panel, root );

    QElapsedTimer timer;
    for ( const auto* text : { "*.txt", "*.log", "3-1", "zzz", "" } ) {
        timer.start();
        panel.setNameFilter( text );
        const auto applied = timer.nsecsElapsed();
        while ( panel.isIndexing() ) {
            QCoreApplication::processEvents( QEventLoop::AllEvents, 1 );
        }
        QCoreApplication::processEvents();
        const auto scanned = timer.nsecsElapsed();
        WARN( text << ": filter applied " << applied / 1000 << " us, folders known "
                   << scanned / 1000 << " us" );
    }

    // From a matching file written in a hidden folder to the folder shown
    panel.setNameFilter( "*.new" );
    childNames( &panel, root );
    timer.start();
    {
        QFile file( root + "/svc3/3/rotated.new" );
        REQUIRE( file.open( QIODevice::WriteOnly ) );
    }
    while ( !panel.indexOf( root + "/svc3" ).isValid() && timer.elapsed() < 10000 ) {
        QCoreApplication::processEvents( QEventLoop::AllEvents, 1 );
    }
    WARN( "new file shown after " << timer.elapsed() << " ms" );

    // Without a filter, a file new in a folder read by the tree (here the root)
    panel.setNameFilter( "" );
    QTest::qWait( 200 );
    timer.start();
    {
        QFile file( root + "/plain.new" );
        REQUIRE( file.open( QIODevice::WriteOnly ) );
    }
    // Looking the path up in the model would read it from the disk: only the rows
    const auto shown = [ & ] {
        auto* view = panel.treeView()->model();
        const auto parent = panel.treeView()->rootIndex();
        for ( int row = 0; row < view->rowCount( parent ); ++row ) {
            if ( view->index( row, 0, parent ).data().toString() == "plain.new" ) {
                return true;
            }
        }
        return false;
    };
    while ( !shown() && timer.elapsed() < 10000 ) {
        QCoreApplication::processEvents( QEventLoop::AllEvents, 1 );
    }
    WARN( "new file in the tree after " << timer.elapsed() << " ms" );
}

// The tool tip of a row from a real help event, next to the cursor (Qt places it)
SCENARIO( "File tree tool tip place", "[ui]" )
{
    QTemporaryDir dir;
    const auto folder = makeFolder( dir );
    FileTreePanel panel;
    panel.resize( 300, 400 );
    panel.move( 100, 100 );
    panel.show();
    panel.setRootFolder( folder );
    childNames( &panel, folder );
    auto* viewport = panel.treeView()->viewport();
    const auto local = panel.treeView()->visualRect( panel.indexOf( folder + "/notes.txt" ) ).center();
    const auto global = viewport->mapToGlobal( local );
    QHelpEvent help( QEvent::ToolTip, local, global );
    QApplication::sendEvent( viewport, &help );
    QWidget* tip = nullptr;
    REQUIRE( waitUiState( [ & ] {
        for ( auto* widget : QApplication::topLevelWidgets() ) {
            if ( widget->inherits( "QTipLabel" ) && widget->isVisible() ) {
                tip = widget;
            }
        }
        return tip != nullptr;
    } ) );
    const auto offset = tip->pos() - global;
    CHECK( offset.x() >= 0 );
    CHECK( offset.x() < 10 );
    CHECK( offset.y() > 0 );
    CHECK( offset.y() < 40 );
    QToolTip::hideText();
}

TEST_CASE( "Tool tip placement out of the cursor", "[ui]" )
{
    const QRect screen( 0, 0, 1000, 800 );
    const QPoint cursor( 900, 300 );
    // Beside the cursor on its right: kept
    CHECK( tooltips::placed( QRect( 920, 300, 60, 20 ), cursor, screen ) == QPoint( 920, 300 ) );
    // Below the cursor: kept
    CHECK( tooltips::placed( QRect( 902, 316, 60, 20 ), cursor, screen ) == QPoint( 902, 316 ) );
    // Moved left by Qt but under the cursor: left of it
    CHECK( tooltips::placed( QRect( 800, 300, 116, 20 ), cursor, screen ) == QPoint( 780, 300 ) );
    // No room on the left either: kept
    CHECK( tooltips::placed( QRect( 0, 300, 950, 20 ), cursor, screen ) == QPoint( 0, 300 ) );
}

SCENARIO( "Tool tip across the cursor is moved left of it", "[ui]" )
{
    tooltips::installPlacement();
    auto* screen = QGuiApplication::primaryScreen();
    const auto area = screen->availableGeometry();
    const QPoint cursor( area.right() - 30, area.top() + 200 );
    QCursor::setPos( screen, cursor );
    if ( QCursor::pos() != cursor ) {
        WARN( "The cursor can't be moved on this platform" );
        return;
    }

    QToolTip::showText( cursor, "12.3 MB  ·  2026-11-15 07:45:28" );
    QWidget* tip = nullptr;
    REQUIRE( waitUiState( [ & ] {
        for ( auto* widget : QApplication::topLevelWidgets() ) {
            if ( widget->inherits( "QTipLabel" ) && widget->isVisible() ) {
                tip = widget;
            }
        }
        return tip != nullptr;
    } ) );
    // As Qt does with a large cursor and no room on the right
    tip->move( cursor.x() + 16 - tip->width(), cursor.y() );
    CHECK( tip->geometry().right() < cursor.x() );
    // The window system (offscreen) may shift it a little
    CHECK( std::abs( tip->y() - cursor.y() ) <= 4 );
    // Placed again by Qt across the cursor (another row): moved again
    tip->move( cursor.x() + 16 - tip->width(), cursor.y() + 2 );
    CHECK( tip->geometry().right() < cursor.x() );
    // Below the cursor, out of it: kept
    tip->move( cursor.x() + 16 - tip->width(), cursor.y() + 40 );
    CHECK( std::abs( tip->x() - ( cursor.x() + 16 - tip->width() ) ) <= 4 );
    QToolTip::hideText();
}
