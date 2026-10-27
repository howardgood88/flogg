/*
 * Copyright (C) 2016 -- 2019 Anton Filimonov and other contributors
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

#include <array>

#include <QSignalSpy>
#include <QTemporaryFile>
#include <QTest>
#include <QTimer>
#include <qglobal.h>
#include <qnamespace.h>
#include <qtestmouse.h>

#include "savedsearches.h"
#include "session.h"
#include "test_utils.h"

#include "logdata.h"
#include "logfiltereddata.h"

#include "crawlerwidget.h"
#include "filterpanel.h"
#include "infoline.h"
#include "styles.h"

#include <QToolTip>

static const qint64 SL_NB_LINES = 100LL;

namespace {
bool generateDataFiles( QTemporaryFile& file )
{
    char newLine[ 90 ];

    if ( file.open() ) {
        for ( int i = 0; i < SL_NB_LINES; i++ ) {
            snprintf( newLine, 89,
                      "LOGDATA \t is a part of glogg, we are going to test it thoroughly, this is "
                      "line %06d",
                      i );
            file.write( newLine, static_cast<qint64>( qstrlen( newLine ) ) );
#ifdef Q_OS_WIN
            file.write( "\r\n", 2 );
#else
            file.write( "\n", 1 );
#endif
        }
        file.flush();
    }

    return true;
}

} // namespace

struct CrawlerWidgetPrivate {
};

template <>
struct CrawlerWidget::access_by<CrawlerWidgetPrivate> {
    std::unique_ptr<CrawlerWidget> crawler;

    bool isLoadingFinished()
    {
        return !crawler->loadingInProgress_;
    }

    LinesCount getLogNbLines()
    {
        return crawler->logData_->getNbLine();
    }

    LinesCount getLogFilteredNbLines()
    {
        return crawler->logFilteredData_->getNbLine();
    }

    void selectAllInMainView()
    {
        crawler->logMainView_->selectAll();
    }

    void selectAllInFilteredView()
    {
        crawler->filteredView_->selectAll();
    }

    QString mainViewSelectedText()
    {
        return crawler->logMainView_->getSelectedText();
    }

    QString filteredViewSelectedText()
    {
        return crawler->filteredView_->getSelectedText();
    }

    void setSearchPattern( const QString& pattern )
    {
        QTest::keyClicks( crawler->searchLineEdit_, pattern );
    }

    void enableCaseSensitiveSearch()
    {
        if ( !crawler->matchCaseButton_->isChecked() ) {
            QTest::mouseClick( crawler->matchCaseButton_, Qt::LeftButton );
            QTest::qWait( 100 );
        }
    }

    void enableInverseMatch()
    {
        if ( !crawler->inverseButton_->isChecked() ) {
            QTest::mouseClick( crawler->inverseButton_, Qt::LeftButton );
            QTest::qWait( 100 );
        }
    }

    void enableBooleanCombinationMode()
    {
        if ( !crawler->booleanButton_->isChecked() ) {
            QTest::mouseClick( crawler->booleanButton_, Qt::LeftButton );
            QTest::qWait( 100 );
        }
    }

    void runSearch()
    {
        QTest::mouseClick( crawler->searchButton_, Qt::LeftButton );

        QTest::qWait( 100 );

        waitUiState( [ & ]() { return crawler->stopButton_->isHidden(); } );
    }

    void render()
    {
        crawler->grab();
    }

    void toggleBookmark( size_t slot, LineNumber line )
    {
        crawler->logFilteredData_->toggleBookmark( slot, line );
        crawler->logMainView_->updateData();
    }

    QImage grabMainView()
    {
        return crawler->logMainView_->grab().toImage();
    }

    void setFilters( const std::vector<FilterNode>& filterNodes )
    {
        std::vector<const FilterNode*> pointers;
        for ( const auto& filter : filterNodes ) {
            pointers.push_back( &filter );
        }
        auto snapshot = std::make_shared<FilterSnapshot>();
        snapshot->filters = filterNodes;
        snapshot->searchPattern = filters::buildSearchPattern( pointers );
        snapshot->highlighters = std::make_shared<HighlighterSet>(
            filters::buildHighlighterSet( pointers ) );
        crawler->setFilters( snapshot );
    }

    QString filterToolTip( LineNumber line )
    {
        return crawler->filterToolTip( line );
    }

    int rowHeight()
    {
        return crawler->logMainView_->fontMetrics().height();
    }

    QWidget* mainViewport()
    {
        return crawler->logMainView_->viewport();
    }

    void addFiltersToSearch( const std::vector<FilterNode>& filterNodes, FilterSearchMode mode )
    {
        crawler->addFiltersToSearch( filterNodes, mode );
    }

    void setSearchText( const QString& text )
    {
        crawler->searchLineEdit_->setEditText( text );
    }

    QString searchText()
    {
        return crawler->searchLineEdit_->currentText();
    }

    // Regex, boolean, inverse and match case buttons
    std::array<bool, 4> searchOptions()
    {
        return { crawler->useRegexpButton_->isChecked(), crawler->booleanButton_->isChecked(),
                 crawler->inverseButton_->isChecked(), crawler->matchCaseButton_->isChecked() };
    }

    void setFilterMode( bool filterMode )
    {
        crawler->setFilterMode( filterMode );
    }

    void findFilterMatch( const std::vector<FilterNode>& filterNodes )
    {
        crawler->findFilterMatch( filterNodes, false );
    }

    QString searchInfo()
    {
        return !crawler->searchInfoLine_->isHidden() ? crawler->searchInfoLine_->text()
                                                     : QString{};
    }
};

using CrawlerWidgetVisitor = CrawlerWidget::access_by<CrawlerWidgetPrivate>;

SCENARIO( "Crawler widget search", "[ui]" )
{
    QTemporaryFile file{ "crawler_test_XXXXXX" };
    REQUIRE( generateDataFiles( file ) );

    Session session;
    session.savedSearches().clear();

    REQUIRE( session.savedSearches().recentSearches().empty() );

    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );

    waitUiState( [ & ]() { return crawlerVisitor.getLogNbLines().get() == SL_NB_LINES; } );
    waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } );

    crawlerVisitor.render();

    REQUIRE( crawlerVisitor.getLogNbLines().get() == SL_NB_LINES );

    GIVEN( "loaded log data" )
    {
        THEN( "Has no lines in log view" )
        {
            REQUIRE( crawlerVisitor.getLogFilteredNbLines().get() == 0 );
        }

        WHEN( "search for lines" )
        {
            crawlerVisitor.setSearchPattern( "this is line" );
            crawlerVisitor.runSearch();

            REQUIRE( waitUiState( [ &crawlerVisitor ]() {
                return crawlerVisitor.getLogFilteredNbLines().get() == SL_NB_LINES;
            } ) );

            THEN( "all lines are matched" )
            {
                REQUIRE( crawlerVisitor.getLogFilteredNbLines().get() == SL_NB_LINES );
            }

            AND_WHEN( "copy all from main view" )
            {
                crawlerVisitor.selectAllInMainView();
                auto text = crawlerVisitor.mainViewSelectedText();
                THEN( "text has same number of lines" )
                {
                    REQUIRE( text.split( QChar::LineFeed ).size() == SL_NB_LINES );
                }
            }

            AND_WHEN( "copy all from filtered view" )
            {
                crawlerVisitor.selectAllInFilteredView();
                auto text = crawlerVisitor.filteredViewSelectedText();
                THEN( "text has same number of lines" )
                {
                    REQUIRE( text.split( QChar::LineFeed ).size() == SL_NB_LINES );
                }
            }
        }

        WHEN( "search for 10" )
        {
            crawlerVisitor.setSearchPattern( "10" );

            crawlerVisitor.runSearch();

            waitUiState( [ & ]() { return crawlerVisitor.getLogFilteredNbLines().get() == 1; } );

            THEN( "single line match" )
            {
                REQUIRE( crawlerVisitor.getLogFilteredNbLines().get() == 1 );
            }
        }

        WHEN( "case sensitive search" )
        {
            crawlerVisitor.setSearchPattern( "THIS" );
            crawlerVisitor.enableCaseSensitiveSearch();
            crawlerVisitor.runSearch();

            THEN( "no lines matched" )
            {
                REQUIRE( crawlerVisitor.getLogFilteredNbLines().get() == 0 );
            }
        }

        WHEN( "inverse match search" )
        {
            crawlerVisitor.setSearchPattern( "not match" );
            crawlerVisitor.enableInverseMatch();
            crawlerVisitor.runSearch();

            THEN( "all lines matched" )
            {
                REQUIRE( crawlerVisitor.getLogFilteredNbLines().get() == SL_NB_LINES );
            }
        }

        WHEN( "boolean search" )
        {
            crawlerVisitor.setSearchPattern( "\"glogg\" or \"klogg\"" );
            crawlerVisitor.enableBooleanCombinationMode();
            crawlerVisitor.runSearch();

            THEN( "has lines matched" )
            {
                REQUIRE( crawlerVisitor.getLogFilteredNbLines().get() >= 2 );
            }
        }
    }
}

// Hidden, saves a picture of the bookmarks area: FLOGG_RENDER_DIR=<dir> klogg_itests "[.render]"
SCENARIO( "Crawler widget bookmarks rendering", "[.render]" )
{
    const auto renderDir = qEnvironmentVariable( "FLOGG_RENDER_DIR" );
    if ( renderDir.isEmpty() ) {
        WARN( "FLOGG_RENDER_DIR is not set" );
        return;
    }

    QTemporaryFile file{ "crawler_test_XXXXXX" };
    REQUIRE( generateDataFiles( file ) );

    Session session;
    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );
    crawlerVisitor.crawler->resize( 900, 600 );

    waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } );

    // No bookmark column until a line has a bookmark
    REQUIRE( crawlerVisitor.grabMainView().save( renderDir + "/no_bookmarks.png" ) );

    crawlerVisitor.toggleBookmark( 0, 1_lnum );
    crawlerVisitor.toggleBookmark( 0, 4_lnum );
    crawlerVisitor.toggleBookmark( 2, 4_lnum );
    crawlerVisitor.toggleBookmark( 7, 4_lnum );
    crawlerVisitor.toggleBookmark( 1, 6_lnum );
    for ( size_t slot = 0; slot < LogFilteredData::NbBookmarks; ++slot ) {
        crawlerVisitor.toggleBookmark( slot, 8_lnum );
    }

    REQUIRE( crawlerVisitor.grabMainView().save( renderDir + "/bookmarks.png" ) );
}

SCENARIO( "Crawler widget filter tool tip", "[ui]" )
{
    QTemporaryFile file{ "crawler_test_XXXXXX" };
    REQUIRE( generateDataFiles( file ) );

    Session session;
    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );
    waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } );

    auto first = FilterNode::makeFilter( "line 00000[1-3]" );
    first.useRegex = true;
    first.backColor = QColor( Qt::yellow );
    first.name = "First lines\nmore text";
    auto glogg = FilterNode::makeFilter( "GLOGG" );
    auto excluded = FilterNode::makeFilter( "line 000002" );
    excluded.exclude = true;
    auto missing = FilterNode::makeFilter( "missing" );
    first.number = 1;
    glogg.number = 2;
    excluded.number = 4;
    missing.number = 5;
    crawlerVisitor.setFilters( { first, glogg, excluded, missing } );

    // Matching filters are listed in priority order
    const auto twoMatches = crawlerVisitor.filterToolTip( 2_lnum );
    CHECK( twoMatches.contains( ">#1</td>" ) );
    CHECK( twoMatches.contains( ">#4</td>" ) );
    CHECK( twoMatches.contains( "line 00000[1-3]" ) );
    CHECK( twoMatches.contains( "First lines" ) );
    CHECK_FALSE( twoMatches.contains( "more text" ) );
    CHECK( twoMatches.contains( "background-color:#ffff00" ) );
    CHECK( twoMatches.indexOf( "line 00000[1-3]" ) < twoMatches.indexOf( "GLOGG" ) );
    CHECK( twoMatches.indexOf( "GLOGG" ) < twoMatches.indexOf( "line 000002" ) );
    CHECK( twoMatches.contains( "exclude" ) );
    CHECK_FALSE( twoMatches.contains( "missing" ) );

    const auto oneMatch = crawlerVisitor.filterToolTip( 50_lnum );
    CHECK( oneMatch.contains( "GLOGG" ) );
    CHECK( oneMatch.contains( ">#2</td>" ) );
    CHECK_FALSE( oneMatch.contains( ">#1</td>" ) );

    // The tool tip shown over a row is the one of the line drawn there
    crawlerVisitor.crawler->resize( 900, 600 );
    crawlerVisitor.crawler->show();
    crawlerVisitor.crawler->grab();
    auto* viewport = crawlerVisitor.mainViewport();
    const auto rowHeight = crawlerVisitor.rowHeight();
    const auto hoveredToolTip = [ & ]( int row ) {
        QToolTip::hideText();
        const QPoint pos( 100, rowHeight * row + rowHeight / 2 );
        QHelpEvent help( QEvent::ToolTip, pos, viewport->mapToGlobal( pos ) );
        QApplication::sendEvent( viewport, &help );
        return QToolTip::text();
    };
    CHECK( hoveredToolTip( 2 ).contains( ">#4</td>" ) );
    CHECK_FALSE( hoveredToolTip( 1 ).contains( ">#4</td>" ) );
    CHECK( hoveredToolTip( 1 ).contains( ">#1</td>" ) );
    CHECK_FALSE( hoveredToolTip( 0 ).contains( ">#1</td>" ) );

    crawlerVisitor.setFilters( { missing } );
    CHECK( crawlerVisitor.filterToolTip( 2_lnum ).isEmpty() );

    // Hidden output: FLOGG_RENDER_DIR=<dir> klogg_itests "Crawler widget filter tool tip"
    const auto renderDir = qEnvironmentVariable( "FLOGG_RENDER_DIR" );
    if ( !renderDir.isEmpty() ) {
        crawlerVisitor.setFilters( { first, glogg, excluded } );
        crawlerVisitor.crawler->resize( 900, 600 );
        crawlerVisitor.crawler->show();
        QApplication::processEvents();

        auto* viewport = crawlerVisitor.mainViewport();
        const QPoint pos( 100, viewport->fontMetrics().height() * 2 + 4 );
        const auto grabToolTip = [ & ]( const QString& name ) {
            QToolTip::hideText();
            QHelpEvent help( QEvent::ToolTip, pos, viewport->mapToGlobal( pos ) );
            QApplication::sendEvent( viewport, &help );
            QApplication::processEvents();
            for ( auto* widget : QApplication::topLevelWidgets() ) {
                if ( widget->inherits( "QTipLabel" ) && widget->isVisible() ) {
                    REQUIRE( widget->grab().save( renderDir + "/" + name ) );
                }
            }
        };
        grabToolTip( "filter_tooltip.png" );

        const auto previousPalette = QApplication::palette();
        StyleManager::applyStyle( StyleManager::DarkStyleKey );
        grabToolTip( "filter_tooltip_dark.png" );
        QApplication::setPalette( previousPalette );
        QToolTip::setPalette( previousPalette );
    }
}

SCENARIO( "Crawler widget filters added to search", "[ui]" )
{
    QTemporaryFile file{ "crawler_test_XXXXXX" };
    REQUIRE( generateDataFiles( file ) );

    Session session;
    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );
    waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } );

    auto plain = FilterNode::makeFilter( "line 000010" );
    auto regex = FilterNode::makeFilter( "line 00001[0-2]" );
    regex.useRegex = true;
    auto dotted = FilterNode::makeFilter( "a.b" );
    auto caseSensitive = FilterNode::makeFilter( "Bar" );
    caseSensitive.caseSensitive = true;
    auto excluded = FilterNode::makeFilter( "glogg" );
    excluded.exclude = true;

    using Options = std::array<bool, 4>;

    // A single plain filter stays plain text
    crawlerVisitor.addFiltersToSearch( { plain }, FilterSearchMode::Replace );
    CHECK( crawlerVisitor.searchText() == "line 000010" );
    CHECK( crawlerVisitor.searchOptions() == Options{ false, false, false, false } );

    // The match case button carries the case of the filters when they agree
    crawlerVisitor.addFiltersToSearch( { caseSensitive }, FilterSearchMode::Replace );
    CHECK( crawlerVisitor.searchText() == "Bar" );
    CHECK( crawlerVisitor.searchOptions() == Options{ false, false, false, true } );

    // Several filters become a regex alternative
    crawlerVisitor.addFiltersToSearch( { regex, dotted }, FilterSearchMode::Replace );
    CHECK( crawlerVisitor.searchText() == "line 00001[0-2]|a\\.b" );
    CHECK( crawlerVisitor.searchOptions() == Options{ true, false, false, false } );

    // Added to a plain search, only a filter of another case gets an inline flag
    crawlerVisitor.addFiltersToSearch( { plain }, FilterSearchMode::Replace );
    crawlerVisitor.setSearchText( "foo.bar" );
    crawlerVisitor.addFiltersToSearch( { caseSensitive, dotted }, FilterSearchMode::Add );
    CHECK( crawlerVisitor.searchText() == "foo\\.bar|(?-i:Bar)|a\\.b" );
    CHECK( crawlerVisitor.searchOptions() == Options{ true, false, false, false } );

    // Excluded from an empty search, the search is inverted
    crawlerVisitor.setSearchText( {} );
    crawlerVisitor.addFiltersToSearch( { plain }, FilterSearchMode::Exclude );
    CHECK( crawlerVisitor.searchText() == "line 000010" );
    CHECK( crawlerVisitor.searchOptions() == Options{ false, false, true, false } );

    // Includes and excludes together need a boolean expression
    crawlerVisitor.addFiltersToSearch( { plain, excluded }, FilterSearchMode::Replace );
    CHECK( crawlerVisitor.searchText()
           == "(\"(?i:line\\ 000010)\") and not(\"(?i:glogg)\")" );
    CHECK( crawlerVisitor.searchOptions() == Options{ true, true, false, false } );
}

SCENARIO( "Crawler widget filter jump in the filtered view", "[ui]" )
{
    QTemporaryFile file{ "crawler_test_XXXXXX" };
    REQUIRE( generateDataFiles( file ) );

    Session session;
    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );
    waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } );

    auto shown = FilterNode::makeFilter( "line 00001" );
    auto notShown = FilterNode::makeFilter( "line 000050" );
    crawlerVisitor.setFilterMode( true );
    crawlerVisitor.setFilters( { shown } );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.searchInfo() == "10 matches found"; } ) );

    // Without a focused main view, the next match is selected in the filtered view
    crawlerVisitor.findFilterMatch( { shown } );
    CHECK( crawlerVisitor.filteredViewSelectedText().endsWith( "line 000011" ) );

    // A filter without cached results is searched in the filtered view
    crawlerVisitor.findFilterMatch( { notShown } );
    CHECK( waitUiState( [ & ]() {
        return crawlerVisitor.searchInfo() == "No match of the filter in the filtered view";
    } ) );
}

SCENARIO( "Crawler widget ctrl+click selection", "[ui]" )
{
    QTemporaryFile file{ "crawler_test_XXXXXX" };
    REQUIRE( generateDataFiles( file ) );

    Session session;
    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );
    waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } );

    crawlerVisitor.crawler->resize( 900, 600 );
    crawlerVisitor.crawler->show();
    crawlerVisitor.crawler->grab();
    auto* viewport = crawlerVisitor.mainViewport();
    const auto rowHeight = crawlerVisitor.rowHeight();
    const auto click = [ & ]( int row, Qt::KeyboardModifiers modifiers ) {
        QTest::mouseClick( viewport, Qt::LeftButton, modifiers,
                           QPoint( 200, rowHeight * row + rowHeight / 2 ) );
    };
    const auto selectedLines = [ & ]() {
        QStringList numbers;
        for ( const auto& line : crawlerVisitor.mainViewSelectedText().split( QChar::LineFeed ) ) {
            numbers.append( line.trimmed().right( 2 ) );
        }
        return numbers;
    };

    click( 1, Qt::NoModifier );
    click( 5, Qt::ControlModifier );
    click( 3, Qt::ControlModifier );
    CHECK( selectedLines() == QStringList{ "01", "03", "05" } );

    // Clicked again, a line leaves the selection
    click( 5, Qt::ControlModifier );
    CHECK( selectedLines() == QStringList{ "01", "03" } );

    // Shift+click selects a range from the line clicked last
    click( 6, Qt::ShiftModifier );
    CHECK( selectedLines() == QStringList{ "05", "06" } );

    // A plain click goes back to a single line
    click( 2, Qt::NoModifier );
    CHECK( selectedLines() == QStringList{ "02" } );
}
