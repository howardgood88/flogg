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

#include <chrono>
#include <functional>

#include <QHeaderView>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QApplication>
#include <QDropEvent>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

#include "configuration.h"
#include "log.h"
#include "test_utils.h"

#include "logdata.h"
#include "logfiltereddata.h"

#include "filtereditdialog.h"
#include "styles.h"
#include "filterpanel.h"
#include "filtertree.h"
#include "persistentinfo.h"

namespace {

constexpr int NbLines = 30000;

void generateFile( QTemporaryFile& file )
{
    REQUIRE( file.open() );
    for ( int i = 0; i < NbLines; ++i ) {
        QByteArray line = QByteArray( "line " ) + QByteArray::number( i );
        if ( i % 2 == 0 ) {
            line += " even";
        }
        if ( i % 3 == 0 ) {
            line += " three";
        }
        if ( i % 5 == 0 ) {
            line += " five";
        }
        line += '\n';
        file.write( line );
    }
    file.flush();
}

LogFilteredData::FilterSpec spec( const QString& pattern, bool exclude = false,
                                  bool caseSensitive = true )
{
    constexpr auto Inverse = false;
    constexpr auto Boolean = false;
    constexpr auto PlainText = true;
    return { RegularExpressionPattern( pattern, caseSensitive, Inverse, Boolean, PlainText ),
             exclude };
}

// Returns true if the search completed without needing the worker thread
bool runFilterSearch( LogFilteredData& filteredData,
                      const std::vector<LogFilteredData::FilterSpec>& filters )
{
    SafeQSignalSpy progressSpy( &filteredData,
                                SIGNAL( searchProgressed( LinesCount, int, LineNumber ) ) );

    filteredData.runFilterSearch( filters, 0_lnum, LineNumber( NbLines ) );

    const auto isDone = [ &progressSpy ] {
        return !progressSpy.isEmpty() && progressSpy.last().at( 1 ).toInt() == 100;
    };

    const auto immediate = isDone();
    while ( !isDone() ) {
        // safeWait returns immediately when anything was already recorded
        const auto recorded = progressSpy.count();
        REQUIRE( ( progressSpy.wait( 10000 ) || progressSpy.count() > recorded ) );
    }
    return immediate;
}

LinesCount expectedCount( const std::function<bool( int )>& isShown )
{
    LinesCount count = 0_lcount;
    for ( int i = 0; i < NbLines; ++i ) {
        if ( isShown( i ) ) {
            ++count;
        }
    }
    return count;
}

void checkLines( const LogFilteredData& filteredData, const std::function<bool( int )>& isShown )
{
    REQUIRE( filteredData.getNbMatches() == expectedCount( isShown ) );

    LineNumber index = 0_lnum;
    for ( int i = 0; i < NbLines; ++i ) {
        if ( isShown( i ) ) {
            REQUIRE( filteredData.getMatchingLineNumber( index ) == LineNumber( i ) );
            ++index;
        }
    }
}

} // namespace

