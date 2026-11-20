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

#include <map>

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QStyleFactory>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>
#include <QTextDocument>
#include <QImage>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextFrame>
#include <QTextList>
#include <QTreeView>

#include <QElapsedTimer>

#ifdef Q_OS_MAC
#include <mach/mach.h>
#endif

#include "configuration.h"
#include "crawlerwidget.h"
#include "rendermodels.h"
#include "renderview.h"
#include "session.h"
#include "test_utils.h"

namespace {

QString writeFile( const QTemporaryDir& dir, const QString& name, const QByteArray& content )
{
    const auto path = dir.filePath( name );
    QFile file( path );
    REQUIRE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    file.write( content );
    return path;
}

void appendToFile( const QString& path, const QByteArray& content )
{
    QFile file( path );
    REQUIRE( file.open( QIODevice::WriteOnly | QIODevice::Append ) );
    file.write( content );
}

struct OpenedFile {
    Session session;
    std::unique_ptr<CrawlerWidget> crawler;

    explicit OpenedFile( const QString& path, const QString& context = {} )
    {
        crawler.reset( static_cast<CrawlerWidget*>(
            session.open( path, []() { return new CrawlerWidget(); } ) ) );
        if ( !context.isEmpty() ) {
            crawler->setViewContext( context );
        }
        crawler->resize( 900, 600 );
        crawler->show();
        REQUIRE( waitUiState( [ this ] { return !crawler->isLoading(); } ) );
    }

    RenderView* render()
    {
        crawler->setRenderMode( true );
        auto* view = crawler->renderView();
        REQUIRE( view );
        return view;
    }
};

// Resident memory in MB, 0 where it is not measured
double residentMb()
{
#ifdef Q_OS_MAC
    mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if ( task_info( mach_task_self(), MACH_TASK_BASIC_INFO,
                     reinterpret_cast<task_info_t>( &info ), &count )
         == KERN_SUCCESS ) {
        return static_cast<double>( info.resident_size ) / ( 1024 * 1024 );
    }
#endif
    return 0;
}

bool rendered( RenderView* view )
{
    return waitUiState( [ view ] { return view->isStarted() && !view->isLoadingDocument(); } );
}

} // namespace

SCENARIO( "Render mode replaces the views of the tab", "[ui]" )
{
    QTemporaryDir dir;
    const auto path = writeFile( dir, "data.json", R"({"b": [1, 2, {"c": null}], "a": "text"})" );
    OpenedFile opened( path );
    auto& crawler = *opened.crawler;

    const auto sizesBefore = crawler.sizes();
    QSignalSpy modeChanged( &crawler, &CrawlerWidget::renderModeChanged );

    auto* view = opened.render();
    CHECK( crawler.isRenderMode() );
    CHECK( modeChanged.count() == 1 );
    REQUIRE( rendered( view ) );
    CHECK( view->format() == RenderFormat::Json );
    CHECK( crawler.renderFormat() == RenderFormat::Json );
    CHECK( qobject_cast<QTreeView*>( view->currentViewer() ) );
    REQUIRE( view->treeModel() );
    CHECK( view->treeModel()->rowCount() == 2 );
    CHECK( view->message().isEmpty() );

    // Only the render view is shown
    CHECK( crawler.count() == 3 );
    CHECK( crawler.widget( 0 )->isHidden() );
    CHECK( crawler.widget( 1 )->isHidden() );
    CHECK( view->isVisible() );

    // Kept in the context of the tab, the views sizes are those from before
    const auto context = crawler.context()->toString();
    CHECK( context.contains( "\"RD\":\"json\"" ) );

    QPointer<RenderView> deleted( view );
    crawler.setRenderMode( false );
    CHECK_FALSE( crawler.isRenderMode() );
    CHECK( modeChanged.count() == 2 );
    CHECK( crawler.count() == 2 );
    CHECK_FALSE( crawler.widget( 0 )->isHidden() );
    CHECK_FALSE( crawler.widget( 1 )->isHidden() );
    // The splitter handle is laid out again, a few pixels may move
    QTest::qWait( 50 );
    const auto sizesAfter = crawler.sizes();
    REQUIRE( sizesAfter.size() == 2 );
    INFO( sizesBefore.at( 0 ) << " " << sizesBefore.at( 1 ) << " -> " << sizesAfter.at( 0 ) << " "
                              << sizesAfter.at( 1 ) );
    CHECK( std::abs( sizesAfter.at( 0 ) - sizesBefore.at( 0 ) ) <= 8 );
    CHECK( std::abs( sizesAfter.at( 1 ) - sizesBefore.at( 1 ) ) <= 8 );
    // Its memory is freed
    CHECK( waitUiState( [ &deleted ] { return deleted.isNull(); } ) );
    CHECK_FALSE( crawler.context()->toString().contains( "\"RD\"" ) );

    // A tab restored with the context comes back in the render mode
    OpenedFile restored( path, context );
    REQUIRE( restored.crawler->isRenderMode() );
    REQUIRE( rendered( restored.crawler->renderView() ) );
    CHECK( restored.crawler->renderView()->format() == RenderFormat::Json );
    CHECK( qobject_cast<QTreeView*>( restored.crawler->renderView()->currentViewer() ) );
}

SCENARIO( "Render mode keeps the main view hidden", "[ui]" )
{
    QTemporaryDir dir;
    const auto path = writeFile( dir, "notes.md", "# Title\n\nSome *text*.\n" );
    OpenedFile opened( path );
    auto& crawler = *opened.crawler;

    crawler.setMainViewVisible( false );
    opened.render();
    // Asked while rendering, done when leaving
    crawler.setMainViewVisible( true );
    CHECK( crawler.widget( 0 )->isHidden() );
    crawler.setRenderMode( false );
    CHECK_FALSE( crawler.widget( 0 )->isHidden() );

    crawler.setMainViewVisible( false );
    opened.render();
    crawler.setRenderMode( false );
    CHECK( crawler.widget( 0 )->isHidden() );
    CHECK_FALSE( crawler.widget( 1 )->isHidden() );
}

