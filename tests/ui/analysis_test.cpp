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
#include "timestamp.h"
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
        log.data(), { "begin request id=(\\d+)", "end request id=(\\d+)", false, {} }, cancel,
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

    // Without groups, an end closes the latest start
    const auto unkeyed
        = measureDurations( log.data(), { "begin request", "end request", false, {} }, cancel, {} );
    CHECK( unkeyed.pairs == 2 );
    CHECK( unkeyed.slowest[ 0 ].us == 3000000 );
    CHECK( unkeyed.slowest[ 0 ].start == 0_lnum );
    CHECK( unkeyed.slowest[ 1 ].start == 1_lnum );

    // Plain text: found as written, the characters of a regular expression too
    const auto text = measureDurations(
        log.data(), { "begin request id=2", "end request id=2", false, {}, false }, cancel, {} );
    REQUIRE( text.error.isEmpty() );
    CHECK( text.pairs == 1 );
    CHECK( text.slowest[ 0 ].us == 500000 );
    CHECK( text.slowest[ 0 ].key.isEmpty() );
    const auto literal = measureDurations(
        log.data(), { "begin request id=.", "end request id=.", false, {}, false }, cancel, {} );
    CHECK( literal.error.isEmpty() );
    CHECK( literal.pairs == 0 );
    CHECK( measureDurations( log.data(), { "(", "end", false, {}, false }, cancel, {} )
               .error.isEmpty() );

    // Rules in plain text
    Rule textRule;
    textRule.patterns = { "end request id=." };
    textRule.regex = false;
    CHECK( runRules( log.data(), { textRule }, cancel, {} ).findings.empty() );
    textRule.regex = true;
    CHECK( runRules( log.data(), { textRule }, cancel, {} ).findings.size() == 5 );

    // Errors
    CHECK( !measureDurations( log.data(), { "(", "end", false, {} }, cancel, {} ).error.isEmpty() );
    CHECK( !measureDurations( log.data(), { "", "end", false, {} }, cancel, {} ).error.isEmpty() );
    CHECK( !measureDurations( log.data(), { "", "end", false, {}, false }, cancel, {} )
                .error.isEmpty() );
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

