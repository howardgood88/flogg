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

#include <QAbstractButton>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QTabBar>
#include <QMessageBox>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QApplication>
#include <QDialogButtonBox>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QTreeWidgetItemIterator>
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
#include "abstractlogview.h"
#include "crawlerwidget.h"
#include "filterpanel.h"
#include "mainwindow.h"
#include "optionsdialog.h"
#include "session.h"
#include "tabbedcrawlerwidget.h"
#include "filtertree.h"
#include "highlighterset.h"
#include "persistentinfo.h"

namespace {

constexpr int NbLines = 30000;

// Answers the question about unsaved filters shown (a message box or the list of the filters
// lost when closing) with Discard, false when none is shown
bool discardQuestion()
{
    auto* modal = QApplication::activeModalWidget();
    if ( auto* box = qobject_cast<QMessageBox*>( modal ) ) {
        box->button( QMessageBox::Discard )->click();
        return true;
    }
    if ( modal && modal->objectName() == "unsavedFilters" ) {
        modal->findChild<QDialogButtonBox*>()->button( QDialogButtonBox::Discard )->click();
        return true;
    }
    return false;
}

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
// Hidden, saves the filter panel with selected rows in both styles:
// FLOGG_RENDER_DIR=<dir> klogg_itests "[.render]"
SCENARIO( "Filter panel selection rendering", "[.render]" )
{
    const auto renderDir = qEnvironmentVariable( "FLOGG_RENDER_DIR" );
    if ( renderDir.isEmpty() ) {
        WARN( "FLOGG_RENDER_DIR is not set" );
        return;
    }

    QTemporaryDir dir;
    auto root = FilterNode::makeGroup( "root" );
    auto errors = FilterNode::makeFilter( "ERROR" );
    errors.backColor = QColor( "#fff3a0" );
    root.children.push_back( errors );
    auto red = FilterNode::makeFilter( "crash" );
    red.foreColor = QColor( "#c00000" );
    root.children.push_back( red );
    auto group = FilterNode::makeGroup( "WiFi" );
    auto wlan = FilterNode::makeFilter( "wlan" );
    wlan.backColor = QColor( "#2e7d32" );
    wlan.foreColor = QColor( Qt::white );
    group.children.push_back( wlan );
    group.children.push_back( FilterNode::makeFilter( "connected" ) );
    root.children.push_back( group );
    const auto filtersFile = dir.filePath( "test.flogg-filters.json" );
    REQUIRE( filters::saveToFile( root, filtersFile ) );

    auto& settings = PersistentInfo::getSettings( app_settings{} );
    const auto previousFile = settings.value( "filterPanel/currentFile" );
    settings.setValue( "filterPanel/currentFile", filtersFile );

    const auto previousPalette = QApplication::palette();
    const auto previousStyle = QApplication::style()->name();
    {
        FilterPanel panel;
        panel.resize( 400, 260 );
        panel.show();
        auto* tree = panel.findChild<QTreeWidget*>();
        REQUIRE( tree );
        tree->expandAll();
        tree->clearSelection();
        tree->topLevelItem( 0 )->setSelected( true );
        tree->topLevelItem( 2 )->child( 0 )->setSelected( true );

        for ( const auto& style : { StyleManager::LightStyleKey, StyleManager::DarkStyleKey } ) {
            StyleManager::applyStyle( style );
            QTest::qWait( 100 );
            REQUIRE( panel.grab().save(
                QString( "%1/panel_selection_%2.png" ).arg( renderDir, style ) ) );
        }
    }

    QApplication::setStyle( previousStyle );
    QApplication::setPalette( previousPalette );
    settings.setValue( "filterPanel/currentFile", previousFile );
}

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

SCENARIO( "Filter panel selected filter has the selection colours", "[filters]" )
{
    QTemporaryDir dir;
    auto root = FilterNode::makeGroup( "root" );
    root.children.push_back( FilterNode::makeFilter( "WWWWWWWWWW" ) );
    root.children.back().foreColor = QColor( Qt::red );
    root.children.back().backColor = QColor( Qt::yellow );
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
    // The current cell is in the hits column
    tree->setCurrentItem( filter, 1 );
    tree->setFocus();
    REQUIRE( filter->isSelected() );

    // Like the selected lines of the log views: white text on the selection colour, the
    // filter colours are not shown
    const auto row = tree->viewport()->grab( tree->visualItemRect( filter ) ).toImage();
    int whitePixels = 0;
    int selectionPixels = 0;
    int filterPixels = 0;
    for ( int y = 0; y < row.height(); ++y ) {
        for ( int x = 0; x < row.width(); ++x ) {
            const auto color = row.pixelColor( x, y );
            if ( color.red() > 240 && color.green() > 240 && color.blue() > 240 ) {
                ++whitePixels;
            }
            if ( color == StyleManager::selectionBackColor() ) {
                ++selectionPixels;
            }
            if ( color.red() > 150 && color.blue() < 120 ) {
                ++filterPixels;
            }
        }
    }
    CHECK( whitePixels > 20 );
    CHECK( filterPixels == 0 );

    // The current cell looks like the rest of the selected row, also in the Light style
    // which fills it
    {
        const auto previousPalette = QApplication::palette();
        const auto previousStyle = QApplication::style()->name();
        StyleManager::applyStyle( StyleManager::LightStyleKey );
        QApplication::processEvents();
        const auto hitsCell = tree->visualRect( tree->currentIndex() );
        const auto descriptionCell = tree->visualRect( tree->currentIndex().siblingAtColumn( 2 ) );
        const auto cells = tree->viewport()->grab().toImage();
        CHECK( cells.pixelColor( hitsCell.topLeft() + QPoint( 4, 4 ) )
               == cells.pixelColor( descriptionCell.topLeft() + QPoint( 4, 4 ) ) );
        QApplication::setStyle( previousStyle );
        QApplication::setPalette( previousPalette );
        QApplication::processEvents();
    }
    CHECK( selectionPixels > row.width() * row.height() / 4 );

    // FLOGG_RENDER_DIR=<dir> also saves pictures of a selected filter, plain filter and group
    const auto renderDir = qEnvironmentVariable( "FLOGG_RENDER_DIR" );
    if ( !renderDir.isEmpty() ) {
        tree->addTopLevelItem( new QTreeWidgetItem( *filter ) );
        tree->topLevelItem( 1 )->setForeground( 0, QBrush{} );
        tree->topLevelItem( 1 )->setBackground( 0, QBrush{} );
        REQUIRE( panel.grab().save( renderDir + "/selected_filter.png" ) );
        tree->setCurrentItem( tree->topLevelItem( 1 ) );
        REQUIRE( panel.grab().save( renderDir + "/selected_plain_filter.png" ) );

        const auto previousPalette = QApplication::palette();
        const auto previousStyle = QApplication::style()->name();
        for ( const auto& style : { StyleManager::DarkStyleKey, StyleManager::LightStyleKey } ) {
            StyleManager::applyStyle( style );
            QApplication::processEvents();
            tree->setCurrentItem( filter );
            REQUIRE( panel.grab().save( renderDir + "/selected_filter_" + style + ".png" ) );
        }
        QApplication::setStyle( previousStyle );
        QApplication::setPalette( previousPalette );
    }

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

    // The numbers shown by the tool tips of the views follow right away
    panel.tabState();
    QStringList numbered;
    for ( const auto& filter : panel.snapshot()->filters ) {
        numbered.append( filters::numberText( filter ) + filter.pattern );
    }
    numbered.sort();
    CHECK( numbered.join( " " ) == "#1C #2B #3A" );

    settings.setValue( "filterPanel/currentFile", previousFile );
}

SCENARIO( "Filter panel tab states and manual save", "[filters]" )
{
    QTemporaryDir dir;
    auto root = FilterNode::makeGroup( "root" );
    root.children.push_back( FilterNode::makeFilter( "ERROR" ) );
    root.children.push_back( FilterNode::makeFilter( "WARN" ) );
    const auto filtersFile = dir.filePath( "tabs.flogg-filters.json" );
    REQUIRE( filters::saveToFile( root, filtersFile ) );
    const auto fileContent = [ & ]() {
        QFile file( filtersFile );
        REQUIRE( file.open( QIODevice::ReadOnly ) );
        return file.readAll();
    };
    const auto savedContent = fileContent();

    auto& settings = PersistentInfo::getSettings( app_settings{} );
    const auto previousFile = settings.value( "filterPanel/currentFile" );
    settings.setValue( "filterPanel/currentFile", filtersFile );

    FilterPanel panel;
    QSignalSpy changed( &panel, &FilterPanel::filtersChanged );
    REQUIRE_FALSE( panel.isModified() );

    changed.clear();
    REQUIRE( QMetaObject::invokeMethod( &panel, "setAllEnabled", Q_ARG( bool, false ) ) );
    REQUIRE( changed.wait( 2000 ) );

    // Changes stay in the panel until saved
    CHECK( panel.isModified() );
    CHECK( fileContent() == savedContent );

    const auto firstTab = panel.tabState();
    CHECK( FilterPanel::isModified( firstTab ) );
    CHECK( firstTab.snapshot->filters.empty() );

    // A tab shown for the first time gets the default file as it is saved
    panel.setTabState( FilterTabState{} );
    CHECK_FALSE( panel.isModified() );
    CHECK( panel.snapshot()->filters.size() == 2 );
    const auto secondTab = panel.tabState();

    // Back to the first tab, its filters are shown without being applied again
    panel.setTabState( firstTab );
    CHECK( panel.isModified() );
    CHECK( panel.snapshot() == firstTab.snapshot );

    REQUIRE( QMetaObject::invokeMethod( &panel, "saveFile" ) );
    CHECK_FALSE( panel.isModified() );
    CHECK( fileContent() != savedContent );

    // The other tab now differs from the file
    CHECK( FilterPanel::isModified(
        FilterPanel::restoredTabState( filtersFile, secondTab.root ) ) );
    CHECK_FALSE( FilterPanel::isModified(
        FilterPanel::restoredTabState( filtersFile, panel.tabState().root ) ) );

    settings.setValue( "filterPanel/currentFile", previousFile );
}

SCENARIO( "Filter panel tab switch timing", "[.timing]" )
{
    const auto filtersFile = qEnvironmentVariable( "FLOGG_TIMING_FILTERS" );
    if ( filtersFile.isEmpty() ) {
        return;
    }
    auto& settings = PersistentInfo::getSettings( app_settings{} );
    const auto previousFile = settings.value( "filterPanel/currentFile" );
    settings.setValue( "filterPanel/currentFile", filtersFile );

    FilterPanel panel;
    panel.resize( 400, 800 );
    panel.show();
    QApplication::processEvents();

    auto first = panel.tabState();
    panel.setTabState( FilterTabState{} );
    auto second = panel.tabState();

    QElapsedTimer timer;
    timer.start();
    const int Switches = std::max( 20, qEnvironmentVariableIntValue( "FLOGG_TIMING_SWITCHES" ) );
    for ( int i = 0; i < Switches; ++i ) {
        auto& target = i % 2 ? second : first;
        auto& previous = i % 2 ? first : second;
        previous = panel.tabState();
        const auto taken = timer.nsecsElapsed();
        panel.setTabState( target );
        const auto set = timer.nsecsElapsed();
        QApplication::processEvents();
        const auto painted = timer.nsecsElapsed();
        if ( i < 4 ) {
            WARN( "tabState " << taken / 1000 << " us, setTabState " << ( set - taken ) / 1000
                              << " us, events " << ( painted - set ) / 1000 << " us" );
        }
        timer.restart();
    }

    settings.setValue( "filterPanel/currentFile", previousFile );
}

SCENARIO( "Main window tab switch timing", "[.timing]" )
{
    const auto filtersFile = qEnvironmentVariable( "FLOGG_TIMING_FILTERS" );
    const auto logs = qEnvironmentVariable( "FLOGG_TIMING_LOGS" ).split( ',', Qt::SkipEmptyParts );
    if ( filtersFile.isEmpty() || logs.size() < 2 ) {
        return;
    }
    FilterPanel::setStartupFile( filtersFile );

    auto appSession = std::make_shared<Session>();
    WindowSession windowSession{ appSession, "Timing", 0 };
    MainWindow mainWindow( windowSession );
    mainWindow.resize( 1400, 900 );
    mainWindow.show();
    for ( const auto& log : logs ) {
        mainWindow.loadInitialFile( log, false );
    }
    auto* tabArea = mainWindow.findChild<TabbedCrawlerWidget*>();
    REQUIRE( waitUiState( [ & ] { return tabArea->count() == logs.size(); } ) );

    // Every tab gets its filters applied once
    for ( int i = 0; i < tabArea->count(); ++i ) {
        tabArea->setCurrentIndex( i );
        QTest::qWait( 8000 );
    }

    QElapsedTimer timer;
    const auto switches = qEnvironmentVariableIntValue( "FLOGG_TIMING_SWITCHES" );
    for ( int i = 0; i < std::max( 6, switches ); ++i ) {
        timer.start();
        tabArea->setCurrentIndex( i % tabArea->count() );
        const auto switched = timer.nsecsElapsed();
        QApplication::processEvents();
        const auto painted = timer.nsecsElapsed();
        WARN( "switch to tab " << i % tabArea->count() << ": " << switched / 1000 << " us, events "
                               << ( painted - switched ) / 1000 << " us" );
        QTest::qWait( switches ? 10 : 500 );
    }

    FilterPanel::setStartupFile( {} );
}

SCENARIO( "Filter panel tabs keep collapsed groups and selection", "[filters]" )
{
    QTemporaryDir dir;
    auto root = FilterNode::makeGroup( "root" );
    auto first = FilterNode::makeGroup( "first" );
    first.children.push_back( FilterNode::makeFilter( "ERROR" ) );
    auto second = FilterNode::makeGroup( "second" );
    second.children.push_back( FilterNode::makeFilter( "WARN" ) );
    root.children.push_back( first );
    root.children.push_back( second );
    const auto filtersFile = dir.filePath( "groups.flogg-filters.json" );
    REQUIRE( filters::saveToFile( root, filtersFile ) );

    auto& settings = PersistentInfo::getSettings( app_settings{} );
    const auto previousFile = settings.value( "filterPanel/currentFile" );
    settings.setValue( "filterPanel/currentFile", filtersFile );

    FilterPanel panel;
    auto* tree = panel.findChild<QTreeWidget*>();
    REQUIRE( tree );
    tree->topLevelItem( 0 )->setExpanded( false );
    tree->setCurrentItem( tree->topLevelItem( 1 )->child( 0 ) );

    const auto firstTab = panel.tabState();
    panel.setTabState( FilterTabState{} );
    CHECK( tree->topLevelItem( 0 )->isExpanded() );

    panel.setTabState( firstTab );
    CHECK_FALSE( tree->topLevelItem( 0 )->isExpanded() );
    CHECK( tree->topLevelItem( 1 )->isExpanded() );
    CHECK( tree->currentItem() == tree->topLevelItem( 1 )->child( 0 ) );
    CHECK( tree->topLevelItem( 1 )->child( 0 )->isSelected() );

    settings.setValue( "filterPanel/currentFile", previousFile );
}

namespace {
// Filters like a big user filter file: groups of 16 filters, every fifth one outside a group,
// some suspended, disabled, colored or regex
FilterNode manyFilters( int count )
{
    auto root = FilterNode::makeGroup( "root" );
    FilterNode* group = nullptr;
    for ( int i = 0; i < count; ++i ) {
        auto filter = FilterNode::makeFilter( QString( "word %1" ).arg( i ) );
        filter.useRegex = i % 20 == 0;
        filter.enabled = i % 7 != 0;
        filter.suspended = i % 11 == 0;
        filter.exclude = i % 25 == 0;
        filter.foreColor = i % 2 ? QColor( Qt::red ) : QColor{};
        filter.backColor = i % 3 ? QColor( Qt::yellow ) : QColor{};
        if ( i % 5 == 4 ) {
            root.children.push_back( filter );
            group = nullptr;
            continue;
        }
        if ( !group || group->children.size() >= 16 ) {
            root.children.push_back( FilterNode::makeGroup( QString( "group %1" ).arg( i ) ) );
            group = &root.children.back();
        }
        group->children.push_back( filter );
    }
    return root;
}
} // namespace

SCENARIO( "Filter panel tab switch builds the tree without per item model changes", "[filters]" )
{
    QTemporaryDir dir;
    const auto filtersFile = dir.filePath( "many.flogg-filters.json" );
    REQUIRE( filters::saveToFile( manyFilters( 300 ), filtersFile ) );

    auto& settings = PersistentInfo::getSettings( app_settings{} );
    const auto previousFile = settings.value( "filterPanel/currentFile" );
    settings.setValue( "filterPanel/currentFile", filtersFile );

    FilterPanel panel;
    auto* tree = panel.findChild<QTreeWidget*>();
    REQUIRE( tree );
    // The first filter is disabled and suspended, the second one is shown normally
    const auto secondId = tree->topLevelItem( 0 )->child( 1 )->data( 0, Qt::UserRole ).toString();
    panel.setHitCounts( { { secondId, 42 } } );
    auto firstTab = panel.tabState();
    // Unchanged filters keep a single copy of their serialized tree
    CHECK( firstTab.json.constData() == firstTab.savedJson.constData() );

    panel.setTabState( FilterTabState{} );
    CHECK( tree->topLevelItem( 0 )->child( 1 )->text( 1 ).isEmpty() );
    const auto secondTab = panel.tabState();

    QSignalSpy dataChanged( tree->model(), &QAbstractItemModel::dataChanged );
    QSignalSpy rowsInserted( tree->model(), &QAbstractItemModel::rowsInserted );
    panel.setTabState( firstTab );

    // Hundreds of filters are added at once, with their look already set
    CHECK( dataChanged.count() <= 2 );
    CHECK( rowsInserted.count() == 1 );

    // Number, hits, suspended look and modified state come back with the tab
    const auto* firstFilter = tree->topLevelItem( 0 )->child( 0 );
    const auto* secondFilter = tree->topLevelItem( 0 )->child( 1 );
    CHECK( firstFilter->text( 3 ) == "1" );
    CHECK( secondFilter->text( 3 ) == "2" );
    CHECK( secondFilter->text( 1 ) == "42" );
    CHECK( firstFilter->data( 0, Qt::UserRole + 1 ).toBool() );
    CHECK( firstFilter->font( 0 ).strikeOut() );
    CHECK_FALSE( secondFilter->font( 0 ).strikeOut() );
    CHECK( tree->topLevelItem( 0 )->text( 1 ) == "42" );
    CHECK_FALSE( panel.isModified() );

    // Switching again changes nothing either
    dataChanged.clear();
    panel.setTabState( secondTab );
    panel.setHitCounts( {} );
    CHECK( dataChanged.count() <= 2 );

    settings.setValue( "filterPanel/currentFile", previousFile );
}

SCENARIO( "Filter highlighters are compiled in the background", "[filters]" )
{
    QTemporaryDir dir;
    auto root = FilterNode::makeGroup( "root" );
    auto error = FilterNode::makeFilter( "ERROR" );
    error.backColor = QColor( Qt::red );
    root.children.push_back( error );
    auto word = FilterNode::makeFilter( "wlan" );
    word.colorMode = FilterNode::ColorMode::MatchOnly;
    word.foreColor = QColor( Qt::blue );
    root.children.push_back( word );
    const auto filtersFile = dir.filePath( "colors.flogg-filters.json" );
    REQUIRE( filters::saveToFile( root, filtersFile ) );

    auto& settings = PersistentInfo::getSettings( app_settings{} );
    const auto previousFile = settings.value( "filterPanel/currentFile" );
    settings.setValue( "filterPanel/currentFile", filtersFile );

    FilterPanel panel;
    QSignalSpy compiled( &panel, &FilterPanel::highlightersCompiled );
    REQUIRE( ( compiled.count() > 0 || compiled.wait( 5000 ) ) );
    const auto highlighters = panel.snapshot()->highlighters;
    CHECK( highlighters->isCompiled() );

    const auto matcher = highlighters->createMatcher();
    HighlightedMatchRanges matches;
    CHECK( highlighters->matchLine( "an ERROR here", matches, *matcher )
           == HighlighterMatchType::LineMatch );
    matches = {};
    CHECK( highlighters->matchLine( "wlan0 up", matches, *matcher )
           == HighlighterMatchType::WordMatch );
    matches = {};
    CHECK( highlighters->matchLine( "nothing", matches, *matcher )
           == HighlighterMatchType::NoMatch );

    // A change compiles a new set
    compiled.clear();
    REQUIRE( QMetaObject::invokeMethod( &panel, "setAllEnabled", Q_ARG( bool, false ) ) );
    REQUIRE( QMetaObject::invokeMethod( &panel, "setAllEnabled", Q_ARG( bool, true ) ) );
    REQUIRE( compiled.wait( 5000 ) );
    CHECK( panel.snapshot()->highlighters != highlighters );
    CHECK( panel.snapshot()->highlighters->isCompiled() );

    settings.setValue( "filterPanel/currentFile", previousFile );
}

SCENARIO( "Filter panel saves the suspended filters", "[filters]" )
{
    QTemporaryDir dir;
    auto root = FilterNode::makeGroup( "root" );
    root.children.push_back( FilterNode::makeFilter( "ERROR" ) );
    root.children.push_back( FilterNode::makeFilter( "WARN" ) );
    const auto filtersFile = dir.filePath( "suspend.flogg-filters.json" );
    REQUIRE( filters::saveToFile( root, filtersFile ) );

    auto& settings = PersistentInfo::getSettings( app_settings{} );
    const auto previousFile = settings.value( "filterPanel/currentFile" );
    settings.setValue( "filterPanel/currentFile", filtersFile );

    {
        FilterPanel panel;
        QSignalSpy changed( &panel, &FilterPanel::filtersChanged );
        REQUIRE( QMetaObject::invokeMethod( &panel, "toggleSuspendAll" ) );
        REQUIRE( changed.wait( 2000 ) );
        CHECK( panel.isModified() );
        REQUIRE( QMetaObject::invokeMethod( &panel, "saveFile" ) );
        CHECK_FALSE( panel.isModified() );
    }

    const auto saved = filters::loadFromFile( filtersFile );
    REQUIRE( saved );
    CHECK( saved->children[ 0 ].suspended );
    CHECK( saved->children[ 1 ].suspended );

    FilterPanel panel;
    CHECK( panel.snapshot()->filters.empty() );
    CHECK_FALSE( panel.isModified() );

    settings.setValue( "filterPanel/currentFile", previousFile );
}

// Hidden benchmark of the filter panel and the filter search with many filters:
// FLOGG_BENCH_FILTERS=<counts, default 343,2000> [FLOGG_BENCH_FILE=<log>] klogg_itests
// "[.benchmark]". The search part runs only with a log.
SCENARIO( "many filters benchmark", "[.benchmark]" )
{
    auto counts = qEnvironmentVariable( "FLOGG_BENCH_FILTERS", "343,2000" ).split( ',' );
    QTemporaryDir dir;
    auto& settings = PersistentInfo::getSettings( app_settings{} );
    const auto previousFile = settings.value( "filterPanel/currentFile" );

    const auto ms = []( const QElapsedTimer& timer ) {
        return QString::number( static_cast<double>( timer.nsecsElapsed() ) / 1e6, 'f', 2 );
    };

    for ( const auto& countText : counts ) {
        const auto count = countText.toInt();
        const auto root = manyFilters( count );
        const auto filtersFile = dir.filePath( countText + ".flogg-filters.json" );
        REQUIRE( filters::saveToFile( root, filtersFile ) );
        settings.setValue( "filterPanel/currentFile", filtersFile );

        QElapsedTimer timer;
        timer.start();
        FilterPanel panel;
        panel.resize( 400, 800 );
        panel.show();
        QApplication::processEvents();
        WARN( count << " filters, panel load: " << ms( timer ).toStdString() << " ms" );

        auto* tree = panel.findChild<QTreeWidget*>();
        QSignalSpy dataChanged( tree->model(), &QAbstractItemModel::dataChanged );

        // Two tabs with the same filters, as when switching between log files
        auto first = panel.tabState();
        panel.setTabState( FilterTabState{} );
        auto second = panel.tabState();
        constexpr int Switches = 20;
        qint64 longest = 0;
        qint64 total = 0;
        dataChanged.clear();
        for ( int i = 0; i < Switches; ++i ) {
            timer.restart();
            ( i % 2 ? first : second ) = panel.tabState();
            panel.setTabState( i % 2 ? second : first );
            QApplication::processEvents();
            longest = std::max( longest, timer.nsecsElapsed() );
            total += timer.nsecsElapsed();
        }
        WARN( count << " filters, tab switch with painting: average "
                    << total / Switches / 1000 << " us, longest " << longest / 1000
                    << " us, model changes per switch " << dataChanged.count() / Switches );

        for ( const auto* method : { "toggleSuspendAll", "toggleSuspendAll" } ) {
            QSignalSpy changed( &panel, &FilterPanel::filtersChanged );
            timer.restart();
            REQUIRE( QMetaObject::invokeMethod( &panel, method ) );
            REQUIRE( changed.wait( 5000 ) );
            // Without the 150 ms debounce delay
            WARN( count << " filters, suspend or resume all: "
                        << ( timer.nsecsElapsed() / 1000 - 150'000 ) / 1000 << " ms" );
        }

        FilterHits hits;
        for ( const auto* filter : filters::effectiveFilters( root ) ) {
            hits.insert( filter->id, 12345 );
        }
        timer.restart();
        panel.setHitCounts( hits );
        QApplication::processEvents();
        WARN( count << " filters, hit counts update: " << ms( timer ).toStdString() << " ms" );

        // The log views compile the highlighters of new filters on the UI thread when they
        // paint, then match every drawn line against them
        const auto effective = filters::effectiveFilters( root );
        timer.restart();
        const auto highlighters = filters::buildHighlighterSet( effective );
        WARN( count << " filters, highlighters creation: " << ms( timer ).toStdString()
                    << " ms (" << effective.size() << " effective)" );
        timer.restart();
        const auto matcher = highlighters.createMatcher();
        WARN( count << " filters, highlighters compile: " << ms( timer ).toStdString()
                    << " ms" );
        QStringList lines;
        for ( int i = 0; i < 60; ++i ) {
            lines.append( QString( "2026-10-22 10:00:00.003 INFO  [wlan1] tid=7955 word %1 "
                                   "crash in module 9156" )
                              .arg( i * 7 ) );
        }
        timer.restart();
        for ( const auto& line : lines ) {
            HighlightedMatchRanges matches;
            highlighters.matchLine( line, matches, *matcher );
        }
        WARN( count << " filters, highlighting 60 drawn lines: " << ms( timer ).toStdString()
                    << " ms" );
    }
    settings.setValue( "filterPanel/currentFile", previousFile );

    const auto benchFile = qEnvironmentVariable( "FLOGG_BENCH_FILE" );
    if ( benchFile.isEmpty() ) {
        WARN( "FLOGG_BENCH_FILE is not set, no search benchmark" );
        return;
    }

    LogData logData;
    SafeQSignalSpy loadEndSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    logData.attachFile( benchFile );
    REQUIRE( loadEndSpy.safeWait( 600000 ) );
    const auto nbLines = logData.getNbLine();

    const auto runAndWait = []( LogFilteredData& data, const std::function<void()>& launch ) {
        SafeQSignalSpy progressSpy( &data,
                                    SIGNAL( searchProgressed( LinesCount, int, LineNumber ) ) );
        launch();
        while ( progressSpy.isEmpty() || progressSpy.last().at( 1 ).toInt() < 100 ) {
            const auto recorded = progressSpy.count();
            REQUIRE( ( progressSpy.wait( 600000 ) || progressSpy.count() > recorded ) );
        }
    };

    // Words of the benchmark logs, most filters match few lines like real ones
    const QStringList words = { "wlan0", "hci0", "ERROR", "WARN", "crash", "timeout",
                                "retry", "login", "cache", "state", "scan", "[db]" };
    for ( const auto& countText : counts ) {
        const auto count = countText.toInt();
        std::vector<LogFilteredData::FilterSpec> specs;
        for ( int i = 0; i < count; ++i ) {
            const auto& word = words[ i % words.size() ];
            specs.push_back( i % 20 == 0
                                 ? LogFilteredData::FilterSpec{ RegularExpressionPattern(
                                                                    word + R"( \d{2})", true,
                                                                    false, false, false ),
                                                                false }
                                 : spec( i % 3 ? QString( "%1 %2" ).arg( word ).arg( i ) : word,
                                         i % 25 == 0, i % 2 == 0 ) );
        }

        auto filteredData = logData.getNewFilteredData();
        QElapsedTimer timer;
        timer.start();
        runAndWait( *filteredData, [ & ] {
            filteredData->runFilterSearch( specs, 0_lnum, LineNumber( nbLines.get() ) );
        } );
        WARN( count << " filters, first search: " << timer.elapsed() << " ms, "
                    << filteredData->getNbMatches().get() << " matches" );

        // Suspending half of them only recombines the cached results
        auto half = specs;
        half.resize( specs.size() / 2 );
        timer.restart();
        runAndWait( *filteredData, [ & ] {
            filteredData->runFilterSearch( half, 0_lnum, LineNumber( nbLines.get() ) );
        } );
        WARN( count << " filters, suspend half: " << timer.elapsed() << " ms" );

        timer.restart();
        runAndWait( *filteredData, [ & ] {
            filteredData->runFilterSearch( specs, 0_lnum, LineNumber( nbLines.get() ) );
        } );
        WARN( count << " filters, resume all: " << timer.elapsed() << " ms" );
    }
}

// Longest time the event loop was blocked while the action and its follow up work ran
namespace {
qint64 longestStallMs( const std::function<void()>& action, int settleMs )
{
    QElapsedTimer clock;
    qint64 last = 0;
    qint64 longest = 0;
    QTimer ticker;
    ticker.setInterval( 1 );
    QObject::connect( &ticker, &QTimer::timeout, [ & ] {
        const auto now = clock.elapsed();
        longest = std::max( longest, now - last );
        last = now;
    } );
    clock.start();
    ticker.start();
    QTest::qWait( 50 );
    longest = 0;
    last = clock.elapsed();
    action();
    longest = std::max( longest, clock.elapsed() - last );
    QTest::qWait( settleMs );
    return longest;
}
} // namespace

SCENARIO( "Main window operations with several big files", "[.timing]" )
{
    const auto filtersFile = qEnvironmentVariable( "FLOGG_TIMING_FILTERS" );
    const auto logs = qEnvironmentVariable( "FLOGG_TIMING_LOGS" ).split( ',', Qt::SkipEmptyParts );
    if ( filtersFile.isEmpty() || logs.size() < 4 ) {
        return;
    }
    FilterPanel::setStartupFile( filtersFile );

    auto appSession = std::make_shared<Session>();
    WindowSession firstSession{ appSession, "Timing1", 0 };
    WindowSession secondSession{ appSession, "Timing2", 1 };
    MainWindow first( firstSession );
    MainWindow second( secondSession );
    // A file is open once per instance, the second window gets the files after the first ones
    const auto half = logs.size() / 2;
    for ( auto* window : { &first, &second } ) {
        window->resize( 1400, 900 );
        window->show();
        const auto offset = window == &first ? 0 : half;
        for ( auto i = offset; i < offset + half; ++i ) {
            window->loadInitialFile( logs[ i ], false );
        }
    }
    auto* tabs = first.findChild<TabbedCrawlerWidget*>();
    auto* secondTabs = second.findChild<TabbedCrawlerWidget*>();
    REQUIRE( waitUiState( [ & ] {
        return tabs->count() == half && secondTabs->count() == half;
    } ) );
    for ( auto* tabArea : { tabs, secondTabs } ) {
        for ( int i = 0; i < tabArea->count(); ++i ) {
            tabArea->setCurrentIndex( i );
            QTest::qWait( 8000 );
        }
    }

    // The biggest file first
    tabs->setCurrentIndex( 0 );
    QTest::qWait( 500 );
    QApplication::setActiveWindow( &first );
    auto* panel = first.findChild<FilterPanel*>();
    auto* tree = panel->findChild<QTreeWidget*>();
    auto* crawler = qobject_cast<CrawlerWidget*>( tabs->currentWidget() );
    auto views = crawler->findChildren<AbstractLogView*>();
    AbstractLogView* mainView = nullptr;
    for ( auto* view : views ) {
        if ( view->inherits( "LogMainView" ) ) {
            mainView = view;
        }
    }
    REQUIRE( mainView );

    const auto report = [ & ]( const char* name, qint64 ms ) { WARN( name << ": " << ms << " ms" ); };

    report( "switch tab", longestStallMs( [ & ] { tabs->setCurrentIndex( 1 ); }, 1000 ) );
    report( "switch back", longestStallMs( [ & ] { tabs->setCurrentIndex( 0 ); }, 1000 ) );

    report( "uncheck one filter", longestStallMs( [ & ] {
                QTreeWidgetItemIterator it( tree, QTreeWidgetItemIterator::Checked );
                ( *it )->setCheckState( 0, Qt::Unchecked );
            },
                                                  3000 ) );
    report( "check it again", longestStallMs( [ & ] {
                QTreeWidgetItemIterator it( tree, QTreeWidgetItemIterator::NotChecked );
                ( *it )->setCheckState( 0, Qt::Checked );
            },
                                              3000 ) );
    report( "disable all filters", longestStallMs(
                                       [ & ] {
                                           QMetaObject::invokeMethod( panel, "setAllEnabled",
                                                                      Q_ARG( bool, false ) );
                                       },
                                       3000 ) );
    report( "enable all filters", longestStallMs(
                                      [ & ] {
                                          QMetaObject::invokeMethod( panel, "setAllEnabled",
                                                                     Q_ARG( bool, true ) );
                                      },
                                      5000 ) );

    mainView->setFocus();
    report( "mark a line", longestStallMs( [ & ] { QTest::keyClick( mainView, Qt::Key_M ); }, 1000 ) );
    report( "page down x20", longestStallMs(
                                 [ & ] {
                                     for ( int i = 0; i < 20; ++i ) {
                                         QTest::keyClick( mainView, Qt::Key_PageDown );
                                         QApplication::processEvents();
                                     }
                                 },
                                 500 ) );
    report( "jump to bottom",
            longestStallMs( [ & ] { QTest::keyClick( mainView, Qt::Key_End, Qt::ControlModifier ); },
                            500 ) );
    report( "resize window", longestStallMs( [ & ] { first.resize( 1300, 850 ); }, 500 ) );
    report( "activate other window", longestStallMs(
                                         [ & ] {
                                             QApplication::setActiveWindow( &second );
                                             second.raise();
                                         },
                                         500 ) );
    // Saved first, a changed filter file asks before the tab is closed
    QMetaObject::invokeMethod( panel, "saveFile" );
    report( "close big tab", longestStallMs( [ & ] { tabs->tabCloseRequested( 0 ); }, 1000 ) );
    CHECK( tabs->count() == half - 1 );

    FilterPanel::setStartupFile( {} );
}

SCENARIO( "Filter panels share the filters of shared tabs", "[filters]" )
{
    QTemporaryDir dir;
    auto root = FilterNode::makeGroup( "root" );
    root.children.push_back( FilterNode::makeFilter( "ERROR" ) );
    root.children.push_back( FilterNode::makeFilter( "WARN" ) );
    const auto sharedFile = dir.filePath( "shared.flogg-filters.json" );
    REQUIRE( filters::saveToFile( root, sharedFile ) );

    auto own = FilterNode::makeGroup( "root" );
    own.children.push_back( FilterNode::makeFilter( "OWN" ) );
    const auto ownFile = dir.filePath( "own.flogg-filters.json" );
    REQUIRE( filters::saveToFile( own, ownFile ) );

    auto& settings = PersistentInfo::getSettings( app_settings{} );
    const auto previousFile = settings.value( "filterPanel/currentFile" );
    settings.setValue( "filterPanel/currentFile", sharedFile );

    auto& sharedFilters = SharedFilters::instance();
    sharedFilters.clear();

    // Two windows, each showing a tab
    QWidget firstTab;
    QWidget secondTab;
    FilterPanel first;
    FilterPanel second;
    first.setTab( &firstTab );
    second.setTab( &secondTab );
    REQUIRE( second.openFiltersFile( ownFile ) );

    // The first tab shared seeds the shared filters with its own ones, still with its file
    REQUIRE( first.setSharedMode( true ) );
    CHECK( sharedFilters.isInitialized() );
    CHECK( sharedFilters.isUser( &firstTab ) );
    CHECK( sharedFilters.state().root.children.size() == 2 );
    CHECK( first.currentFile() == sharedFile );
    CHECK_FALSE( first.isModified() );
    CHECK( sharedFilters.state().copyOfFile );

    // Their first change makes them untitled, saving them does not overwrite the tab file
    first.appendFilters( { FilterNode::makeFilter( "COPY" ) } );
    CHECK( first.currentFile().isEmpty() );
    CHECK( first.isModified() );
    CHECK_FALSE( sharedFilters.state().copyOfFile );
    CHECK( filters::loadFromFile( sharedFile )->children.size() == 2 );

    // Opened from a file instead (the untitled changes are dropped)
    REQUIRE( first.openFiltersFile( sharedFile, false ) );
    CHECK( sharedFilters.state().file == sharedFile );
    CHECK_FALSE( first.isModified() );

    // Another tab entering the shared mode shows them, with the same snapshot
    QSignalSpy secondChanged( &second, &FilterPanel::filtersChanged );
    REQUIRE( second.setSharedMode( true ) );
    CHECK( secondChanged.count() == 1 );
    CHECK( second.snapshot() == first.snapshot() );
    CHECK( second.currentFile() == sharedFile );

    // A change shows in the other panel right away, before it is saved
    const auto revision = first.snapshot()->revision;
    secondChanged.clear();
    first.appendFilters( { FilterNode::makeFilter( "NEW" ) } );
    CHECK( secondChanged.count() == 1 );
    CHECK( second.snapshot() == first.snapshot() );
    CHECK( second.snapshot()->filters.size() == 3 );
    CHECK( second.snapshot()->revision != revision );
    CHECK( second.isModified() );

    // Saving in one panel saves for both
    REQUIRE( QMetaObject::invokeMethod( &second, "saveFile" ) );
    CHECK_FALSE( second.isModified() );
    CHECK_FALSE( first.isModified() );
    CHECK_FALSE( FilterPanel::isModified( sharedFilters.state() ) );
    CHECK( filters::loadFromFile( sharedFile )->children.size() == 3 );

    // The tab state keeps the own filters of the tab aside, they come from the file just
    // saved and had no changes: they follow it
    const auto firstState = first.tabState();
    CHECK( firstState.shared );
    CHECK( firstState.root.children.size() == 3 );

    // Leaving the shared mode brings the own filters back, the other tab keeps sharing
    REQUIRE( second.setSharedMode( false ) );
    CHECK_FALSE( sharedFilters.isUser( &secondTab ) );
    CHECK( second.currentFile() == ownFile );
    REQUIRE( second.snapshot()->filters.size() == 1 );
    CHECK( second.snapshot()->filters.front().pattern == "OWN" );
    CHECK( first.snapshot()->filters.size() == 3 );

    // Another tab shown in the first panel, then the shared one again
    first.setTabState( FilterTabState{} );
    CHECK_FALSE( first.isSharedMode() );
    first.setTabState( firstState );
    CHECK( first.isSharedMode() );
    CHECK( first.snapshot()->filters.size() == 3 );

    // Users in a window being closed do not count
    CHECK( sharedFilters.hasOtherUsers( &secondTab ) );
    CHECK_FALSE( sharedFilters.hasOtherUsers( &secondTab, &firstTab ) );

    sharedFilters.clear();
    settings.setValue( "filterPanel/currentFile", previousFile );
}

SCENARIO( "Main windows run command lines", "[filters]" )
{
    QTemporaryDir dir;
    const auto log = dir.filePath( "command.log" );
    {
        QFile file( log );
        REQUIRE( file.open( QIODevice::WriteOnly ) );
        for ( int i = 1; i <= 100; ++i ) {
            file.write( QByteArray( "a line " ) + QByteArray::number( i ) + '\n' );
        }
    }

    auto& settings = PersistentInfo::getSettings( app_settings{} );
    const auto previousFile = settings.value( "filterPanel/currentFile" );
    settings.setValue( "filterPanel/currentFile", dir.filePath( "none.flogg-filters.json" ) );
    SharedFilters::instance().clear();

    auto appSession = std::make_shared<Session>();
    WindowSession firstSession{ appSession, "Command1", 0 };
    WindowSession secondSession{ appSession, "Command2", 1 };
    MainWindow first( firstSession );
    MainWindow second( secondSession );

    // Fails the test instead of waiting for an answer
    auto asked = false;
    QTimer dialogCloser;
    QObject::connect( &dialogCloser, &QTimer::timeout, [ & ] {
        if ( auto* dialog = QApplication::activeModalWidget() ) {
            asked = true;
            dialog->close();
        }
    } );
    dialogCloser.start( 200 );

    auto fileFilters = FilterNode::makeGroup( "root" );
    fileFilters.children.push_back( FilterNode::makeFilter( "line 5" ) );
    const auto filtersFile = dir.filePath( "command.flogg-filters.json" );
    REQUIRE( filters::saveToFile( fileFilters, filtersFile ) );

    // The shared filters started by the command get the filter file without a question
    WindowCommand command;
    command.files = { log };
    command.line = 42;
    command.filtersFile = filtersFile;
    command.addedFilters = { FilterNode::makeFilter( "line 4" ) };
    command.sharedFilters = true;
    first.runCommand( command );
    CHECK_FALSE( asked );

    // The same file in another window gets a tab of its own
    WindowCommand secondCommand;
    secondCommand.files = { log };
    secondCommand.sharedFilters = true;
    second.runCommand( secondCommand );

    const auto firstTabs = first.tabInfos();
    const auto secondTabs = second.tabInfos();
    REQUIRE( firstTabs.size() == 1 );
    REQUIRE( secondTabs.size() == 1 );
    CHECK( firstTabs.front().file == log );
    CHECK( firstTabs.front().sharedFilters );
    CHECK( secondTabs.front().sharedFilters );

    auto* firstPanel = first.findChild<FilterPanel*>();
    auto* secondPanel = second.findChild<FilterPanel*>();
    CHECK( firstPanel->snapshot() == secondPanel->snapshot() );
    REQUIRE( secondPanel->snapshot()->filters.size() == 2 );
    CHECK( secondPanel->snapshot()->filters.front().pattern == "line 5" );
    CHECK( secondPanel->snapshot()->filters.back().pattern == "line 4" );
    CHECK( secondPanel->currentFile() == filtersFile );

    // Both windows and their tabs show that they use the shared filters
    const auto link = QString::fromUtf8( "\xF0\x9F\x94\x97" );
    const auto hasMark = []( MainWindow& window ) {
        auto* tabBar = window.findChild<TabbedCrawlerWidget*>()->tabBar();
        return qobject_cast<QLabel*>( tabBar->tabButton( 0, QTabBar::LeftSide ) )
               || qobject_cast<QLabel*>( tabBar->tabButton( 0, QTabBar::RightSide ) );
    };
    QTest::qWait( 100 );
    CHECK( first.windowTitle().startsWith( link ) );
    CHECK( second.windowTitle().startsWith( link ) );
    CHECK( hasMark( first ) );
    CHECK( hasMark( second ) );

    REQUIRE( secondPanel->setSharedMode( false ) );
    QTest::qWait( 100 );
    CHECK_FALSE( second.windowTitle().startsWith( link ) );
    CHECK_FALSE( hasMark( second ) );
    CHECK( first.windowTitle().startsWith( link ) );
    REQUIRE( secondPanel->setSharedMode( true ) );

    // The line is selected once the file is loaded
    auto* crawler = qobject_cast<CrawlerWidget*>(
        first.findChild<TabbedCrawlerWidget*>()->currentWidget() );
    REQUIRE( waitUiState( [ & ] { return crawler->getSelectedText() == "a line 42"; } ) );

    first.setWindowName( "wifi" );
    CHECK( first.windowTitle().startsWith( link + " wifi - flogg" ) );

    // The changed shared filters are still used by the other window: closing the tab does
    // not ask to save them
    auto* firstTabArea = first.findChild<TabbedCrawlerWidget*>();
    Q_EMIT firstTabArea->tabCloseRequested( 0 );
    QTest::qWait( 500 );
    CHECK_FALSE( asked );
    CHECK( firstTabArea->count() == 0 );
    CHECK( SharedFilters::instance().hasOtherUsers( nullptr ) );

    // The last tab using them asks
    auto* secondTabArea = second.findChild<TabbedCrawlerWidget*>();
    Q_EMIT secondTabArea->tabCloseRequested( 0 );
    QTest::qWait( 500 );
    CHECK( asked );

    dialogCloser.stop();
    SharedFilters::instance().clear();
    settings.setValue( "filterPanel/currentFile", previousFile );
}

SCENARIO( "Tabs using a filter file follow its saves", "[filters]" )
{
    QTemporaryDir dir;
    auto root = FilterNode::makeGroup( "root" );
    root.children.push_back( FilterNode::makeFilter( "ERROR" ) );
    const auto filtersFile = dir.filePath( "common.flogg-filters.json" );
    REQUIRE( filters::saveToFile( root, filtersFile ) );

    auto& settings = PersistentInfo::getSettings( app_settings{} );
    const auto previousFile = settings.value( "filterPanel/currentFile" );
    settings.setValue( "filterPanel/currentFile", filtersFile );

    // Two windows on the same file, and a tab in the background without changes
    FilterPanel first;
    FilterPanel second;
    auto background = first.tabState();
    auto changedBackground = first.tabState();
    changedBackground.root.children.push_back( FilterNode::makeFilter( "MINE" ) );
    changedBackground.json.clear();

    first.appendFilters( { FilterNode::makeFilter( "WARN" ) } );
    REQUIRE( QMetaObject::invokeMethod( &first, "saveFile" ) );

    // The panel without changes shows the saved filters
    CHECK_FALSE( second.isModified() );
    REQUIRE( second.snapshot()->filters.size() == 2 );
    CHECK( second.snapshot()->filters.back().pattern == "WARN" );

    // A tab in the background takes them when shown, one with changes keeps them
    CHECK( FilterFiles::follow( background, filtersFile, first.tabState().savedJson ) );
    CHECK( background.root.children.size() == 2 );
    CHECK_FALSE( background.snapshot );
    CHECK_FALSE( FilterPanel::isModified( background ) );
    CHECK( FilterFiles::follow( changedBackground, filtersFile, first.tabState().savedJson ) );
    CHECK( changedBackground.fileChanged );
    CHECK( changedBackground.root.children.size() == 2 );

    // Changed in the second panel while the first saves again: marked, then saving asks
    second.appendFilters( { FilterNode::makeFilter( "SECOND" ) } );
    first.appendFilters( { FilterNode::makeFilter( "FIRST" ) } );
    REQUIRE( QMetaObject::invokeMethod( &first, "saveFile" ) );
    CHECK( second.tabState().fileChanged );
    CHECK( second.snapshot()->filters.size() == 3 );

    QString question;
    QTimer::singleShot( 200, [ & ] {
        if ( auto* box = qobject_cast<QMessageBox*>( QApplication::activeModalWidget() ) ) {
            question = box->text();
            for ( auto* button : box->buttons() ) {
                if ( box->buttonRole( button ) == QMessageBox::DestructiveRole ) {
                    button->click();
                }
            }
        }
    } );
    bool saved = true;
    REQUIRE( QMetaObject::invokeMethod( &second, "saveFile", Q_RETURN_ARG( bool, saved ) ) );
    CHECK_FALSE( saved );
    CHECK( question.contains( "was changed" ) );

    // Reloaded: the filters of the file, without the change
    CHECK_FALSE( second.isModified() );
    CHECK_FALSE( second.tabState().fileChanged );
    CHECK( second.snapshot()->filters.size() == 3 );
    CHECK( second.snapshot()->filters.back().pattern == "FIRST" );

    settings.setValue( "filterPanel/currentFile", previousFile );
}

// Hidden, saves a picture of the tab bar: FLOGG_RENDER_DIR=<dir> klogg_itests "[.render]"
SCENARIO( "Main window tab bar rendering", "[.render]" )
{
    const auto renderDir = qEnvironmentVariable( "FLOGG_RENDER_DIR" );
    if ( renderDir.isEmpty() ) {
        WARN( "FLOGG_RENDER_DIR is not set" );
        return;
    }

    const auto previousPalette = QApplication::palette();
    const auto previousStyle = QApplication::style()->name();
    StyleManager::applyStyle( StyleManager::LightStyleKey );

    QTemporaryDir dir;
    QStringList logs;
    for ( const auto* name : { "mid.log", "big.log", "mid2.log" } ) {
        logs.append( dir.filePath( name ) );
        QFile file( logs.back() );
        REQUIRE( file.open( QIODevice::WriteOnly ) );
        file.write( "line\n" );
    }

    auto appSession = std::make_shared<Session>();
    WindowSession windowSession{ appSession, "TabBar", 0 };
    MainWindow mainWindow( windowSession );
    mainWindow.resize( 900, 400 );
    // Painted without being shown, also with the platform style
    for ( const auto& log : logs ) {
        mainWindow.loadInitialFile( log, false );
    }
    auto* tabArea = mainWindow.findChild<TabbedCrawlerWidget*>();
    REQUIRE( waitUiState( [ & ] { return tabArea->count() == logs.size(); } ) );
    tabArea->setCurrentIndex( 0 );
    QTest::qWait( 200 );

    auto* tabBar = tabArea->tabBar();
    tabBar->show();
    REQUIRE( tabBar->grab().save( renderDir + "/tab_bar.png" ) );

    // The first tab with the shared filters
    SharedFilters::instance().clear();
    auto* panel = mainWindow.findChild<FilterPanel*>();
    REQUIRE( panel->setSharedMode( true ) );
    QTest::qWait( 200 );
    REQUIRE( tabBar->grab().save( renderDir + "/tab_bar_shared.png" ) );
    panel->resize( 320, 200 );
    REQUIRE( panel->grab().save( renderDir + "/panel_shared.png" ) );
    WARN( mainWindow.windowTitle().toStdString() );
    REQUIRE( sharedFiltersIcon( tabBar->palette().color( QPalette::Highlight ) )
                 .pixmap( QSize( 16, 16 ), 4 )
                 .save( renderDir + "/shared_icon.png" ) );
    REQUIRE( panel->setSharedMode( false ) );
    SharedFilters::instance().clear();

    QApplication::setStyle( previousStyle );
    QApplication::setPalette( previousPalette );
}

SCENARIO( "Main window shows the tab bar from the first file", "[ui]" )
{
    QTemporaryDir dir;
    const auto log = dir.filePath( "single.log" );
    {
        QFile file( log );
        REQUIRE( file.open( QIODevice::WriteOnly ) );
        file.write( "line\n" );
    }

    auto appSession = std::make_shared<Session>();
    WindowSession windowSession{ appSession, "SingleTab", 0 };
    MainWindow mainWindow( windowSession );
    auto* tabArea = mainWindow.findChild<TabbedCrawlerWidget*>();
    CHECK( tabArea->tabBar()->isHidden() );

    mainWindow.loadInitialFile( log, false );
    REQUIRE( waitUiState( [ & ] { return tabArea->count() == 1; } ) );
    CHECK_FALSE( tabArea->tabBar()->isHidden() );
}

// Also saves the main window in each style with
// FLOGG_RENDER_DIR=<dir> klogg_itests "Main window follows a style change"
SCENARIO( "Main window follows a style change", "[ui]" )
{
    const auto renderDir = qEnvironmentVariable( "FLOGG_RENDER_DIR" );
    const auto previousPalette = QApplication::palette();
    const auto previousStyle = QApplication::style()->name();

    QTemporaryDir dir;
    const auto log = dir.filePath( "styles.log" );
    {
        QFile file( log );
        REQUIRE( file.open( QIODevice::WriteOnly ) );
        for ( int i = 0; i < 40; ++i ) {
            file.write( QString( "2026-11-03 line %1 %2\n" )
                            .arg( i )
                            .arg( i % 3 ? "INFO" : "ERROR" )
                            .toUtf8() );
        }
    }

    auto appSession = std::make_shared<Session>();
    WindowSession windowSession{ appSession, "Styles", 0 };
    MainWindow mainWindow( windowSession );
    mainWindow.resize( 1000, 650 );
    mainWindow.loadInitialFile( log, false );
    auto* tabArea = mainWindow.findChild<TabbedCrawlerWidget*>();
    REQUIRE( waitUiState( [ & ] { return tabArea->count() == 1; } ) );
    QTest::qWait( 200 );

    // Each style is applied on top of the previous one, like a change in the options
    int step = 0;
    for ( const auto& style :
          { QString( StyleManager::LightStyleKey ), QString( StyleManager::DarkStyleKey ),
            StyleManager::defaultPlatformStyle(), QString( StyleManager::DarkStyleKey ),
            QString( StyleManager::FusionKey ), QString( StyleManager::LightStyleKey ) } ) {
        StyleManager::applyStyle( style );
        QTest::qWait( 200 );

        // The tab widget is skipped by Qt because of the style sheet of its tab bar
        const auto* view = mainWindow.findChild<AbstractLogView*>();
        REQUIRE( view );
        CHECK( view->viewport()->palette().color( QPalette::Base )
               == QApplication::palette().color( QPalette::Base ) );

        // Dark: the log views and the filter panel have the grey text of the terminal
        const auto* tree = mainWindow.findChild<FilterPanel*>()->findChild<QTreeWidget*>();
        const bool dark = style == QString( StyleManager::DarkStyleKey );
        const QColor viewText = dark ? QColor( "#cccccc" ) : QApplication::palette().color( QPalette::Text );
        INFO( style.toStdString() << " view "
                                  << view->viewport()->palette().color( QPalette::Text ).name().toStdString()
                                  << " tree " << tree->palette().color( QPalette::Text ).name().toStdString()
                                  << " expected " << viewText.name().toStdString() );
        CHECK( view->viewport()->palette().color( QPalette::Text ) == viewText );
        CHECK( tree->palette().color( QPalette::Text ) == viewText );
        if ( dark ) {
            CHECK( QApplication::palette().color( QPalette::Text ) == QColor( "#ffffff" ) );
            // Also where the platform theme has its own palette (macOS line edits)
            for ( const auto* edit : mainWindow.findChildren<QLineEdit*>() ) {
                CHECK( edit->palette().color( QPalette::Text ) == QColor( "#ffffff" ) );
            }
        }

        ++step;
        if ( !renderDir.isEmpty() ) {
            REQUIRE( mainWindow.grab().save(
                QString( "%1/style_%2_%3.png" ).arg( renderDir ).arg( step ).arg( style ) ) );
        }
    }

    QApplication::setStyle( previousStyle );
    QApplication::setPalette( previousPalette );
}

// Hidden, saves the View tab of the options: FLOGG_RENDER_DIR=<dir> klogg_itests "[.render]"
SCENARIO( "Options view tab rendering", "[.render]" )
{
    const auto renderDir = qEnvironmentVariable( "FLOGG_RENDER_DIR" );
    if ( renderDir.isEmpty() ) {
        WARN( "FLOGG_RENDER_DIR is not set" );
        return;
    }

    auto appSession = std::make_shared<Session>();
    WindowSession windowSession{ appSession, "Options", 0 };
    MainWindow mainWindow( windowSession );
    OptionsDialog dialog( &mainWindow );
    dialog.findChild<QTabWidget*>()->setCurrentIndex( 1 );
    dialog.show();
    QTest::qWait( 100 );
    REQUIRE( dialog.grab().save( renderDir + "/options_view.png" ) );
    dialog.findChild<QTabWidget*>()->setCurrentIndex( 0 );
    QTest::qWait( 100 );
    REQUIRE( dialog.grab().save( renderDir + "/options_general.png" ) );
}

SCENARIO( "Shared filters across windows ask only for the last tab", "[filters]" )
{
    QTemporaryDir dir;
    QStringList logs;
    for ( const auto* name : { "first.log", "second.log" } ) {
        logs.append( dir.filePath( name ) );
        QFile file( logs.back() );
        REQUIRE( file.open( QIODevice::WriteOnly ) );
        file.write( "line\n" );
    }

    auto own = FilterNode::makeGroup( "root" );
    own.children.push_back( FilterNode::makeFilter( "OWN" ) );
    const auto ownFile = dir.filePath( "own.flogg-filters.json" );
    REQUIRE( filters::saveToFile( own, ownFile ) );

    auto& settings = PersistentInfo::getSettings( app_settings{} );
    const auto previousFile = settings.value( "filterPanel/currentFile" );
    settings.setValue( "filterPanel/currentFile", ownFile );
    SharedFilters::instance().clear();

    auto appSession = std::make_shared<Session>();
    WindowSession firstSession{ appSession, "Last1", 0 };
    WindowSession secondSession{ appSession, "Last2", 1 };
    MainWindow first( firstSession );
    MainWindow second( secondSession );
    first.loadInitialFile( logs.at( 0 ), false );
    second.loadInitialFile( logs.at( 1 ), false );
    auto* firstPanel = first.findChild<FilterPanel*>();
    auto* secondPanel = second.findChild<FilterPanel*>();

    // Answers No (discard) and counts the questions
    auto questions = 0;
    QTimer dialogCloser;
    QObject::connect( &dialogCloser, &QTimer::timeout, [ & ] {
        if ( discardQuestion() ) {
            ++questions;
        }
    } );
    dialogCloser.start( 100 );

    REQUIRE( firstPanel->setSharedMode( true ) );
    REQUIRE( secondPanel->setSharedMode( true ) );
    secondPanel->appendFilters( { FilterNode::makeFilter( "SHARED" ) } );
    CHECK( firstPanel->isModified() );

    // The tab of the other window still uses them
    REQUIRE( firstPanel->setSharedMode( false ) );
    CHECK( questions == 0 );

    // The last one asks, the untitled shared filters are dropped when discarded
    REQUIRE( secondPanel->setSharedMode( false ) );
    CHECK( questions == 1 );
    CHECK_FALSE( SharedFilters::instance().isInitialized() );

    // Shared again: the filters of the tab start them again, not an empty set
    REQUIRE( firstPanel->setSharedMode( true ) );
    CHECK( questions == 1 );
    REQUIRE( firstPanel->snapshot()->filters.size() == 1 );
    CHECK( firstPanel->snapshot()->filters.front().pattern == "OWN" );

    dialogCloser.stop();
    SharedFilters::instance().clear();
    settings.setValue( "filterPanel/currentFile", previousFile );
}

SCENARIO( "Closing a window keeps the shared filters of the others", "[filters]" )
{
    QTemporaryDir dir;
    QStringList logs;
    for ( const auto* name : { "first.log", "second.log", "third.log" } ) {
        logs.append( dir.filePath( name ) );
        QFile file( logs.back() );
        REQUIRE( file.open( QIODevice::WriteOnly ) );
        file.write( "line\n" );
    }

    auto& settings = PersistentInfo::getSettings( app_settings{} );
    const auto previousFile = settings.value( "filterPanel/currentFile" );
    settings.setValue( "filterPanel/currentFile", dir.filePath( "none.flogg-filters.json" ) );
    SharedFilters::instance().clear();

    auto appSession = std::make_shared<Session>();
    WindowSession firstSession{ appSession, "Close1", 0 };
    WindowSession secondSession{ appSession, "Close2", 1 };
    MainWindow first( firstSession );
    MainWindow second( secondSession );
    first.show();
    second.show();

    auto questions = 0;
    QTimer dialogCloser;
    QObject::connect( &dialogCloser, &QTimer::timeout, [ & ] {
        if ( discardQuestion() ) {
            ++questions;
        }
    } );
    dialogCloser.start( 100 );

    // The second window shares in a tab in the background
    WindowCommand shared;
    shared.files = { logs.at( 0 ) };
    shared.sharedFilters = true;
    first.runCommand( shared );
    shared.files = { logs.at( 1 ) };
    second.runCommand( shared );
    second.runCommand( WindowCommand{ { logs.at( 2 ) } } );
    first.findChild<FilterPanel*>()->appendFilters( { FilterNode::makeFilter( "SHARED" ) } );

    CHECK( first.close() );
    QTest::qWait( 300 );
    CHECK( questions == 0 );

    CHECK( second.close() );
    QTest::qWait( 300 );
    CHECK( questions == 1 );

    dialogCloser.stop();
    SharedFilters::instance().clear();
    settings.setValue( "filterPanel/currentFile", previousFile );
}

SCENARIO( "A window without a file uses the shared filters too", "[filters]" )
{
    QTemporaryDir dir;
    const auto log = dir.filePath( "only.log" );
    {
        QFile file( log );
        REQUIRE( file.open( QIODevice::WriteOnly ) );
        file.write( "line\n" );
    }

    auto& settings = PersistentInfo::getSettings( app_settings{} );
    const auto previousFile = settings.value( "filterPanel/currentFile" );
    settings.setValue( "filterPanel/currentFile", dir.filePath( "none.flogg-filters.json" ) );
    SharedFilters::instance().clear();

    auto appSession = std::make_shared<Session>();
    WindowSession firstSession{ appSession, "Empty1", 0 };
    WindowSession secondSession{ appSession, "Empty2", 1 };
    MainWindow first( firstSession );
    MainWindow empty( secondSession );
    first.loadInitialFile( log, false );
    auto* firstPanel = first.findChild<FilterPanel*>();
    auto* emptyPanel = empty.findChild<FilterPanel*>();

    auto questions = 0;
    QTimer dialogCloser;
    QObject::connect( &dialogCloser, &QTimer::timeout, [ & ] {
        if ( discardQuestion() ) {
            ++questions;
        }
    } );
    dialogCloser.start( 100 );

    REQUIRE( firstPanel->setSharedMode( true ) );
    REQUIRE( emptyPanel->setSharedMode( true ) );
    firstPanel->appendFilters( { FilterNode::makeFilter( "SHARED" ) } );
    CHECK( emptyPanel->snapshot() == firstPanel->snapshot() );

    // The window without a file still uses them
    REQUIRE( firstPanel->setSharedMode( false ) );
    CHECK( questions == 0 );
    CHECK( SharedFilters::instance().isInitialized() );

    // Its first file takes them over
    empty.loadInitialFile( log, false );
    CHECK( SharedFilters::instance().isInitialized() );
    CHECK( emptyPanel->isSharedMode() );
    CHECK( emptyPanel->snapshot()->filters.size() == 1 );

    dialogCloser.stop();
    SharedFilters::instance().clear();
    settings.setValue( "filterPanel/currentFile", previousFile );
}

SCENARIO( "Unchanged shared filters leave without a question", "[filters]" )
{
    QTemporaryDir dir;
    auto own = FilterNode::makeGroup( "root" );
    own.children.push_back( FilterNode::makeFilter( "OWN" ) );
    const auto ownFile = dir.filePath( "own.flogg-filters.json" );
    REQUIRE( filters::saveToFile( own, ownFile ) );

    auto& settings = PersistentInfo::getSettings( app_settings{} );
    const auto previousFile = settings.value( "filterPanel/currentFile" );
    settings.setValue( "filterPanel/currentFile", ownFile );
    SharedFilters::instance().clear();

    QWidget tab;
    FilterPanel panel;
    panel.setTab( &tab );

    auto questions = 0;
    QTimer dialogCloser;
    QObject::connect( &dialogCloser, &QTimer::timeout, [ & ] {
        if ( discardQuestion() ) {
            ++questions;
        }
    } );
    dialogCloser.start( 100 );

    REQUIRE( panel.setSharedMode( true ) );
    CHECK( panel.currentFile() == ownFile );
    REQUIRE( panel.setSharedMode( false ) );
    CHECK( questions == 0 );
    CHECK( panel.currentFile() == ownFile );
    // A copy left by its last user is dropped
    CHECK_FALSE( SharedFilters::instance().isInitialized() );

    dialogCloser.stop();
    SharedFilters::instance().clear();
    settings.setValue( "filterPanel/currentFile", previousFile );
}

SCENARIO( "Shared filters with their own file are dropped by their last user", "[filters]" )
{
    QTemporaryDir dir;
    auto own = FilterNode::makeGroup( "root" );
    own.children.push_back( FilterNode::makeFilter( "OWN" ) );
    const auto ownFile = dir.filePath( "own.flogg-filters.json" );
    REQUIRE( filters::saveToFile( own, ownFile ) );
    auto other = FilterNode::makeGroup( "root" );
    other.children.push_back( FilterNode::makeFilter( "OTHER" ) );
    const auto otherFile = dir.filePath( "other.flogg-filters.json" );
    REQUIRE( filters::saveToFile( other, otherFile ) );

    auto& settings = PersistentInfo::getSettings( app_settings{} );
    const auto previousFile = settings.value( "filterPanel/currentFile" );
    settings.setValue( "filterPanel/currentFile", ownFile );
    SharedFilters::instance().clear();

    QWidget tab;
    FilterPanel panel;
    panel.setTab( &tab );

    // The shared filters get another file, then their last user leaves
    REQUIRE( panel.setSharedMode( true ) );
    REQUIRE( panel.openFiltersFile( otherFile, false ) );
    CHECK( SharedFilters::instance().state().file == otherFile );
    REQUIRE( panel.setSharedMode( false ) );
    CHECK( panel.currentFile() == ownFile );
    CHECK_FALSE( SharedFilters::instance().isInitialized() );

    // Shared again: they start from the filters of the tab, as the first time
    REQUIRE( panel.setSharedMode( true ) );
    CHECK( panel.currentFile() == ownFile );
    REQUIRE( panel.snapshot()->filters.size() == 1 );
    CHECK( panel.snapshot()->filters.front().pattern == "OWN" );

    SharedFilters::instance().clear();
    settings.setValue( "filterPanel/currentFile", previousFile );
}

SCENARIO( "Unsaved filters are taken into the shared mode and asked about on close",
          "[filters]" )
{
    QTemporaryDir dir;
    const auto log = dir.filePath( "own.log" );
    {
        QFile file( log );
        REQUIRE( file.open( QIODevice::WriteOnly ) );
        file.write( "line\n" );
    }
    auto own = FilterNode::makeGroup( "root" );
    own.children.push_back( FilterNode::makeFilter( "OWN" ) );
    const auto ownFile = dir.filePath( "own.flogg-filters.json" );
    REQUIRE( filters::saveToFile( own, ownFile ) );

    auto& settings = PersistentInfo::getSettings( app_settings{} );
    const auto previousFile = settings.value( "filterPanel/currentFile" );
    settings.setValue( "filterPanel/currentFile", ownFile );
    SharedFilters::instance().clear();

    auto appSession = std::make_shared<Session>();
    WindowSession session{ appSession, "Unsaved1", 0 };
    MainWindow window( session );
    window.show();
    window.loadInitialFile( log, false );
    auto* panel = window.findChild<FilterPanel*>();
    auto* label = panel->findChild<QLabel*>();

    // Discards the shared filters left, on close saves the own filters only and records what
    // is listed
    QStringList listed;
    auto questions = 0;
    QTimer dialogCloser;
    QObject::connect( &dialogCloser, &QTimer::timeout, [ & ] {
        auto* modal = QApplication::activeModalWidget();
        if ( !modal ) {
            return;
        }
        ++questions;
        if ( modal->objectName() != "unsavedFilters" ) {
            discardQuestion();
            return;
        }
        auto* list = modal->findChild<QTreeWidget*>();
        for ( int i = 0; i < list->topLevelItemCount(); ++i ) {
            auto* item = list->topLevelItem( i );
            listed.append( item->text( 0 ) + " | " + item->text( 1 ) );
            if ( item->text( 0 ).startsWith( "Shared" ) ) {
                item->setCheckState( 0, Qt::Unchecked );
            }
        }
        modal->findChild<QDialogButtonBox*>()->button( QDialogButtonBox::Save )->click();
    } );
    dialogCloser.start( 100 );

    // Entering the shared mode with unsaved changes does not ask, the shared filters start
    // with them (untitled) and the own ones keep them aside
    panel->appendFilters( { FilterNode::makeFilter( "EDIT" ) } );
    REQUIRE( panel->setSharedMode( true ) );
    CHECK( questions == 0 );
    CHECK( panel->currentFile().isEmpty() );
    REQUIRE( panel->snapshot()->filters.size() == 2 );
    CHECK( panel->isPrivateModified() );
    CHECK( label->toolTip().contains( "own.flogg-filters.json" ) );
    label->resize( 2000, label->height() );
    CHECK( label->text().endsWith( "own: own.flogg-filters.json *" ) );

    // A narrow panel shows the end as "...", the label does not make it wider
    label->resize( 60, label->height() );
    CHECK( label->text().endsWith( QChar( 0x2026 ) ) );
    CHECK( label->minimumSizeHint().width() == 0 );
    CHECK( filters::loadFromFile( ownFile )->children.size() == 1 );

    // Leaving asks about the shared filters only, it brings the own ones back, still unsaved,
    // and entering again does not ask either
    panel->appendFilters( { FilterNode::makeFilter( "TEMP" ) } );
    REQUIRE( panel->setSharedMode( false ) );
    CHECK( questions == 1 );
    questions = 0;
    CHECK( panel->currentFile() == ownFile );
    CHECK( panel->isModified() );
    REQUIRE( panel->snapshot()->filters.size() == 2 );
    CHECK( panel->snapshot()->filters.back().pattern == "EDIT" );
    CHECK_FALSE( label->text().contains( "own:" ) );

    REQUIRE( panel->setSharedMode( true ) );
    CHECK( questions == 0 );
    panel->appendFilters( { FilterNode::makeFilter( "TEMP" ) } );

    // Closing lists both in one question, the own filters are saved without being shown
    CHECK( window.close() );
    QTest::qWait( 300 );
    CHECK( questions == 1 );
    REQUIRE( listed.size() == 2 );
    CHECK( listed.at( 0 ) == "Shared filters | untitled" );
    CHECK( listed.at( 1 )
           == "Filters of own.log (hidden while shared) | own.flogg-filters.json" );
    const auto saved = filters::loadFromFile( ownFile );
    REQUIRE( saved );
    REQUIRE( saved->children.size() == 2 );
    CHECK( saved->children.back().pattern == "EDIT" );

    dialogCloser.stop();
    SharedFilters::instance().clear();
    settings.setValue( "filterPanel/currentFile", previousFile );
}

SCENARIO( "A file opened again gets the filters saved since", "[filters]" )
{
    QTemporaryDir dir;
    const auto log = dir.filePath( "again.log" );
    {
        QFile file( log );
        REQUIRE( file.open( QIODevice::WriteOnly ) );
        file.write( "line\n" );
    }
    auto own = FilterNode::makeGroup( "root" );
    own.children.push_back( FilterNode::makeFilter( "OWN" ) );
    const auto ownFile = dir.filePath( "again.flogg-filters.json" );
    REQUIRE( filters::saveToFile( own, ownFile ) );

    auto& settings = PersistentInfo::getSettings( app_settings{} );
    const auto previousFile = settings.value( "filterPanel/currentFile" );
    settings.setValue( "filterPanel/currentFile", ownFile );
    SharedFilters::instance().clear();

    auto questions = 0;
    QTimer dialogCloser;
    QObject::connect( &dialogCloser, &QTimer::timeout, [ & ] {
        if ( discardQuestion() ) {
            ++questions;
        }
    } );
    dialogCloser.start( 100 );

    // The session keeps the file with filters older than the file saved afterwards
    auto appSession = std::make_shared<Session>();
    {
        WindowSession session{ appSession, "Again1", 0 };
        MainWindow window( session );
        window.show();
        window.loadInitialFile( log, false );
        auto* crawler = qobject_cast<CrawlerWidget*>(
            window.findChild<TabbedCrawlerWidget*>()->currentWidget() );
        auto* panel = window.findChild<FilterPanel*>();

        // The session keeps the filters only while they have unsaved changes
        const auto keepsRoot = [ & ] {
            crawler->setFilterTabState( panel->tabState() );
            return crawler->context()->toString().contains( "OLD" );
        };
        panel->appendFilters( { FilterNode::makeFilter( "OLD" ) } );
        CHECK( keepsRoot() );

        // Discarded when the window is closed, they are not kept either (the window opened
        // below does not get them)
        CHECK( window.close() );
        QTest::qWait( 300 );
    }
    CHECK( questions == 1 );
    own.children.push_back( FilterNode::makeFilter( "SAVED" ) );
    REQUIRE( filters::saveToFile( own, ownFile ) );

    WindowSession session{ appSession, "Again2", 1 };
    MainWindow window( session );
    window.show();
    window.loadInitialFile( log, false );
    auto* panel = window.findChild<FilterPanel*>();
    CHECK( panel->currentFile() == ownFile );
    CHECK_FALSE( panel->isModified() );
    REQUIRE( panel->snapshot()->filters.size() == 2 );
    CHECK( panel->snapshot()->filters.back().pattern == "SAVED" );

    dialogCloser.stop();
    SharedFilters::instance().clear();
    settings.setValue( "filterPanel/currentFile", previousFile );
}