SCENARIO( "Render mode shows Markdown and HTML", "[ui]" )
{
    QTemporaryDir dir;

    GIVEN( "Markdown with a table and a diagram" )
    {
        const auto path = writeFile( dir, "README.md",
                                     "# Title\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\n"
                                     "```mermaid\ngraph TD; A-->B\n```\n\n- [x] done\n" );
        OpenedFile opened( path );
        auto* view = opened.render();
        REQUIRE( rendered( view ) );
        CHECK( view->format() == RenderFormat::Markdown );
        CHECK( qobject_cast<QTextBrowser*>( view->currentViewer() ) );
        REQUIRE( view->textDocument() );
        CHECK( view->textDocument()->toPlainText().contains( "Title" ) );
        // The text document was made in another thread, it is used here
        CHECK( view->textDocument()->thread() == view->thread() );
        // Mermaid is shown by the browser
        CHECK( view->message().contains( "browser" ) );
    }

    GIVEN( "HTML" )
    {
        const auto path = writeFile(
            dir, "page.html", "<html><body><h1>Head</h1><p>Para <b>bold</b></p></body></html>" );
        OpenedFile opened( path );
        auto* view = opened.render();
        REQUIRE( rendered( view ) );
        CHECK( view->format() == RenderFormat::Html );
        REQUIRE( view->textDocument() );
        CHECK( view->textDocument()->toPlainText().contains( "Para bold" ) );
    }
}

SCENARIO( "Render mode errors", "[ui]" )
{
    QTemporaryDir dir;

    GIVEN( "JSON with an error" )
    {
        const auto path = writeFile( dir, "broken.json", "{\n  \"a\": 1,\n  \"b\": oops\n}\n" );
        OpenedFile opened( path );
        auto* view = opened.render();
        REQUIRE( rendered( view ) );
        INFO( view->message().toStdString() );
        CHECK( view->message().contains( "line 3" ) );
        CHECK( view->treeModel() == nullptr );

        WHEN( "The line is asked for" )
        {
            QSignalSpy showLine( view, &RenderView::showLineRequested );
            auto* button = view->findChild<QPushButton*>();
            for ( auto* candidate : view->findChildren<QPushButton*>() ) {
                if ( candidate->text().contains( "log view" ) ) {
                    button = candidate;
                }
            }
            REQUIRE( button );
            CHECK( button->isVisible() );
            button->click();
            REQUIRE( showLine.count() == 1 );
            CHECK( showLine.at( 0 ).at( 0 ).toLongLong() == 2 );
            // Back to the log view
            CHECK_FALSE( opened.crawler->isRenderMode() );
        }
    }

    GIVEN( "A document larger than the limit" )
    {
        auto& config = Configuration::get();
        const auto previousLimit = config.renderSizeLimitMb();
        config.setRenderSizeLimitMb( 1 );

        QByteArray big = "[";
        while ( big.size() < 1100 * 1024 ) {
            big += "\"0123456789012345678901234567890123456789\",";
        }
        big += "0]";
        const auto path = writeFile( dir, "big.json", big );
        OpenedFile opened( path );
        auto* view = opened.render();
        REQUIRE( rendered( view ) );
        CHECK( view->message().contains( "limit" ) );
        CHECK( view->treeModel() == nullptr );

        config.setRenderSizeLimitMb( previousLimit );
    }

    GIVEN( "A file of an unknown format" )
    {
        const auto path = writeFile( dir, "app.log", "2026-01-01 INFO started\n" );
        OpenedFile opened( path );
        auto* view = opened.render();
        REQUIRE( rendered( view ) );
        CHECK( view->format() == RenderFormat::None );
        CHECK( qobject_cast<QLabel*>( view->currentViewer() ) );
    }
}

SCENARIO( "Render mode tables", "[ui]" )
{
    QTemporaryDir dir;

    GIVEN( "A CSV file with semicolons" )
    {
        const auto path = writeFile( dir, "table.csv",
                                     "name;size;comment\nalpha;1;\"a; b\"\nbeta;2;plain\n" );
        OpenedFile opened( path );
        auto* view = opened.render();
        REQUIRE( rendered( view ) );
        CHECK( view->format() == RenderFormat::Csv );
        CHECK( view->currentViewer()->findChild<QTableView*>() );
        auto* model = view->tableModel();
        REQUIRE( model );
        CHECK( model->delimiter() == ';' );
        CHECK( model->columnCount() == 3 );
        CHECK( model->headerData( 0, Qt::Horizontal ).toString() == "name" );
        REQUIRE( model->rowCount() == 2 );
        CHECK( model->index( 0, 2 ).data().toString() == "a; b" );
        // The line numbers of the file
        CHECK( model->headerData( 0, Qt::Vertical ).toString() == "2" );

        WHEN( "Lines are appended" )
        {
            appendToFile( path, "gamma;3;new\n" );
            opened.crawler->reload();
            CHECK( waitUiState( [ view ] { return view->tableModel()->rowCount() == 3; } ) );
            CHECK( view->tableModel()->index( 2, 0 ).data().toString() == "gamma" );
        }
    }

    GIVEN( "A JSON Lines file" )
    {
        const auto path = writeFile( dir, "events.jsonl",
                                     "{\"time\":\"t1\",\"level\":\"info\",\"msg\":\"start\"}\n"
                                     "{\"time\":\"t2\",\"level\":\"warn\",\"msg\":\"slow\","
                                     "\"ctx\":{\"ms\":250}}\n" );
        OpenedFile opened( path );
        auto* view = opened.render();
        REQUIRE( rendered( view ) );
        CHECK( view->format() == RenderFormat::JsonLines );
        auto* model = view->tableModel();
        REQUIRE( model );
        CHECK( model->columnCount() == 4 );
        CHECK( model->headerData( 3, Qt::Horizontal ).toString() == "ctx" );
        REQUIRE( model->rowCount() == 2 );
        CHECK( model->index( 1, 3 ).data().toString() == "{\"ms\":250}" );

    }
}