SCENARIO( "Rules pair the open instances by their key", "[analysis]" )
{
    AtomicFlag cancel;

    // An end with a key goes with the oldest start of its key or without one; an end
    // without a key with the oldest start
    {
        const LoadedLog log( "2026-11-22 10:00:00 open id=1\n"   // 0
                             "2026-11-22 10:00:00 open id=2\n"   // 1
                             "2026-11-22 10:00:00 open\n"        // 2
                             "2026-11-22 10:00:01 done id=2\n"   // 3: closes 1
                             "2026-11-22 10:00:01 done\n" );     // 4: closes 0
        const auto set = parseRules(
            R"json({"rules":[{"name":"a","type":"absence",)json"
            R"json("patterns":["open(?: id=(\\d+))?","done(?: id=(\\d+))?"]}]})json" );
        REQUIRE( set.error.isEmpty() );
        const auto result = runRules( log.data(), set.rules, cancel, {} );
        REQUIRE( result.findings.size() == 1 );
        CHECK( result.findings[ 0 ].line == 2_lnum );
        CHECK( result.findings[ 0 ].detail.contains( "Never followed" ) );
    }

    // Sequences of two keys going on side by side
    {
        const LoadedLog log( "2026-11-22 10:00:00 step a x\n"   // 0
                             "2026-11-22 10:00:00 step a y\n"   // 1
                             "2026-11-22 10:00:01 step b y\n"   // 2
                             "2026-11-22 10:00:02 step c y\n"   // 3
                             "2026-11-22 10:00:03 step b x\n"   // 4
                             "2026-11-22 10:00:04 step c x\n" ); // 5
        const auto set = parseRules(
            R"json({"rules":[{"name":"s","type":"sequence",)json"
            R"json("patterns":["step a (\\w+)","step b (\\w+)","step c (\\w+)"]}]})json" );
        REQUIRE( set.error.isEmpty() );
        const auto result = runRules( log.data(), set.rules, cancel, {} );
        REQUIRE( result.findings.size() == 2 );
        CHECK( lines( result.findings[ 0 ] ) == std::vector<uint64_t>{ 0, 4, 5 } );
        CHECK( lines( result.findings[ 1 ] ) == std::vector<uint64_t>{ 1, 2, 3 } );
    }

    // Many open at once, closed newest first: each end finds its start right away
    {
        QByteArray content;
        constexpr int Open = 60000;
        for ( int i = 0; i < Open; ++i ) {
            content += "2026-11-22 10:00:00 open id=" + QByteArray::number( i ) + "\n";
        }
        for ( int i = Open - 1; i >= 1; --i ) {
            content += "2026-11-22 10:00:01 done id=" + QByteArray::number( i ) + "\n";
        }
        const LoadedLog log( content );
        const auto set = parseRules( R"json({"rules":[{"name":"a","type":"absence",)json"
                                     R"json("patterns":["open id=(\\d+)","done id=(\\d+)"]}]})json" );
        REQUIRE( set.error.isEmpty() );
        QElapsedTimer timer;
        timer.start();
        const auto result = runRules( log.data(), set.rules, cancel, {} );
        WARN( "60 000 open instances: " << timer.elapsed() << " ms" );
        REQUIRE( result.findings.size() == 1 );
        CHECK( result.findings[ 0 ].line == 0_lnum );
    }

    // Count: the keys past the time go, so a key coming after many others is still counted
    {
        QByteArray content;
        for ( int i = 0; i < 100500; ++i ) {
            content += "2026-11-22 10:00:00 retry request=k" + QByteArray::number( i ) + "\n";
        }
        for ( int i = 0; i < 3; ++i ) {
            content += "2026-11-22 10:02:0" + QByteArray::number( i ) + " retry request=z\n";
        }
        const LoadedLog log( content );
        const auto set = parseRules( R"json({"rules":[{"name":"c","type":"count",)json"
                                     R"json("patterns":["retry request=(\\w+)"],)json"
                                     R"json("count":3,"withinSeconds":60}]})json" );
        REQUIRE( set.error.isEmpty() );
        const auto result = runRules( log.data(), set.rules, cancel, {} );
        REQUIRE( result.findings.size() == 1 );
        CHECK( lines( result.findings[ 0 ] ) == std::vector<uint64_t>{ 100500, 100501, 100502 } );
    }
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
    const auto result = measureDurations( log.data(), { "begin", "end", false, {} }, cancel, {} );
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

    // More or fewer: by the share of the lines, and only when it can not be chance
    {
        const auto table = []( uint64_t lines, std::vector<std::pair<QString, uint64_t>> counts ) {
            templates::Table t;
            t.lines = lines;
            for ( auto& [ text, count ] : counts ) {
                templates::Template one;
                one.text = text;
                one.count = count;
                t.templates.push_back( one );
            }
            return t;
        };
        const auto status = []( const std::vector<templates::Compared>& rows, const QString& text ) {
            for ( const auto& row : rows ) {
                if ( row.text == text ) {
                    return row.status;
                }
            }
            return Status::OnlyFirst;
        };
        // 1 line in each, files 15 times apart: the share differs but it is chance
        const auto fewLines = table( 7680, { { "once", 1 } } );
        const auto manyLines = table( 117823, { { "once", 1 } } );
        CHECK( status( templates::compare( fewLines, &manyLines, false ), "once" ) == Status::Same );
        // 3 against 1 in files of the same size: twice the share, still chance
        const auto three = table( 100, { { "few", 3 } } );
        const auto one = table( 100, { { "few", 1 } } );
        CHECK( status( templates::compare( three, &one, false ), "few" ) == Status::Same );
        // Clear changes, small and large counts (exact and approximated chance)
        const auto before = table( 10000, { { "a", 40 }, { "b", 2 }, { "c", 1000 }, { "d", 9 } } );
        const auto after = table( 10000, { { "a", 2 }, { "b", 40 }, { "c", 400 }, { "d", 3 } } );
        const auto rows = templates::compare( before, &after, false );
        CHECK( status( rows, "a" ) == Status::Fewer );
        CHECK( status( rows, "b" ) == Status::More );
        CHECK( status( rows, "c" ) == Status::Fewer );
        CHECK( status( rows, "d" ) == Status::Same );
        // The changes before the others, even with a larger ratio
        CHECK( rows.back().text == "d" );
    }

    // Alone: the rarest first
    const auto alone = templates::compare( a, nullptr, false );
    CHECK( alone.front().text == "shutdown clean" );

    // Line by line: the text of the lines, the time stamps left out when asked
    const auto count = []( const templates::Diff& diff, templates::DiffLine::Op op ) {
        size_t n = 0;
        for ( const auto& line : diff.lines() ) {
            n += line.op == op ? 1 : 0;
        }
        return n;
    };
    using Op = templates::DiffLine::Op;
    {
        const auto text = templates::diff( first.data(), {}, second.data(), {}, {}, cancel );
        REQUIRE( text.error.isEmpty() );
        CHECK( count( text, Op::Added ) == 121 );
        CHECK( count( text, Op::Removed ) == 111 );
    }
    templates::DiffOptions noTimes;
    noTimes.ignoreTimes = true;
    const auto lineDiff = templates::diff( first.data(), {}, second.data(), {}, noTimes, cancel );
    REQUIRE( lineDiff.error.isEmpty() );
    CHECK( count( lineDiff, Op::Added ) == 11 );
    CHECK( count( lineDiff, Op::Removed ) == 1 );
    const auto lineDiffLines = lineDiff.lines();
    CHECK( lineDiffLines.size() == 122 );
    CHECK( lineDiffLines.back().op == Op::Added );
    CHECK( *lineDiffLines.back().second == 120_lnum );
}

