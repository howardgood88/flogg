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

#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "test_utils.h"

#include "loganalysis.h"
#include "logdata.h"

using namespace analysis;

namespace {
class LoadedLog {
  public:
    explicit LoadedLog( const QByteArray& content )
    {
        fileName_ = dir_.filePath( "analysis.log" );
        QFile file( fileName_ );
        REQUIRE( file.open( QIODevice::WriteOnly ) );
        file.write( content );
        file.close();

        SafeQSignalSpy loadEndSpy( &logData_, SIGNAL( loadingFinished( LoadingStatus ) ) );
        logData_.attachFile( fileName_ );
        REQUIRE( loadEndSpy.safeWait( 10000 ) );
    }

    const LogData& data() const
    {
        return logData_;
    }

  private:
    QTemporaryDir dir_;
    QString fileName_;
    LogData logData_;
};

std::vector<uint64_t> lines( const Finding& finding )
{
    std::vector<uint64_t> numbers;
    for ( const auto& line : finding.lines ) {
        numbers.push_back( line.get() );
    }
    return numbers;
}
} // namespace

SCENARIO( "Durations of paired events", "[analysis]" )
{
    const LoadedLog log( "2026-11-22 10:00:00.000 begin request id=1\n"   // 0
                         "2026-11-22 10:00:00.100 begin request id=2\n"   // 1
                         "2026-11-22 10:00:00.200 noise\n"                // 2
                         "2026-11-22 10:00:00.600 end request id=2\n"     // 3
                         "2026-11-22 10:00:03.000 end request id=1\n"     // 4
                         "2026-11-22 10:00:04.000 end request id=9\n"     // 5
                         "2026-11-22 10:00:05.000 begin request id=3\n"   // 6
                         "end request id=3 without time\n"                // 7
                         "2026-11-22 10:00:06.000 begin request id=4\n"   // 8
                         "2026-11-22 10:00:05.500 end request id=4\n"     // 9
                         "2026-11-22 10:00:07.000 begin request id=5\n" ); // 10

    AtomicFlag cancel;
    int lastProgress = 0;
    const auto result = measureDurations(
        log.data(), { "begin request id=(\\d+)", "end request id=(\\d+)", false }, cancel,
        [ &lastProgress ]( int percent ) { lastProgress = percent; } );

    REQUIRE( result.error.isEmpty() );
    CHECK( !result.cancelled );
    CHECK( lastProgress >= 70 );
    CHECK( result.pairs == 2 );
    CHECK( result.unmatchedEnds == 1 );
    CHECK( result.withoutTime == 1 );
    CHECK( result.negative == 1 );
    CHECK( result.openStarts == 1 );
    REQUIRE( result.openStartLines.size() == 1 );
    CHECK( result.openStartLines[ 0 ] == 10_lnum );

    REQUIRE( result.slowest.size() == 2 );
    CHECK( result.slowest[ 0 ].us == 3000000 );
    CHECK( result.slowest[ 0 ].start == 0_lnum );
    CHECK( result.slowest[ 0 ].end == 4_lnum );
    CHECK( result.slowest[ 0 ].key == "1" );
    CHECK( result.slowest[ 1 ].us == 500000 );
    CHECK( result.minUs == 500000 );
    CHECK( result.maxUs == 3000000 );
    CHECK( result.meanUs == Approx( 1750000 ) );
    CHECK( result.fractionDigits == 3 );

    // Without groups, an end closes the oldest start
    const auto unkeyed
        = measureDurations( log.data(), { "begin request", "end request", false }, cancel, {} );
    CHECK( unkeyed.pairs == 2 );
    CHECK( unkeyed.slowest[ 0 ].us == 2900000 );
    CHECK( unkeyed.slowest[ 0 ].start == 1_lnum );

    // Errors
    CHECK( !measureDurations( log.data(), { "(", "end", false }, cancel, {} ).error.isEmpty() );
    CHECK( !measureDurations( log.data(), { "", "end", false }, cancel, {} ).error.isEmpty() );
}