bool searched( RenderView* view )
{
    return waitUiState( [ view ] { return !view->isSearching(); } );
}

SCENARIO( "Render mode search of a document", "[ui]" )
{
    QTemporaryDir dir;
    const auto path = writeFile( dir, "notes.md",
                                 "# Errors\n\nFirst error here.\n\n| col | x |\n|---|---|\n"
                                 "| an ERROR | y |\n\nNo problem.\n" );
    OpenedFile opened( path );
    auto* view = opened.render();
    REQUIRE( rendered( view ) );
    auto* browser = qobject_cast<QTextBrowser*>( view->currentViewer() );
    REQUIRE( browser );

    view->showFindBar();
    view->setFindText( "error" );
    view->findNext();
    REQUIRE( searched( view ) );
    CHECK( view->findStatus().toStdString() == "1 / 3" );
    CHECK( browser->textCursor().selectedText().toLower() == "error" );
    // Every match shown is highlighted
    CHECK( browser->extraSelections().size() == 3 );

    view->findNext();
    CHECK( view->findStatus().toStdString() == "2 / 3" );
    view->findNext();
    view->findNext();
    // Wrapped around
    CHECK( view->findStatus().toStdString() == "1 / 3" );
    view->findPrevious();
    CHECK( view->findStatus().toStdString() == "3 / 3" );
    CHECK( browser->textCursor().selectedText() == "ERROR" );

    view->setFindText( "error", true );
    view->findNext();
    REQUIRE( searched( view ) );
    CHECK( view->findStatus().endsWith( "/ 1" ) );

    view->setFindText( "err(or", false, true );
    view->findNext();
    REQUIRE( searched( view ) );
    CHECK( view->findStatus().toStdString() == "Invalid regular expression" );

    view->setFindText( "pro.lem", false, true );
    view->findNext();
    REQUIRE( searched( view ) );
    CHECK( view->findStatus().toStdString() == "1 / 1" );

    view->setFindText( "missing" );
    view->findNext();
    REQUIRE( searched( view ) );
    CHECK( view->findStatus().toStdString() == "No match" );

    view->hideFindBar();
    CHECK( browser->extraSelections().isEmpty() );
    CHECK( view->findStatus().isEmpty() );
}

SCENARIO( "Render mode search of a tree", "[ui]" )
{
    QTemporaryDir dir;

    GIVEN( "JSON" )
    {
        const auto path = writeFile(
            dir, "data.json",
            R"({"a":{"deep":{"deeper":{"target":"found it"}}},"b":[1,2,{"x":"target"}]})" );
        OpenedFile opened( path );
        auto* view = opened.render();
        REQUIRE( rendered( view ) );
        auto* tree = qobject_cast<QTreeView*>( view->currentViewer() );
        REQUIRE( tree );

        view->setFindText( "target" );
        view->findNext();
        REQUIRE( searched( view ) );
        // The key of the first, the value of the second ("found it" does not match)
        CHECK( view->findStatus().toStdString() == "1 / 2" );
        // Expanded down to the key
        auto current = tree->currentIndex();
        CHECK( current.data().toString() == "target" );
        CHECK( tree->isExpanded( current.parent() ) );
        CHECK( static_cast<JsonTreeModel*>( view->treeModel() )->path( current )
               == "$.a.deep.deeper.target" );

        view->findNext();
        CHECK( view->findStatus().toStdString() == "2 / 2" );
        current = tree->currentIndex();
        CHECK( current.column() == JsonTreeModel::ValueColumn );
        CHECK( static_cast<JsonTreeModel*>( view->treeModel() )->path( current ) == "$.b[2].x" );
        view->findNext();
        CHECK( view->findStatus().toStdString() == "1 / 2" );
    }

    GIVEN( "XML" )
    {
        const auto path = writeFile(
            dir, "pom.xml", "<project><a><b><c name=\"needle\"/></b></a><d>needle</d></project>" );
        OpenedFile opened( path );
        auto* view = opened.render();
        REQUIRE( rendered( view ) );
        auto* tree = qobject_cast<QTreeView*>( view->currentViewer() );
        REQUIRE( tree );

        view->setFindText( "NEEDLE" );
        view->findPrevious();
        REQUIRE( searched( view ) );
        CHECK( view->findStatus().toStdString() == "2 / 2" );
        // The cell of the match: the text of <d>, then the attributes of <c>
        CHECK( tree->currentIndex().siblingAtColumn( 0 ).data().toString() == "<d>" );
        CHECK( tree->currentIndex().column() == XmlTreeModel::ValueColumn );
        view->findPrevious();
        CHECK( tree->currentIndex().siblingAtColumn( 0 ).data().toString() == "<c>" );
        // Shown like the selection of a document, the row is not selected
        CHECK( tree->selectionModel()->selectedIndexes().isEmpty() );
        CHECK( tree->isExpanded( tree->currentIndex().parent() ) );
    }
}