SCENARIO( "filter search with per filter cache", "[logdata][filters]" )
{
    QTemporaryFile file{ "filter_search_test_XXXXXX" };
    generateFile( file );

    LogData logData;
    SafeQSignalSpy loadEndSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    logData.attachFile( file.fileName() );
    REQUIRE( loadEndSpy.safeWait( 10000 ) );

    auto filteredData = logData.getNewFilteredData();

    WHEN( "Searching an include filter" )
    {
        runFilterSearch( *filteredData, { spec( "even" ) } );

        THEN( "Matching lines are shown" )
        {
            checkLines( *filteredData, []( int i ) { return i % 2 == 0; } );
        }
    }

    WHEN( "Searching include and exclude filters" )
    {
        runFilterSearch( *filteredData, { spec( "even" ), spec( "three", true ) } );

        THEN( "Excluded lines are removed" )
        {
            checkLines( *filteredData, []( int i ) { return i % 2 == 0 && i % 3 != 0; } );
        }

        AND_WHEN( "Toggling a filter off and on again" )
        {
            const auto immediateWithoutExclude
                = runFilterSearch( *filteredData, { spec( "even" ) } );
            checkLines( *filteredData, []( int i ) { return i % 2 == 0; } );

            const auto immediateWithExclude
                = runFilterSearch( *filteredData, { spec( "even" ), spec( "three", true ) } );

            THEN( "Cached results are used without searching the file" )
            {
                REQUIRE( immediateWithoutExclude );
                REQUIRE( immediateWithExclude );
                checkLines( *filteredData, []( int i ) { return i % 2 == 0 && i % 3 != 0; } );
            }
        }

        AND_WHEN( "Adding a new filter" )
        {
            const auto immediate = runFilterSearch(
                *filteredData, { spec( "even" ), spec( "three", true ), spec( "five" ) } );

            THEN( "Only the new filter is searched and results are combined" )
            {
                REQUIRE_FALSE( immediate );
                checkLines( *filteredData, []( int i ) {
                    return ( i % 2 == 0 || i % 5 == 0 ) && i % 3 != 0;
                } );
            }
        }

        AND_WHEN( "Turning an exclude filter into an include filter" )
        {
            const auto immediate
                = runFilterSearch( *filteredData, { spec( "even" ), spec( "three" ) } );

            THEN( "The cached matches of the pattern are reused" )
            {
                REQUIRE( immediate );
                checkLines( *filteredData, []( int i ) { return i % 2 == 0 || i % 3 == 0; } );
            }
        }
    }

    WHEN( "Searching only exclude filters" )
    {
        runFilterSearch( *filteredData, { spec( "five", true ) } );

        THEN( "All the other lines are shown" )
        {
            checkLines( *filteredData, []( int i ) { return i % 5 != 0; } );
        }
    }

    WHEN( "Filters have different case sensitivity" )
    {
        runFilterSearch( *filteredData, { spec( "EVEN", false, false ), spec( "THREE" ) } );

        THEN( "Each filter uses its own setting" )
        {
            checkLines( *filteredData, []( int i ) { return i % 2 == 0; } );
        }
    }

    WHEN( "Searching many filters at once" )
    {
        std::vector<LogFilteredData::FilterSpec> filters;
        for ( int i = 0; i < 60; ++i ) {
            filters.push_back( spec( QString( "line %1" ).arg( 100 + i ) ) );
        }
        runFilterSearch( *filteredData, filters );

        THEN( "Every filter has its own matching lines" )
        {
            for ( const auto& filter : filters ) {
                for ( int i = 0; i < 300; ++i ) {
                    const auto text = QString( "line %1\n" ).arg( i );
                    INFO( filter.pattern.pattern.toStdString() << " line " << i );
                    REQUIRE( filteredData->isFilterMatch( filter.pattern, LineNumber( i ) )
                             == std::optional<bool>( text.startsWith( filter.pattern.pattern ) ) );
                }
            }
        }
    }

    WHEN( "Looking for filter matches and counts" )
    {
        runFilterSearch( *filteredData,
                         { spec( "five" ), spec( "three", true ), spec( "missing" ) } );

        const auto five = spec( "five" ).pattern;
        const auto three = spec( "three" ).pattern;

        THEN( "Counts are available per filter" )
        {
            REQUIRE( filteredData->getFilterMatchCount( five )
                     == expectedCount( []( int i ) { return i % 5 == 0; } ) );
            REQUIRE( filteredData->getFilterMatchCount( three )
                     == expectedCount( []( int i ) { return i % 3 == 0; } ) );
            REQUIRE_FALSE( filteredData->getFilterMatchCount( spec( "even" ).pattern ) );
        }

        THEN( "Next and previous matches are found from the cache" )
        {
            REQUIRE( filteredData->findFilterMatch( { five }, 10_lnum, false )
                     == OptionalLineNumber( 15_lnum ) );
            REQUIRE( filteredData->findFilterMatch( { five }, 10_lnum, true )
                     == OptionalLineNumber( 5_lnum ) );
            REQUIRE( filteredData->findFilterMatch( { five, three }, 10_lnum, false )
                     == OptionalLineNumber( 12_lnum ) );
            // The search wraps around the file
            REQUIRE( filteredData->findFilterMatch( { five }, 0_lnum, true )
                     == OptionalLineNumber( LineNumber( NbLines - 5 ) ) );
            REQUIRE( filteredData->findFilterMatch( { five }, LineNumber( NbLines - 1 ), false )
                     == OptionalLineNumber( 0_lnum ) );
            // Searched patterns without a match give an empty line number
            const auto noMatch
                = filteredData->findFilterMatch( { spec( "missing" ).pattern }, 10_lnum, false );
            REQUIRE( noMatch );
            REQUIRE_FALSE( noMatch->has_value() );
            REQUIRE_FALSE( filteredData->findFilterMatch( { spec( "even" ).pattern }, 10_lnum,
                                                          false ) );
        }

        THEN( "Lines are tested against the cached results" )
        {
            REQUIRE( filteredData->isFilterMatch( five, 10_lnum ) == std::optional<bool>( true ) );
            REQUIRE( filteredData->isFilterMatch( five, 11_lnum ) == std::optional<bool>( false ) );
            REQUIRE_FALSE( filteredData->isFilterMatch( spec( "even" ).pattern, 10_lnum ) );
        }
    }
}

