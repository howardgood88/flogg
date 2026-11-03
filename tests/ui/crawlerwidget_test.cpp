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

#include <algorithm>
#include <array>

#include <QPainter>
#include <QScrollBar>
#include <QStyleFactory>
#include <QStyleOptionSlider>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QTest>
#include <QTimer>
#include <qglobal.h>
#include <qnamespace.h>
#include <qtestmouse.h>

#include "configuration.h"
#include "savedsearches.h"
#include "session.h"
#include "test_utils.h"

#include "logdata.h"
#include "logfiltereddata.h"

#include "crawlerwidget.h"
#include "filterpanel.h"
#include "infoline.h"
#include "overviewwidget.h"
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

    // Rows of the overview with matches, for an overview of the passed height
    std::vector<int> overviewMatchRows( unsigned height )
    {
        crawler->overview_.updateView( height );
        std::vector<int> rows;
        for ( const auto& line : *crawler->overview_.getMatchLines() ) {
            rows.push_back( line.position() );
        }
        return rows;
    }

    // Rows of the drawn overview with red (match) pixels
    std::vector<int> overviewRedRows()
    {
        const auto image = crawler->overviewWidget_->grab().toImage();
        std::vector<int> rows;
        for ( int y = 0; y < image.height(); ++y ) {
            for ( int x = 2; x < image.width() - 2; ++x ) {
                const auto color = image.pixelColor( x, y );
                if ( color.red() > color.green() + 40 && color.red() > color.blue() + 40 ) {
                    rows.push_back( y );
                    break;
                }
            }
        }
        return rows;
    }

    void showMainViewLine( LineNumber line )
    {
        crawler->logMainView_->selectAndDisplayLine( line );
    }

    // The search line, with some of its buttons on
    QImage grabSearchLine()
    {
        crawler->matchCaseButton_->setChecked( true );
        crawler->useRegexpButton_->setChecked( true );
        crawler->searchRefreshButton_->setChecked( false );
        auto* line = crawler->searchLineEdit_->parentWidget();
        const auto top = crawler->searchLineEdit_->mapTo( line, QPoint( 0, 0 ) ).y();
        return line->grab( QRect( 0, top - 4, line->width(), crawler->searchLineEdit_->height() + 8 ) )
            .toImage();
    }

    // A double click on the word at x on a row of the view
    void doubleClickMainView( int x, int row )
    {
        auto* viewport = crawler->logMainView_->viewport();
        const auto rowHeight = crawler->logMainView_->fontMetrics().height();
        QTest::mouseDClick( viewport, Qt::LeftButton, {},
                            QPoint( x, row * rowHeight + rowHeight / 2 ) );
    }

    LogFilteredData* filteredData()
    {
        return crawler->logFilteredData_.get();
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

    crawlerVisitor.showMainViewLine( 4_lnum );
    REQUIRE( crawlerVisitor.grabMainView().save( renderDir + "/selected_line.png" ) );
}

// Hidden, saves the selected line among filter colours in both styles:
// FLOGG_RENDER_DIR=<dir> klogg_itests "[.render]"
SCENARIO( "Crawler widget selected line rendering", "[.render]" )
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
    crawlerVisitor.crawler->resize( 700, 400 );
    waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } );

    auto yellow = FilterNode::makeFilter( "line 00000[2-4]" );
    yellow.useRegex = true;
    yellow.backColor = QColor( "#fff3a0" );
    auto red = FilterNode::makeFilter( "line 00000[6-7]" );
    red.useRegex = true;
    red.foreColor = QColor( "#c00000" );
    auto green = FilterNode::makeFilter( "line 000009" );
    green.backColor = QColor( "#2e7d32" );
    green.foreColor = QColor( Qt::white );
    crawlerVisitor.setFilters( { yellow, red, green } );

    const auto previousPalette = QApplication::palette();
    const auto previousStyle = QApplication::style()->name();
    for ( const auto& style : { StyleManager::LightStyleKey, StyleManager::DarkStyleKey } ) {
        StyleManager::applyStyle( style );
        QApplication::processEvents();
        // The selected line inside a filter colour, plain, on a filter, and a selected word
        const int rowHeight = crawlerVisitor.rowHeight();
        const QSize cropSize( 330, rowHeight * 11 );
        QImage sheet( 4 * ( cropSize.width() + 8 ), cropSize.height(), QImage::Format_RGB32 );
        sheet.fill( QApplication::palette().color( QPalette::Window ) );
        QPainter painter( &sheet );
        const auto drawView = [ & ]( int column ) {
            const auto image = crawlerVisitor.grabMainView();
            painter.drawImage(
                QPoint( column * ( cropSize.width() + 8 ), 0 ),
                image.copy( QRect( QPoint( 0, 0 ), cropSize * image.devicePixelRatio() ) )
                    .scaled( cropSize ) );
        };
        int column = 0;
        for ( const auto line : { 3_lnum, 6_lnum, 9_lnum } ) {
            crawlerVisitor.showMainViewLine( line );
            drawView( column++ );
        }
        crawlerVisitor.doubleClickMainView( 220, 3 );
        drawView( column );
        painter.end();
        REQUIRE( sheet.save( QString( "%1/selection_%2.png" ).arg( renderDir, style ) ) );
        REQUIRE( crawlerVisitor.grabSearchLine().save(
            QString( "%1/search_buttons_%2.png" ).arg( renderDir, style ) ) );
    }
    QApplication::setStyle( previousStyle );
    QApplication::setPalette( previousPalette );
}