SCENARIO( "Render mode search of a table", "[ui]" )
{
    QTemporaryDir dir;
    const auto path = writeFile( dir, "table.csv",
                                 "id,level,message\n1,info,start\n2,error,disk\n3,info,ok\n"
                                 "4,error,net\n" );
    OpenedFile opened( path );
    RenderView* view = opened.render();
    REQUIRE( rendered( view ) );
    const auto status = [ view ] { return view->findStatus().toStdString(); };
    auto* table = view->table();
    auto* model = view->tableModel();
    REQUIRE( model );

    // The header matches too, it is not a row
    view->setFindText( "s" );
    view->findNext();
    REQUIRE( searched( view ) );
    CHECK( status() == "1 / 2" );
    CHECK( table->currentIndex().row() == 0 );

    // A new search starts after the current row
    table->setCurrentIndex( {} );
    view->setFindText( "error" );
    view->findNext();
    REQUIRE( searched( view ) );
    CHECK( status() == "1 / 2" );
    CHECK( table->currentIndex().row() == 1 );
    // The cell with the text
    CHECK( table->currentIndex().column() == 1 );
    view->findNext();
    CHECK( table->currentIndex().row() == 3 );
    CHECK( status() == "2 / 2" );
    view->findNext();
    CHECK( table->currentIndex().row() == 1 );
    view->findPrevious();
    CHECK( table->currentIndex().row() == 3 );

    // From the row the user is on
    table->setCurrentIndex( model->index( 2, 0 ) );
    view->findPrevious();
    CHECK( table->currentIndex().row() == 1 );
    table->setCurrentIndex( model->index( 2, 0 ) );
    view->findNext();
    CHECK( table->currentIndex().row() == 3 );

    view->setFindText( "n.t", false, true );
    view->findNext();
    REQUIRE( searched( view ) );
    CHECK( table->currentIndex().row() == 3 );
    CHECK( view->findStatus().toStdString() == "1 / 1" );

    // The other matches are on a yellow background, like in the documents
    {
        view->setFindText( "error" );
        view->findNext();
        REQUIRE( searched( view ) );
        const auto other = model->index( table->currentIndex().row() == 1 ? 3 : 1, 1 );
        const auto image = table->viewport()->grab( table->visualRect( other ) ).toImage();
        bool yellow = false;
        for ( int y = 0; y < image.height() && !yellow; ++y ) {
            for ( int x = 0; x < image.width() && !yellow; ++x ) {
                const auto color = image.pixelColor( x, y );
                yellow = color.red() > 200 && color.green() > 180 && color.blue() < 170;
            }
        }
        CHECK( yellow );
    }

    view->setFindText( "ERROR", true );
    view->findNext();
    REQUIRE( searched( view ) );
    CHECK( view->findStatus().toStdString() == "No match" );

    WHEN( "Lines are appended" )
    {
        view->setFindText( "error" );
        view->findNext();
        REQUIRE( searched( view ) );
        appendToFile( path, "5,error,new\n" );
        opened.crawler->reload();
        REQUIRE( waitUiState( [ view ] { return view->tableModel()->rowCount() == 5; } ) );
        REQUIRE( waitUiState( [ view, table ] {
            view->findNext();
            return table->currentIndex().row() == 4;
        } ) );
        CHECK( view->findStatus().toStdString() == "3 / 3" );
    }
}

SCENARIO( "Render mode search of JSON Lines cells", "[ui]" )
{
    QTemporaryDir dir;
    const auto path = writeFile( dir, "events.jsonl",
                                 "{\"time\":\"t1\",\"msg\":\"start\"}\n"
                                 "{\"time\":\"t2\",\"msg\":\"time out\",\"id\":\"time\"}\n"
                                 "{\"time\":\"t3\",\"msg\":\"done\"}\n" );
    OpenedFile opened( path );
    auto* view = opened.render();
    REQUIRE( rendered( view ) );
    auto* table = view->table();

    // Every line has the key "time", only the second one has it in its cells
    view->setFindText( "time" );
    view->findNext();
    REQUIRE( searched( view ) );
    CHECK( table->currentIndex().row() == 1 );
    CHECK( table->currentIndex().column() == 1 );
    CHECK( view->findStatus().toStdString() == "1 / 2" );
    // Then the next cell of the same row
    view->findNext();
    CHECK( table->currentIndex().row() == 1 );
    CHECK( table->currentIndex().column() == 2 );
    // The lines matching only by their keys are left out, around to the first cell
    view->findNext();
    CHECK( table->currentIndex().row() == 1 );
    CHECK( table->currentIndex().column() == 1 );
    view->findPrevious();
    CHECK( table->currentIndex().column() == 2 );
    CHECK( table->selectionModel()->selectedIndexes().isEmpty() );

    view->setFindText( "msg" );
    view->findNext();
    REQUIRE( searched( view ) );
    CHECK( view->findStatus().toStdString() == "No match" );
}

SCENARIO( "Render mode search counts every occurrence", "[ui]" )
{
    QTemporaryDir dir;
    const auto path = writeFile( dir, "table.tsv",
                                 "a\tb\nerror one\terror, error two\nok\tfine\nlast\terror\n" );
    OpenedFile opened( path );
    auto* view = opened.render();
    REQUIRE( rendered( view ) );
    auto* table = view->table();

    view->setFindText( "error" );
    view->findNext();
    REQUIRE( searched( view ) );
    // Four times in three cells of two rows
    CHECK( view->findStatus().toStdString() == "1 / 4" );
    CHECK( table->currentIndex() == view->tableModel()->index( 0, 0 ) );
    view->findNext();
    CHECK( view->findStatus().toStdString() == "2 / 4" );
    CHECK( table->currentIndex() == view->tableModel()->index( 0, 1 ) );
    // The second occurrence of the same cell
    view->findNext();
    CHECK( view->findStatus().toStdString() == "3 / 4" );
    CHECK( table->currentIndex() == view->tableModel()->index( 0, 1 ) );
    view->findNext();
    CHECK( view->findStatus().toStdString() == "4 / 4" );
    CHECK( table->currentIndex() == view->tableModel()->index( 2, 1 ) );
    view->findNext();
    CHECK( view->findStatus().toStdString() == "1 / 4" );
    view->findPrevious();
    view->findPrevious();
    CHECK( view->findStatus().toStdString() == "3 / 4" );
    CHECK( table->currentIndex() == view->tableModel()->index( 0, 1 ) );

    WHEN( "Lines are appended" )
    {
        appendToFile( path, "more\terror error\n" );
        opened.crawler->reload();
        REQUIRE( waitUiState( [ view ] { return view->tableModel()->rowCount() == 4; } ) );
        REQUIRE( searched( view ) );
        CHECK( waitUiState( [ view ] {
            return QString( view->findStatus() ).endsWith( "/ 6" );
        } ) );
    }
}