SCENARIO( "numbered bookmarks", "[logdata][bookmarks]" )
{
    QTemporaryFile file{ "bookmarks_test_XXXXXX" };
    generateFile( file );

    LogData logData;
    SafeQSignalSpy loadEndSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    logData.attachFile( file.fileName() );
    REQUIRE( loadEndSpy.safeWait( 10000 ) );

    auto filteredData = logData.getNewFilteredData();
    const auto isMarked = [ &filteredData ]( LineNumber line ) {
        return filteredData->lineTypeByLine( line ).testFlag(
            AbstractLogData::LineTypeFlags::Mark );
    };

    WHEN( "Setting a bookmark" )
    {
        filteredData->toggleBookmark( 0, 10_lnum );

        THEN( "The line is bookmarked and marked" )
        {
            REQUIRE( filteredData->getBookmarkSlots( 10_lnum ) == 0b1u );
            REQUIRE( isMarked( 10_lnum ) );
        }

        AND_WHEN( "Setting the same bookmark on other lines" )
        {
            filteredData->toggleBookmark( 0, 20_lnum );
            filteredData->toggleBookmark( 0, 40_lnum );

            THEN( "All the lines keep the bookmark" )
            {
                REQUIRE( filteredData->getBookmarkSlots( 10_lnum ) == 0b1u );
                REQUIRE( filteredData->getBookmarkSlots( 20_lnum ) == 0b1u );
                REQUIRE( filteredData->getBookmarkSlots( 40_lnum ) == 0b1u );
                REQUIRE( isMarked( 10_lnum ) );
                REQUIRE( isMarked( 20_lnum ) );
            }

            THEN( "Jumping goes through the lines and wraps around" )
            {
                REQUIRE( filteredData->findBookmark( 0, 10_lnum, false ) == OptionalLineNumber( 20_lnum ) );
                REQUIRE( filteredData->findBookmark( 0, 25_lnum, false ) == OptionalLineNumber( 40_lnum ) );
                REQUIRE( filteredData->findBookmark( 0, 40_lnum, false ) == OptionalLineNumber( 10_lnum ) );
                REQUIRE( filteredData->findBookmark( 0, 20_lnum, true ) == OptionalLineNumber( 10_lnum ) );
                REQUIRE( filteredData->findBookmark( 0, 10_lnum, true ) == OptionalLineNumber( 40_lnum ) );
                REQUIRE( filteredData->findBookmark( 0, 0_lnum, true ) == OptionalLineNumber( 40_lnum ) );
            }

            THEN( "Other bookmarks are not found" )
            {
                REQUIRE_FALSE( filteredData->findBookmark( 1, 10_lnum, false ) );
            }
        }

        AND_WHEN( "Setting another bookmark on the same line" )
        {
            filteredData->toggleBookmark( 2, 10_lnum );

            THEN( "The line holds both bookmarks" )
            {
                REQUIRE( filteredData->getBookmarkSlots( 10_lnum ) == 0b101u );
            }

            AND_WHEN( "Removing one of them" )
            {
                filteredData->toggleBookmark( 0, 10_lnum );

                THEN( "The other one and the mark stay" )
                {
                    REQUIRE( filteredData->getBookmarkSlots( 10_lnum ) == 0b100u );
                    REQUIRE( isMarked( 10_lnum ) );
                }
            }
        }

        AND_WHEN( "Toggling the bookmark on the same line" )
        {
            filteredData->toggleBookmark( 0, 10_lnum );

            THEN( "The bookmark and its mark are removed" )
            {
                REQUIRE( filteredData->getBookmarkSlots( 10_lnum ) == 0u );
                REQUIRE_FALSE( filteredData->findBookmark( 0, 0_lnum, false ) );
                REQUIRE_FALSE( isMarked( 10_lnum ) );
            }
        }

        AND_WHEN( "The user removes the mark" )
        {
            filteredData->toggleMark( 10_lnum );

            THEN( "The bookmark is removed too" )
            {
                REQUIRE( filteredData->getBookmarkSlots( 10_lnum ) == 0u );
            }
        }
    }

    WHEN( "Bookmarking a line that was already marked" )
    {
        filteredData->addMark( 30_lnum );
        filteredData->toggleBookmark( 1, 30_lnum );
        filteredData->toggleBookmark( 1, 30_lnum );

        THEN( "Removing the bookmark keeps the user mark" )
        {
            REQUIRE( filteredData->getBookmarkSlots( 30_lnum ) == 0u );
            REQUIRE( isMarked( 30_lnum ) );
        }
    }
}