SCENARIO( "Crawler widget selected line has the configured colours", "[ui]" )
{
    QTemporaryFile file{ "crawler_test_XXXXXX" };
    REQUIRE( generateDataFiles( file ) );

    Session session;
    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );
    crawlerVisitor.crawler->resize( 700, 400 );
    waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } );

    auto& config = Configuration::get();
    const auto previousBack = config.selectionBackColor();
    const auto previousText = config.selectionTextColor();
    const auto previousPalette = QApplication::palette();
    const auto previousStyle = QApplication::style()->name();

    const QColor back( "#7a1fa2" );
    const QColor text( "#ffeb3b" );
    config.setSelectionBackColor( back );
    config.setSelectionTextColor( text );
    StyleManager::applyStyle( StyleManager::LightStyleKey );

    // The other widgets select with the same colours
    CHECK( QApplication::palette().color( QPalette::Highlight ) == back );
    CHECK( QApplication::palette().color( QPalette::HighlightedText ) == text );

    crawlerVisitor.showMainViewLine( 2_lnum );
    const auto image = crawlerVisitor.grabMainView();
    const auto rowHeight = crawlerVisitor.rowHeight() * image.devicePixelRatio();
    int backPixels = 0;
    int textPixels = 0;
    for ( int y = static_cast<int>( 2 * rowHeight ); y < static_cast<int>( 3 * rowHeight ); ++y ) {
        for ( int x = 0; x < image.width(); ++x ) {
            const auto color = image.pixelColor( x, y );
            backPixels += color == back;
            textPixels += color == text;
        }
    }
    CHECK( backPixels > image.width() * rowHeight / 2 );
    CHECK( textPixels > 10 );

    config.setSelectionBackColor( previousBack );
    config.setSelectionTextColor( previousText );
    QApplication::setStyle( previousStyle );
    QApplication::setPalette( previousPalette );
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

SCENARIO( "Crawler widget filtered view follows the main view selection", "[ui]" )
{
    QTemporaryFile file{ "crawler_test_XXXXXX" };
    REQUIRE( generateDataFiles( file ) );

    Session session;
    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );
    waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } );

    crawlerVisitor.setFilterMode( true );
    crawlerVisitor.setFilters( { FilterNode::makeFilter( "line 00001" ) } );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.searchInfo() == "10 matches found"; } ) );

    // A line in the filtered results is selected in the filtered view too
    crawlerVisitor.showMainViewLine( 13_lnum );
    CHECK( crawlerVisitor.filteredViewSelectedText().endsWith( "line 000013" ) );

    // A line out of the results leaves the filtered selection alone
    crawlerVisitor.showMainViewLine( 50_lnum );
    CHECK( crawlerVisitor.filteredViewSelectedText().endsWith( "line 000013" ) );
    CHECK( crawlerVisitor.mainViewSelectedText().endsWith( "line 000050" ) );
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