TEST_CASE( "Texts compared line by line", "[templates]" )
{
    using Op = templates::DiffLine::Op;
    AtomicFlag cancel;
    const std::vector<QString> first{ "2026-11-22 09:00:58.261 start", "same",
                                      "2026-11-22 09:00:59.000 retry id=77", "gone" };
    const std::vector<QString> second{ "2026-11-23 10:00:00.000 start", "same",
                                       "2026-11-23 10:00:01.000 retry id=78", "new" };

    // Pure text: only "same" is alike
    const auto textDiff = templates::diff( first, second, {}, cancel );
    REQUIRE( textDiff.error.isEmpty() );
    const auto text = textDiff.lines();
    REQUIRE( text.size() == 7 );
    CHECK( text[ 0 ].op == Op::Removed );
    CHECK( text[ 1 ].op == Op::Added );
    CHECK( text[ 2 ].op == Op::Same );
    CHECK( *text[ 2 ].first == 1_lnum );
    CHECK( *text[ 2 ].second == 1_lnum );

    templates::DiffOptions noTimes;
    noTimes.ignoreTimes = true;
    CHECK( templates::diffKey( first[ 0 ], noTimes ) == templates::diffKey( second[ 0 ], noTimes ) );
    CHECK( templates::diffKey( u"plain 12", {} ) == "plain 12" );
    const auto withoutTimes = templates::diff( first, second, noTimes, cancel ).lines();
    CHECK( withoutTimes[ 0 ].op == Op::Same );
    CHECK( withoutTimes[ 1 ].op == Op::Same );
    CHECK( withoutTimes[ 2 ].op != Op::Same );

    auto noNumbers = noTimes;
    noNumbers.ignoreNumbers = true;
    const auto alike = templates::diff( first, second, noNumbers, cancel ).lines();
    REQUIRE( alike.size() == 5 );
    CHECK( alike[ 2 ].op == Op::Same );
    CHECK( alike[ 3 ].op == Op::Removed );
    CHECK( alike[ 4 ].op == Op::Added );

    // The characters that differ in a changed line
    {
        const auto parts = templates::changedParts( u"retry id=77 now", u"retry id=78 now", {} );
        REQUIRE( parts.first.size() == 1 );
        CHECK( parts.first[ 0 ] == std::make_pair( 10, 1 ) );
        REQUIRE( parts.second.size() == 1 );
        CHECK( parts.second[ 0 ] == std::make_pair( 10, 1 ) );
    }
    {
        // Added characters on one side only, next to each other in one range
        const auto parts = templates::changedParts( u"connect", u"reconnected", {} );
        CHECK( parts.first.empty() );
        REQUIRE( parts.second.size() == 2 );
        CHECK( parts.second[ 0 ] == std::make_pair( 0, 2 ) );
        CHECK( parts.second[ 1 ] == std::make_pair( 9, 2 ) );
    }
    {
        // The time stamps left out never differ
        const auto parts = templates::changedParts( first[ 2 ], second[ 2 ], noTimes );
        REQUIRE( parts.first.size() == 1 );
        CHECK( parts.first[ 0 ].first == 34 );
        REQUIRE( parts.second.size() == 1 );
        CHECK( parts.second[ 0 ].first == 34 );
    }
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

#include <QTreeView>

#include "analysisdialog.h"
#include "configuration.h"

TEST_CASE( "Time of each line drawn", "[templates][.benchmark]" )
{
    // FLOGG_BENCH_FILE=<log> klogg_itests "Time of each line drawn": the cost of a column of
    // the time from a reference line and from the line before, per line drawn
    const auto fileName = qEnvironmentVariable( "FLOGG_BENCH_FILE" );
    if ( fileName.isEmpty() ) {
        WARN( "FLOGG_BENCH_FILE is not set" );
        return;
    }
    LogData logData;
    SafeQSignalSpy loadEndSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    logData.attachFile( fileName );
    REQUIRE( loadEndSpy.safeWait( 600000 ) );
    constexpr uint64_t Count = 200000;
    const auto lines = logData.getLines( 0_lnum, LinesCount( Count ) );
    REQUIRE( lines.size() == Count );

    QElapsedTimer timer;
    timer.start();
    const auto reference = timestamps::parseLine( lines[ 0 ] );
    auto previous = reference;
    int64_t total = 0;
    QString text;
    for ( const auto& line : lines ) {
        const auto times = timestamps::parseLine( line );
        const auto fromReference = timestamps::difference( reference, times, {} );
        const auto fromPrevious = timestamps::difference( previous, times, {} );
        if ( fromReference.us && fromPrevious.us ) {
            total += *fromReference.us + *fromPrevious.us;
            text = timestamps::formatDuration( *fromPrevious.us, 3 );
        }
        previous = times;
    }
    const auto ns = static_cast<double>( timer.nsecsElapsed() ) / Count;
    WARN( "parse, two differences and the text: " << ns << " ns a line, " << ns * 60 / 1000
                                                    << " us for a screen of 60 lines ("
                                                    << total % 7 << ")" );
}

TEST_CASE( "Line by line of a large file", "[templates][.benchmark]" )
{
    // FLOGG_BENCH_FILE=<log> klogg_itests "Line by line of a large file"
    const auto fileName = qEnvironmentVariable( "FLOGG_BENCH_FILE" );
    if ( fileName.isEmpty() ) {
        WARN( "FLOGG_BENCH_FILE is not set" );
        return;
    }
    LogData logData;
    SafeQSignalSpy loadEndSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    logData.attachFile( fileName );
    REQUIRE( loadEndSpy.safeWait( 600000 ) );
    const auto n = templates::MaxDiffLines;
    REQUIRE( logData.getNbLine().get() >= 2 * n + 1000 );

    AtomicFlag cancel;
    QElapsedTimer timer;
    const auto range = []( uint64_t first, uint64_t count ) {
        templates::Scope scope;
        scope.range = std::make_pair( LineNumber( first ), LineNumber( first + count - 1 ) );
        return scope;
    };
    const auto report = [ & ]( const char* name, const templates::Diff& diff ) {
        uint64_t changed = 0;
        for ( const auto step : diff.steps ) {
            changed += step == templates::DiffStep::Same ? 0 : 1;
        }
        WARN( name << ": " << timer.elapsed() << " ms, " << changed << " lines changed, "
                   << diff.unpairedLines << " unpaired" );
    };

    timer.start();
    report( "same 1M lines", templates::diff( logData, range( 0, n ), logData, range( 0, n ), {},
                                              cancel ) );
    timer.restart();
    report( "1M lines shifted by 1000",
            templates::diff( logData, range( 0, n ), logData, range( 1000, n ), {}, cancel ) );

    // A copy with every 50th line changed and every 1000th left out
    std::vector<QString> first;
    std::vector<QString> second;
    first.reserve( n );
    second.reserve( n );
    for ( uint64_t i = 0; i < n; ++i ) {
        auto line = logData.getLineString( LineNumber( i ) );
        first.push_back( line );
        if ( i % 1000 == 999 ) {
            continue;
        }
        second.push_back( i % 50 == 0 ? line + " changed" : line );
    }
    timer.restart();
    report( "1M lines, 2% changed", templates::diff( first, second, {}, cancel ) );

    // Every third line changed: many small parts between the lines alike
    for ( size_t i = 0; i < second.size(); i += 3 ) {
        second[ i ] += " again";
    }
    timer.restart();
    report( "1M lines, a third changed", templates::diff( first, second, {}, cancel ) );

    // Nothing alike: every line changed
    for ( auto& line : second ) {
        line += "#";
    }
    timer.restart();
    report( "1M lines, all changed", templates::diff( first, second, {}, cancel ) );

    // Half of the file moved to the end
    timer.restart();
    report( "1M lines, halves swapped",
            templates::diff( logData, range( 0, n ), logData, range( n / 2 + 137, n ), {},
                             cancel ) );

    // The same lines, times and numbers left out
    templates::DiffOptions noTimes;
    noTimes.ignoreTimes = true;
    noTimes.ignoreNumbers = true;
    timer.restart();
    report( "1M lines shifted by 1000, times and numbers left out",
            templates::diff( logData, range( 0, n ), logData, range( 1000, n ), noTimes, cancel ) );

    // The worst: no line once on each side and the halves swapped, the work runs out
    timer.restart();
    report( "1M lines, halves swapped, times and numbers left out",
            templates::diff( logData, range( 0, n ), logData, range( n / 2 + 137, n ), noTimes,
                             cancel ) );

    // A million rows in the two columns: given to the view, shown, scrolled to the end
    {
        std::vector<DiffModel::Row> rows( n );
        for ( uint64_t i = 0; i < n; ++i ) {
            rows[ i ].first = i;
            rows[ i ].second = i;
            rows[ i ].changed = true;
        }
        DiffModel model;
        QTreeView view;
        view.setUniformRowHeights( true );
        view.setRootIsDecorated( false );
        view.setModel( &model );
        view.resize( 900, 500 );
        view.show();
        timer.restart();
        model.setRows( std::move( rows ),
                       [ &first ]( int, LineNumber line ) { return first[ line.get() ]; }, {} );
        view.grab();
        WARN( "1M rows set and shown: " << timer.elapsed() << " ms" );
        timer.restart();
        view.scrollToBottom();
        view.grab();
        WARN( "scrolled to the end and shown: " << timer.elapsed() << " ms" );
    }

    // The characters of the rows shown: a screen of long lines
    QString longA;
    QString longB;
    for ( int i = 0; i < 100; ++i ) {
        longA += QString( "word%1 " ).arg( i );
        longB += QString( "word%1 " ).arg( i % 3 == 0 ? i + 1 : i );
    }
    timer.restart();
    for ( int i = 0; i < 50; ++i ) {
        templates::changedParts( longA, longB, {} );
    }
    WARN( "50 long changed lines: " << timer.elapsed() << " ms" );
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
    timer.restart();
    uint64_t visited = 0;
    scanMatches( logData,
                 { RegularExpressionPattern( qEnvironmentVariable( "FLOGG_BENCH_START", "ERROR" ),
                                             false, false, false, false ),
                   RegularExpressionPattern( qEnvironmentVariable( "FLOGG_BENCH_END", "INFO" ),
                                             false, false, false, false ) },
                 cancel, {}, [ &visited ]( LineNumber, const QString&, const auto& ) { ++visited; } );
    WARN( "scan and read only: " << visited << " lines in " << timer.elapsed() << " ms" );
    timer.restart();

    const auto start = qEnvironmentVariable( "FLOGG_BENCH_START", "ERROR" );
    const auto end = qEnvironmentVariable( "FLOGG_BENCH_END", "INFO" );
    const auto durations = measureDurations( logData, { start, end, false, {} }, cancel, {} );
    WARN( "durations: " << durations.pairs << " pairs in " << timer.elapsed() << " ms" );

    timer.restart();
    {
        std::vector<Rule> rules( 3 );
        rules[ 0 ].name = "absence";
        rules[ 0 ].type = Rule::Type::Absence;
        rules[ 0 ].patterns = { "scan started", "connected to AP" };
        rules[ 0 ].withinSeconds = 5;
        rules[ 1 ].name = "sequence";
        rules[ 1 ].type = Rule::Type::Sequence;
        rules[ 1 ].patterns = { "\\[(wlan0)\\] .*scan started", "\\[(wlan0)\\] .*connected to AP" };
        rules[ 1 ].withinSeconds = 2;
        rules[ 2 ].name = "count";
        rules[ 2 ].type = Rule::Type::Count;
        rules[ 2 ].patterns = { "user login id=(\\d+)" };
        rules[ 2 ].count = 3;
        rules[ 2 ].withinSeconds = 1;
        const auto found = runRules( logData, rules, cancel, {} );
        WARN( "rules: " << found.findings.size() << " findings kept, "
                        << found.totalFindings[ 0 ] << "/" << found.totalFindings[ 1 ] << "/"
                        << found.totalFindings[ 2 ] << " in " << timer.elapsed() << " ms" );
    }
}

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QToolButton>
#include <QTreeView>
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

    // The "?" next to Stop tells how to use the page shown
    dialog.showPage( AnalysisDialog::Rules );
    dialog.helpButton()->click();
    REQUIRE( dialog.helpWindow() );
    CHECK( dialog.helpWindow()->windowTitle() == "Rules - how to use" );
    dialog.showPage( AnalysisDialog::Durations );
    dialog.helpButton()->click();
    CHECK( dialog.helpWindow()->windowTitle() == "Durations - how to use" );
    dialog.helpWindow()->close();
    QCoreApplication::sendPostedEvents( nullptr, QEvent::DeferredDelete );

    // The tab of the durations and the rules: the current one until another is chosen
    REQUIRE( dialog.durationTab()->count() == 2 );
    CHECK( dialog.durationTab()->currentText() == "bad.log" );
    CHECK( dialog.rulesTab()->currentText() == "bad.log" );

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
    CHECK( tree->topLevelItem( 0 )->text( 1 ) == "match" );
    CHECK( tree->topLevelItem( 0 )->text( 2 ) == "error" );
    // The details of a rule: its note, with its web address as a link
    auto* login = tree->findItems( "Login then logout right away (0)", Qt::MatchExactly ).value( 0 );
    REQUIRE( login );
    tree->setCurrentItem( login );
    CHECK( dialog.ruleDetails()->text().contains(
        "<a href=\"https://example.com/wiki/login-loop\">https://example.com/wiki/login-loop</a>" ) );
    REQUIRE( tree->topLevelItem( 0 )->childCount() == 1 );
    CHECK( tree->topLevelItem( 0 )->child( 0 )->text( 0 ) == "Line 100" );
    CHECK( dialog.message() == "Rules that found something: 1 of 4" );

    // A finding of several lines: one row under it for each line
    {
        QFile file( rulesFile );
        REQUIRE( file.open( QIODevice::WriteOnly ) );
        file.write( R"json({"rules":[{"name":"Pair","type":"sequence","patterns":)json"
                    R"json(["begin request id=(\\d+)","end request id=(\\d+)"],)json"
                    R"json("withinSeconds":5}]})json" );
    }
    dialog.runRules( rulesFile );
    REQUIRE( waitUiState( [ & ]() { return !dialog.isRunning(); } ) );
    REQUIRE( tree->topLevelItemCount() == 1 );
    auto* pairRule = tree->topLevelItem( 0 );
    CHECK( pairRule->text( 0 ) == "Pair (49)" );
    REQUIRE( pairRule->childCount() > 0 );
    auto* pair = pairRule->child( 0 );
    CHECK( pair->text( 0 ) == "Lines 1 - 2 (2)" );
    REQUIRE( pair->childCount() == 2 );
    CHECK( pair->child( 1 )->text( 0 ) == "Line 2" );
    CHECK( pair->child( 1 )->text( 4 ).contains( "end request id=0" ) );

    // The list opens under it, also when its current item is not the first one
    {
        auto* list = dialog.durationTab();
        list->setCurrentIndex( 1 );
        list->showPopup();
        auto* popup = list->view()->window();
        REQUIRE( waitUiState( [ & ] { return popup->isVisible(); } ) );
        const auto under = list->mapToGlobal( QPoint( 0, list->height() ) ).y();
        CHECK( popup->geometry().top() >= under - 2 );
        list->hidePopup();
    }

    // Durations of another tab, chosen in the list
    dialog.durationTab()->setCurrentIndex( 0 );
    Q_EMIT dialog.durationTab()->activated( 0 );
    dialog.runDurations( "begin request id=(\\d+)", "end request id=(\\d+)" );
    REQUIRE( waitUiState( [ & ]() { return !dialog.isRunning(); } ) );
    REQUIRE( table->topLevelItemCount() == 50 );
    CHECK( table->topLevelItem( 0 )->text( 0 ) == "250 ms" );
    CHECK( dialog.durationTab()->currentText() == "good.log" );

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
    CHECK( model->headerData( CompareModel::FirstCount, Qt::Horizontal, Qt::DisplayRole ) == "bad.log" );
    CHECK( model->data( model->index( 0, CompareModel::Status ), Qt::DisplayRole ) == "only in bad.log" );
    // The counts with their share of the lines
    {
        const auto count
            = model->data( model->index( 0, CompareModel::FirstCount ), Qt::DisplayRole ).toString();
        CHECK( count.startsWith( "1 (" ) );
        CHECK( count.endsWith( "%)" ) );
        CHECK( model->data( model->index( 0, CompareModel::SecondCount ), Qt::DisplayRole )
               == "0 (0.0%)" );
        CHECK( model->data( model->index( 0, CompareModel::FirstCount ), Qt::ToolTipRole )
                   .toString()
                   .contains( "lines of bad.log" ) );
    }

    // A template opens on its lines in each file, a double click shows one
    {
        const auto fatal = model->index( 0, 0 );
        REQUIRE( model->rowCount( fatal ) == 1 );
        const auto fatalLine = model->index( 0, CompareModel::Template, fatal );
        CHECK( model->data( fatalLine, Qt::DisplayRole ).toString().contains( "FATAL EXCEPTION" ) );
        CHECK( model->data( model->index( 0, CompareModel::Status, fatal ), Qt::DisplayRole )
               == "bad.log" );
        CHECK( model->data( model->index( 0, CompareModel::FirstLine, fatal ), Qt::DisplayRole )
               == 100 );
        REQUIRE( model->lineAt( fatalLine ) );
        CHECK( model->lineAt( fatalLine )->first == 0 );
        CHECK( model->lineAt( fatalLine )->second == 99_lnum );

        // 50 begin lines on each side: the first ones of the bad log, then of the good one
        int beginRow = -1;
        for ( int row = 0; row < model->rowCount(); ++row ) {
            if ( model->compared( row ).text.startsWith( "begin request" ) ) {
                beginRow = row;
            }
        }
        REQUIRE( beginRow >= 0 );
        const auto begin = model->index( beginRow, 0 );
        REQUIRE( model->rowCount( begin ) == 2 * static_cast<int>( templates::MaxTemplateLines ) );
        CHECK( model->lineAt( model->index( 0, 0, begin ) )->second == 0_lnum );
        const auto other = model->lineAt(
            model->index( static_cast<int>( templates::MaxTemplateLines ), 0, begin ) );
        REQUIRE( other );
        CHECK( other->first == 1 );
        CHECK( other->second == 0_lnum );
        CHECK( model->rowCount( model->index( 0, 0, begin ) ) == 0 );

        Q_EMIT dialog.compareView()->activated( fatalLine );
        CHECK( current == second.get() );
        CHECK( second->selectedLineBounds() == std::make_pair( 99_lnum, 99_lnum ) );
    }

    // Line by line: the text of the lines, every line differs by its time
    dialog.diffTimes()->setChecked( false );
    dialog.diffNumbers()->setChecked( false );
    dialog.runCompare( true );
    REQUIRE( waitUiState( [ & ]() { return !dialog.isRunning(); } ) );
    CHECK( dialog.message() == "Lines only in bad.log: 100, only in good.log: 100" );
    const auto cell = []( DiffModel* model, int row, int column, int role = Qt::DisplayRole ) {
        return model->data( model->index( row, column ), role );
    };
    {
        // The line changed into another: the characters that differ marked
        auto* lines = dialog.diffModel();
        REQUIRE( lines->rowCount() == 100 );
        CHECK( cell( lines, 0, 1 ).toString().contains( "11:00:00.000 begin request id=0" ) );
        CHECK( cell( lines, 0, 3 ).toString().contains( "10:00:00.000 begin request id=0" ) );
        CHECK( !cell( lines, 0, 1, DiffModel::ChangesRole ).toList().isEmpty() );
    }
    // The time stamps left out
    dialog.diffTimes()->setChecked( true );
    dialog.runCompare( true );
    REQUIRE( waitUiState( [ & ]() { return !dialog.isRunning(); } ) );
    CHECK( dialog.message() == "Lines only in bad.log: 1, only in good.log: 1" );
    auto* diff = dialog.diffModel();
    CHECK( diff->headerData( 1, Qt::Horizontal, Qt::DisplayRole ) == "bad.log" );
    CHECK( diff->headerData( 3, Qt::Horizontal, Qt::DisplayRole ) == "good.log" );
    REQUIRE( diff->rowCount() > 3 );
    // Side by side: a line missing on the left next to the line of the right
    bool onlyRight = false;
    bool onlyLeft = false;
    bool folded = false;
    for ( int i = 0; i < diff->rowCount(); ++i ) {
        onlyRight |= cell( diff, i, 0 ).toString().isEmpty()
                     && cell( diff, i, 3 ).toString().contains( "end request id=7" );
        onlyLeft |= cell( diff, i, 2 ).toString().isEmpty()
                    && cell( diff, i, 1 ).toString().contains( "FATAL" );
        folded |= cell( diff, i, 1 ).toString().endsWith( "lines alike" );
    }
    CHECK( onlyRight );
    CHECK( onlyLeft );
    CHECK( folded );

    // Two pasted texts
    dialog.showPage( AnalysisDialog::CompareText );
    dialog.textTimes()->setChecked( false );
    dialog.textNumbers()->setChecked( false );
    dialog.firstText()->setPlainText( "10:00:00.000 start\nsame\nretry id=7\n" );
    dialog.secondText()->setPlainText( "11:00:00.000 start\nsame\nretry id=8\nextra" );
    dialog.runCompareText();
    REQUIRE( waitUiState( [ & ]() { return !dialog.isRunning(); } ) );
    CHECK( dialog.message() == "Lines only in the first text: 2, only in the second: 3" );
    CHECK( dialog.textDiffModel()->rowCount() == 4 );
    dialog.textTimes()->setChecked( true );
    dialog.textNumbers()->setChecked( true );
    dialog.runCompareText();
    REQUIRE( waitUiState( [ & ]() { return !dialog.isRunning(); } ) );
    CHECK( dialog.message() == "Lines only in the first text: 0, only in the second: 1" );
    // A double click goes to the line in its box
    {
        auto* texts = dialog.textDiffModel();
        REQUIRE( texts->rowCount() == 4 );
        CHECK( cell( texts, 3, 3 ).toString() == "extra" );
        Q_EMIT dialog.textDiffView()->activated( texts->index( 3, 3 ) );
        CHECK( dialog.secondText()->textCursor().blockNumber() == 3 );
        CHECK( dialog.secondText()->textCursor().selectedText() == "extra" );
    }
    // A changed line marks the characters that differ
    {
        dialog.textNumbers()->setChecked( false );
        dialog.firstText()->setPlainText( "retry id=7\n" );
        dialog.secondText()->setPlainText( "retry id=8\n" );
        dialog.runCompareText();
        REQUIRE( waitUiState( [ & ]() { return !dialog.isRunning(); } ) );
        auto* texts = dialog.textDiffModel();
        REQUIRE( texts->rowCount() == 1 );
        CHECK( !texts->index( 0, 1 ).data( DiffModel::ChangesRole ).toList().isEmpty() );
        CHECK( texts->index( 0, 1 ).data( Qt::BackgroundRole ).isValid() );
    }
    // Lines alike far from the changes are folded, as when comparing files
    {
        QString lines;
        for ( auto i = 0; i < 20; ++i ) {
            lines += QString( "same %1\n" ).arg( i );
        }
        dialog.firstText()->setPlainText( lines );
        dialog.secondText()->setPlainText( lines + "extra\n" );
        dialog.runCompareText();
        REQUIRE( waitUiState( [ & ]() { return !dialog.isRunning(); } ) );
        auto* texts = dialog.textDiffModel();
        REQUIRE( texts->rowCount() == 5 );
        CHECK( cell( texts, 0, 1 ).toString().endsWith( "lines alike" ) );
        CHECK( cell( texts, 4, 3 ).toString() == "extra" );
    }
    // Past the limit, the first lines compared and the others told
    {
        std::vector<QString> many( templates::MaxDiffLines + 5, "line" );
        AtomicFlag cancel;
        const auto cut = templates::diff( many, { "line" }, {}, cancel );
        CHECK( cut.firstLeftOut == 5 );
        CHECK( cut.secondLeftOut == 0 );
        CHECK( cut.steps.size() == templates::MaxDiffLines );
    }
    dialog.textTimes()->setChecked( false );
    dialog.textNumbers()->setChecked( false );
    dialog.showPage( AnalysisDialog::Compare );

    // The first side can be another tab
    REQUIRE( dialog.firstTab()->count() == 2 );
    dialog.firstTab()->setCurrentIndex( 0 );
    Q_EMIT dialog.firstTab()->activated( 0 );
    CHECK( dialog.otherTab()->itemText( 1 ) == "bad.log" );
    dialog.otherTab()->setCurrentIndex( 1 );
    dialog.runCompare( false );
    REQUIRE( waitUiState( [ & ]() { return !dialog.isRunning(); } ) );
    CHECK( model->compared( 0 ).status == templates::Compared::Status::OnlySecond );
    CHECK( model->compared( 0 ).text == "FATAL EXCEPTION in main" );

    const auto renderDir = qEnvironmentVariable( "FLOGG_RENDER_DIR" );
    if ( !renderDir.isEmpty() ) {
        dialog.diffTimes()->setChecked( false );
        dialog.runCompare( true );
        waitUiState( [ & ]() { return !dialog.isRunning(); } );
        dialog.grab().save( renderDir + "/analysis_diff.png" );
        dialog.showPage( AnalysisDialog::CompareText );
        dialog.firstText()->setPlainText(
            "2026-11-22 09:00:58.261 INFO wlan0: connected to ap-office (ch 36)\n"
            "2026-11-22 09:00:58.300 INFO dhcp: lease 10.0.0.12 for 3600 s\n"
            "2026-11-22 09:01:02.010 WARN retry request=77 attempt=2\n"
            "2026-11-22 09:01:05.000 INFO ready\n" );
        dialog.secondText()->setPlainText(
            "2026-11-23 14:12:01.004 INFO wlan0: connected to ap-lobby (ch 149)\n"
            "2026-11-23 14:12:01.050 INFO dhcp: lease 10.0.0.57 for 3600 s\n"
            "2026-11-23 14:12:03.512 ERROR auth timeout\n"
            "2026-11-23 14:12:09.000 INFO ready\n" );
        dialog.textTimes()->setChecked( true );
        dialog.runCompareText();
        waitUiState( [ & ]() { return !dialog.isRunning(); } );
        dialog.grab().save( renderDir + "/analysis_text_diff.png" );
        dialog.textTimes()->setChecked( false );
        dialog.showPage( AnalysisDialog::Compare );
        dialog.runCompare( false );
        waitUiState( [ & ]() { return !dialog.isRunning(); } );
        dialog.compareView()->expand( dialog.compareModel()->index( 0, 0 ) );
        dialog.grab().save( renderDir + "/analysis_compare.png" );
        dialog.showPage( AnalysisDialog::Rules );
        {
            QFile file( rulesFile );
            REQUIRE( file.open( QIODevice::WriteOnly ) );
            file.write( analysis::exampleRules() );
        }
        dialog.runRules( rulesFile );
        waitUiState( [ & ]() { return !dialog.isRunning(); } );
        dialog.rulesTree()->setCurrentItem(
            dialog.rulesTree()->findItems( "Login then logout right away (0)", Qt::MatchExactly ).value( 0 ) );
        dialog.grab().save( renderDir + "/analysis_rule_note.png" );
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
        // The help of each page, from the "?" next to Stop
        for ( int page = 0; page < 4; ++page ) {
            dialog.showPage( static_cast<AnalysisDialog::Page>( page ) );
            dialog.helpButton()->click();
            REQUIRE( dialog.helpWindow() );
            dialog.helpWindow()->grab().save( renderDir
                                              + QString( "/analysis_help_%1.png" ).arg( page ) );
        }
        dialog.showPage( AnalysisDialog::Rules );
        dialog.grab().save( renderDir + "/analysis_bottom.png" );
    }
}