SCENARIO( "Render mode search starts from the selected node", "[ui]" )
{
    QTemporaryDir dir;
    const auto path
        = writeFile( dir, "data.json", R"({"a":"x","b":{"c":"x","d":"x"},"e":"x"})" );
    OpenedFile opened( path );
    auto* view = opened.render();
    REQUIRE( rendered( view ) );
    auto* tree = qobject_cast<QTreeView*>( view->currentViewer() );
    REQUIRE( tree );
    auto* model = static_cast<JsonTreeModel*>( view->treeModel() );

    // The user is on "d"
    const auto b = model->index( 1, 0 );
    tree->expand( b );
    tree->setCurrentIndex( model->index( 1, JsonTreeModel::KeyColumn, b ) );
    view->setFindText( "x" );
    view->findNext();
    REQUIRE( searched( view ) );
    CHECK( view->findStatus().toStdString() == "3 / 4" );
    CHECK( model->path( tree->currentIndex() ) == "$.b.d" );

    // Backward from "c": "a"
    tree->setCurrentIndex( model->index( 0, JsonTreeModel::KeyColumn, b ) );
    view->findPrevious();
    CHECK( view->findStatus().toStdString() == "1 / 4" );
    view->findNext();
    view->findNext();
    CHECK( view->findStatus().toStdString() == "3 / 4" );
}

SCENARIO( "Render mode table search with anchors", "[ui]" )
{
    QTemporaryDir dir;
    const auto path = writeFile( dir, "table.csv",
                                 "id,level,message\n1,info,net up\n2,error,disk\n3,info,the net\n"
                                 "4,error,net\n" );
    OpenedFile opened( path );
    RenderView* view = opened.render();
    REQUIRE( rendered( view ) );
    auto* table = view->table();
    auto* model = view->tableModel();

    // At the start of a cell, not of the line
    view->setFindText( "^net", false, true );
    view->findNext();
    REQUIRE( searched( view ) );
    CHECK( view->findStatus().toStdString() == "1 / 2" );
    CHECK( table->currentIndex() == model->index( 0, 2 ) );
    view->findNext();
    CHECK( table->currentIndex() == model->index( 3, 2 ) );
    view->findNext();
    CHECK( table->currentIndex() == model->index( 0, 2 ) );
    view->findPrevious();
    CHECK( table->currentIndex() == model->index( 3, 2 ) );
    CHECK( view->findStatus().toStdString() == "2 / 2" );

    // The end of a cell, looking around, from the top
    table->setCurrentIndex( {} );
    view->setFindText( "net$", false, true );
    view->findNext();
    REQUIRE( searched( view ) );
    CHECK( view->findStatus().toStdString() == "1 / 2" );
    CHECK( table->currentIndex() == model->index( 2, 2 ) );
    view->setFindText( "(?<=the )net", false, true );
    view->findNext();
    REQUIRE( searched( view ) );
    CHECK( view->findStatus().toStdString() == "1 / 1" );
    view->setFindText( "^nothing", false, true );
    view->findNext();
    REQUIRE( searched( view ) );
    CHECK( view->findStatus().toStdString() == "No match" );
}

SCENARIO( "Render mode shows a match cut by its column", "[ui]" )
{
    QTemporaryDir dir;
    const auto path = writeFile( dir, "table.tsv",
                                 "a\tb\nshort\t" + QByteArray( 300, 'y' ) + " needle end\n" );
    OpenedFile opened( path );
    RenderView* view = opened.render();
    REQUIRE( rendered( view ) );
    auto* table = view->table();
    table->setColumnWidth( 1, 100 );

    view->setFindText( "needle" );
    view->findNext();
    REQUIRE( searched( view ) );
    CHECK( view->findStatus().toStdString() == "1 / 1" );
    // Widened up to the match, scrolled to it
    const auto metrics = table->fontMetrics();
    CHECK( table->columnWidth( 1 )
           > metrics.horizontalAdvance( QString( 300, 'y' ) + " needle" ) );
    const auto cell = table->visualRect( view->tableModel()->index( 0, 1 ) );
    const auto matchLeft = cell.left() + metrics.horizontalAdvance( QString( 300, 'y' ) + " " );
    CHECK( matchLeft >= 0 );
    CHECK( matchLeft < table->viewport()->width() );

    // Still there once the view is laid out again
    QTest::qWait( 100 );
    const auto laidOut = table->visualRect( view->tableModel()->index( 0, 1 ) ).left()
                         + metrics.horizontalAdvance( QString( 300, 'y' ) + " " );
    CHECK( laidOut >= 0 );
    CHECK( laidOut < table->viewport()->width() );
}

