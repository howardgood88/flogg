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
#include <QDir>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QPointer>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCodec>
#include <QTimer>

#ifdef Q_OS_MAC
#include <mach/mach.h>
#include <malloc/malloc.h>
#endif

#include "Scintilla.h"

#include "crawlerwidget.h"
#include "editview.h"
#include "loadingstatus.h"
#include "logdata.h"
#include "session.h"
#include "test_utils.h"

namespace {

QString writeFile( const QString& path, const QByteArray& content )
{
    QFile file( path );
    REQUIRE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    file.write( content );
    return path;
}

QByteArray readFile( const QString& path )
{
    QFile file( path );
    REQUIRE( file.open( QIODevice::ReadOnly ) );
    return file.readAll();
}

struct OpenedFile {
    Session session;
    std::unique_ptr<CrawlerWidget> crawler;

    explicit OpenedFile( const QString& path )
    {
        crawler.reset( static_cast<CrawlerWidget*>(
            session.open( path, []() { return new CrawlerWidget(); } ) ) );
        crawler->resize( 900, 600 );
        crawler->show();
        REQUIRE( waitUiState( [ this ] { return !crawler->isLoading(); } ) );
    }

    EditView* edit()
    {
        REQUIRE( crawler->setEditMode( true ) );
        auto* view = crawler->editView();
        REQUIRE( view );
        REQUIRE( waitUiState( [ view ] { return view->isLoaded(); } ) );
        return view;
    }
};

Scintilla::sptr_t send( EditView* view, unsigned int message, Scintilla::uptr_t wParam = 0,
                        Scintilla::sptr_t lParam = 0 )
{
    return view->editor()->send( message, wParam, lParam );
}

void insert( EditView* view, Scintilla::sptr_t position, const QByteArray& text )
{
    send( view, SCI_INSERTTEXT, static_cast<Scintilla::uptr_t>( position ),
          reinterpret_cast<Scintilla::sptr_t>( text.constData() ) );
}

// Answers the next message box
void answerNextMessageBox( QMessageBox::StandardButton button )
{
    QTimer::singleShot( 50, [ button ] {
        for ( auto* widget : QApplication::topLevelWidgets() ) {
            if ( auto* box = qobject_cast<QMessageBox*>( widget ); box && box->isVisible() ) {
                box->button( button )->click();
                return;
            }
        }
    } );
}

// Memory in use by malloc (not what it keeps for later), 0 where not measured
double heapMb()
{
#ifdef Q_OS_MAC
    malloc_statistics_t stats;
    malloc_zone_statistics( nullptr, &stats );
    return static_cast<double>( stats.size_in_use ) / ( 1024 * 1024 );
#else
    return 0;
#endif
}

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

} // namespace

SCENARIO( "Edit mode edits and saves the file", "[ui]" )
{
    // Not named like the temporary folders of flogg, whose files are read only
    QTemporaryDir dir( QDir::tempPath() + "/edit-test-XXXXXX" );
    const auto path = writeFile( dir.filePath( "app.log" ), "line 1\r\nline 2\r\nline 3\r\n" );
    OpenedFile opened( path );
    auto& crawler = *opened.crawler;

    QSignalSpy modeChanged( &crawler, &CrawlerWidget::editModeChanged );
    QSignalSpy modified( &crawler, &CrawlerWidget::editModifiedChanged );
    auto* view = opened.edit();
    CHECK( crawler.isEditMode() );
    CHECK( modeChanged.count() == 1 );
    CHECK( view->text() == "line 1\r\nline 2\r\nline 3\r\n" );
    CHECK( view->fileFormat().endOfLine == "\r\n" );
    CHECK( view->message().contains( "CRLF" ) );
    CHECK_FALSE( view->isReadOnly() );
    // Only the editor is shown
    CHECK( crawler.widget( 0 )->isHidden() );
    CHECK( crawler.widget( 1 )->isHidden() );

    insert( view, send( view, SCI_POSITIONFROMLINE, 1 ), "inserted\r\n" );
    CHECK( view->isModified() );
    CHECK( crawler.isEditModified() );
    REQUIRE( modified.count() >= 1 );
    CHECK( modified.last().at( 0 ).toBool() );

    // Saved in the background, the log view reads the file again
    REQUIRE( view->save() );
    REQUIRE( waitUiState( [ view ] { return !view->isBusy(); } ) );
    CHECK_FALSE( view->isModified() );
    CHECK( modified.last().at( 0 ).toBool() == false );
    CHECK( readFile( path ) == "line 1\r\ninserted\r\nline 2\r\nline 3\r\n" );
    // The log data read the file again
    CHECK( waitUiState(
        [ view ] { return view->logData()->getNbLine().get() == 4; } ) );

    // Not modified: left without a question
    CHECK( crawler.setEditMode( false ) );
    CHECK_FALSE( crawler.isEditMode() );
    CHECK_FALSE( crawler.widget( 0 )->isHidden() );
}