// Hidden benchmark, run with FLOGG_BENCH_FILE=<big log> klogg_itests "[.benchmark]"
SCENARIO( "filter search benchmark", "[.benchmark]" )
{
    const auto benchFile = qEnvironmentVariable( "FLOGG_BENCH_FILE" );
    if ( benchFile.isEmpty() ) {
        WARN( "FLOGG_BENCH_FILE is not set" );
        return;
    }

    if ( const auto blockLines = qEnvironmentVariableIntValue( "FLOGG_BENCH_BLOCK_LINES" ) ) {
        Configuration::get().setSearchReadBufferSizeLines( blockLines );
    }

    using Clock = std::chrono::steady_clock;
    const auto elapsedMs = []( Clock::time_point start ) {
        return std::chrono::duration_cast<std::chrono::milliseconds>( Clock::now() - start )
            .count();
    };

    LogData logData;
    SafeQSignalSpy loadEndSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    auto start = Clock::now();
    logData.attachFile( benchFile );
    REQUIRE( loadEndSpy.safeWait( 600000 ) );
    const auto nbLines = logData.getNbLine();
    WARN( "Indexing " << nbLines.get() << " lines: " << elapsedMs( start ) << " ms" );

    // 30 filters: 26 plain text includes, 2 regex includes and 2 excludes
    std::vector<LogFilteredData::FilterSpec> filters;
    const QStringList words = { "wlan0",   "wlan1",   "hci0",    "ERROR",   "WARN",    "crash",
                                "timeout", "retry",   "login",   "cache",   "state",   "scan",
                                "AP",      "[auth]",  "[db]",    "[ui]",    "[net]",   "[kernel]",
                                "tid=1234", "tid=999", "id=42",  "id=4242", "done in", "miss key",
                                "changed", "module 7" };
    for ( const auto& word : words ) {
        filters.push_back( spec( word, false, false ) );
    }
    constexpr auto Inverse = false;
    constexpr auto Boolean = false;
    constexpr auto PlainText = false;
    filters.push_back(
        { RegularExpressionPattern( R"(in \d{2}ms)", true, Inverse, Boolean, PlainText ), false } );
    filters.push_back(
        { RegularExpressionPattern( R"(key=9\d+)", true, Inverse, Boolean, PlainText ), false } );
    filters.push_back( spec( "heartbeat", true ) );
    filters.push_back( spec( "DEBUG", true ) );

    const auto runAndWait = [ &nbLines ]( LogFilteredData& data,
                                          const std::function<void()>& launch ) {
        SafeQSignalSpy progressSpy( &data,
                                    SIGNAL( searchProgressed( LinesCount, int, LineNumber ) ) );
        launch();
        while ( progressSpy.isEmpty() || progressSpy.last().at( 1 ).toInt() < 100 ) {
            const auto recorded = progressSpy.count();
            REQUIRE( ( progressSpy.wait( 600000 ) || progressSpy.count() > recorded ) );
        }
        ( void )nbLines;
    };

    // Baseline: klogg search with the same filters combined in one boolean expression
    if ( qEnvironmentVariableIsEmpty( "FLOGG_BENCH_SKIP_BASELINE" ) ) {
        QStringList includes;
        QStringList excludes;
        for ( const auto& filter : filters ) {
            auto regex = filter.pattern.isPlainText
                             ? QRegularExpression::escape( filter.pattern.pattern )
                             : filter.pattern.pattern;
            regex = QString( filter.pattern.isCaseSensitive ? "(?-i:%1)" : "(?i:%1)" ).arg( regex );
            regex.replace( "\"", "\\\"" );
            ( filter.exclude ? excludes : includes ).append( "\"" + regex + "\"" );
        }
        const auto expression = "(" + includes.join( " or " ) + ") and not("
                                + excludes.join( " or " ) + ")";

        auto baselineData = logData.getNewFilteredData();
        start = Clock::now();
        runAndWait( *baselineData, [ & ] {
            baselineData->runSearch( RegularExpressionPattern( expression, true, false, true,
                                                               false ) );
        } );
        WARN( "klogg boolean search, 30 patterns: " << elapsedMs( start ) << " ms, "
                                                   << baselineData->getNbMatches().get()
                                                   << " matches" );
    }

    {
        auto singleData = logData.getNewFilteredData();
        start = Clock::now();
        runAndWait( *singleData, [ & ] {
            singleData->runSearch( RegularExpressionPattern( "wlan", false, false, false, true ) );
        } );
        WARN( "klogg single pattern search: " << elapsedMs( start ) << " ms, "
                                              << singleData->getNbMatches().get() << " matches" );
    }

    {
        auto singleFilterData = logData.getNewFilteredData();
        start = Clock::now();
        runAndWait( *singleFilterData, [ & ] {
            singleFilterData->runFilterSearch( { spec( "wlan", false, false ) }, 0_lnum,
                                               LineNumber( nbLines.get() ) );
        } );
        WARN( "Single filter search: " << elapsedMs( start ) << " ms, "
                                       << singleFilterData->getNbMatches().get() << " matches" );
    }

    auto filteredData = logData.getNewFilteredData();

    start = Clock::now();
    runAndWait( *filteredData, [ & ] {
        filteredData->runFilterSearch( filters, 0_lnum, LineNumber( nbLines.get() ) );
    } );
    const auto matchesAll = filteredData->getNbMatches();
    WARN( "First filter search, 30 filters: " << elapsedMs( start ) << " ms, " << matchesAll.get()
                                              << " matches" );

    // Toggling filters off and on only combines cached results
    auto withoutExclude = filters;
    withoutExclude.pop_back();
    start = Clock::now();
    runAndWait( *filteredData, [ & ] {
        filteredData->runFilterSearch( withoutExclude, 0_lnum, LineNumber( nbLines.get() ) );
    } );
    WARN( "Toggle exclude filter off: " << elapsedMs( start ) << " ms, "
                                        << filteredData->getNbMatches().get() << " matches" );

    auto withoutIncludes = filters;
    withoutIncludes.erase( withoutIncludes.begin(), withoutIncludes.begin() + 10 );
    start = Clock::now();
    runAndWait( *filteredData, [ & ] {
        filteredData->runFilterSearch( withoutIncludes, 0_lnum, LineNumber( nbLines.get() ) );
    } );
    WARN( "Toggle 10 include filters off: " << elapsedMs( start ) << " ms, "
                                            << filteredData->getNbMatches().get() << " matches" );

    start = Clock::now();
    runAndWait( *filteredData, [ & ] {
        filteredData->runFilterSearch( filters, 0_lnum, LineNumber( nbLines.get() ) );
    } );
    WARN( "Toggle everything back on: " << elapsedMs( start ) << " ms" );
    REQUIRE( filteredData->getNbMatches() == matchesAll );

    // Editing one filter rescans only that pattern
    auto edited = filters;
    edited[ 0 ] = spec( "wlan", false, false );
    start = Clock::now();
    runAndWait( *filteredData, [ & ] {
        filteredData->runFilterSearch( edited, 0_lnum, LineNumber( nbLines.get() ) );
    } );
    WARN( "Edit one filter (rescan 1 pattern): " << elapsedMs( start ) << " ms" );
}