SCENARIO( "Crawler widget overview line counts", "[ui]" )
{
    QTemporaryFile file{ "crawler_test_XXXXXX" };
    REQUIRE( generateDataFiles( file ) );

    Session session;
    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );
    waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } );

    crawlerVisitor.setFilterMode( true );
    auto filter = FilterNode::makeFilter( "line 0000[1-4][0-9]" );
    filter.useRegex = true;
    crawlerVisitor.setFilters( { filter } );
    REQUIRE( waitUiState(
        [ & ]() { return crawlerVisitor.searchInfo() == "40 matches found"; } ) );

    auto* data = crawlerVisitor.filteredData();
    // Marks inside and outside of the matches
    for ( const auto line : { 3, 12, 15, 47, 60, 61, 99 } ) {
        data->toggleMark( LineNumber( static_cast<LineNumber::UnderlyingType>( line ) ) );
    }

    // Uneven ranges, some of them empty
    const std::vector<uint64_t> bounds = { 0, 0, 7, 13, 14, 30, 45, 61, 62, 90, 100 };
    using Visibility = LogFilteredData::VisibilityFlags;
    for ( const auto visibility :
          { LogFilteredData::Visibility( Visibility::Matches ),
            LogFilteredData::Visibility( Visibility::Marks ),
            LogFilteredData::Visibility( Visibility::Matches ) | Visibility::Marks } ) {
        data->setVisibility( visibility );

        std::vector<uint64_t> expectedMatches( bounds.size() - 1 );
        std::vector<uint64_t> expectedMarks( bounds.size() - 1 );
        data->iterateOverLines( [ & ]( LineNumber line ) {
            const auto range = static_cast<size_t>(
                std::upper_bound( bounds.begin(), bounds.end(), line.get() ) - bounds.begin()
                - 1 );
            if ( data->lineTypeByLine( line ).testFlag( LogFilteredData::LineTypeFlags::Match ) ) {
                expectedMatches[ range ]++;
            }
            else {
                expectedMarks[ range ]++;
            }
        } );

        std::vector<uint64_t> matches;
        std::vector<uint64_t> marks;
        data->countLinesInRanges( bounds, matches, marks );
        CHECK( matches == expectedMatches );
        CHECK( marks == expectedMarks );
    }
}

SCENARIO( "Crawler widget overview follows the filters", "[ui]" )
{
    QTemporaryFile file{ "crawler_test_XXXXXX" };
    REQUIRE( generateDataFiles( file ) );

    Session session;
    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );
    waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } );
    crawlerVisitor.setFilterMode( true );

    // A single match is drawn
    crawlerVisitor.setFilters( { FilterNode::makeFilter( "line 000050" ) } );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.searchInfo() == "1 match found"; } ) );
    CHECK( crawlerVisitor.overviewMatchRows( 100 ) == std::vector<int>{ 50 } );

    // Another filter with as many matches
    crawlerVisitor.setFilters( { FilterNode::makeFilter( "line 000060" ) } );
    QTest::qWait( 500 );
    CHECK( crawlerVisitor.overviewMatchRows( 100 ) == std::vector<int>{ 60 } );
}

SCENARIO( "Crawler widget overview draws a single match in a big file", "[ui]" )
{
    QTemporaryFile file{ "crawler_big_XXXXXX" };
    REQUIRE( file.open() );
    constexpr int NbLines = 3'000'000;
    {
        QByteArray content;
        content.reserve( NbLines * 12 );
        for ( int i = 0; i < NbLines; ++i ) {
            content.append( i == NbLines / 2 ? "UNIQUEMARK\n" : "plain line\n" );
        }
        file.write( content );
        file.flush();
    }

    Session session;
    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } ) );
    crawlerVisitor.crawler->resize( 900, 600 );
    crawlerVisitor.crawler->show();

    crawlerVisitor.setFilterMode( true );
    crawlerVisitor.setFilters( { FilterNode::makeFilter( "UNIQUEMARK" ) } );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.searchInfo() == "1 match found"; } ) );
    QApplication::processEvents();

    // As the main window does for a new tab
    crawlerVisitor.crawler->applyConfiguration();
    QApplication::processEvents();
    auto* overviewWidget = crawlerVisitor.crawler->findChild<OverviewWidget*>();
    const auto renderDir = qEnvironmentVariable( "FLOGG_RENDER_DIR" );
    if ( !renderDir.isEmpty() ) {
        overviewWidget->grab().save( renderDir + "/overview_single_match.png" );
    }
    CHECK( crawlerVisitor.overviewMatchRows( static_cast<unsigned>( overviewWidget->height() ) )
           == std::vector<int>{ overviewWidget->height() / 2 } );

    // Away from the line marking the view, the match is drawn
    crawlerVisitor.showMainViewLine( 0_lnum );
    QApplication::processEvents();
    if ( !renderDir.isEmpty() ) {
        overviewWidget->grab().save( renderDir + "/overview_single_match_top.png" );
    }
    const auto rows = crawlerVisitor.overviewRedRows();
    INFO( "red rows: " << rows.size() );
    REQUIRE( rows.size() == 1 );
    CHECK( std::abs( rows.front() - overviewWidget->height() / 2 ) <= 1 );
}

