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

#ifndef FLOGG_TIMESTAMP_H
#define FLOGG_TIMESTAMP_H

#include <cstdint>
#include <optional>

#include <QString>
#include <QStringList>
#include <QStringView>

// Time stamps of single lines, parsed on demand (two selected lines, the candidate lines
// of an analysis): nothing is detected or indexed for the whole file. Each line is parsed
// on its own, so lines out of order or a format change in the file do not matter.
namespace timestamps {

enum class Format {
    DateTime,  // 2026-11-22 14:32:05.120, 2026/11/22T14:32:05Z, with an optional zone
    MonthDay,  // 11-22 14:32:05.120 (Android logcat), no year
    Syslog,    // Nov 22 14:32:05, no year unless one follows the day
    Apache,    // 22/Nov/2026:14:32:05 +0800
    TimeOfDay, // 14:32:05.120, no date
    Epoch,     // 1763822400, 1763822400123 (ms), 1763822400.123 at the start of the line
    Kernel,    // [12345.678901]: seconds since boot (dmesg), a monotonic clock
};

QString formatName( Format format );

struct Timestamp {
    // Microseconds: since 1970 for the wall clock (the zone applied when there is one, the
    // time read as UTC otherwise), since boot for the monotonic clock
    int64_t us = 0;
    Format format = Format::DateTime;
    bool hasZone = false;
    // Offset of the zone east of UTC, already taken out of `us`
    int64_t zoneUs = 0;
    // Digits after the seconds (0 to 9), to show the difference at that precision
    int fractionDigits = 0;
    // Position and length in the line
    int start = 0;
    int length = 0;

    bool isMonotonic() const
    {
        return format == Format::Kernel;
    }
    bool hasDate() const
    {
        return format != Format::TimeOfDay && format != Format::Kernel;
    }
    bool hasYear() const;

    // Set for Syslog when a year follows the day
    bool syslogYear = false;
};

struct LineTimes {
    // The leftmost wall clock time of the start of the line, and a monotonic one
    std::optional<Timestamp> wall;
    std::optional<Timestamp> monotonic;

    bool isEmpty() const
    {
        return !wall && !monotonic;
    }
};

// Looks at the first 160 characters of the line. A few microseconds, no allocation.
LineTimes parseLine( QStringView line );

struct Difference {
    // Second line minus first line, empty when the times can not be compared
    std::optional<int64_t> us;
    // Why there is no difference
    QString error;
    // The times compared (both of the same format and clock)
    std::optional<Timestamp> first;
    std::optional<Timestamp> second;
    // Assumptions made: a year or a midnight crossed, zones ignored
    QStringList notes;
};

// The monotonic clock is preferred when both lines have one: it does not jump when the
// wall clock is set. Lines of different formats are not compared.
Difference difference( const LineTimes& first, const LineTimes& second );

// The timestamp of the line in the same format and clock as `reference`
std::optional<Timestamp> sameKind( const LineTimes& times, const Timestamp& reference );

// "+1.234 s", "-850 ms", "+2 h 03 m 04.500 s"; fractionDigits limits the precision shown
QString formatDuration( int64_t us, int fractionDigits = 3, bool withSign = true );

// "2026-11-22 14:32:05.120", "11-22 14:32:05.120", "12345.678901 s since boot"
QString formatTimestamp( const Timestamp& timestamp );

} // namespace timestamps

#endif