// Hidden, saves pictures of the filter panel while dragging an item below the last row:
// FLOGG_RENDER_DIR=<dir> klogg_itests "[.render]"
SCENARIO( "Filter panel drop below the last row rendering", "[.render]" )
{
    const auto renderDir = qEnvironmentVariable( "FLOGG_RENDER_DIR" );
    if ( renderDir.isEmpty() ) {
        WARN( "FLOGG_RENDER_DIR is not set" );
        return;
    }

    QTemporaryDir dir;
    auto root = FilterNode::makeGroup( "root" );
    root.children.push_back( FilterNode::makeFilter( "ERROR" ) );
    root.children.back().name = "Errors\nof any kind";
    auto group = FilterNode::makeGroup( "WiFi" );
    group.children.push_back( FilterNode::makeFilter( "wlan" ) );
    auto inner = FilterNode::makeGroup( "Inner" );
    group.children.push_back( inner );
    root.children.push_back( group );
    const auto filtersFile = dir.filePath( "test.flogg-filters.json" );
    REQUIRE( filters::saveToFile( root, filtersFile ) );

    auto& settings = PersistentInfo::getSettings( app_settings{} );
    const auto previousFile = settings.value( "filterPanel/currentFile" );
    settings.setValue( "filterPanel/currentFile", filtersFile );

    FilterPanel panel;
    panel.resize( 400, 300 );
    panel.show();
    auto* tree = panel.findChild<QTreeWidget*>();
    REQUIRE( tree );
    REQUIRE( tree->topLevelItemCount() == 2 );

    tree->clearSelection();
    tree->topLevelItem( 0 )->setSelected( true );
    auto* mimeData = tree->model()->mimeData( tree->selectionModel()->selectedIndexes() );

    const auto lastRow = tree->visualItemRect( tree->topLevelItem( 1 )->child( 1 ) );
    const auto y = lastRow.bottom() + 20;
    const auto indent = tree->indentation();
    // The tree column follows the number column
    const auto treeLeft = tree->header()->sectionViewportPosition( 0 );

    // Drag move events are only delivered once the drag enter is accepted, which an internal
    // move tree only does for its own drags
    tree->setDragDropMode( QAbstractItemView::DragDrop );
    const auto sendToViewport
        = [ & ]( QEvent* event ) { QApplication::sendEvent( tree->viewport(), event ); };

    QDragEnterEvent enter( QPoint( 5, y ), Qt::MoveAction, mimeData, Qt::LeftButton,
                           Qt::NoModifier );
    sendToViewport( &enter );
    REQUIRE( enter.isAccepted() );

    for ( int level = 0; level < 4; ++level ) {
        QDragMoveEvent move( QPoint( treeLeft + indent * ( level + 1 ) + 2, y ), Qt::MoveAction, mimeData,
                             Qt::LeftButton, Qt::NoModifier );
        sendToViewport( &move );
        REQUIRE( move.isAccepted() );
        REQUIRE( panel.grab().save( renderDir + QString( "/drop_level%1.png" ).arg( level ) ) );
    }

    QDragLeaveEvent leave;
    sendToViewport( &leave );
    delete mimeData;

    // Edit dialog with a filter number and colour clear buttons
    auto colored = FilterNode::makeFilter( "ERROR" );
    colored.number = 3;
    colored.backColor = QColor( "#ffe08a" );
    FilterEditDialog editDialog( colored );
    editDialog.show();
    REQUIRE( editDialog.grab().save( renderDir + "/edit_dialog.png" ) );
    {
        const auto previousPalette = QApplication::palette();
        const auto previousStyle = QApplication::style()->name();
        StyleManager::applyStyle( StyleManager::LightStyleKey );
        FilterEditDialog lightDialog( colored );
        lightDialog.show();
        REQUIRE( lightDialog.grab().save( renderDir + "/edit_dialog_light.png" ) );
        QApplication::setStyle( previousStyle );
        QApplication::setPalette( previousPalette );
    }

    // First line of a multi line description in the column, all of it in the tool tip
    auto* error = tree->topLevelItem( 0 );
    CHECK( error->text( 2 ) == QString( "Errors \u2026" ) );
    CHECK( error->toolTip( 2 ) == "Errors\nof any kind" );

    // Close up of the tool bar, with the move buttons enabled
    tree->setCurrentItem( error );
    const auto toolBar = panel.grab( QRect( 0, 0, 220, 26 ) );
    REQUIRE( toolBar.scaled( toolBar.size() * 4 ).save( renderDir + "/toolbar_zoom.png" ) );

    // The tool buttons wrap to more rows when the panel is narrow
    for ( const auto width : { 400, 200, 120 } ) {
        panel.resize( width, 300 );
        QApplication::processEvents();
        REQUIRE( panel.grab().save( renderDir + QString( "/toolbar_%1.png" ).arg( width ) ) );
    }

    settings.setValue( "filterPanel/currentFile", previousFile );
}