SCENARIO( "Edit mode asks before its changes are lost", "[ui]" )
{
    // Not named like the temporary folders of flogg, whose files are read only
    QTemporaryDir dir( QDir::tempPath() + "/edit-test-XXXXXX" );
    const auto path = writeFile( dir.filePath( "app.log" ), "a\nb\n" );
    OpenedFile opened( path );
    auto& crawler = *opened.crawler;
    auto* view = opened.edit();
    insert( view, 0, "changed " );

    // Cancel: still in the edit mode
    answerNextMessageBox( QMessageBox::Cancel );
    CHECK_FALSE( crawler.setEditMode( false ) );
    CHECK( crawler.isEditMode() );

    // The render mode leaves the edit mode, it asks too
    answerNextMessageBox( QMessageBox::Cancel );
    crawler.setRenderMode( true );
    CHECK_FALSE( crawler.isRenderMode() );
    CHECK( crawler.isEditMode() );

    // Save: written, then left
    answerNextMessageBox( QMessageBox::Save );
    CHECK( crawler.setEditMode( false ) );
    CHECK( readFile( path ) == "changed a\nb\n" );

    // Discard: the file stays as it is
    view = opened.edit();
    insert( view, 0, "lost " );
    answerNextMessageBox( QMessageBox::Discard );
    CHECK( crawler.confirmEditClose() );
    // Still changed: asked again when leaving
    answerNextMessageBox( QMessageBox::Discard );
    CHECK( crawler.setEditMode( false ) );
    CHECK( readFile( path ) == "changed a\nb\n" );

    // The edit mode leaves the render mode
    crawler.setRenderMode( true );
    REQUIRE( crawler.isRenderMode() );
    opened.edit();
    CHECK_FALSE( crawler.isRenderMode() );
}

SCENARIO( "Edit mode keeps the keys of the text", "[ui]" )
{
    // Not named like the temporary folders of flogg, whose files are read only
    QTemporaryDir dir( QDir::tempPath() + "/edit-test-XXXXXX" );
    OpenedFile opened( writeFile( dir.filePath( "app.log" ), "a\n" ) );
    auto* view = opened.edit();
    auto* editor = view->editor();

    const auto overridden = [ editor ]( int key, Qt::KeyboardModifiers modifiers,
                                        const QString& text = {} ) {
        QKeyEvent event( QEvent::ShortcutOverride, key, modifiers, text );
        QApplication::sendEvent( editor, &event );
        return event.isAccepted();
    };
    // Letters and digits are shortcuts of the log views (W, F, 1...), typed here
    CHECK( overridden( Qt::Key_W, Qt::NoModifier, "w" ) );
    CHECK( overridden( Qt::Key_1, Qt::NoModifier, "1" ) );
    CHECK( overridden( Qt::Key_Slash, Qt::NoModifier, "/" ) );
    CHECK( overridden( Qt::Key_Z, Qt::ControlModifier ) );
    CHECK( overridden( Qt::Key_S, Qt::ControlModifier ) );
    // Left to the window
    CHECK_FALSE( overridden( Qt::Key_F3, Qt::NoModifier ) );
    CHECK_FALSE( overridden( Qt::Key_Escape, Qt::NoModifier ) );
    CHECK_FALSE( overridden( Qt::Key_F, Qt::ControlModifier ) );

    // Ctrl+S saves
    QSignalSpy saveRequested( editor, &EditorWidget::saveRequested );
    QKeyEvent save( QEvent::KeyPress, Qt::Key_S, Qt::ControlModifier );
    QApplication::sendEvent( editor, &save );
    CHECK( saveRequested.count() == 1 );
}