SCENARIO( "Render mode find bar takes the selected text", "[ui]" )
{
    QTemporaryDir dir;

    GIVEN( "A table" )
    {
        const auto path = writeFile( dir, "table.csv", "a,b\nfirst cell,second words\n" );
        OpenedFile opened( path );
        RenderView* view = opened.render();
        REQUIRE( rendered( view ) );
        auto* table = view->table();
        auto* model = view->tableModel();

        // A cell selected
        table->setCurrentIndex( model->index( 0, 1 ) );
        view->showFindBar();
        CHECK( view->findText().toStdString() == "second words" );
        view->hideFindBar();

        // A part of a cell, in its read only editor
        table->setFocus();
        table->edit( model->index( 0, 0 ) );
        auto* editor = table->findChild<QLineEdit*>();
        REQUIRE( editor );
        CHECK( editor->isReadOnly() );
        CHECK( editor->text() == "first cell" );
        editor->setFocus();
        editor->setSelection( 6, 4 );
        REQUIRE( waitUiState( [ editor ] { return editor->hasFocus(); } ) );
        view->showFindBar();
        CHECK( view->findText().toStdString() == "cell" );

        // A whole row: its current cell
        view->hideFindBar();
        table->selectRow( 0 );
        CHECK( table->selectionModel()->isRowSelected( 0, {} ) );
    }

    GIVEN( "A tree" )
    {
        const auto path = writeFile( dir, "data.json", R"({"name":"flogg viewer"})" );
        OpenedFile opened( path );
        RenderView* view = opened.render();
        REQUIRE( rendered( view ) );
        auto* tree = qobject_cast<QTreeView*>( view->currentViewer() );
        tree->setCurrentIndex( view->treeModel()->index( 0, JsonTreeModel::ValueColumn ) );
        view->showFindBar();
        CHECK( view->findText().toStdString() == "flogg viewer" );
    }

    GIVEN( "A document" )
    {
        const auto path = writeFile( dir, "notes.md", "Some selected text\n" );
        OpenedFile opened( path );
        RenderView* view = opened.render();
        REQUIRE( rendered( view ) );
        auto* browser = qobject_cast<QTextBrowser*>( view->currentViewer() );
        auto cursor = browser->textCursor();
        cursor.setPosition( 5 );
        cursor.setPosition( 13, QTextCursor::KeepAnchor );
        browser->setTextCursor( cursor );
        view->showFindBar();
        CHECK( view->findText().toStdString() == "selected" );
    }
}

SCENARIO( "Render mode links and images of Markdown", "[ui]" )
{
    QTemporaryDir dir;
    // A wide image, in a list
    QImage wide( 3000, 300, QImage::Format_RGB32 );
    wide.fill( Qt::blue );
    REQUIRE( wide.save( dir.filePath( "wide.png" ) ) );
    QByteArray other = "# Other\n\n";
    for ( int i = 0; i < 200; ++i ) {
        other += "Line " + QByteArray::number( i ) + "\n\n";
    }
    other += "## Deep part\n\nHere.\n";
    writeFile( dir, "other.md", other );
    QByteArray markdown = "# Title\n\n[other](other.md) [section](#my-section) "
                          "[missing](nothing.md)\n\n- item  \n  ![wide](wide.png)\n\n";
    for ( int i = 0; i < 200; ++i ) {
        markdown += "Paragraph " + QByteArray::number( i ) + "\n\n";
    }
    markdown += "## My Section\n\nEnd.\n";
    const auto path = writeFile( dir, "README.md", markdown );

    OpenedFile opened( path );
    auto* view = opened.render();
    REQUIRE( rendered( view ) );
    auto* browser = qobject_cast<QTextBrowser*>( view->currentViewer() );
    REQUIRE( browser );

    // The image no wider than the view
    bool imageFound = false;
    for ( auto block = view->textDocument()->begin(); block.isValid(); block = block.next() ) {
        for ( auto it = block.begin(); !it.atEnd(); ++it ) {
            const auto format = it.fragment().charFormat();
            if ( format.isImageFormat() ) {
                imageFound = true;
#if QT_VERSION >= QT_VERSION_CHECK( 6, 8, 0 )
                const auto maximum = format.toImageFormat().maximumWidth();
                CHECK( maximum.type() == QTextLength::FixedLength );
                CHECK( maximum.rawValue() < browser->viewport()->width() );
#else
                // Sized to fit, keeping its proportions (3000 x 300)
                const auto image = format.toImageFormat();
                CHECK( image.width() > 0 );
                CHECK( image.width() < browser->viewport()->width() );
                CHECK( image.height() == Approx( image.width() / 10 ) );
#endif
            }
        }
    }
    CHECK( imageFound );

    // A file: opened in flogg
    QSignalSpy openFile( view, &RenderView::openFileRequested );
    Q_EMIT browser->anchorClicked( QUrl( "other.md" ) );
    REQUIRE( openFile.count() == 1 );
    CHECK( openFile.at( 0 ).at( 0 ).toString() == QFileInfo( dir.filePath( "other.md" ) ).absoluteFilePath() );
    CHECK( openFile.at( 0 ).at( 1 ).toString().isEmpty() );

    // A section of another file: its anchor goes with it
    Q_EMIT browser->anchorClicked( QUrl( "other.md#deep-part" ) );
    REQUIRE( openFile.count() == 2 );
    CHECK( openFile.at( 1 ).at( 1 ).toString() == "deep-part" );

    // A file that is not there
    Q_EMIT browser->anchorClicked( QUrl( "nothing.md" ) );
    CHECK( view->message().contains( "does not exist" ) );
    CHECK( openFile.count() == 2 );

    // A section of the document: its anchor made like GitHub's
    CHECK( browser->verticalScrollBar()->value() == 0 );
    Q_EMIT browser->anchorClicked( QUrl( "#my-section" ) );
    CHECK( browser->verticalScrollBar()->value() > 0 );

    // A section of this document by its name: in place, not opened again
    browser->verticalScrollBar()->setValue( 0 );
    Q_EMIT browser->anchorClicked( QUrl( "README.md#my-section" ) );
    CHECK( browser->verticalScrollBar()->value() > 0 );
    CHECK( openFile.count() == 2 );

    // The other file opened as the main window does: the section once it is shown
    OpenedFile linked( dir.filePath( "other.md" ) );
    linked.crawler->setRenderMode( true );
    auto* linkedView = linked.crawler->renderView();
    linkedView->scrollToAnchor( "deep-part" );
    REQUIRE( rendered( linkedView ) );
    auto* linkedBrowser = qobject_cast<QTextBrowser*>( linkedView->currentViewer() );
    REQUIRE( linkedBrowser );
    CHECK( waitUiState( [ linkedBrowser ] { return linkedBrowser->verticalScrollBar()->value() > 0; } ) );
}

