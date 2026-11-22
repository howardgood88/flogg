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

#include "timestamp.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <limits>

#include <QCoreApplication>
#include <QMutex>
#include <QRegularExpression>
#include <QSet>

namespace timestamps {

namespace {

constexpr int64_t UsPerSecond = 1000000;
constexpr int64_t UsPerDay = 86400 * UsPerSecond;
// Lines without a year are read in a leap year, so that 02-29 is a date
constexpr int NoYear = 2000;
constexpr int ScannedLength = 160;

QString translate( const char* text )
{
    return QCoreApplication::translate( "timestamps", text );
}

// Days since 1970-01-01 of a date of the proleptic Gregorian calendar
int64_t daysFromCivil( int64_t y, int m, int d )
{
    y -= m <= 2 ? 1 : 0;
    const int64_t era = ( y >= 0 ? y : y - 399 ) / 400;
    const auto yoe = static_cast<int64_t>( y - era * 400 );
    const int64_t doy = ( 153 * ( m + ( m > 2 ? -3 : 9 ) ) + 2 ) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

struct Civil {
    int64_t year;
    int month;
    int day;
};

Civil civilFromDays( int64_t z )
{
    z += 719468;
    const int64_t era = ( z >= 0 ? z : z - 146096 ) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = ( doe - doe / 1460 + doe / 36524 - doe / 146096 ) / 365;
    const int64_t doy = doe - ( 365 * yoe + yoe / 4 - yoe / 100 );
    const int64_t mp = ( 5 * doy + 2 ) / 153;
    const auto day = static_cast<int>( doy - ( 153 * mp + 2 ) / 5 + 1 );
    const auto month = static_cast<int>( mp < 10 ? mp + 3 : mp - 9 );
    return { yoe + era * 400 + ( month <= 2 ? 1 : 0 ), month, day };
}

bool isDigit( QStringView s, int i )
{
    return i >= 0 && i < s.size() && s[ i ] >= u'0' && s[ i ] <= u'9';
}

bool isChar( QStringView s, int i, char16_t c )
{
    return i >= 0 && i < s.size() && s[ i ] == c;
}

// Exactly n digits at i
std::optional<int> fixedDigits( QStringView s, int i, int n )
{
    if ( i < 0 || i + n > s.size() ) {
        return {};
    }
    int value = 0;
    for ( int k = 0; k < n; ++k ) {
        if ( !isDigit( s, i + k ) ) {
            return {};
        }
        value = value * 10 + ( s[ i + k ].unicode() - u'0' );
    }
    return value;
}

// 1 to maxDigits digits at i, the count in `count`
std::optional<int64_t> someDigits( QStringView s, int i, int maxDigits, int& count )
{
    int64_t value = 0;
    count = 0;
    while ( count < maxDigits && isDigit( s, i + count ) ) {
        value = value * 10 + ( s[ i + count ].unicode() - u'0' );
        ++count;
    }
    if ( count == 0 ) {
        return {};
    }
    return value;
}

int64_t fractionToUs( int64_t fraction, int digits )
{
    // Digits after the sixth are dropped
    while ( digits > 6 ) {
        fraction /= 10;
        --digits;
    }
    while ( digits < 6 ) {
        fraction *= 10;
        ++digits;
    }
    return fraction;
}

struct Parsed {
    int end = 0;
    int64_t us = 0;
    int fractionDigits = 0;
};

// Optional .123 or ,123 after the seconds
void parseFraction( QStringView s, Parsed& parsed )
{
    if ( isChar( s, parsed.end, u'.' ) || isChar( s, parsed.end, u',' ) ) {
        int count = 0;
        const auto fraction = someDigits( s, parsed.end + 1, 9, count );
        if ( fraction ) {
            parsed.us += fractionToUs( *fraction, count );
            parsed.fractionDigits = count;
            parsed.end += 1 + count;
        }
    }
}

// hh:mm:ss[.fff], the microseconds of the day
std::optional<Parsed> parseTime( QStringView s, int i )
{
    int hourDigits = 0;
    const auto hour = someDigits( s, i, 2, hourDigits );
    if ( !hour || *hour > 23 || !isChar( s, i + hourDigits, u':' ) ) {
        return {};
    }
    const auto minute = fixedDigits( s, i + hourDigits + 1, 2 );
    if ( !minute || *minute > 59 || !isChar( s, i + hourDigits + 3, u':' ) ) {
        return {};
    }
    const auto second = fixedDigits( s, i + hourDigits + 4, 2 );
    if ( !second || *second > 60 ) {
        return {};
    }
    Parsed parsed;
    parsed.end = i + hourDigits + 6;
    parsed.us = ( ( *hour * 60 + *minute ) * 60 + *second ) * UsPerSecond;
    parseFraction( s, parsed );
    // 1:23:45:67 or 12:34:567 are not times
    if ( isDigit( s, parsed.end ) || isChar( s, parsed.end, u':' ) ) {
        return {};
    }
    return parsed;
}

// Z, +08:00, +0800 or +08 after the time, also after one space; the offset east of UTC
std::optional<int64_t> parseZone( QStringView s, int& end )
{
    if ( isChar( s, end, u'Z' ) && !( end + 1 < s.size() && s[ end + 1 ].isLetterOrNumber() ) ) {
        end += 1;
        return 0;
    }
    auto i = end;
    if ( isChar( s, i, u' ' ) ) {
        ++i;
    }
    if ( !isChar( s, i, u'+' ) && !isChar( s, i, u'-' ) ) {
        return {};
    }
    const auto sign = s[ i ] == u'-' ? -1 : 1;
    const auto hours = fixedDigits( s, i + 1, 2 );
    if ( !hours || *hours > 14 ) {
        return {};
    }
    auto next = i + 3;
    std::optional<int> minutes = 0;
    if ( isChar( s, next, u':' ) ) {
        minutes = fixedDigits( s, next + 1, 2 );
        next += 3;
    }
    else if ( isDigit( s, next ) ) {
        minutes = fixedDigits( s, next, 2 );
        next += 2;
    }
    else if ( i != end ) {
        // "14:32:05 -08" with a space is more likely something else
        return {};
    }
    if ( !minutes || *minutes > 59 || isDigit( s, next ) ) {
        return {};
    }
    end = next;
    return sign * ( *hours * 60 + *minutes ) * 60 * UsPerSecond;
}

std::optional<int> monthFromName( QStringView s, int i )
{
    static constexpr std::array<const char16_t*, 12> names
        = { u"Jan", u"Feb", u"Mar", u"Apr", u"May", u"Jun",
            u"Jul", u"Aug", u"Sep", u"Oct", u"Nov", u"Dec" };
    if ( i + 3 > s.size() ) {
        return {};
    }
    for ( size_t m = 0; m < names.size(); ++m ) {
        if ( s[ i ] == names[ m ][ 0 ] && s[ i + 1 ] == names[ m ][ 1 ]
             && s[ i + 2 ] == names[ m ][ 2 ] ) {
            return static_cast<int>( m ) + 1;
        }
    }
    return {};
}

bool validDate( int month, int day )
{
    return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

Timestamp makeWall( Format format, int start, const Parsed& time, int64_t days,
                    std::optional<int64_t> zone )
{
    Timestamp timestamp;
    timestamp.format = format;
    timestamp.start = start;
    timestamp.length = time.end - start;
    timestamp.fractionDigits = time.fractionDigits;
    timestamp.us = days * UsPerDay + time.us;
    if ( zone ) {
        timestamp.hasZone = true;
        timestamp.zoneUs = *zone;
        timestamp.us -= *zone;
    }
    return timestamp;
}

// 2026-11-22 14:32:05.120, 2026/11/22T14:32:05Z
std::optional<Timestamp> parseDateTime( QStringView s, int i )
{
    const auto year = fixedDigits( s, i, 4 );
    if ( !year || *year < 1970 || *year > 2200 ) {
        return {};
    }
    const auto separator = i + 4 < s.size() ? s[ i + 4 ] : QChar();
    if ( separator != u'-' && separator != u'/' && separator != u'.' ) {
        return {};
    }
    const auto month = fixedDigits( s, i + 5, 2 );
    const auto day = fixedDigits( s, i + 8, 2 );
    if ( !month || !day || !isChar( s, i + 7, separator.unicode() )
         || !validDate( *month, *day ) ) {
        return {};
    }
    if ( !isChar( s, i + 10, u' ' ) && !isChar( s, i + 10, u'T' )
         && !isChar( s, i + 10, u'_' ) ) {
        return {};
    }
    auto time = parseTime( s, i + 11 );
    if ( !time ) {
        return {};
    }
    const auto zone = parseZone( s, time->end );
    return makeWall( Format::DateTime, i, *time, daysFromCivil( *year, *month, *day ), zone );
}

// 11-22 14:32:05.120
std::optional<Timestamp> parseMonthDay( QStringView s, int i )
{
    const auto month = fixedDigits( s, i, 2 );
    const auto day = fixedDigits( s, i + 3, 2 );
    if ( !month || !day || !isChar( s, i + 2, u'-' ) || !validDate( *month, *day )
         || !isChar( s, i + 5, u' ' ) ) {
        return {};
    }
    const auto time = parseTime( s, i + 6 );
    if ( !time ) {
        return {};
    }
    return makeWall( Format::MonthDay, i, *time, daysFromCivil( NoYear, *month, *day ), {} );
}

// Nov 22 14:32:05, Nov  2 14:32:05, Nov 22 2026 14:32:05
std::optional<Timestamp> parseSyslog( QStringView s, int i )
{
    const auto month = monthFromName( s, i );
    if ( !month || !isChar( s, i + 3, u' ' ) ) {
        return {};
    }
    auto next = i + 4;
    if ( isChar( s, next, u' ' ) ) {
        ++next;
    }
    int dayDigits = 0;
    const auto day = someDigits( s, next, 2, dayDigits );
    if ( !day || !validDate( *month, static_cast<int>( *day ) )
         || !isChar( s, next + dayDigits, u' ' ) ) {
        return {};
    }
    next += dayDigits + 1;
    std::optional<int> year;
    if ( const auto maybeYear = fixedDigits( s, next, 4 );
         maybeYear && isChar( s, next + 4, u' ' ) && *maybeYear >= 1970 ) {
        year = maybeYear;
        next += 5;
    }
    const auto time = parseTime( s, next );
    if ( !time ) {
        return {};
    }
    auto timestamp = makeWall( Format::Syslog, i, *time,
                               daysFromCivil( year.value_or( NoYear ), *month,
                                              static_cast<int>( *day ) ),
                               {} );
    timestamp.syslogYear = year.has_value();
    return timestamp;
}

// 22/Nov/2026:14:32:05 +0800
std::optional<Timestamp> parseApache( QStringView s, int i )
{
    const auto day = fixedDigits( s, i, 2 );
    if ( !day || !isChar( s, i + 2, u'/' ) ) {
        return {};
    }
    const auto month = monthFromName( s, i + 3 );
    if ( !month || !isChar( s, i + 6, u'/' ) ) {
        return {};
    }
    const auto year = fixedDigits( s, i + 7, 4 );
    if ( !year || !isChar( s, i + 11, u':' ) || !validDate( *month, *day ) ) {
        return {};
    }
    auto time = parseTime( s, i + 12 );
    if ( !time ) {
        return {};
    }
    const auto zone = parseZone( s, time->end );
    return makeWall( Format::Apache, i, *time, daysFromCivil( *year, *month, *day ), zone );
}

std::optional<Timestamp> parseTimeOfDay( QStringView s, int i )
{
    const auto time = parseTime( s, i );
    if ( !time ) {
        return {};
    }
    return makeWall( Format::TimeOfDay, i, *time, 0, {} );
}

// 1763822400, 1763822400123, 1763822400123456 or 1763822400.123, between 2000 and 2100
std::optional<Timestamp> parseEpoch( QStringView s, int i )
{
    int count = 0;
    const auto value = someDigits( s, i, 17, count );
    if ( !value || isDigit( s, i + count ) ) {
        return {};
    }
    Parsed parsed;
    parsed.end = i + count;
    if ( count == 10 ) {
        parsed.us = *value * UsPerSecond;
        parseFraction( s, parsed );
    }
    else if ( count == 13 ) {
        parsed.us = *value * 1000;
        parsed.fractionDigits = 3;
    }
    else if ( count == 16 ) {
        parsed.us = *value;
        parsed.fractionDigits = 6;
    }
    else {
        return {};
    }
    const auto seconds = parsed.us / UsPerSecond;
    if ( seconds < 946684800 || seconds > 4102444800 ) {
        return {};
    }
    if ( parsed.end < s.size() && s[ parsed.end ].isLetter() ) {
        return {};
    }
    return makeWall( Format::Epoch, i, parsed, 0, {} );
}

// [12345.678901] or [    5.123456]
std::optional<Timestamp> parseKernel( QStringView s, int i )
{
    if ( !isChar( s, i, u'[' ) ) {
        return {};
    }
    auto next = i + 1;
    while ( isChar( s, next, u' ' ) ) {
        ++next;
    }
    int secondDigits = 0;
    const auto seconds = someDigits( s, next, 9, secondDigits );
    if ( !seconds || !isChar( s, next + secondDigits, u'.' ) ) {
        return {};
    }
    int fractionDigits = 0;
    const auto fraction = someDigits( s, next + secondDigits + 1, 9, fractionDigits );
    const auto end = next + secondDigits + 1 + fractionDigits;
    if ( !fraction || fractionDigits < 3 || !isChar( s, end, u']' ) ) {
        return {};
    }
    Timestamp timestamp;
    timestamp.format = Format::Kernel;
    timestamp.start = i;
    timestamp.length = end + 1 - i;
    timestamp.fractionDigits = fractionDigits;
    timestamp.us = *seconds * UsPerSecond + fractionToUs( *fraction, fractionDigits );
    return timestamp;
}

QString twoDigits( int64_t value )
{
    return QString::number( value ).rightJustified( 2, u'0' );
}

QString fractionText( int64_t usOfSecond, int digits )
{
    if ( digits <= 0 ) {
        return {};
    }
    digits = qMin( digits, 6 );
    auto text = QString::number( usOfSecond ).rightJustified( 6, u'0' );
    return "." + text.left( digits );
}

} // namespace

QString formatName( Format format )
{
    if ( isCustom( format ) ) {
        const auto index = static_cast<int>( format ) - static_cast<int>( Format::FirstCustom );
        const auto patterns = customFormats();
        return index < patterns.size() ? patterns[ index ] : translate( "format of the user" );
    }
    switch ( format ) {
    case Format::DateTime:
        return translate( "date and time" );
    case Format::MonthDay:
        return translate( "month-day time (no year)" );
    case Format::Syslog:
        return translate( "syslog" );
    case Format::Apache:
        return translate( "web server" );
    case Format::TimeOfDay:
        return translate( "time of day (no date)" );
    case Format::Epoch:
        return translate( "Unix time" );
    case Format::Kernel:
        return translate( "seconds since boot" );
    }
    return {};
}

bool Timestamp::hasYear() const
{
    if ( isCustom( format ) ) {
        return customYear;
    }
    switch ( format ) {
    case Format::DateTime:
    case Format::Apache:
    case Format::Epoch:
        return true;
    case Format::Syslog:
        return syslogYear;
    default:
        return false;
    }
}

//
// Formats of the user
//

namespace {

struct CustomFormats {
    QStringList patterns;
    uint64_t generation = 0;
};

QMutex& customMutex()
{
    static QMutex mutex;
    return mutex;
}

CustomFormats& customStore()
{
    static CustomFormats store;
    return store;
}

std::atomic<uint64_t> customGeneration{ 0 };

struct Token {
    const char* text;
    const char* regex;
};

// Longest first
constexpr std::array<Token, 12> Tokens = { {
    { "yyyy", "(?<Y>\\d{4})" },
    { "yy", "(?<y>\\d{2})" },
    { "MMM", "(?<N>Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec)" },
    { "MM", "(?<M>\\d{2})" },
    { "M", "(?<M>\\d{1,2})" },
    { "dd", "(?<D>\\d{2})" },
    { "d", "(?<D>\\d{1,2})" },
    { "HH", "(?<h>\\d{2})" },
    { "H", "(?<h>\\d{1,2})" },
    { "mm", "(?<m>\\d{2})" },
    { "ss", "(?<s>\\d{2})" },
    { "f", "(?<f>\\d{1,9})" },
} };

struct Compiled {
    QRegularExpression regex;
    bool year = false;
    bool date = false;
};

Compiled compileCustom( const QString& pattern )
{
    QString regex = "(?<![\\p{L}\\p{N}])";
    QSet<QString> used;
    Compiled compiled;
    for ( int i = 0; i < pattern.size(); ) {
        bool matched = false;
        for ( const auto& token : Tokens ) {
            const auto text = QLatin1String( token.text );
            if ( pattern.mid( i, text.size() ) == text ) {
                // f stands for all the digits, whatever the number of f
                const auto name = text == QLatin1String( "f" ) ? QString( "f" ) : QString( text.front() );
                if ( !used.contains( name ) ) {
                    regex += QLatin1String( token.regex );
                    used.insert( name );
                }
                else {
                    regex += QRegularExpression::escape( text );
                }
                i += static_cast<int>( text.size() );
                while ( text == QLatin1String( "f" ) && i < pattern.size() && pattern[ i ] == u'f' ) {
                    ++i;
                }
                matched = true;
                break;
            }
        }
        if ( !matched ) {
            regex += QRegularExpression::escape( pattern.mid( i, 1 ) );
            ++i;
        }
    }
    regex += "(?![\\p{N}])";
    compiled.regex = QRegularExpression( regex );
    compiled.year = used.contains( "y" );
    compiled.date = ( used.contains( "M" ) || used.contains( "N" ) ) && used.contains( "d" );
    return compiled;
}

// Each thread its own expressions, rebuilt when the formats change
const std::vector<Compiled>& threadCustom()
{
    thread_local uint64_t generation = std::numeric_limits<uint64_t>::max();
    thread_local std::vector<Compiled> compiled;
    const auto current = customGeneration.load();
    if ( generation != current ) {
        QStringList patterns;
        {
            QMutexLocker lock( &customMutex() );
            patterns = customStore().patterns;
        }
        compiled.clear();
        for ( const auto& pattern : patterns ) {
            compiled.push_back( compileCustom( pattern ) );
        }
        generation = current;
    }
    return compiled;
}

std::optional<Timestamp> parseCustom( QStringView line )
{
    const auto& formats = threadCustom();
    std::optional<Timestamp> best;
    for ( size_t index = 0; index < formats.size(); ++index ) {
        const auto& format = formats[ index ];
        if ( !format.regex.isValid() ) {
            continue;
        }
        const auto match = format.regex.matchView( line );
        if ( !match.hasMatch() || ( best && match.capturedStart() >= best->start ) ) {
            continue;
        }
        const auto number = [ &match ]( const QString& name, int fallback ) {
            const auto text = match.capturedView( name );
            return text.isEmpty() ? fallback : text.toInt();
        };
        int month = number( "M", 1 );
        const auto monthName = match.capturedView( QStringLiteral( "N" ) );
        if ( !monthName.isEmpty() ) {
            month = *monthFromName( monthName, 0 );
        }
        auto year = number( "Y", NoYear );
        if ( !match.capturedView( QStringLiteral( "y" ) ).isEmpty() ) {
            year = 2000 + number( "y", 0 );
        }
        const auto day = number( "D", 1 );
        const auto hour = number( "h", 0 );
        const auto minute = number( "m", 0 );
        const auto second = number( "s", 0 );
        if ( !validDate( month, day ) || hour > 23 || minute > 59 || second > 60 ) {
            continue;
        }
        Parsed time;
        time.us = ( ( static_cast<int64_t>( hour ) * 60 + minute ) * 60 + second ) * UsPerSecond;
        const auto fraction = match.capturedView( QStringLiteral( "f" ) );
        if ( !fraction.isEmpty() ) {
            time.fractionDigits = static_cast<int>( fraction.size() );
            time.us += fractionToUs( fraction.toLongLong(), time.fractionDigits );
        }
        time.end = static_cast<int>( match.capturedEnd() );
        const auto start = static_cast<int>( match.capturedStart() );
        auto stamp = makeWall( customFormat( static_cast<int>( index ) ), start, time,
                               format.date ? daysFromCivil( year, month, day ) : 0, {} );
        stamp.customDate = format.date;
        stamp.customYear = format.year && format.date;
        best = stamp;
    }
    return best;
}

} // namespace

void setCustomFormats( const QStringList& patterns )
{
    {
        QMutexLocker lock( &customMutex() );
        customStore().patterns = patterns;
    }
    ++customGeneration;
}

QStringList customFormats()
{
    QMutexLocker lock( &customMutex() );
    return customStore().patterns;
}

QString checkCustomFormat( const QString& pattern )
{
    if ( pattern.trimmed().isEmpty() ) {
        return translate( "The format is empty" );
    }
    const auto compiled = compileCustom( pattern );
    if ( !compiled.regex.isValid() ) {
        return compiled.regex.errorString();
    }
    if ( !compiled.regex.namedCaptureGroups().contains( "h" )
         || !compiled.regex.namedCaptureGroups().contains( "m" ) ) {
        return translate( "The hours (H or HH) and the minutes (mm) are needed" );
    }
    return {};
}

std::vector<FormatInfo> builtInFormats()
{
    return {
        { Format::DateTime, formatName( Format::DateTime ),
          "2026-11-22 14:32:05.120, 2026/11/22T14:32:05Z, 2026-11-22 14:32:05,123 +08:00" },
        { Format::MonthDay, formatName( Format::MonthDay ), "11-22 14:32:05.120 (Android logcat)" },
        { Format::Syslog, formatName( Format::Syslog ), "Nov 22 14:32:05, Nov 22 2026 14:32:05" },
        { Format::Apache, formatName( Format::Apache ), "22/Nov/2026:14:32:05 +0800" },
        { Format::TimeOfDay, formatName( Format::TimeOfDay ), "14:32:05.123456" },
        { Format::Epoch, formatName( Format::Epoch ),
          translate( "1763822400, 1763822400123 (ms), at the start of the line" ) },
        { Format::Kernel, formatName( Format::Kernel ), "[12345.678901] (dmesg, kernel)" },
    };
}

LineTimes parseLine( QStringView line )
{
    LineTimes times;
    const auto length = qMin<int>( static_cast<int>( line.size() ), ScannedLength );
    const auto scanned = line.left( length );

    int firstNonSpace = 0;
    while ( firstNonSpace < length && scanned[ firstNonSpace ].isSpace() ) {
        ++firstNonSpace;
    }

    for ( int i = 0; i < length && ( !times.wall || !times.monotonic ); ++i ) {
        const auto c = scanned[ i ];
        // A time stamp does not start inside a word or a number
        if ( i > 0 && scanned[ i - 1 ].isLetterOrNumber() ) {
            continue;
        }

        if ( !times.monotonic && c == u'[' ) {
            if ( auto kernel = parseKernel( scanned, i ) ) {
                times.monotonic = kernel;
                continue;
            }
        }
        if ( times.wall ) {
            continue;
        }

        std::optional<Timestamp> found;
        if ( c.isDigit() ) {
            found = parseDateTime( scanned, i );
            if ( !found ) {
                found = parseApache( scanned, i );
            }
            if ( !found ) {
                found = parseMonthDay( scanned, i );
            }
            const auto atStart
                = i == firstNonSpace
                  || ( i == firstNonSpace + 1 && scanned[ firstNonSpace ] == u'[' );
            if ( !found && atStart ) {
                found = parseEpoch( scanned, i );
            }
            if ( !found ) {
                found = parseTimeOfDay( scanned, i );
            }
        }
        else if ( c.isUpper() ) {
            found = parseSyslog( scanned, i );
        }

        if ( found ) {
            times.wall = found;
            i = found->start + found->length - 1;
        }
    }

    // A format of the user wins over a built-in one starting at the same place or after it
    if ( customGeneration.load() > 0 ) {
        if ( auto custom = parseCustom( scanned ) ) {
            if ( !times.wall || custom->start <= times.wall->start ) {
                times.wall = custom;
            }
        }
    }

    return times;
}

std::optional<Timestamp> sameKind( const LineTimes& times, const Timestamp& reference )
{
    if ( reference.isMonotonic() ) {
        return times.monotonic;
    }
    if ( times.wall && times.wall->format == reference.format ) {
        return times.wall;
    }
    return {};
}

std::vector<Timestamp> inOrder( const LineTimes& times )
{
    std::vector<Timestamp> stamps;
    if ( times.wall ) {
        stamps.push_back( *times.wall );
    }
    if ( times.monotonic ) {
        stamps.push_back( *times.monotonic );
    }
    std::sort( stamps.begin(), stamps.end(),
               []( const Timestamp& a, const Timestamp& b ) { return a.start < b.start; } );
    return stamps;
}

std::optional<Timestamp> ofFormat( const LineTimes& times, Format format )
{
    if ( format == Format::Kernel ) {
        return times.monotonic;
    }
    if ( times.wall && times.wall->format == format ) {
        return times.wall;
    }
    return {};
}

Difference difference( const LineTimes& first, const LineTimes& second,
                       std::optional<Format> format )
{
    Difference result;

    if ( first.isEmpty() || second.isEmpty() ) {
        result.error = first.isEmpty() && second.isEmpty()
                           ? translate( "No time stamp in the lines" )
                       : first.isEmpty() ? translate( "No time stamp in the first line" )
                                         : translate( "No time stamp in the last line" );
        return result;
    }

    const auto chosen = format ? *format : inOrder( first ).front().format;
    const auto a0 = ofFormat( first, chosen );
    const auto b0 = ofFormat( second, chosen );
    if ( !a0 || !b0 ) {
        if ( format ) {
            result.error = ( !a0 ? translate( "No %1 time stamp in the first line" )
                                 : translate( "No %1 time stamp in the last line" ) )
                               .arg( formatName( chosen ) );
        }
        else {
            result.error = translate( "The lines have different time formats (%1 / %2)" )
                               .arg( formatName( chosen ),
                                     formatName( inOrder( second ).front().format ) );
        }
        return result;
    }

    if ( chosen == Format::Kernel ) {
        result.first = a0;
        result.second = b0;
        result.us = b0->us - a0->us;
        return result;
    }

    const auto& a = *a0;
    const auto& b = *b0;
    result.first = a;
    result.second = b;
    auto us = b.us - a.us;
    if ( a.hasZone != b.hasZone ) {
        // Compare the times as written
        us = ( b.us + b.zoneUs ) - ( a.us + a.zoneUs );
        result.notes.append( translate( "Only one line has a time zone: the zone is ignored" ) );
    }

    if ( !a.hasDate() ) {
        result.notes.append(
            translate( "No date in the lines: they are taken as less than 12 hours apart" ) );
        if ( us > UsPerDay / 2 ) {
            us -= UsPerDay;
        }
        else if ( us < -UsPerDay / 2 ) {
            us += UsPerDay;
        }
    }
    else if ( !a.hasYear() || !b.hasYear() ) {
        // The closest of the same year, the year before or the year after
        int64_t best = us;
        for ( const auto days : { -366, -365, 365, 366 } ) {
            const auto candidate = us + days * UsPerDay;
            if ( std::llabs( candidate ) < std::llabs( best ) ) {
                best = candidate;
            }
        }
        if ( best != us ) {
            result.notes.append(
                translate( "No year in the lines: a new year is taken as crossed" ) );
            us = best;
        }
    }

    result.us = us;
    return result;
}

QString formatDuration( int64_t us, int fractionDigits, bool withSign )
{
    const auto sign = !withSign ? QString() : us < 0 ? QStringLiteral( "-" ) : QStringLiteral( "+" );
    const auto absolute = std::llabs( us );
    fractionDigits = qBound( 0, fractionDigits, 6 );

    if ( absolute < 1000 && fractionDigits > 3 ) {
        return sign + translate( "%1 µs" ).arg( absolute );
    }
    if ( absolute < UsPerSecond ) {
        const auto decimals = qMax( 0, fractionDigits - 3 );
        return sign
               + translate( "%1 ms" ).arg( static_cast<double>( absolute ) / 1000.0, 0, 'f',
                                           decimals );
    }

    const auto seconds = absolute / UsPerSecond;
    const auto fraction = fractionText( absolute % UsPerSecond, fractionDigits );
    if ( seconds < 60 ) {
        return sign + translate( "%1 s" ).arg( QString::number( seconds ) + fraction );
    }

    const auto days = seconds / 86400;
    const auto hours = ( seconds / 3600 ) % 24;
    const auto minutes = ( seconds / 60 ) % 60;
    const auto secondText = twoDigits( seconds % 60 ) + fraction;
    if ( days > 0 ) {
        return sign
               + translate( "%1 d %2 h %3 m %4 s" )
                     .arg( days )
                     .arg( twoDigits( hours ), twoDigits( minutes ), secondText );
    }
    if ( hours > 0 ) {
        return sign
               + translate( "%1 h %2 m %3 s" )
                     .arg( hours )
                     .arg( twoDigits( minutes ), secondText );
    }
    return sign + translate( "%1 m %2 s" ).arg( minutes ).arg( secondText );
}

QString formatTimestamp( const Timestamp& timestamp )
{
    if ( timestamp.isMonotonic() ) {
        return translate( "%1 s since boot" )
            .arg( QString::number( timestamp.us / UsPerSecond )
                  + fractionText( timestamp.us % UsPerSecond, timestamp.fractionDigits ) );
    }

    // Shown as written in the line
    const auto local = timestamp.us + timestamp.zoneUs;
    auto days = local / UsPerDay;
    auto ofDay = local % UsPerDay;
    if ( ofDay < 0 ) {
        ofDay += UsPerDay;
        --days;
    }
    const auto seconds = ofDay / UsPerSecond;
    const auto time = QString( "%1:%2:%3%4" )
                          .arg( twoDigits( seconds / 3600 ), twoDigits( ( seconds / 60 ) % 60 ),
                                twoDigits( seconds % 60 ),
                                fractionText( ofDay % UsPerSecond, timestamp.fractionDigits ) );
    if ( !timestamp.hasDate() ) {
        return time;
    }

    const auto civil = civilFromDays( days );
    auto date = QString( "%1-%2" ).arg( twoDigits( civil.month ), twoDigits( civil.day ) );
    if ( timestamp.hasYear() ) {
        date = QString::number( civil.year ) + "-" + date;
    }
    auto text = date + " " + time;
    if ( timestamp.hasZone ) {
        const auto zoneMinutes = timestamp.zoneUs / ( 60 * UsPerSecond );
        text += QString( " %1%2:%3" )
                    .arg( zoneMinutes < 0 ? "-" : "+" )
                    .arg( twoDigits( std::llabs( zoneMinutes ) / 60 ),
                          twoDigits( std::llabs( zoneMinutes ) % 60 ) );
    }
    return text;
}

} // namespace timestamps