SCENARIO( "Filter panel remembered and startup files", "[filters]" )
{
    const auto portableDir = PersistentInfo::portableDirectory();
    REQUIRE( !portableDir.isEmpty() );

    QTemporaryDir insideDir( portableDir + "/filters_test_XXXXXX" );
    QTemporaryDir outsideDir;
    REQUIRE( insideDir.isValid() );
    REQUIRE( outsideDir.isValid() );

    const auto saveFilters = []( const QString& path, const QString& pattern ) {
        auto root = FilterNode::makeGroup( "root" );
        root.children.push_back( FilterNode::makeFilter( pattern ) );
        REQUIRE( filters::saveToFile( root, path ) );
    };
    const auto insideFile = insideDir.filePath( "inside.flogg-filters.json" );
    const auto outsideFile = outsideDir.filePath( "outside.flogg-filters.json" );
    saveFilters( insideFile, "inside" );
    saveFilters( outsideFile, "outside" );

    auto& settings = PersistentInfo::getSettings( app_settings{} );
    const auto previousFile = settings.value( "filterPanel/currentFile" );
    const auto previousRecentFiles = settings.value( "filterPanel/recentFiles" );

    const auto firstPattern = []( const FilterPanel& panel ) {
        const auto& filters = panel.snapshot()->filters;
        return filters.empty() ? QString{} : filters.front().pattern;
    };
    const auto relativeInsideFile = QDir( portableDir ).relativeFilePath( insideFile );

    GIVEN( "a filter file in the folder of the settings" )
    {
        settings.setValue( "filterPanel/currentFile", insideFile );

        THEN( "it is remembered relative to that folder" )
        {
            FilterPanel panel;
            CHECK( panel.currentFile() == insideFile );
            CHECK( firstPattern( panel ) == "inside" );
            CHECK( settings.value( "filterPanel/currentFile" ).toString() == relativeInsideFile );
            CHECK( settings.value( "filterPanel/recentFiles" ).toStringList().front()
                   == relativeInsideFile );

            FilterPanel reopened;
            CHECK( reopened.currentFile() == insideFile );
            CHECK( firstPattern( reopened ) == "inside" );
        }
    }

    GIVEN( "a filter file elsewhere" )
    {
        settings.setValue( "filterPanel/currentFile", outsideFile );

        THEN( "it is remembered as is" )
        {
            FilterPanel panel;
            CHECK( panel.currentFile() == outsideFile );
            CHECK( settings.value( "filterPanel/currentFile" ).toString() == outsideFile );
        }
    }

    GIVEN( "a startup file" )
    {
        settings.setValue( "filterPanel/currentFile", relativeInsideFile );
        FilterPanel::setStartupFile( outsideFile );

        THEN( "new panels load it without remembering it" )
        {
            FilterPanel panel;
            CHECK( panel.currentFile() == outsideFile );
            CHECK( firstPattern( panel ) == "outside" );
            CHECK( settings.value( "filterPanel/currentFile" ).toString() == relativeInsideFile );
        }

        FilterPanel::setStartupFile( {} );
    }

    settings.setValue( "filterPanel/currentFile", previousFile );
    settings.setValue( "filterPanel/recentFiles", previousRecentFiles );
}