SCENARIO( "Render mode Markdown like GitHub", "[ui]" )
{
    QTemporaryDir dir;
    QByteArray markdown = "Before<br>after, <img src=\"none.png\" width=\"10\"> kept.\n\n"
                          "> [!NOTE]\n> A note.\n\nA reference[^x].\n\n";
    for ( int i = 0; i < 200; ++i ) {
        markdown += "Filler " + QByteArray::number( i ) + "\n\n";
    }
    markdown += "Emoji \xF0\x9F\x8E\x89 end.\n\n[^x]: The note.\n";
    const auto path = writeFile( dir, "doc.md", markdown );
    OpenedFile opened( path );
    auto* view = opened.render();
    REQUIRE( rendered( view ) );
    const auto text = view->textDocument()->toPlainText();

    // Nothing lost after an element Qt does not see closed
    CHECK( text.contains( "after" ) );
    CHECK( text.contains( "kept." ) );
    // An alert of GitHub
    CHECK( text.contains( "Note" ) );
    CHECK_FALSE( text.contains( "[!NOTE]" ) );
    // UTF-8, though detected as something else for a file nearly all ASCII
    CHECK( text.contains( QStringLiteral( "Emoji \U0001F389 end." ) ) );
    // The footnote, linked both ways
    CHECK_FALSE( text.contains( "[^x]" ) );
    CHECK( text.contains( "The note." ) );
    auto* browser = qobject_cast<QTextBrowser*>( view->currentViewer() );
    REQUIRE( browser );
    Q_EMIT browser->anchorClicked( QUrl( "#fn-x" ) );
    const auto atNote = browser->verticalScrollBar()->value();
    CHECK( atNote > 0 );
    // Back up to the reference
    Q_EMIT browser->anchorClicked( QUrl( "#fnref-x" ) );
    CHECK( browser->verticalScrollBar()->value() < atNote );
}

SCENARIO( "Render mode Markdown details drawn like GitHub", "[ui]" )
{
    QTemporaryDir dir;
    const auto path = writeFile( dir, "doc.md",
                                 "# Title\n\nOne  \ntwo\\\nthree\n\n> quote\n\n"
                                 "- item\n- [x] done\n- [ ] open\n\n---\n\n```\ncode\n```\n" );
    OpenedFile opened( path );
    auto* view = opened.render();
    REQUIRE( rendered( view ) );
    auto* document = view->textDocument();

    const auto findBlock = [ document ]( const QString& text ) {
        for ( auto block = document->begin(); block.isValid(); block = block.next() ) {
            if ( block.text().startsWith( text ) ) {
                return block;
            }
        }
        return QTextBlock{};
    };
    constexpr int CodeBox = QTextFormat::UserProperty + 1;
    constexpr int Rule = QTextFormat::UserProperty + 2;
    constexpr int Task = QTextFormat::UserProperty + 3;

    // Hard breaks: one paragraph, the lines without a space before
    const auto paragraph = findBlock( "One" );
    REQUIRE( paragraph.isValid() );
    CHECK( paragraph.text()
           == QStringLiteral( "One two three" ) );

    // Tasks out of the list, a check box for each
    const auto done = findBlock( "done" );
    REQUIRE( done.isValid() );
    CHECK( done.blockFormat().intProperty( Task ) == 2 );
    CHECK( done.textList() == nullptr );
    CHECK( findBlock( "open" ).blockFormat().intProperty( Task ) == 1 );
    CHECK( findBlock( "item" ).textList() != nullptr );

    // A rule under the title, a thick one for ---
    int thin = 0;
    int thick = 0;
    for ( auto block = document->begin(); block.isValid(); block = block.next() ) {
        const auto rule = block.blockFormat().intProperty( Rule );
        thin += rule == 1;
        thick += rule == 2;
        CHECK_FALSE( block.blockFormat().hasProperty( QTextFormat::BlockTrailingHorizontalRulerWidth ) );
    }
    CHECK( thin == 1 );
    CHECK( thick == 1 );

    // The code in a box drawn rounded
    bool box = false;
    for ( auto* frame : document->rootFrame()->childFrames() ) {
        box = box || frame->frameFormat().boolProperty( CodeBox );
    }
    CHECK( box );
    CHECK( findBlock( "quote" ).blockFormat().intProperty( QTextFormat::BlockQuoteLevel ) == 1 );

    // Drawn without trouble
    REQUIRE_FALSE( view->grab().isNull() );

    // Copied from the document (the window gives Cmd+C to the view)
    auto* browser = qobject_cast<QTextBrowser*>( view->currentViewer() );
    auto cursor = browser->textCursor();
    cursor.setPosition( findBlock( "quote" ).position() );
    cursor.movePosition( QTextCursor::EndOfBlock, QTextCursor::KeepAnchor );
    browser->setTextCursor( cursor );
    QApplication::clipboard()->clear();
    view->copy();
    CHECK( QApplication::clipboard()->text() == "quote" );
}