SCENARIO( "Edit mode find and replace", "[ui]" )
{
    // Not named like the temporary folders of flogg, whose files are read only
    QTemporaryDir dir( QDir::tempPath() + "/edit-test-XXXXXX" );
    const auto path = writeFile( dir.filePath( "app.log" ), "error one\nok\nERROR two\nerror 3\n" );
    OpenedFile opened( path );
    auto* view = opened.edit();
    send( view, SCI_GOTOPOS, 0 );

    view->setFindText( "error" );
    view->findNext();
    CHECK( send( view, SCI_GETSELECTIONSTART ) == 0 );
    CHECK( view->findStatus() == "Line 1" );
    view->findNext();
    CHECK( view->findStatus() == "Line 3" );
    view->findNext();
    view->findNext();
    CHECK( view->findStatus() == "Line 1, wrapped" );
    view->findPrevious();
    CHECK( view->findStatus() == "Line 4, wrapped" );

    view->setFindText( "error", true );
    CHECK( view->replaceAll( "warn" ) == 2 );
    CHECK( view->text() == "warn one\nok\nERROR two\nwarn 3\n" );
    // One undo for all
    send( view, SCI_UNDO );
    CHECK( view->text() == "error one\nok\nERROR two\nerror 3\n" );

    view->setFindText( "(\\w+) (\\d)", false, true );
    CHECK( view->replaceAll( "\\2-\\1" ) == 1 );
    CHECK( view->text() == "error one\nok\nERROR two\n3-error\n" );

    view->setFindText( "nothing" );
    view->findNext();
    CHECK( view->findStatus() == "No match" );
    view->setFindText( "err(or", false, true );
    view->findNext();
    CHECK( view->findStatus() == "Invalid regular expression" );
}

SCENARIO( "Edit mode starts at the line of the log view", "[ui]" )
{
    // Not named like the temporary folders of flogg, whose files are read only
    QTemporaryDir dir( QDir::tempPath() + "/edit-test-XXXXXX" );
    QByteArray content;
    for ( int i = 0; i < 500; ++i ) {
        content += "line " + QByteArray::number( i ) + "\n";
    }
    OpenedFile opened( writeFile( dir.filePath( "app.log" ), content ) );
    opened.crawler->selectLineWhenLoaded( LineNumber( 300 ) );
    QTest::qWait( 50 );
    auto* view = opened.edit();
    CHECK( send( view, SCI_LINEFROMPOSITION, static_cast<Scintilla::uptr_t>(
                                                 send( view, SCI_GETCURRENTPOS ) ) )
           == 300 );
}

SCENARIO( "Edit mode of copies and changed files", "[ui]" )
{
    GIVEN( "A copy made by flogg" )
    {
        // The temporary folder of a window
        QTemporaryDir windowDir;
        REQUIRE( QFileInfo( windowDir.path() ).fileName().startsWith(
            QCoreApplication::applicationName() + "-" ) );
        OpenedFile opened( writeFile( windowDir.filePath( "archive.log" ), "a\n" ) );
        auto* view = opened.edit();
        CHECK( view->isReadOnly() );
        CHECK( view->message().contains( "read only" ) );
        insert( view, 0, "x" );
        CHECK( view->text() == "a\n" );
    }

    GIVEN( "A file changed by another program" )
    {
        // Not named like the temporary folders of flogg, whose files are read only
    QTemporaryDir dir( QDir::tempPath() + "/edit-test-XXXXXX" );
        const auto path = writeFile( dir.filePath( "app.log" ), "a\n" );
        OpenedFile opened( path );
        auto* view = opened.edit();
        QFile file( path );
        REQUIRE( file.open( QIODevice::Append ) );
        file.write( "b\n" );
        file.close();
        // As the watcher of the file tells it
        Q_EMIT view->logData()->fileChanged( MonitoredFileStatus::DataAdded );
        REQUIRE( waitUiState( [ view ] { return view->message().contains( "changed on disk" ); } ) );
        bool reloadShown = false;
        for ( auto* button : view->findChildren<QPushButton*>() ) {
            if ( button->text() == "Reload" ) {
                reloadShown = button->isVisible();
                button->click();
            }
        }
        CHECK( reloadShown );
        REQUIRE( waitUiState( [ view ] { return view->isLoaded() && !view->isBusy(); } ) );
        CHECK( view->text() == "a\nb\n" );
    }
}