// The claims of demo/README.md, on the files of demo/make_demo.py:
// FLOGG_DEMO_DIR=<repo>/demo klogg_itests "[.demo]"
SCENARIO( "Demo files", "[.demo]" )
{
    const auto demo = qEnvironmentVariable( "FLOGG_DEMO_DIR" );
    if ( demo.isEmpty() ) {
        WARN( "FLOGG_DEMO_DIR is not set" );
        return;
    }
    Session session;
    const auto open = [ &session ]( const QString& fileName ) {
        std::unique_ptr<CrawlerWidget> crawler( static_cast<CrawlerWidget*>(
            session.open( fileName, []() { return new CrawlerWidget(); } ) ) );
        REQUIRE( waitUiState( [ & ]() { return !crawler->isLoading(); } ) );
        crawler->resize( 1200, 800 );
        return crawler;
    };
    const auto line = []( CrawlerWidget& crawler, uint64_t number ) {
        return crawler.logData()->getLineString( LineNumber( number - 1 ) );
    };
    const auto delta = [ &line ]( CrawlerWidget& crawler, uint64_t first, uint64_t last ) {
        return timestamps::difference( timestamps::parseLine( line( crawler, first ) ),
                                       timestamps::parseLine( line( crawler, last ) ) );
    };

    auto good = open( demo + "/app-good.log" );
    auto bad = open( demo + "/app-bad.log" );

    // 1. Time between lines
    CHECK( line( *bad, 1421 ).contains( "db error" ) );
    CHECK( line( *bad, 1477 ).contains( "FATAL EXCEPTION" ) );
    CHECK( line( *bad, 1478 ).contains( "at com.pay" ) );
    CHECK( line( *bad, 1515 ).contains( "clock adjusted" ) );
    CHECK( timestamps::formatDuration( *delta( *bad, 1421, 1477 ).us ) == "+18.794 s" );
    CHECK( delta( *bad, 1477, 1478 ).error == "No time stamp in the last line" );
    bad->showLines( 1504_lnum, 1524_lnum );
    REQUIRE( waitUiState( [ & ]() { return bad->selectionTimeText().endsWith( "⚠" ); } ) );
    WARN( "1505-1525: " << bad->selectionTimeText().toStdString() );

    auto boot = open( demo + "/device-boot.log" );
    const auto bootLines = boot->logData()->getNbLine().get();
    const auto bootDelta = delta( *boot, 1, bootLines );
    REQUIRE( bootDelta.us );
    CHECK( bootDelta.first->isMonotonic() );
    WARN( "boot: " << timestamps::formatDuration( *bootDelta.us, 6 ).toStdString() );

    auto logcat = open( demo + "/logcat.txt" );
    const auto logcatDelta = delta( *logcat, 1, logcat->logData()->getNbLine().get() );
    REQUIRE( logcatDelta.us );
    CHECK( logcatDelta.notes.size() == 1 );
    WARN( "logcat: " << timestamps::formatDuration( *logcatDelta.us ).toStdString() );

    AtomicFlag cancel;
    // 3. Durations
    const auto requests = measureDurations(
        *bad->logData(), { "request begin id=(\\d+)", "request end id=(\\d+)", false, {} }, cancel, {} );
    CHECK( requests.openStarts == 3 );
    CHECK( requests.slowest.front().us > 2500000 );
    CHECK( requests.slowest.front().start.get() > 1422 );
    WARN( "requests: " << requests.pairs << " pairs, slowest "
                       << timestamps::formatDuration( requests.slowest.front().us ).toStdString()
                       << ", median " << timestamps::formatDuration( requests.p50Us ).toStdString() );
    const auto launches = measureDurations( *logcat->logData(),
                                            { "START u0 \\{cmp=com\\.demo/\\.(\\w+)\\}",
                                              "Displayed com\\.demo/\\.(\\w+)", false, {} },
                                            cancel, {} );
    CHECK( launches.pairs == 5 );
    CHECK( launches.slowest.front().us == 4200000 );
    CHECK( launches.slowest.front().key == "PayActivity" );

    // 4. Rules
    QFile rulesFile( demo + "/rules.json" );
    REQUIRE( rulesFile.open( QIODevice::ReadOnly ) );
    const auto set = parseRules( rulesFile.readAll() );
    REQUIRE( set.error.isEmpty() );
    const auto badRules = runRules( *bad->logData(), set.rules, cancel, {} );
    const auto goodRules = runRules( *good->logData(), set.rules, cancel, {} );
    for ( size_t r = 0; r < set.rules.size(); ++r ) {
        WARN( set.rules[ r ].name.toStdString() << ": bad " << badRules.totalFindings[ r ]
                                                << ", good " << goodRules.totalFindings[ r ] );
    }
    CHECK( badRules.totalFindings[ 0 ] == 1 );
    CHECK( badRules.totalFindings[ 1 ] >= 1 );
    CHECK( badRules.totalFindings[ 2 ] == 3 );
    CHECK( badRules.totalFindings[ 3 ] == 1 );
    CHECK( badRules.totalFindings[ 4 ] == 1 );
    CHECK( goodRules.totalFindings[ 0 ] == 0 );
    CHECK( goodRules.totalFindings[ 1 ] == 0 );
    CHECK( goodRules.totalFindings[ 2 ] == 0 );
    CHECK( goodRules.totalFindings[ 3 ] == 0 );
    CHECK( goodRules.totalFindings[ 4 ] == 0 );
    CHECK( badRules.totalFindings[ 5 ] >= 1 );
    CHECK( goodRules.totalFindings[ 5 ] == 0 );
    CHECK( badRules.totalFindings[ 6 ] == 1 );
    const auto logcatRules = runRules( *logcat->logData(), set.rules, cancel, {} );
    CHECK( logcatRules.totalFindings[ 7 ] == 1 );

    // 5. Compare
    const auto badTable = templates::build( *bad->logData(), {}, cancel, {} );
    const auto goodTable = templates::build( *good->logData(), {}, cancel, {} );
    const auto compared = templates::compare( badTable, &goodTable, false );
    for ( size_t i = 0; i < compared.size() && i < 12; ++i ) {
        WARN( static_cast<int>( compared[ i ].status )
              << " " << compared[ i ].firstCount << "/" << compared[ i ].secondCount << " "
              << compared[ i ].text.toStdString() );
    }
}

