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

#include <atomic>
#include <thread>

#include "timestamp.h"

using namespace timestamps;

namespace {
std::optional<int64_t> delta( const QString& first, const QString& second )
{
    return difference( parseLine( first ), parseLine( second ) ).us;
}
} // namespace

TEST_CASE( "Time stamps of lines", "[timestamp]" )
{
    SECTION( "Date and time" )
    {
        const auto times = parseLine( u"2026-11-22 14:32:05.120 INFO main started" );
        REQUIRE( times.wall );
        CHECK( times.wall->format == Format::DateTime );
        CHECK( times.wall->fractionDigits == 3 );
        CHECK( times.wall->start == 0 );
        CHECK( times.wall->length == 23 );
        CHECK( formatTimestamp( *times.wall ) == "2026-11-22 14:32:05.120" );
        CHECK( !times.monotonic );

        CHECK( *delta( "2026-11-22 14:32:05.120 a", "2026-11-22 14:32:06.354 b" ) == 1234000 );
        CHECK( *delta( "2026-11-22T23:59:59Z a", "2026-11-23T00:00:01Z b" ) == 2000000 );
        CHECK( *delta( "[2026/02/28 23:00:00] a", "[2026/03/01 01:00:00] b" )
               == 2 * 3600 * 1000000LL );
        // Comma before the milliseconds (log4j)
        CHECK( *delta( "2026-11-22 14:32:05,100 a", "2026-11-22 14:32:05,350 b" ) == 250000 );
    }

    SECTION( "Zones" )
    {
        CHECK( *delta( "2026-11-22T14:00:00+08:00 a", "2026-11-22T07:00:00Z b" ) == 3600000000LL );
        CHECK( *delta( "2026-11-22 14:00:00 +0800 a", "2026-11-22 15:00:00 +0900 b" ) == 0 );

        const auto mixed = difference( parseLine( u"2026-11-22T14:00:00+08:00 a" ),
                                       parseLine( u"2026-11-22T14:00:01 b" ) );
        REQUIRE( mixed.us );
        CHECK( *mixed.us == 1000000 );
        CHECK( mixed.notes.size() == 1 );
    }

    SECTION( "Android logcat, no year" )
    {
        const auto times = parseLine( u"11-22 14:32:05.120  1234  1240 I ActivityManager: x" );
        REQUIRE( times.wall );
        CHECK( times.wall->format == Format::MonthDay );
        CHECK( !times.wall->hasYear() );
        CHECK( formatTimestamp( *times.wall ) == "11-22 14:32:05.120" );

        // A new year between the lines
        const auto newYear = difference( parseLine( u"12-31 23:59:59.500 a" ),
                                         parseLine( u"01-01 00:00:00.500 b" ) );
        REQUIRE( newYear.us );
        CHECK( *newYear.us == 1000000 );
        CHECK( newYear.notes.size() == 1 );
    }

    SECTION( "Syslog" )
    {
        CHECK( *delta( "Nov 22 14:32:05 host sshd[12]: a", "Nov 22 14:33:05 host sshd[12]: b" )
               == 60000000 );
        CHECK( *delta( "Nov  2 01:00:00 host a", "Nov  2 01:00:30 host b" ) == 30000000 );
        const auto withYear = parseLine( u"Nov 22 2026 14:32:05 host a" );
        REQUIRE( withYear.wall );
        CHECK( withYear.wall->hasYear() );
        // Not a month name at the start of a word
        CHECK( parseLine( u"Novel 22 14:32:05" ).wall->format == Format::TimeOfDay );
    }

    SECTION( "Web server" )
    {
        const auto times
            = parseLine( u"10.0.0.5 - - [22/Nov/2026:14:32:05 +0800] \"GET / HTTP/1.1\" 200" );
        REQUIRE( times.wall );
        CHECK( times.wall->format == Format::Apache );
        CHECK( times.wall->hasZone );
        CHECK( formatTimestamp( *times.wall ) == "2026-11-22 14:32:05 +08:00" );
    }

    SECTION( "Time of day" )
    {
        const auto times = parseLine( u"[main] 14:32:05.123456 started" );
        REQUIRE( times.wall );
        CHECK( times.wall->format == Format::TimeOfDay );
        CHECK( times.wall->fractionDigits == 6 );
        // Midnight crossed
        const auto midnight = difference( parseLine( u"23:59:59 a" ), parseLine( u"00:00:01 b" ) );
        REQUIRE( midnight.us );
        CHECK( *midnight.us == 2000000 );
        CHECK( !midnight.notes.isEmpty() );
        // Not times
        CHECK( !parseLine( u"version 1:23:45:67" ).wall );
        CHECK( !parseLine( u"ratio 12:34:567" ).wall );
        CHECK( !parseLine( u"id=a14:32:05" ).wall );
    }

    SECTION( "Unix time" )
    {
        CHECK( *delta( "1763822400 a", "1763822401 b" ) == 1000000 );
        CHECK( *delta( "1763822400123 a", "1763822400456 b" ) == 333000 );
        CHECK( *delta( "[1763822400.5] a", "[1763822401.0] b" ) == 500000 );
        // Only at the start, and only plausible values
        CHECK( !parseLine( u"user 1763822400 logged" ).wall );
        CHECK( !parseLine( u"1234567890123456789 x" ).wall );
        CHECK( !parseLine( u"0000000001 x" ).wall );
    }

    SECTION( "Monotonic clock preferred" )
    {
        const auto first = parseLine( u"[ 5.000100] 2026-11-22 14:00:00 a" );
        REQUIRE( first.monotonic );
        REQUIRE( first.wall );
        // The wall clock was set back between the lines: the time since boot is right
        const auto result = difference( first, parseLine( u"[ 7.500100] 2026-11-22 13:00:00 b" ) );
        REQUIRE( result.us );
        CHECK( *result.us == 2500000 );
        CHECK( result.first->isMonotonic() );
        // Brackets that are not times since boot
        CHECK( !parseLine( u"[1.2] version" ).monotonic );
        CHECK( !parseLine( u"[ab.123]" ).monotonic );
    }

    SECTION( "Lines that can not be compared" )
    {
        auto result = difference( parseLine( u"no time here" ),
                                  parseLine( u"2026-11-22 14:00:00 b" ) );
        CHECK( !result.us );
        CHECK( !result.error.isEmpty() );

        result = difference( parseLine( u"2026-11-22 14:00:00 a" ), parseLine( u"14:00:01 b" ) );
        CHECK( !result.us );
        CHECK( result.error.contains( "different" ) );

        result = difference( parseLine( u"[ 5.000100] a" ), parseLine( u"2026-11-22 14:00:00 b" ) );
        CHECK( !result.us );
    }

    SECTION( "Negative differences" )
    {
        CHECK( *delta( "2026-11-22 14:00:01 a", "2026-11-22 14:00:00 b" ) == -1000000 );
    }

    SECTION( "Durations" )
    {
        CHECK( formatDuration( 1234000 ) == "+1.234 s" );
        CHECK( formatDuration( -850000 ) == "-850 ms" );
        CHECK( formatDuration( 850123, 6 ) == "+850.123 ms" );
        CHECK( formatDuration( 12, 6 ) == "+12 µs" );
        CHECK( formatDuration( 3000000, 0 ) == "+3 s" );
        CHECK( formatDuration( ( 2 * 3600 + 3 * 60 + 4 ) * 1000000LL + 500000 )
               == "+2 h 03 m 04.500 s" );

        // Typed by the user, shown back
        CHECK( parseDuration( u"250 ms" ) == 250000 );
        CHECK( parseDuration( u"250" ) == 250000 );
        CHECK( parseDuration( u" 1.5s " ) == 1500000 );
        CHECK( parseDuration( u"1,5 S" ) == 1500000 );
        CHECK( parseDuration( u"2 min" ) == 120000000 );
        CHECK( parseDuration( u"2m" ) == 120000000 );
        CHECK( parseDuration( u"1 h" ) == 3600000000LL );
        CHECK( parseDuration( u"500 us" ) == 500 );
        CHECK( parseDuration( u"500 µs" ) == 500 );
        CHECK( parseDuration( u".5 s" ) == 500000 );
        CHECK( !parseDuration( u"" ) );
        CHECK( !parseDuration( u"0" ) );
        CHECK( !parseDuration( u"-1 s" ) );
        CHECK( !parseDuration( u"1 day" ) );
        CHECK( !parseDuration( u"fast" ) );
        CHECK( !parseDuration( u"2 years" ) );
        CHECK( durationText( 250000 ) == "250 ms" );
        CHECK( durationText( 1500000 ) == "1.5 s" );
        CHECK( durationText( 120000000 ) == "2 min" );
        CHECK( durationText( 3600000000LL ) == "1 h" );
        CHECK( durationText( 500 ) == "500 µs" );
        CHECK( formatDuration( ( 86400 + 3600 ) * 1000000LL, 0 ) == "+1 d 01 h 00 m 00 s" );
        CHECK( formatDuration( 61 * 1000000LL, 0, false ) == "1 m 01 s" );
    }

    SECTION( "Fast enough for the candidate lines of an analysis" )
    {
        const QString line = "11-22 14:32:05.120  1234  1240 I ActivityManager: Start proc "
                             "12345:com.example/u0a123 for activity";
        const QString noTime( 200, u'x' );
        QElapsedTimer timer;
        timer.start();
        int64_t sum = 0;
        for ( int i = 0; i < 100000; ++i ) {
            sum += parseLine( line ).wall->us;
            sum += parseLine( noTime ).isEmpty() ? 1 : 0;
        }
        CHECK( sum != 0 );
        // 200 000 lines, about 1 µs each in a release build
        CHECK( timer.elapsed() < 2000 );
    }
}