// Every style paints the groove under the rounded handle: styles that draw the groove in pages
// (Windows, Windows Vista) used to leave the handle's area unpainted, black on Windows
TEST_CASE( "Scroll bar groove is painted under the rounded handle", "[ui]" )
{
    const auto previousPalette = QApplication::palette();
    const auto previousStyle = QApplication::style()->name();

    QStringList styles = { StyleManager::LightStyleKey, StyleManager::DarkStyleKey,
                           StyleManager::DarkWindowsStyleKey, StyleManager::FusionKey };
    for ( const auto& key : QStyleFactory::keys() ) {
        if ( key.compare( StyleManager::WindowsKey, Qt::CaseInsensitive ) == 0
             || key.compare( StyleManager::VistaKey, Qt::CaseInsensitive ) == 0 ) {
            styles << key;
        }
    }

    for ( const auto& style : styles ) {
        StyleManager::applyStyle( style );
        QApplication::processEvents();

        QScrollBar bar( Qt::Vertical );
        bar.setRange( 0, 100 );
        bar.setPageStep( 50 );
        bar.setValue( 50 );
        bar.resize( bar.sizeHint().width(), 300 );

        QImage image( bar.size(), QImage::Format_ARGB32 );
        image.fill( Qt::transparent );
        bar.render( &image, {}, {}, QWidget::DrawChildren );

        QStyleOptionSlider option;
        option.initFrom( &bar );
        option.orientation = bar.orientation();
        option.minimum = bar.minimum();
        option.maximum = bar.maximum();
        option.sliderPosition = bar.sliderPosition();
        option.sliderValue = bar.value();
        option.singleStep = bar.singleStep();
        option.pageStep = bar.pageStep();
        option.upsideDown = bar.invertedAppearance();
        option.subControls = QStyle::SC_All;
        const auto handle = bar.style()->subControlRect( QStyle::CC_ScrollBar, &option,
                                                         QStyle::SC_ScrollBarSlider, &bar );
        REQUIRE_FALSE( handle.isEmpty() );

        int unpainted = 0;
        for ( int y = handle.top(); y <= handle.bottom(); ++y ) {
            for ( int x = handle.left(); x <= handle.right(); ++x ) {
                if ( qAlpha( image.pixel( x, y ) ) == 0 ) {
                    ++unpainted;
                }
            }
        }
        INFO( "style " << style.toStdString() << ", handle " << handle.width() << "x"
                       << handle.height() );
        CHECK( unpainted == 0 );

        // Dragged: the new position painted over the old one leaves nothing of it, like
        // the backing store of Windows keeping the previous frame. Translucent pixels (the
        // rounded groove frame of Fusion) get the parent's background under them in a window.
        bar.setValue( 20 );
        QImage fresh( bar.size(), QImage::Format_ARGB32 );
        fresh.fill( Qt::transparent );
        bar.render( &fresh, {}, {}, QWidget::DrawChildren );
        bar.render( &image, {}, {}, QWidget::DrawChildren );
        int stale = 0;
        for ( int y = 0; y < fresh.height(); ++y ) {
            for ( int x = 0; x < fresh.width(); ++x ) {
                if ( qAlpha( fresh.pixel( x, y ) ) == 255
                     && image.pixel( x, y ) != fresh.pixel( x, y ) ) {
                    ++stale;
                }
            }
        }
        CHECK( stale == 0 );
    }

    StyleManager::applyStyle( previousStyle );
    QApplication::setPalette( previousPalette );
}