SCENARIO( "Edit mode of a file in GBK", "[ui]" )
{
    // Not named like the temporary folders of flogg, whose files are read only
    QTemporaryDir dir( QDir::tempPath() + "/edit-test-XXXXXX" );
    auto* gbk = QTextCodec::codecForName( "GBK" );
    const auto path
        = writeFile( dir.filePath( "app.log" ), gbk->fromUnicode( QStringLiteral( "日志\n" ) ) );
    OpenedFile opened( path );
    opened.crawler->setEncoding( gbk->mibEnum() );
    auto* view = opened.edit();
    CHECK( view->text() == QStringLiteral( "日志\n" ) );
    CHECK( view->message().contains( "GBK" ) );
    insert( view, send( view, SCI_GETLENGTH ), QStringLiteral( "第二行\n" ).toUtf8() );
    REQUIRE( view->save() );
    REQUIRE( waitUiState( [ view ] { return !view->isBusy(); } ) );
    CHECK( readFile( path ) == gbk->fromUnicode( QStringLiteral( "日志\n第二行\n" ) ) );
}

// Hidden benchmark of the edit mode on a big file:
// FLOGG_BENCH_FILE=<log> klogg_itests "Scenario: Edit mode benchmark"
SCENARIO( "Edit mode benchmark", "[.benchmark]" )
{
    const auto benchFile = qEnvironmentVariable( "FLOGG_BENCH_FILE" );
    if ( benchFile.isEmpty() ) {
        WARN( "FLOGG_BENCH_FILE is not set" );
        return;
    }
    EditView::setAskBeforeLoading( false );
    // A copy: it is saved
    // Not named like the temporary folders of flogg, whose files are read only
    QTemporaryDir dir( QDir::tempPath() + "/edit-test-XXXXXX" );
    const auto path = dir.filePath( "bench.log" );
    REQUIRE( QFile::copy( benchFile, path ) );

    OpenedFile opened( path );
    QTest::qWait( 200 );
    const auto before = residentMb();
    const auto heapBefore = heapMb();

    QElapsedTimer timer;
    timer.start();
    REQUIRE( opened.crawler->setEditMode( true ) );
    auto* view = opened.crawler->editView();
    while ( !view->isLoaded() && timer.elapsed() < 600000 ) {
        QTest::qWait( 20 );
    }
    REQUIRE( view->isLoaded() );
    const auto loadMs = timer.elapsed();
    const auto loaded = residentMb();
    const auto heapLoaded = heapMb();

    // Typing in the middle, moving to the end
    timer.restart();
    const auto lines = send( view, SCI_GETLINECOUNT );
    send( view, SCI_GOTOLINE, static_cast<Scintilla::uptr_t>( lines / 2 ) );
    for ( int i = 0; i < 100; ++i ) {
        send( view, SCI_ADDTEXT, 1, reinterpret_cast<Scintilla::sptr_t>( "x" ) );
        view->editor()->viewport()->repaint();
    }
    send( view, SCI_DOCUMENTEND );
    view->editor()->viewport()->repaint();
    const auto typeMs = timer.elapsed();

    timer.restart();
    view->setFindText( "zzz-no-such-text" );
    view->findNext();
    const auto findMs = timer.elapsed();

    timer.restart();
    REQUIRE( view->save() );
    while ( view->isBusy() && timer.elapsed() < 600000 ) {
        QTest::qWait( 20 );
    }
    const auto saveMs = timer.elapsed();
    const auto saved = residentMb();
    const auto heapSaved = heapMb();
    // The log view indexed the file again after the save
    REQUIRE( waitUiState( [ &opened ] { return !opened.crawler->isLoading(); } ) );
    const auto heapReindexed = heapMb();

    QPointer<EditView> deleted( view );
    REQUIRE( opened.crawler->setEditMode( false ) );
    QCoreApplication::sendPostedEvents( nullptr, QEvent::DeferredDelete );
    QTest::qWait( 300 );
    CHECK( deleted.isNull() );
    const auto after = residentMb();
    const auto heapAfter = heapMb();
    EditView::setAskBeforeLoading( true );

    WARN( "lines " << lines << ", load " << loadMs << " ms, 100 keys and paint " << typeMs
                   << " ms, find through the file " << findMs << " ms, save " << saveMs
                   << " ms, memory " << before << " -> " << loaded << " -> " << saved << " -> "
                   << after << " MB, in use " << heapBefore << " -> " << heapLoaded << " -> "
                   << heapSaved << " -> reindexed " << heapReindexed << " -> " << heapAfter
                   << " MB" );
}