// Hidden, saves the render views: FLOGG_RENDER_DIR=<dir> klogg_itests "[.render]"
SCENARIO( "Render mode rendering", "[.render]" )
{
    const auto renderDir = qEnvironmentVariable( "FLOGG_RENDER_DIR" );
    if ( renderDir.isEmpty() ) {
        WARN( "FLOGG_RENDER_DIR is not set" );
        return;
    }

    // A file of the user, as tall as a page: FLOGG_RENDER_FILE=<file>
    if ( const auto userFile = qEnvironmentVariable( "FLOGG_RENDER_FILE" ); !userFile.isEmpty() ) {
        OpenedFile opened( userFile );
        opened.crawler->resize( 1000, 1500 );
        auto* view = opened.render();
        REQUIRE( rendered( view ) );
        QTest::qWait( 200 );
        REQUIRE( opened.crawler->grab().save( renderDir + "/render_file.png" ) );
    }

    QTemporaryDir dir;
    const std::vector<std::pair<QString, QByteArray>> files{
        { "README.md",
          "# flogg\n\nA **fast** log viewer.\n\n## Table\n\n| Format | Kind |\n|---|---|\n"
          "| JSON | tree |\n| CSV | table |\n\n```cpp\nint main() { return 0; }\n```\n\n"
          "- [x] render mode\n- [ ] edit mode\n\n> A quote.\n" },
        { "data.json",
          R"({"name":"flogg","version":"1.3.0","tags":["log","viewer"],"build":{"qt":"6.7.3","lto":true,"warnings":null}})" },
        { "table.csv", "id,level,message\n1,info,started\n2,warn,\"slow, 250 ms\"\n3,error,failed\n" },
        { "events.jsonl", "{\"time\":\"10:00\",\"level\":\"info\",\"msg\":\"start\"}\n"
                          "{\"time\":\"10:01\",\"level\":\"warn\",\"msg\":\"slow\",\"ctx\":{\"ms\":250}}\n" },
        { "pom.xml",
          "<?xml version=\"1.0\"?>\n<project><groupId>org.flogg</groupId>"
          "<dependencies><dependency scope=\"test\"><artifactId>catch2</artifactId></dependency>"
          "</dependencies></project>\n" },
    };

    for ( const auto& file : files ) {
        const auto path = writeFile( dir, file.first, file.second );
        OpenedFile opened( path );
        auto* view = opened.render();
        REQUIRE( rendered( view ) );
        if ( auto* table = view->table(); view->tableModel() ) {
            table->setCurrentIndex( view->tableModel()->index( 1, 0 ) );
        }
        // The find bar, with the first match of a search
        view->showFindBar();
        const std::map<QString, QString> searches{ { "md", "mode" },  { "json", "lo" },
                                                   { "csv", "e" },    { "jsonl", "i" },
                                                   { "xml", "flogg" } };
        view->setFindText( searches.at( QFileInfo( file.first ).suffix() ) );
        view->findNext();
        REQUIRE( waitUiState( [ view ] { return !view->isSearching(); } ) );
        QTest::qWait( 100 );
        const auto name = QFileInfo( file.first ).suffix();
        REQUIRE( opened.crawler->grab().save( renderDir + "/render_" + name + ".png" ) );
    }
}

// Hidden benchmark of the render mode on a big file, every line a row of a table, or as
// FLOGG_BENCH_RENDER (json, markdown...):
// FLOGG_BENCH_FILE=<log> klogg_itests "Scenario: Render mode table benchmark"
SCENARIO( "Render mode table benchmark", "[.benchmark]" )
{
    const auto benchFile = qEnvironmentVariable( "FLOGG_BENCH_FILE" );
    if ( benchFile.isEmpty() ) {
        WARN( "FLOGG_BENCH_FILE is not set" );
        return;
    }

    // Shown as a TSV table whatever the file is, or as the format given
    const auto format = qEnvironmentVariable( "FLOGG_BENCH_RENDER", "tsv" );
    OpenedFile opened( benchFile, QStringLiteral( R"({"RD":"%1"})" ).arg( format ) );
    auto& crawler = *opened.crawler;
    crawler.setRenderMode( false );
    QTest::qWait( 200 );
    const auto before = residentMb();

    QElapsedTimer timer;
    timer.start();
    auto* view = opened.render();
    REQUIRE( rendered( view ) );
    QTest::qWait( 0 );
    const auto enterMs = timer.elapsed();
    INFO( view->message().toStdString() );
    auto* model = view->tableModel();
    int rows = 0;
    double scrollMs = 0;
    if ( model ) {
        rows = model->rowCount();
        timer.restart();
        for ( int i = 1; i <= 10; ++i ) {
            view->table()->scrollTo( model->index( rows / 10 * i - 1, 0 ) );
            view->table()->viewport()->repaint();
        }
        scrollMs = timer.elapsed() / 10.0;
    }
    const auto rendering = residentMb();

    // The search of the find bar, from the top
    qint64 firstMatchMs = 0;
    qint64 searchMs = 0;
    qint64 stepMs = 0;
    QString searchStatus;
    if ( const auto text = qEnvironmentVariable( "FLOGG_BENCH_SEARCH" ); !text.isEmpty() ) {
        view->table()->setCurrentIndex( {} );
        view->setFindText( text, false, !qEnvironmentVariable( "FLOGG_BENCH_REGEX" ).isEmpty() );
        timer.restart();
        view->findNext();
        // The first match is shown once the lines are searched, the count goes on
        REQUIRE( waitUiState( [ view ] {
            return !view->findStatus().startsWith( QStringLiteral( "Searching" ) );
        } ) );
        firstMatchMs = timer.elapsed();
        // The count of a big file can take longer than waitUiState waits
        while ( view->isSearching() && timer.elapsed() < 600000 ) {
            QTest::qWait( 20 );
        }
        REQUIRE_FALSE( view->isSearching() );
        searchMs = timer.elapsed();
        timer.restart();
        for ( int i = 0; i < 100; ++i ) {
            view->findNext();
        }
        stepMs = timer.elapsed();
        searchStatus = view->findStatus();
    }
    const auto searching = residentMb();

    crawler.setRenderMode( false );
    QTest::qWait( 200 );
    const auto after = residentMb();

    WARN( "rows " << rows << ", enter " << enterMs << " ms, jump and paint " << scrollMs
                  << " ms, memory " << before << " MB -> " << rendering << " MB -> " << after
                  << " MB" );
    if ( searchMs > 0 ) {
        WARN( "first match " << firstMatchMs << " ms, counted " << searchMs
                             << " ms, 100 next matches " << stepMs << " ms, at "
                        << searchStatus.toStdString() << ", memory " << searching << " MB" );
    }
}