SCENARIO( "Filter panel enable, disable and suspend all", "[filters]" )
{
    QTemporaryDir dir;
    auto root = FilterNode::makeGroup( "root" );
    root.children.push_back( FilterNode::makeFilter( "ERROR" ) );
    auto group = FilterNode::makeGroup( "WiFi" );
    group.children.push_back( FilterNode::makeFilter( "wlan" ) );
    auto disabled = FilterNode::makeFilter( "hci0" );
    disabled.enabled = false;
    group.children.push_back( disabled );
    root.children.push_back( group );
    const auto filtersFile = dir.filePath( "test.flogg-filters.json" );
    REQUIRE( filters::saveToFile( root, filtersFile ) );

    auto& settings = PersistentInfo::getSettings( app_settings{} );
    const auto previousFile = settings.value( "filterPanel/currentFile" );
    settings.setValue( "filterPanel/currentFile", filtersFile );

    FilterPanel panel;
    QSignalSpy changed( &panel, &FilterPanel::filtersChanged );
    const auto run = [ & ]( const char* method ) {
        changed.clear();
        REQUIRE( QMetaObject::invokeMethod( &panel, method ) );
        REQUIRE( changed.wait( 2000 ) );
        return panel.snapshot()->filters.size();
    };
    const auto runWithArg = [ & ]( const char* method, bool arg ) {
        changed.clear();
        REQUIRE( QMetaObject::invokeMethod( &panel, method, Q_ARG( bool, arg ) ) );
        REQUIRE( changed.wait( 2000 ) );
        return panel.snapshot()->filters.size();
    };

    CHECK( runWithArg( "setAllEnabled", true ) == 3 );
    CHECK( runWithArg( "setAllEnabled", false ) == 0 );
    CHECK( runWithArg( "setAllEnabled", true ) == 3 );
    CHECK( run( "toggleSuspendAll" ) == 0 );
    CHECK( run( "toggleSuspendAll" ) == 3 );

    // A group has no check box, enabling it switches all its filters
    auto* tree = panel.findChild<QTreeWidget*>();
    REQUIRE( tree );
    auto* groupItem = tree->topLevelItem( 1 );
    CHECK( !groupItem->flags().testFlag( Qt::ItemIsUserCheckable ) );
    CHECK( !groupItem->data( 0, Qt::CheckStateRole ).isValid() );

    tree->clearSelection();
    groupItem->setSelected( true );
    CHECK( runWithArg( "setSelectedEnabled", false ) == 1 );
    CHECK( groupItem->child( 0 )->checkState( 0 ) == Qt::Unchecked );
    CHECK( runWithArg( "setSelectedEnabled", true ) == 3 );

    tree->clearSelection();
    tree->topLevelItem( 0 )->setSelected( true );
    CHECK( runWithArg( "setSelectedEnabled", false ) == 2 );
    CHECK( !groupItem->data( 0, Qt::CheckStateRole ).isValid() );

    settings.setValue( "filterPanel/currentFile", previousFile );
}

SCENARIO( "Filter panel group hit counts", "[filters]" )
{
    QTemporaryDir dir;
    auto root = FilterNode::makeGroup( "root" );
    root.children.push_back( FilterNode::makeFilter( "ERROR" ) );
    auto group = FilterNode::makeGroup( "WiFi" );
    group.children.push_back( FilterNode::makeFilter( "wlan" ) );
    auto disabled = FilterNode::makeFilter( "hci0" );
    disabled.enabled = false;
    group.children.push_back( disabled );
    auto inner = FilterNode::makeGroup( "Inner" );
    inner.children.push_back( FilterNode::makeFilter( "dhcp" ) );
    group.children.push_back( inner );
    group.children.push_back( FilterNode::makeGroup( "Empty" ) );
    root.children.push_back( group );
    const auto filtersFile = dir.filePath( "test.flogg-filters.json" );
    REQUIRE( filters::saveToFile( root, filtersFile ) );

    auto& settings = PersistentInfo::getSettings( app_settings{} );
    const auto previousFile = settings.value( "filterPanel/currentFile" );
    settings.setValue( "filterPanel/currentFile", filtersFile );

    FilterPanel panel;
    auto* tree = panel.findChild<QTreeWidget*>();
    REQUIRE( tree );

    // Only the effective filters get counts, as a crawler widget reports them
    const QHash<QString, qint64> countsByPattern{ { "ERROR", 7 }, { "wlan", 3 }, { "dhcp", 5 } };
    FilterHits hits;
    for ( const auto& filter : panel.snapshot()->filters ) {
        hits[ filter.id ] = countsByPattern.value( filter.pattern );
    }
    panel.setHitCounts( hits );

    // Every filter is numbered in priority order, disabled ones included
    std::vector<int> numbers;
    for ( const auto& filter : panel.snapshot()->filters ) {
        numbers.push_back( filter.number );
    }
    CHECK( numbers == std::vector<int>{ 1, 2, 4 } );
    CHECK( tree->topLevelItem( 1 )->child( 1 )->toolTip( 0 ).startsWith( "#3" ) );
    // Number column, shown first
    CHECK( tree->topLevelItem( 0 )->text( 3 ) == "1" );
    CHECK( tree->topLevelItem( 1 )->child( 2 )->child( 0 )->text( 3 ) == "4" );
    CHECK( tree->topLevelItem( 1 )->text( 3 ).isEmpty() );
    CHECK( tree->header()->visualIndex( 3 ) == 0 );
    CHECK_FALSE( tree->header()->isSectionHidden( 3 ) );

    // Header state saved before the number column existed
    {
        QTreeWidget oldTree;
        oldTree.setColumnCount( 3 );
        oldTree.header()->resizeSection( 2, 123 );
        const auto previousState = settings.value( "filterPanel/headerState" );
        settings.setValue( "filterPanel/headerState", oldTree.header()->saveState() );

        FilterPanel oldStatePanel;
        auto* oldStateTree = oldStatePanel.findChild<QTreeWidget*>();
        REQUIRE( oldStateTree );
        CHECK( oldStateTree->header()->visualIndex( 3 ) == 0 );
        CHECK_FALSE( oldStateTree->header()->isSectionHidden( 3 ) );
        CHECK( oldStateTree->header()->sectionSize( 3 ) > 0 );

        settings.setValue( "filterPanel/headerState", previousState );
    }

    auto* groupItem = tree->topLevelItem( 1 );
    CHECK( tree->topLevelItem( 0 )->text( 1 ) == "7" );
    CHECK( groupItem->text( 1 ) == "8" );
    CHECK( groupItem->child( 1 )->text( 1 ).isEmpty() );
    CHECK( groupItem->child( 2 )->text( 1 ) == "5" );
    CHECK( groupItem->child( 3 )->text( 1 ).isEmpty() );

    // No count at all leaves the group blank
    panel.setHitCounts( {} );
    CHECK( groupItem->text( 1 ).isEmpty() );

    settings.setValue( "filterPanel/currentFile", previousFile );
}