TEST_CASE( "Time stamps compared: the first of the line or a chosen one", "[timestamp]" )
{
    // A driver log: the wall clock first, the time since boot after it; the wall clock is set
    // back 2 hours between the lines
    const auto first = parseLine( u"2026-11-20 06:11:55.776556,[ 2835.295153] main sync" );
    const auto last = parseLine( u"2026-11-20 04:11:45.000000,[ 2838.392001] main scan" );

    const auto formats = inOrder( first );
    REQUIRE( formats.size() == 2 );
    CHECK( formats[ 0 ].format == Format::DateTime );
    CHECK( formats[ 1 ].format == Format::Kernel );

    // By default the first time stamp of the line
    const auto byDefault = difference( first, last );
    REQUIRE( byDefault.us );
    CHECK( byDefault.first->format == Format::DateTime );
    CHECK( *byDefault.us < 0 );

    // Chosen: the time since boot
    const auto sinceBoot = difference( first, last, Format::Kernel );
    REQUIRE( sinceBoot.us );
    CHECK( *sinceBoot.us == 3096848 );

    // A format a line does not have
    const auto missing = difference( first, parseLine( u"2026-11-20 06:12:00 no boot time" ),
                                     Format::Kernel );
    CHECK( !missing.us );
    CHECK( missing.error.contains( "last line" ) );

    // The kernel time first in the line
    CHECK( difference( parseLine( u"[ 5.000100] 2026-11-22 14:00:00 a" ),
                       parseLine( u"[ 7.500100] 2026-11-22 13:00:00 b" ) )
               .first->isMonotonic() );
}