TEST_CASE( "Main view painting of a large file", "[ui][.benchmark]" )
{
    // FLOGG_BENCH_FILE=<log> klogg_itests "Main view painting of a large file"
    const auto fileName = qEnvironmentVariable( "FLOGG_BENCH_FILE" );
    if ( fileName.isEmpty() ) {
        WARN( "FLOGG_BENCH_FILE is not set" );
        return;
    }
    Session session;
    std::unique_ptr<CrawlerWidget> crawler( static_cast<CrawlerWidget*>(
        session.open( fileName, []() { return new CrawlerWidget(); } ) ) );
    for ( int i = 0; i < 600 && crawler->isLoading(); ++i ) {
        QTest::qWait( 100 );
    }
    REQUIRE( !crawler->isLoading() );
    crawler->resize( 1400, 1100 );
    crawler->show();
    crawler->grab();
    auto* view = crawler->findChild<AbstractLogView*>();
    REQUIRE( view );
    // FLOGG_BENCH_TIME_COLUMNS=1: with the time columns, from a reference line
    auto& config = Configuration::get();
    const auto previousColumns = config.timeColumns( false );
    const auto timeColumns = !qEnvironmentVariableIsEmpty( "FLOGG_BENCH_TIME_COLUMNS" );
    if ( timeColumns ) {
        config.setTimeColumns( false, Configuration::TimeFromReference
                                          | Configuration::TimeFromPrevious );
        crawler->toggleTimeReference( 0_lnum );
    }
    QElapsedTimer timer;
    timer.start();
    constexpr int Paints = 100;
    for ( int i = 0; i < Paints; ++i ) {
        // A new screen each time: the text area is drawn again
        view->selectAndDisplayLine( LineNumber( static_cast<uint64_t>( i ) * 1000 ) );
        view->viewport()->grab();
    }
    WARN( "a screen of the main view drawn" << ( timeColumns ? " with the time columns" : "" )
                                             << ": "
                                             << static_cast<double>( timer.nsecsElapsed() ) / Paints
                                                    / 1000
                                             << " us" );
    config.setTimeColumns( false, previousColumns );
}