SCENARIO( "Filter panel check box lined up with the group arrows", "[filters]" )
{
    QTemporaryDir dir;
    auto root = FilterNode::makeGroup( "root" );
    root.children.push_back( FilterNode::makeFilter( "ERROR" ) );
    root.children.push_back( FilterNode::makeGroup( "G" ) );
    const auto filtersFile = dir.filePath( "test.flogg-filters.json" );
    REQUIRE( filters::saveToFile( root, filtersFile ) );

    auto& settings = PersistentInfo::getSettings( app_settings{} );
    const auto previousFile = settings.value( "filterPanel/currentFile" );
    settings.setValue( "filterPanel/currentFile", filtersFile );

    FilterPanel panel;
    panel.resize( 400, 300 );
    panel.show();
    auto* tree = panel.findChild<QTreeWidget*>();
    REQUIRE( tree );
    auto* filter = tree->topLevelItem( 0 );
    REQUIRE( filter->checkState( 0 ) == Qt::Checked );

    // The check box is drawn where a group has its expand arrow
    const auto rowRect = tree->visualItemRect( filter );
    // Top level items start one indentation into the filter column, after the number column
    const QPoint arrowArea( tree->header()->sectionViewportPosition( 0 ) + tree->indentation() / 2,
                            rowRect.center().y() );
    QTest::mouseClick( tree->viewport(), Qt::LeftButton, Qt::NoModifier, arrowArea );
    CHECK( filter->checkState( 0 ) == Qt::Unchecked );
    QTest::mouseClick( tree->viewport(), Qt::LeftButton, Qt::NoModifier, arrowArea );
    CHECK( filter->checkState( 0 ) == Qt::Checked );

    settings.setValue( "filterPanel/currentFile", previousFile );
}

SCENARIO( "Filter panel move up and down across groups", "[filters]" )
{
    QTemporaryDir dir;
    auto root = FilterNode::makeGroup( "root" );
    root.children.push_back( FilterNode::makeFilter( "A" ) );
    auto group = FilterNode::makeGroup( "G" );
    group.children.push_back( FilterNode::makeFilter( "B" ) );
    root.children.push_back( group );
    root.children.push_back( FilterNode::makeFilter( "C" ) );
    const auto filtersFile = dir.filePath( "test.flogg-filters.json" );
    REQUIRE( filters::saveToFile( root, filtersFile ) );

    auto& settings = PersistentInfo::getSettings( app_settings{} );
    const auto previousFile = settings.value( "filterPanel/currentFile" );
    settings.setValue( "filterPanel/currentFile", filtersFile );

    FilterPanel panel;
    auto* tree = panel.findChild<QTreeWidget*>();
    REQUIRE( tree );

    // Texts of the tree, children of a group between brackets
    const std::function<QString( QTreeWidgetItem* )> layout = [ & ]( QTreeWidgetItem* parent ) {
        QStringList texts;
        for ( int i = 0; i < parent->childCount(); ++i ) {
            auto* item = parent->child( i );
            texts.append( item->childIndicatorPolicy() == QTreeWidgetItem::ShowIndicator
                              ? item->text( 0 ) + "[" + layout( item ) + "]"
                              : item->text( 0 ) );
        }
        return texts.join( " " );
    };
    const auto move = [ & ]( bool up ) {
        REQUIRE( QMetaObject::invokeMethod( &panel, "moveCurrent", Q_ARG( bool, up ) ) );
        return layout( tree->invisibleRootItem() );
    };

    REQUIRE( layout( tree->invisibleRootItem() ) == "A G[B] C" );

    tree->setCurrentItem( tree->topLevelItem( 2 ) );
    CHECK( move( true ) == "A G[B C]" );
    CHECK( move( true ) == "A G[C B]" );
    CHECK( move( true ) == "A C G[B]" );
    CHECK( move( true ) == "C A G[B]" );
    CHECK( move( true ) == "C A G[B]" );
    CHECK( move( false ) == "A C G[B]" );
    CHECK( move( false ) == "A G[C B]" );

    // A collapsed group is jumped over
    tree->topLevelItem( 1 )->setExpanded( false );
    tree->setCurrentItem( tree->topLevelItem( 0 ) );
    CHECK( move( false ) == "G[C B] A" );

    settings.setValue( "filterPanel/currentFile", previousFile );
}