SCENARIO( "Rules", "[analysis]" )
{
    const LoadedLog log( "2026-11-22 10:00:00 connect start id=1\n"       // 0
                         "2026-11-22 10:00:01 connect start id=2\n"       // 1
                         "2026-11-22 10:00:02 connected id=2\n"           // 2
                         "2026-11-22 10:00:03 retry request=a\n"          // 3
                         "2026-11-22 10:00:04 retry request=b\n"          // 4
                         "2026-11-22 10:00:05 retry request=a\n"          // 5
                         "2026-11-22 10:00:10 retry request=a\n"          // 6
                         "2026-11-22 10:00:11 login user=amy\n"           // 7
                         "2026-11-22 10:00:11 login user=bob\n"           // 8
                         "2026-11-22 10:00:12 logout user=amy\n"          // 9
                         "2026-11-22 10:00:20 logout user=bob\n"          // 10
                         "2026-11-22 10:00:30 FATAL EXCEPTION in main\n"  // 11
                         "2026-11-22 10:00:31 connect start id=3\n" );    // 12

    const auto set = parseRules( exampleRules() );
    REQUIRE( set.error.isEmpty() );
    REQUIRE( set.rules.size() == 4 );

    AtomicFlag cancel;
    const auto result = runRules( log.data(), set.rules, cancel, {} );
    REQUIRE( result.error.isEmpty() );

    std::map<size_t, std::vector<const Finding*>> byRule;
    for ( const auto& finding : result.findings ) {
        byRule[ finding.rule ].push_back( &finding );
    }

    // Crash
    REQUIRE( byRule[ 0 ].size() == 1 );
    CHECK( byRule[ 0 ][ 0 ]->line == 11_lnum );

    // Connection 1 never connected within 5 s; 3 is at the end of the file
    REQUIRE( byRule[ 1 ].size() == 2 );
    CHECK( byRule[ 1 ][ 0 ]->line == 0_lnum );
    CHECK( byRule[ 1 ][ 0 ]->detail.contains( "within 5 s" ) );
    CHECK( byRule[ 1 ][ 1 ]->line == 12_lnum );
    CHECK( byRule[ 1 ][ 1 ]->detail.contains( "end of the file" ) );

    // Request a retried 3 times within 60 s, b only once
    REQUIRE( byRule[ 2 ].size() == 1 );
    CHECK( lines( *byRule[ 2 ][ 0 ] ) == std::vector<uint64_t>{ 3, 5, 6 } );
    CHECK( byRule[ 2 ][ 0 ]->detail.contains( "3 times" ) );

    // amy logged out 1 s after logging in, bob 9 s after
    REQUIRE( byRule[ 3 ].size() == 1 );
    CHECK( lines( *byRule[ 3 ][ 0 ] ) == std::vector<uint64_t>{ 7, 9 } );

    // Rule files with mistakes
    CHECK( !parseRules( "not json" ).error.isEmpty() );
    CHECK( parseRules( R"({"rules":[{"type":"bogus","patterns":["a"]}]})" )
               .error.contains( "unknown type" ) );
    CHECK( parseRules( R"({"rules":[{"type":"absence","patterns":["a"]}]})" )
               .error.contains( "pattern" ) );
    CHECK( parseRules( R"({"rules":[{"type":"match","patterns":["("]}]})" )
               .error.contains( "invalid" ) );
    CHECK( !parseRules( R"({"rules":[]})" ).error.isEmpty() );
    // A single "pattern" is accepted
    CHECK( parseRules( R"({"rules":[{"name":"x","pattern":"a"}]})" ).rules.size() == 1 );
}

SCENARIO( "Analysis cancelled", "[analysis]" )
{
    QByteArray content;
    for ( int i = 0; i < 20000; ++i ) {
        content += "2026-11-22 10:00:00 begin " + QByteArray::number( i ) + "\n";
    }
    const LoadedLog log( content );
    AtomicFlag cancel;
    cancel.set();
    const auto result = measureDurations( log.data(), { "begin", "end", false }, cancel, {} );
    CHECK( result.cancelled );
    CHECK( result.pairs == 0 );
}

#include "logtemplates.h"

TEST_CASE( "Templates of lines", "[templates]" )
{
    using templates::maskLine;
    CHECK( maskLine( u"2026-11-22 14:32:05.120 INFO Connection 10.0.0.5:443 timed out after "
                     u"5023 ms" )
           == "INFO Connection <NUM> timed out after <NUM> ms" );
    CHECK( maskLine( u"11-22 14:32:05.120  1234  1240 I ActivityManager: Start proc "
                     u"12345:com.example/u0a123 for activity" )
           == "<NUM> <NUM> I ActivityManager: Start proc <NUM>:com.example/u<NUM>a<NUM> for "
              "activity" );
    CHECK( maskLine( u"[ 12.345678] usb 1-1: new device id=0x7f3a serial=a3f9c2d1e5" )
           == "usb <NUM>: new device id=<*> serial=<*>" );
    CHECK( maskLine( u"key 0x7f3a and a3f9c2d1e5" ) == "key <HEX> and <HEX>" );
    CHECK( maskLine( u"request 550e8400-e29b-41d4-a716-446655440000 done" )
           == "request <HEX> done" );
    CHECK( maskLine( u"user=alice level=INFO path=\"/var/log/a b.log\"" )
           == "user=<*> level=INFO path=\"<*>\"" );
    CHECK( maskLine( u"worker-12 started" ) == "worker-<NUM> started" );
    CHECK( maskLine( u"  plain   text  " ) == "plain text" );
    CHECK( maskLine( u"版本 3 已啟動" ) == "版本 <NUM> 已啟動" );
}

SCENARIO( "Templates of logs compared", "[templates]" )
{
    QByteArray good;
    QByteArray bad;
    for ( int i = 0; i < 100; ++i ) {
        const auto n = QByteArray::number( i );
        good += "2026-11-22 10:00:00 request " + n + " done in " + n + " ms\n";
        bad += "2026-11-22 11:00:00 request " + n + " done in " + n + " ms\n";
        if ( i % 10 == 0 ) {
            good += "2026-11-22 10:00:00 cache hit for user=" + n + "\n";
            bad += "2026-11-22 11:00:00 cache hit for user=" + n + "\n";
            bad += "2026-11-22 11:00:00 retry request " + n + "\n";
        }
    }
    good += "2026-11-22 10:00:01 shutdown clean\n";
    bad += "2026-11-22 11:00:01 FATAL out of memory\n";
    const LoadedLog first( good );
    const LoadedLog second( bad );

    AtomicFlag cancel;
    int progress = 0;
    const auto a
        = templates::build( first.data(), {}, cancel, [ &progress ]( int p ) { progress = p; } );
    const auto b = templates::build( second.data(), {}, cancel, {} );
    CHECK( progress == 100 );
    CHECK( a.lines == 111 );
    REQUIRE( a.templates.size() == 3 );
    CHECK( a.templates[ 0 ].text == "request <NUM> done in <NUM> ms" );
    CHECK( a.templates[ 0 ].count == 100 );
    CHECK( a.templates[ 0 ].first == 0_lnum );
    CHECK( b.templates.size() == 4 );

    const auto compared = templates::compare( a, &b, false );
    REQUIRE( compared.size() == 5 );
    using Status = templates::Compared::Status;
    CHECK( compared[ 0 ].status == Status::OnlySecond );
    CHECK( compared[ 0 ].text == "retry request <NUM>" );
    CHECK( compared[ 0 ].secondCount == 10 );
    CHECK( compared[ 1 ].status == Status::OnlySecond );
    CHECK( compared[ 1 ].text == "FATAL out of memory" );
    CHECK( compared[ 2 ].status == Status::OnlyFirst );
    CHECK( compared[ 2 ].text == "shutdown clean" );
    CHECK( *compared[ 2 ].firstLine == 110_lnum );
    CHECK( compared[ 3 ].status == Status::Same );

    // A part of the file: a range, or a set of lines
    templates::Scope range;
    range.range = std::make_pair( 0_lnum, 10_lnum );
    CHECK( templates::build( first.data(), range, cancel, {} ).lines == 11 );
    templates::Scope set;
    set.lines = SearchResultArray();
    set.lines->add( uint64_t{ 10 } );
    set.lines->add( uint64_t{ 110 } );
    const auto ofSet = templates::build( first.data(), set, cancel, {} );
    CHECK( ofSet.lines == 2 );
    CHECK( ofSet.templates.size() == 2 );
    CHECK( set.size( first.data() ) == 2 );

    // Alone: the rarest first
    const auto alone = templates::compare( a, nullptr, false );
    CHECK( alone.front().text == "shutdown clean" );

    // Line by line
    const auto lineDiff = templates::diff( first.data(), {}, second.data(), {}, cancel );
    REQUIRE( lineDiff.error.isEmpty() );
    size_t added = 0;
    size_t removed = 0;
    for ( const auto& line : lineDiff.lines ) {
        added += line.op == templates::DiffLine::Op::Added ? 1 : 0;
        removed += line.op == templates::DiffLine::Op::Removed ? 1 : 0;
    }
    CHECK( added == 11 );
    CHECK( removed == 1 );
    CHECK( lineDiff.lines.size() == 122 );
    CHECK( lineDiff.lines.back().op == templates::DiffLine::Op::Added );
    CHECK( *lineDiff.lines.back().second == 120_lnum );
}

TEST_CASE( "Similar templates merged", "[templates]" )
{
    const auto merged = templates::mergeSimilar( { "Start proc for user alice now",
                                                   "Start proc for user bob now",
                                                   "Connection opened",
                                                   "Connection closed",
                                                   "something else entirely here" } );
    CHECK( merged[ 0 ] == "Start proc for user <*> now" );
    CHECK( merged[ 1 ] == "Start proc for user <*> now" );
    // Too short to tell a name from a state
    CHECK( merged[ 2 ] == "Connection opened" );
    CHECK( merged[ 3 ] == "Connection closed" );
    CHECK( merged[ 4 ] == "something else entirely here" );
}

TEST_CASE( "Templates of a large file in the background", "[templates][.benchmark]" )
{
    // FLOGG_BENCH_FILE=<log> klogg_itests "Templates of a large file in the background"
    const auto fileName = qEnvironmentVariable( "FLOGG_BENCH_FILE" );
    if ( fileName.isEmpty() ) {
        WARN( "FLOGG_BENCH_FILE is not set" );
        return;
    }
    LogData logData;
    SafeQSignalSpy loadEndSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    logData.attachFile( fileName );
    REQUIRE( loadEndSpy.safeWait( 600000 ) );

    AtomicFlag cancel;
    QElapsedTimer timer;
    timer.start();
    const auto table = templates::build( logData, {}, cancel, {} );
    WARN( "templates: " << table.lines << " lines, " << table.templates.size() << " templates in "
                        << timer.elapsed() << " ms" );

    timer.restart();
    const auto durations = measureDurations( logData, { "ERROR", "INFO", false }, cancel, {} );
    WARN( "durations: " << durations.pairs << " pairs in " << timer.elapsed() << " ms" );
}

#include <QComboBox>
#include <QTreeWidget>

#include "analysisdialog.h"
#include "crawlerwidget.h"
#include "session.h"

namespace {
std::unique_ptr<CrawlerWidget> openLog( Session& session, const QString& fileName,
                                        const QByteArray& content )
{
    QFile file( fileName );
    REQUIRE( file.open( QIODevice::WriteOnly ) );
    file.write( content );
    file.close();
    std::unique_ptr<CrawlerWidget> crawler( static_cast<CrawlerWidget*>(
        session.open( fileName, []() { return new CrawlerWidget(); } ) ) );
    REQUIRE( waitUiState( [ & ]() { return !crawler->isLoading(); } ) );
    return crawler;
}
} // namespace

SCENARIO( "Analysis window", "[ui][analysis]" )
{
    QTemporaryDir dir;
    Session session;
    QByteArray good;
    QByteArray bad;
    for ( int i = 0; i < 50; ++i ) {
        const auto n = QByteArray::number( i );
        good += "2026-11-22 10:00:0" + QByteArray::number( i % 10 ) + ".000 begin request id=" + n
                + "\n";
        good += "2026-11-22 10:00:0" + QByteArray::number( i % 10 ) + ".250 end request id=" + n
                + "\n";
        bad += "2026-11-22 11:00:00.000 begin request id=" + n + "\n";
        if ( i != 7 ) {
            bad += "2026-11-22 11:00:01.000 end request id=" + n + "\n";
        }
    }
    bad += "2026-11-22 11:00:02.000 FATAL EXCEPTION in main\n";

    auto first = openLog( session, dir.filePath( "good.log" ), good );
    auto second = openLog( session, dir.filePath( "bad.log" ), bad );
    CrawlerWidget* current = second.get();

    AnalysisDialog::Context context;
    context.current = [ &current ] { return current; };
    context.tabs = [ & ] {
        return std::vector<AnalysisDialog::Tab>{ { "good.log", first.get() },
                                                 { "bad.log", second.get() } };
    };
    context.activate = [ &current ]( CrawlerWidget* crawler ) { current = crawler; };
    AnalysisDialog dialog( context );
    dialog.resize( 900, 600 );
    dialog.show();

    // Durations of the bad log: 49 pairs of 1 s, one start never ended
    dialog.runDurations( "begin request id=(\\d+)", "end request id=(\\d+)" );
    REQUIRE( waitUiState( [ & ]() { return !dialog.isRunning(); } ) );
    auto* table = dialog.durationTable();
    REQUIRE( table->topLevelItemCount() == 50 );
    CHECK( table->topLevelItem( 0 )->text( 0 ) == "1.000 s" );
    CHECK( table->topLevelItem( 49 )->text( 0 ) == "no end" );
    CHECK( table->topLevelItem( 49 )->text( 1 ) == "15" );

    // A pair selects its two lines: the time between them shows
    Q_EMIT table->itemActivated( table->topLevelItem( 0 ), 0 );
    REQUIRE( waitUiState( [ & ]() { return second->selectionTimeText() == "Δ +1.000 s"; } ) );

    // Rules
    const auto rulesFile = dir.filePath( "rules.json" );
    {
        QFile file( rulesFile );
        REQUIRE( file.open( QIODevice::WriteOnly ) );
        file.write( analysis::exampleRules() );
    }
    dialog.runRules( rulesFile );
    REQUIRE( waitUiState( [ & ]() { return !dialog.isRunning(); } ) );
    auto* tree = dialog.rulesTree();
    REQUIRE( tree->topLevelItemCount() == 4 );
    CHECK( tree->topLevelItem( 0 )->text( 0 ) == "Crash (1)" );
    REQUIRE( tree->topLevelItem( 0 )->childCount() == 1 );
    CHECK( tree->topLevelItem( 0 )->child( 0 )->text( 0 ) == "Line 100" );
    CHECK( dialog.message() == "Rules that found something: 1 of 4" );

    // A rule file with a mistake
    {
        QFile file( rulesFile );
        REQUIRE( file.open( QIODevice::WriteOnly ) );
        file.write( R"({"rules":[{"type":"bogus","patterns":["a"]}]})" );
    }
    dialog.runRules( rulesFile );
    CHECK( dialog.message().contains( "unknown type" ) );

    // Compare the bad log with the good one
    dialog.showPage( AnalysisDialog::Compare );
    auto* other = dialog.otherTab();
    REQUIRE( other->count() == 2 );
    CHECK( other->itemText( 1 ) == "good.log" );
    other->setCurrentIndex( 1 );
    dialog.runCompare( false );
    REQUIRE( waitUiState( [ & ]() { return !dialog.isRunning(); } ) );
    auto* model = dialog.compareModel();
    REQUIRE( model->rowCount() == 3 );
    CHECK( model->compared( 0 ).text == "FATAL EXCEPTION in main" );
    CHECK( model->compared( 0 ).status == templates::Compared::Status::OnlyFirst );
    CHECK( model->compared( 0 ).firstCount == 1 );

    // Line by line
    dialog.runCompare( true );
    REQUIRE( waitUiState( [ & ]() { return !dialog.isRunning(); } ) );
    CHECK( dialog.message() == "Lines only here: 1, only there: 1" );
    CHECK( dialog.diffTree()->topLevelItemCount() > 3 );

    const auto renderDir = qEnvironmentVariable( "FLOGG_RENDER_DIR" );
    if ( !renderDir.isEmpty() ) {
        dialog.grab().save( renderDir + "/analysis_diff.png" );
        dialog.runCompare( false );
        waitUiState( [ & ]() { return !dialog.isRunning(); } );
        dialog.grab().save( renderDir + "/analysis_compare.png" );
        dialog.showPage( AnalysisDialog::Rules );
        dialog.runRules( dir.filePath( "missing.json" ) );
        {
            QFile file( rulesFile );
            REQUIRE( file.open( QIODevice::WriteOnly ) );
            file.write( analysis::exampleRules() );
        }
        dialog.runRules( rulesFile );
        waitUiState( [ & ]() { return !dialog.isRunning(); } );
        dialog.grab().save( renderDir + "/analysis_rules.png" );
        dialog.showPage( AnalysisDialog::Durations );
        dialog.grab().save( renderDir + "/analysis_durations.png" );
    }
}