TEST_CASE( "Time formats of the user", "[timestamp]" )
{
    CHECK( checkCustomFormat( "yyyy/MM/dd HH:mm:ss.f" ).isEmpty() );
    CHECK( !checkCustomFormat( "yyyy-MM-dd" ).isEmpty() );
    CHECK( !checkCustomFormat( "" ).isEmpty() );

    // Not a built-in format: a day first, a dot between the seconds and a space
    const QString first = "<I> 22.11.2026 at 14:32:05 . 120 main started";
    const QString last = "<I> 22.11.2026 at 14:32:07 . 370 main done";
    // Without the format only the time of day is seen
    CHECK( parseLine( first ).wall->format == Format::TimeOfDay );

    setCustomFormats( { "dd.MM.yyyy at HH:mm:ss . fff" } );
    const auto times = parseLine( first );
    REQUIRE( times.wall );
    CHECK( times.wall->format == customFormat( 0 ) );
    CHECK( times.wall->hasYear() );
    CHECK( times.wall->start == 4 );
    CHECK( formatName( times.wall->format ) == "dd.MM.yyyy at HH:mm:ss . fff" );
    CHECK( formatTimestamp( *times.wall ) == "2026-11-22 14:32:05.120" );
    const auto result = difference( times, parseLine( last ) );
    REQUIRE( result.us );
    CHECK( *result.us == 2250000 );

    // Month names, no year, no date
    setCustomFormats( { "MMM d HH:mm", "H.mm.ss" } );
    const auto named = parseLine( u"x Nov 2 14:32 y" );
    REQUIRE( named.wall );
    CHECK( named.wall->format == customFormat( 0 ) );
    CHECK( !named.wall->hasYear() );
    const auto noDate = difference( parseLine( u"at 23.59.58 a" ), parseLine( u"at 0.00.01 b" ) );
    REQUIRE( noDate.us );
    CHECK( *noDate.us == 3000000 );

    // Several threads at once
    std::vector<std::thread> threads;
    std::atomic<int> found{ 0 };
    for ( int t = 0; t < 4; ++t ) {
        threads.emplace_back( [ & ] {
            for ( int i = 0; i < 1000; ++i ) {
                found += parseLine( u"x Nov 2 14:32 y" ).wall ? 1 : 0;
            }
        } );
    }
    for ( auto& thread : threads ) {
        thread.join();
    }
    CHECK( found == 4000 );

    setCustomFormats( {} );
    CHECK( parseLine( u"x Nov 2 14:32 y" ).wall->format != customFormat( 0 ) );
    CHECK( builtInFormats().size() == 7 );
}
