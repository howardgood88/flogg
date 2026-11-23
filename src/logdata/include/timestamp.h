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
#include <vector>

#include <QString>
#include <QStringList>
#include <QStringView>
#include <QStringList>

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
    // The formats of the user come after, see setCustomFormats()
    FirstCustom = 100,
};

QString formatName( Format format );

// The built-in formats, with an example of each
struct FormatInfo {
    Format format;
    QString name;
    QString example;
};
std::vector<FormatInfo> builtInFormats();

// Formats of the user, looked for before the built-in ones, written like
// "yyyy/MM/dd HH:mm:ss.f": yyyy, yy, MM, M, MMM (Jan...), dd, d, HH, H, mm, ss, f (digits after
// the seconds), any other character as it is. The time (H and mm) is needed, the date is not.
// Thread safe: the analyses parse lines in several threads.
void setCustomFormats( const QStringList& patterns );
QStringList customFormats();
// Why the pattern can not be used, empty when it can
QString checkCustomFormat( const QString& pattern );
inline Format customFormat( int index )
{
    return static_cast<Format>( static_cast<int>( Format::FirstCustom ) + index );
}
inline bool isCustom( Format format )
{
    return static_cast<int>( format ) >= static_cast<int>( Format::FirstCustom );
}

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
        if ( isCustom( format ) ) {
            return customDate;
        }
        return format != Format::TimeOfDay && format != Format::Kernel;
    }
    bool hasYear() const;

    // Set for Syslog when a year follows the day
    bool syslogYear = false;
    // What a format of the user has
    bool customDate = false;
    bool customYear = false;
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

// The time stamps of the line in the order they are written
std::vector<Timestamp> inOrder( const LineTimes& times );
// The time stamp of the format, if the line has one
std::optional<Timestamp> ofFormat( const LineTimes& times, Format format );

// By default the format of the first time stamp of the first line is compared (the same
// format must be in the second line); `format` asks for another one, such as the time since
// boot of a line that starts with the wall clock. Lines of different formats are not
// compared.
Difference difference( const LineTimes& first, const LineTimes& second,
                       std::optional<Format> format = {} );

// The timestamp of the line in the same format and clock as `reference`
std::optional<Timestamp> sameKind( const LineTimes& times, const Timestamp& reference );

// "+1.234 s", "-850 ms", "+2 h 03 m 04.500 s"; fractionDigits limits the precision shown
QString formatDuration( int64_t us, int fractionDigits = 3, bool withSign = true );

// A duration typed by the user: "250 ms", "1.5 s", "2 min", "1 h", "500 us" (or µs); a
// number alone is in milliseconds. Empty unless a time above zero, up to a year.
std::optional<int64_t> parseDuration( QStringView text );
// The duration in the largest unit it fills: "250 ms", "1.5 s", "2 min"
QString durationText( int64_t us );

// "2026-11-22 14:32:05.120", "11-22 14:32:05.120", "12345.678901 s since boot"
QString formatTimestamp( const Timestamp& timestamp );

} // namespace timestamps

#endif
