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

#include "logtemplates.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <iterator>
#include <numeric>
#include <unordered_map>

#include <QCoreApplication>
#include <QHash>

#include <tbb/blocked_range.h>
#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>

#include "filedigest.h"
#include "logdata.h"
#include "timestamp.h"

namespace templates {

namespace {

QString translate( const char* text )
{
    return QCoreApplication::translate( "templates", text );
}

const QString Number = QStringLiteral( "<NUM>" );
const QString Hex = QStringLiteral( "<HEX>" );
const QString Any = QStringLiteral( "<*>" );

bool isWordChar( QChar c )
{
    return c.isLetterOrNumber() || c == u'_';
}

bool isHexDigit( QChar c )
{
    return ( c >= u'0' && c <= u'9' ) || ( c >= u'a' && c <= u'f' ) || ( c >= u'A' && c <= u'F' );
}

// A word, with the dots, colons and dashes inside it (10.0.0.5:443, worker-12, a.b.C)
int wordEnd( QStringView line, int start )
{
    auto end = start;
    while ( end < line.size() ) {
        const auto c = line[ end ];
        if ( isWordChar( c ) ) {
            ++end;
        }
        else if ( ( c == u'.' || c == u':' || c == u'-' ) && end + 1 < line.size()
                  && isWordChar( line[ end + 1 ] ) ) {
            ++end;
        }
        else {
            break;
        }
    }
    return end;
}

void appendWord( QString& out, QStringView word )
{
    bool hasDigit = false;
    bool hasLetter = false;
    bool allHex = true;
    int hexDigits = 0;
    for ( const auto c : word ) {
        if ( c.isDigit() ) {
            hasDigit = true;
        }
        else if ( c.isLetter() ) {
            hasLetter = true;
        }
        if ( isHexDigit( c ) ) {
            ++hexDigits;
        }
        else if ( c != u'-' && c != u':' ) {
            allHex = false;
        }
    }

    if ( !hasDigit ) {
        out += word;
        return;
    }
    // 0x7f3a, a3f9c2d1, 550e8400-e29b-41d4-a716-446655440000
    if ( ( word.size() > 2 && word[ 0 ] == u'0' && ( word[ 1 ] == u'x' || word[ 1 ] == u'X' ) )
         || ( allHex && hexDigits >= 8 && hasLetter ) ) {
        out += Hex;
        return;
    }
    // 42, 3.14, 10.0.0.5:443, 1.2.3
    if ( !hasLetter ) {
        out += Number;
        return;
    }
    // worker-12, u0a123: the numbers in the word
    for ( int i = 0; i < word.size(); ) {
        if ( word[ i ].isDigit() ) {
            while ( i < word.size() && word[ i ].isDigit() ) {
                ++i;
            }
            out += Number;
        }
        else {
            out += word[ i ];
            ++i;
        }
    }
}

bool isEnumWord( QStringView word )
{
    // INFO, ERROR, TRUE: kept as values of key=value
    if ( word.size() < 2 ) {
        return false;
    }
    return std::all_of( word.begin(), word.end(),
                        []( QChar c ) { return ( c.isLetter() && c.isUpper() ) || c == u'_'; } );
}

struct Info {
    uint64_t count = 0;
    uint64_t first = 0;
    uint64_t last = 0;
    // The first lines, sorted, up to MaxTemplateLines
    std::vector<uint64_t> lines;
};

// Keeps the first MaxTemplateLines lines, sorted; the lines of a block come in order, the
// blocks in any order
void keepLine( std::vector<uint64_t>& lines, uint64_t line )
{
    if ( lines.size() == MaxTemplateLines && line > lines.back() ) {
        return;
    }
    if ( lines.empty() || line > lines.back() ) {
        lines.push_back( line );
        return;
    }
    lines.insert( std::lower_bound( lines.begin(), lines.end(), line ), line );
    if ( lines.size() > MaxTemplateLines ) {
        lines.pop_back();
    }
}

// The first MaxTemplateLines lines of both, sorted
template <typename Line>
void mergeLines( std::vector<Line>& into, const std::vector<Line>& from )
{
    std::vector<Line> merged;
    merged.reserve( qMin( MaxTemplateLines, into.size() + from.size() ) );
    std::merge( into.begin(), into.end(), from.begin(), from.end(), std::back_inserter( merged ) );
    if ( merged.size() > MaxTemplateLines ) {
        merged.resize( MaxTemplateLines );
    }
    into = std::move( merged );
}

using Map = std::unordered_map<QString, Info>;

constexpr uint64_t BlockLines = 65536;
constexpr uint64_t ReadLines = 4096;

struct Bounds {
    uint64_t begin = 0;
    uint64_t end = 0;
};

Bounds boundsOf( const LogData& logData, const Scope& scope )
{
    const auto nbLines = logData.getNbLine().get();
    Bounds bounds{ 0, nbLines };
    if ( scope.range ) {
        bounds.begin = qMin( scope.range->first.get(), nbLines );
        bounds.end = qMin( scope.range->second.get() + 1, nbLines );
    }
    if ( scope.lines && !scope.lines->isEmpty() ) {
        bounds.begin = qMax( bounds.begin, scope.lines->minimum() );
        bounds.end = qMin( bounds.end, scope.lines->maximum() + 1 );
    }
    else if ( scope.lines ) {
        bounds.end = bounds.begin;
    }
    return bounds;
}

// Calls `visit` with runs of lines of the scope in [begin, end)
template <typename Visit>
void forRuns( const Scope& scope, uint64_t begin, uint64_t end, Visit&& visit )
{
    if ( begin >= end ) {
        return;
    }
    if ( !scope.lines ) {
        for ( auto start = begin; start < end; start += ReadLines ) {
            visit( start, qMin( ReadLines, end - start ) );
        }
        return;
    }

    SearchResultArray block;
    block.addRange( begin, end );
    block &= *scope.lines;
    uint64_t runStart = 0;
    uint64_t runLength = 0;
    for ( const auto line : block ) {
        if ( runLength > 0 && ( line != runStart + runLength || runLength >= ReadLines ) ) {
            visit( runStart, runLength );
            runLength = 0;
        }
        if ( runLength == 0 ) {
            runStart = line;
        }
        ++runLength;
    }
    if ( runLength > 0 ) {
        visit( runStart, runLength );
    }
}

struct ReaderAttached {
    explicit ReaderAttached( const LogData& logData )
        : logData_( logData )
    {
        logData_.attachReader();
    }
    ~ReaderAttached()
    {
        logData_.detachReader();
    }
    const LogData& logData_;
};

} // namespace

QString maskLine( QStringView line )
{
    // The time stamps of the line are left out
    const auto times = timestamps::parseLine( line );
    std::array<std::pair<int, int>, 2> skipped{ { { -1, -1 }, { -1, -1 } } };
    if ( times.wall ) {
        skipped[ 0 ] = { times.wall->start, times.wall->start + times.wall->length };
    }
    if ( times.monotonic ) {
        skipped[ 1 ] = { times.monotonic->start, times.monotonic->start + times.monotonic->length };
    }

    QString out;
    out.reserve( line.size() );
    bool afterEquals = false;
    int i = 0;
    const int n = static_cast<int>( line.size() );
    while ( i < n ) {
        bool skip = false;
        for ( const auto& [ from, to ] : skipped ) {
            if ( i == from ) {
                i = to;
                skip = true;
            }
        }
        if ( skip ) {
            continue;
        }

        const auto c = line[ i ];
        if ( c == u'"' ) {
            // Quoted text, up to a few hundred characters
            int close = i + 1;
            while ( close < n && close - i < 512 && line[ close ] != u'"' ) {
                close += line[ close ] == u'\\' ? 2 : 1;
            }
            if ( close < n && line[ close ] == u'"' ) {
                out += u'"';
                out += Any;
                out += u'"';
                i = close + 1;
                afterEquals = false;
                continue;
            }
        }
        if ( isWordChar( c ) ) {
            const auto end = wordEnd( line, i );
            const auto word = line.mid( i, end - i );
            if ( afterEquals && !isEnumWord( word ) ) {
                out += Any;
            }
            else {
                appendWord( out, word );
            }
            afterEquals = false;
            i = end;
            continue;
        }
        if ( c.isSpace() ) {
            if ( !out.isEmpty() && !out.back().isSpace() ) {
                out += u' ';
            }
            afterEquals = false;
            ++i;
            continue;
        }
        afterEquals = c == u'=';
        out += c;
        ++i;
    }

    while ( !out.isEmpty() && out.back().isSpace() ) {
        out.chop( 1 );
    }
    while ( !out.isEmpty() && out.front().isSpace() ) {
        out.remove( 0, 1 );
    }
    return out;
}

uint64_t Scope::size( const LogData& logData ) const
{
    const auto bounds = boundsOf( logData, *this );
    if ( lines ) {
        if ( bounds.begin >= bounds.end ) {
            return 0;
        }
        SearchResultArray inRange;
        inRange.addRange( bounds.begin, bounds.end );
        return ( inRange & *lines ).cardinality();
    }
    return bounds.end > bounds.begin ? bounds.end - bounds.begin : 0;
}

Table build( const LogData& logData, const Scope& scope, AtomicFlag& cancel,
             const analysis::Progress& progress )
{
    Table table;
    ReaderAttached reader( logData );
    const auto bounds = boundsOf( logData, scope );
    if ( bounds.begin >= bounds.end ) {
        return table;
    }

    const auto nbBlocks = ( bounds.end - bounds.begin + BlockLines - 1 ) / BlockLines;
    struct Local {
        Map map;
        uint64_t lines = 0;
        uint64_t otherLines = 0;
    };
    tbb::enumerable_thread_specific<Local> locals;
    std::atomic<uint64_t> doneBlocks{ 0 };
    std::atomic<int> reported{ 0 };

    tbb::parallel_for( tbb::blocked_range<uint64_t>( 0, nbBlocks, 1 ),
                       [ & ]( const tbb::blocked_range<uint64_t>& blocks ) {
                           auto& local = locals.local();
                           for ( auto block = blocks.begin(); block != blocks.end(); ++block ) {
                               if ( cancel ) {
                                   return;
                               }
                               const auto begin = bounds.begin + block * BlockLines;
                               const auto end = qMin( bounds.end, begin + BlockLines );
                               forRuns( scope, begin, end, [ & ]( uint64_t start, uint64_t count ) {
                                   const auto lines
                                       = logData.getLines( LineNumber( start ), LinesCount( count ) );
                                   for ( size_t i = 0; i < lines.size(); ++i ) {
                                       const auto line = start + i;
                                       auto text = maskLine( lines[ i ] );
                                       ++local.lines;
                                       auto found = local.map.find( text );
                                       if ( found == local.map.end() ) {
                                           if ( local.map.size() >= MaxTemplates ) {
                                               ++local.otherLines;
                                               continue;
                                           }
                                           local.map.emplace( std::move( text ),
                                                              Info{ 1, line, line, { line } } );
                                           continue;
                                       }
                                       ++found->second.count;
                                       found->second.first = qMin( found->second.first, line );
                                       found->second.last = qMax( found->second.last, line );
                                       keepLine( found->second.lines, line );
                                   }
                               } );

                               const auto done = ++doneBlocks;
                               if ( progress ) {
                                   const auto percent = static_cast<int>( done * 100 / nbBlocks );
                                   auto previous = reported.load();
                                   if ( percent > previous
                                        && reported.compare_exchange_strong( previous, percent ) ) {
                                       progress( percent );
                                   }
                               }
                           }
                       } );

    if ( cancel ) {
        table.cancelled = true;
        return table;
    }

    Map all;
    for ( auto& local : locals ) {
        table.lines += local.lines;
        table.otherLines += local.otherLines;
        for ( auto& [ text, info ] : local.map ) {
            auto& merged = all[ text ];
            if ( merged.count == 0 ) {
                merged = std::move( info );
            }
            else {
                merged.count += info.count;
                merged.first = qMin( merged.first, info.first );
                merged.last = qMax( merged.last, info.last );
                mergeLines( merged.lines, info.lines );
            }
        }
    }

    table.templates.reserve( all.size() );
    for ( auto& [ text, info ] : all ) {
        std::vector<LineNumber> lines;
        lines.reserve( info.lines.size() );
        for ( const auto line : info.lines ) {
            lines.emplace_back( line );
        }
        table.templates.push_back( { text, info.count, LineNumber( info.first ),
                                     LineNumber( info.last ), std::move( lines ) } );
    }
    std::sort( table.templates.begin(), table.templates.end(),
               []( const Template& a, const Template& b ) {
                   return a.count != b.count ? a.count > b.count : a.first < b.first;
               } );
    if ( table.templates.size() > MaxTemplates ) {
        for ( auto t = MaxTemplates; t < table.templates.size(); ++t ) {
            table.otherLines += table.templates[ t ].count;
        }
        table.templates.resize( MaxTemplates );
    }
    table.truncated = table.otherLines > 0;
    return table;
}

std::vector<QString> mergeSimilar( const std::vector<QString>& templates )
{
    std::vector<QString> merged( templates );
    if ( templates.size() > MaxMerged ) {
        return merged;
    }

    // Union-find of the templates differing by one word
    std::vector<size_t> parent( templates.size() );
    std::iota( parent.begin(), parent.end(), 0 );
    const auto root = [ &parent ]( size_t i ) {
        while ( parent[ i ] != i ) {
            parent[ i ] = parent[ parent[ i ] ];
            i = parent[ i ];
        }
        return i;
    };

    std::vector<QStringList> words( templates.size() );
    // Hash of the words with one of them left out, and its position
    std::unordered_map<uint64_t, std::vector<size_t>> buckets;
    for ( size_t t = 0; t < templates.size(); ++t ) {
        words[ t ] = templates[ t ].split( u' ', Qt::SkipEmptyParts );
        const auto& w = words[ t ];
        if ( w.size() < 4 || w.size() > 40 ) {
            continue;
        }
        for ( int k = 0; k < w.size(); ++k ) {
            FileDigest digest;
            digest.addData( QByteArray::number( k ) + ":" + QByteArray::number( w.size() ) );
            for ( int j = 0; j < w.size(); ++j ) {
                if ( j != k ) {
                    digest.addData( w[ j ].toUtf8() );
                    digest.addData( "\x1f", 1 );
                }
            }
            buckets[ digest.digest() ].push_back( t );
        }
    }

    for ( const auto& [ hash, members ] : buckets ) {
        if ( members.size() < 2 ) {
            continue;
        }
        for ( size_t m = 1; m < members.size(); ++m ) {
            parent[ root( members[ m ] ) ] = root( members[ 0 ] );
        }
    }

    // The merged text has <*> where the words of the group differ
    std::unordered_map<size_t, std::vector<size_t>> groups;
    for ( size_t t = 0; t < templates.size(); ++t ) {
        groups[ root( t ) ].push_back( t );
    }
    for ( const auto& [ groupRoot, members ] : groups ) {
        if ( members.size() < 2 ) {
            continue;
        }
        auto result = words[ members[ 0 ] ];
        for ( const auto member : members ) {
            const auto& w = words[ member ];
            for ( int k = 0; k < result.size() && k < w.size(); ++k ) {
                if ( result[ k ] != w[ k ] ) {
                    result[ k ] = Any;
                }
            }
        }
        const auto text = result.join( u' ' );
        for ( const auto member : members ) {
            merged[ member ] = text;
        }
    }
    return merged;
}

namespace {

// Chance that X >= k (upper) or X <= k (lower) for X ~ Binomial(n, p): how likely it is
// that the lines of a template fall on the sides as they did if its share were the same in
// both. Exact on small counts, the normal approximation on large ones: bounded work per
// template whatever its count.
double binomialTail( uint64_t n, uint64_t k, double p, bool upper )
{
    const auto nd = static_cast<double>( n );
    const auto kd = static_cast<double>( k );
    const auto mean = nd * p;
    // On the side of the mean: not unusual, and the exact sum would be long
    if ( upper ? kd <= mean : kd >= mean ) {
        return 1.0;
    }
    const auto variance = mean * ( 1 - p );
    if ( variance > 100 ) {
        const auto z = upper ? ( kd - 0.5 - mean ) : ( mean - kd - 0.5 );
        return 0.5 * std::erfc( z / std::sqrt( 2.0 * variance ) );
    }
    // Sum away from the mean from k, the terms only get smaller
    auto term = std::exp( std::lgamma( nd + 1 ) - std::lgamma( kd + 1 ) - std::lgamma( nd - kd + 1 )
                          + kd * std::log( p ) + ( nd - kd ) * std::log1p( -p ) );
    auto sum = term;
    const auto odds = p / ( 1 - p );
    for ( auto i = k; upper ? i < n : i > 0; upper ? ++i : --i ) {
        const auto id = static_cast<double>( i );
        term *= upper ? ( nd - id ) / ( id + 1 ) * odds : id / ( nd - id + 1 ) / odds;
        sum += term;
        if ( term <= sum * 1e-12 ) {
            break;
        }
    }
    return std::min( sum, 1.0 );
}

} // namespace

std::vector<Compared> compare( const Table& first, const Table* second, bool merge )
{
    std::vector<QString> texts;
    texts.reserve( first.templates.size() + ( second ? second->templates.size() : 0 ) );
    for ( const auto& t : first.templates ) {
        texts.push_back( t.text );
    }
    if ( second ) {
        for ( const auto& t : second->templates ) {
            texts.push_back( t.text );
        }
    }
    const auto names = merge ? mergeSimilar( texts ) : texts;

    std::unordered_map<QString, Compared> byName;
    for ( size_t i = 0; i < names.size(); ++i ) {
        const auto inFirst = i < first.templates.size();
        const auto& t = inFirst ? first.templates[ i ]
                                : second->templates[ i - first.templates.size() ];
        auto& compared = byName[ names[ i ] ];
        compared.text = names[ i ];
        auto& count = inFirst ? compared.firstCount : compared.secondCount;
        auto& line = inFirst ? compared.firstLine : compared.secondLine;
        count += t.count;
        if ( !line || t.first < *line ) {
            line = t.first;
        }
        mergeLines( inFirst ? compared.firstLines : compared.secondLines, t.lines );
    }

    // Shares of the lines, the files may have different sizes
    const auto firstLines = static_cast<double>( qMax<uint64_t>( 1, first.lines ) );
    const auto secondLines
        = static_cast<double>( qMax<uint64_t>( 1, second ? second->lines : 1 ) );
    std::vector<std::pair<double, Compared>> ranked;
    ranked.reserve( byName.size() );
    for ( auto& [ name, compared ] : byName ) {
        double score = 0;
        if ( !second ) {
            // The rarest first
            score = -static_cast<double>( compared.firstCount );
        }
        else if ( compared.secondCount == 0 ) {
            compared.status = Compared::Status::OnlyFirst;
            score = 1e12 + static_cast<double>( compared.firstCount );
        }
        else if ( compared.firstCount == 0 ) {
            compared.status = Compared::Status::OnlySecond;
            score = 2e12 + static_cast<double>( compared.secondCount );
        }
        else {
            const auto firstShare = static_cast<double>( compared.firstCount ) / firstLines;
            const auto secondShare = static_cast<double>( compared.secondCount ) / secondLines;
            const auto ratio = secondShare / firstShare;
            // A large change of the share, and too unlikely to be chance: 1 line in each of
            // two files of very different sizes is not "fewer"
            const auto n = compared.firstCount + compared.secondCount;
            const auto pFirst = firstLines / ( firstLines + secondLines );
            if ( ratio >= 2.0
                 && binomialTail( n, compared.firstCount, pFirst, false ) < SignificantChance ) {
                compared.status = Compared::Status::More;
            }
            else if ( ratio <= 0.5
                      && binomialTail( n, compared.firstCount, pFirst, true )
                             < SignificantChance ) {
                compared.status = Compared::Status::Fewer;
            }
            score = std::abs( std::log( ratio ) );
            // The changes first, then the others by the change of their share
            if ( compared.status != Compared::Status::Same ) {
                score += 1e6;
            }
        }
        ranked.emplace_back( score, std::move( compared ) );
    }
    std::sort( ranked.begin(), ranked.end(), []( const auto& a, const auto& b ) {
        if ( a.first != b.first ) {
            return a.first > b.first;
        }
        return a.second.text < b.second.text;
    } );

    std::vector<Compared> result;
    result.reserve( ranked.size() );
    for ( auto& [ score, compared ] : ranked ) {
        result.push_back( std::move( compared ) );
    }
    return result;
}

namespace {

// Stands for a run of digits when the numbers are left out: a character of the private use
// area, so that it is never equal to a character of the line
constexpr char16_t NumberKey = 0xE000;

// Calls `visit( key, start, length )` for each part of the line compared: a character, or a
// run of digits when the numbers are left out; the time stamps left out are skipped
template <typename Visit>
void forParts( QStringView line, const DiffOptions& options, Visit&& visit )
{
    std::array<std::pair<int, int>, 2> skipped{ { { -1, -1 }, { -1, -1 } } };
    if ( options.ignoreTimes ) {
        const auto times = timestamps::parseLine( line );
        if ( times.wall ) {
            skipped[ 0 ] = { times.wall->start, times.wall->start + times.wall->length };
        }
        if ( times.monotonic ) {
            skipped[ 1 ] = { times.monotonic->start,
                             times.monotonic->start + times.monotonic->length };
        }
    }
    const int n = static_cast<int>( line.size() );
    int i = 0;
    while ( i < n ) {
        bool skip = false;
        for ( const auto& [ from, to ] : skipped ) {
            if ( i == from ) {
                i = to;
                skip = true;
            }
        }
        if ( skip ) {
            continue;
        }
        if ( options.ignoreNumbers && line[ i ].isDigit() ) {
            int end = i + 1;
            while ( end < n && line[ end ].isDigit() ) {
                ++end;
            }
            visit( QChar( NumberKey ), i, end - i );
            i = end;
            continue;
        }
        visit( line[ i ], i, 1 );
        ++i;
    }
}

enum class Step { Same, Removed, Added };

// The steps from the first sequence (n items) to the second (m items), `equal( x, y )`
// telling whether their items x and y are alike; nothing when there are more than `maxD`
// changes or the comparison is cancelled
template <typename Equal>
std::optional<std::vector<Step>> myers( int n, int m, Equal&& equal, int maxD,
                                        const AtomicFlag* cancel )
{
    // Same start and end are taken out of the comparison
    int prefix = 0;
    while ( prefix < n && prefix < m && equal( prefix, prefix ) ) {
        ++prefix;
    }
    int suffix = 0;
    while ( suffix < n - prefix && suffix < m - prefix
            && equal( n - 1 - suffix, m - 1 - suffix ) ) {
        ++suffix;
    }
    const int innerN = n - prefix - suffix;
    const int innerM = m - prefix - suffix;
    const auto at = [ &equal, prefix ]( int x, int y ) { return equal( prefix + x, prefix + y ); };

    // The furthest x on each diagonal for each number of changes d, only the diagonals
    // reached kept (memory in d squared)
    maxD = std::min( innerN + innerM, maxD );
    const auto offset = maxD + 1;
    std::vector<int> furthest( static_cast<size_t>( 2 * maxD + 3 ), 0 );
    const auto v = [ &furthest, offset ]( int k ) -> int& {
        return furthest[ static_cast<size_t>( offset + k ) ];
    };
    std::vector<std::vector<int>> trace;
    int distance = -1;
    for ( int d = 0; d <= maxD && distance < 0; ++d ) {
        if ( cancel && *cancel ) {
            return {};
        }
        for ( int k = -d; k <= d; k += 2 ) {
            int x = ( k == -d || ( k != d && v( k - 1 ) < v( k + 1 ) ) ) ? v( k + 1 )
                                                                          : v( k - 1 ) + 1;
            int y = x - k;
            while ( x < innerN && y < innerM && at( x, y ) ) {
                ++x;
                ++y;
            }
            v( k ) = x;
            if ( x >= innerN && y >= innerM ) {
                distance = d;
            }
        }
        trace.emplace_back( furthest.begin() + offset - d, furthest.begin() + offset + d + 1 );
    }
    if ( distance < 0 ) {
        return {};
    }

    // Back from the end
    std::vector<Step> middle;
    int x = innerN;
    int y = innerM;
    for ( int d = distance; d > 0; --d ) {
        const auto& previous = trace[ static_cast<size_t>( d - 1 ) ];
        const auto value = [ &previous, d ]( int k ) {
            return previous[ static_cast<size_t>( k + d - 1 ) ];
        };
        const int k = x - y;
        const int previousK
            = ( k == -d || ( k != d && value( k - 1 ) < value( k + 1 ) ) ) ? k + 1 : k - 1;
        const int previousX = value( previousK );
        const int previousY = previousX - previousK;
        while ( x > previousX && y > previousY ) {
            --x;
            --y;
            middle.push_back( Step::Same );
        }
        if ( x == previousX ) {
            --y;
            middle.push_back( Step::Added );
        }
        else {
            --x;
            middle.push_back( Step::Removed );
        }
    }
    while ( x > 0 && y > 0 ) {
        --x;
        --y;
        middle.push_back( Step::Same );
    }

    std::vector<Step> steps( static_cast<size_t>( prefix ), Step::Same );
    steps.reserve( static_cast<size_t>( prefix ) + middle.size() + static_cast<size_t>( suffix ) );
    steps.insert( steps.end(), middle.rbegin(), middle.rend() );
    steps.insert( steps.end(), static_cast<size_t>( suffix ), Step::Same );
    return steps;
}

// The lines of both sides as hashes of their keys, and the line number of each
struct Keyed {
    std::vector<uint64_t> a;
    std::vector<uint64_t> b;
    std::vector<LineNumber> aLines;
    std::vector<LineNumber> bLines;
    QString key;

    void add( bool first, QStringView line, LineNumber number, const DiffOptions& options )
    {
        uint64_t hash = 0;
        if ( !options.ignoreTimes && !options.ignoreNumbers ) {
            hash = qHash( line, 0x9e3779b9u );
        }
        else {
            key.clear();
            forParts( line, options, [ this ]( QChar part, int, int ) { key += part; } );
            hash = qHash( key, 0x9e3779b9u );
        }
        ( first ? a : b ).push_back( hash );
        ( first ? aLines : bLines ).push_back( number );
    }
};

// Patience diff: the lines found once on each side pair up in order (the longest such
// sequence), the parts between are compared again the same way; a part without such lines
// goes to Myers within the limits of the work, else it is removed and added.
class Differ {
  public:
    Differ( const std::vector<uint64_t>& a, const std::vector<uint64_t>& b, AtomicFlag& cancel )
        : a_( a )
        , b_( b )
        , cancel_( cancel )
    {
    }

    void run( size_t a0, size_t a1, size_t b0, size_t b1, int depth )
    {
        if ( cancel_ ) {
            return;
        }
        size_t prefix = 0;
        while ( a0 + prefix < a1 && b0 + prefix < b1 && a_[ a0 + prefix ] == b_[ b0 + prefix ] ) {
            ++prefix;
        }
        add( DiffStep::Same, prefix );
        a0 += prefix;
        b0 += prefix;
        size_t suffix = 0;
        while ( suffix < a1 - a0 && suffix < b1 - b0
                && a_[ a1 - 1 - suffix ] == b_[ b1 - 1 - suffix ] ) {
            ++suffix;
        }
        a1 -= suffix;
        b1 -= suffix;

        if ( a0 == a1 || b0 == b1 ) {
            add( DiffStep::Removed, a1 - a0 );
            add( DiffStep::Added, b1 - b0 );
        }
        else {
            bool common = true;
            const auto anchors = depth < MaxDepth ? uniqueAnchors( a0, a1, b0, b1, common )
                                                  : std::vector<std::pair<size_t, size_t>>{};
            if ( !common ) {
                // Nothing alike: removed and added
                add( DiffStep::Removed, a1 - a0 );
                add( DiffStep::Added, b1 - b0 );
            }
            else if ( anchors.empty() ) {
                myersOrBlock( a0, a1, b0, b1 );
            }
            else {
                auto x = a0;
                auto y = b0;
                for ( const auto& [ ia, ib ] : anchors ) {
                    run( x, ia, y, ib, depth + 1 );
                    add( DiffStep::Same, 1 );
                    x = ia + 1;
                    y = ib + 1;
                }
                run( x, a1, y, b1, depth + 1 );
            }
        }
        add( DiffStep::Same, suffix );
    }

    std::vector<DiffStep> steps;
    uint64_t unpaired = 0;

  private:
    static constexpr int MaxDepth = 48;

    void add( DiffStep step, size_t count )
    {
        steps.insert( steps.end(), count, step );
    }

    // The lines once in each part, paired: the longest sequence in the order of both.
    // `common` tells whether any line is in both.
    std::vector<std::pair<size_t, size_t>> uniqueAnchors( size_t a0, size_t a1, size_t b0,
                                                          size_t b1, bool& common ) const
    {
        common = false;
        // The hash and the place of each line (its side in the lowest bit), by hash
        std::vector<std::pair<uint64_t, uint64_t>> lines;
        lines.reserve( ( a1 - a0 ) + ( b1 - b0 ) );
        for ( auto i = a0; i < a1; ++i ) {
            lines.emplace_back( a_[ i ], static_cast<uint64_t>( i ) << 1 );
        }
        for ( auto i = b0; i < b1; ++i ) {
            lines.emplace_back( b_[ i ], ( static_cast<uint64_t>( i ) << 1 ) | 1 );
        }
        std::sort( lines.begin(), lines.end() );

        std::vector<std::pair<size_t, size_t>> pairs;
        for ( size_t i = 0; i < lines.size(); ) {
            auto j = i;
            size_t inA = 0;
            size_t inB = 0;
            while ( j < lines.size() && lines[ j ].first == lines[ i ].first ) {
                ( lines[ j ].second & 1 ? inB : inA ) += 1;
                ++j;
            }
            common = common || ( inA > 0 && inB > 0 );
            if ( inA == 1 && inB == 1 ) {
                const auto& one = lines[ i ];
                const auto& other = lines[ i + 1 ];
                const auto& inFirst = ( one.second & 1 ) == 0 ? one : other;
                const auto& inSecond = ( one.second & 1 ) == 0 ? other : one;
                pairs.emplace_back( static_cast<size_t>( inFirst.second >> 1 ),
                                    static_cast<size_t>( inSecond.second >> 1 ) );
            }
            i = j;
        }
        if ( pairs.empty() ) {
            return pairs;
        }
        std::sort( pairs.begin(), pairs.end() );

        // Longest increasing sequence of the second places (patience sorting)
        std::vector<size_t> tails;
        std::vector<size_t> previous( pairs.size(), SIZE_MAX );
        for ( size_t i = 0; i < pairs.size(); ++i ) {
            const auto it = std::lower_bound(
                tails.begin(), tails.end(), pairs[ i ].second,
                [ &pairs ]( size_t index, size_t value ) { return pairs[ index ].second < value; } );
            if ( it != tails.begin() ) {
                previous[ i ] = *( it - 1 );
            }
            if ( it == tails.end() ) {
                tails.push_back( i );
            }
            else {
                *it = i;
            }
        }
        std::vector<std::pair<size_t, size_t>> anchors;
        for ( auto i = tails.back(); i != SIZE_MAX; i = previous[ i ] ) {
            anchors.push_back( pairs[ i ] );
        }
        std::reverse( anchors.begin(), anchors.end() );
        return anchors;
    }

    // Myers in linear space (the bisection of diff-match-patch): the middle of the shortest
    // path splits the part in two, compared the same way. The work (diagonals and lines
    // followed) is counted; once there is none left, the parts not done yet are removed and
    // added.
    void myersOrBlock( size_t a0, size_t a1, size_t b0, size_t b1 )
    {
        // Same start and end
        while ( a0 < a1 && b0 < b1 && a_[ a0 ] == b_[ b0 ] ) {
            add( DiffStep::Same, 1 );
            ++a0;
            ++b0;
        }
        size_t suffix = 0;
        while ( a1 > a0 && b1 > b0 && a_[ a1 - 1 ] == b_[ b1 - 1 ] ) {
            --a1;
            --b1;
            ++suffix;
        }
        if ( a0 == a1 || b0 == b1 ) {
            add( DiffStep::Removed, a1 - a0 );
            add( DiffStep::Added, b1 - b0 );
        }
        else {
            bool outOfWork = false;
            if ( const auto split = bisect( a0, a1, b0, b1, outOfWork ) ) {
                myersOrBlock( a0, split->first, b0, split->second );
                myersOrBlock( split->first, a1, split->second, b1 );
            }
            else {
                // Nothing alike, or no work left to find it
                add( DiffStep::Removed, a1 - a0 );
                add( DiffStep::Added, b1 - b0 );
                if ( outOfWork ) {
                    unpaired += ( a1 - a0 ) + ( b1 - b0 );
                }
            }
        }
        add( DiffStep::Same, suffix );
    }

    // Where the shortest path crosses its middle; nothing when the parts have nothing alike,
    // or (`outOfWork` set) when the work runs out or the comparison is cancelled
    std::optional<std::pair<size_t, size_t>> bisect( size_t a0, size_t a1, size_t b0, size_t b1,
                                                     bool& outOfWork )
    {
        const auto n = static_cast<int64_t>( a1 - a0 );
        const auto m = static_cast<int64_t>( b1 - b0 );
        const auto maxD = ( n + m + 1 ) / 2;
        const auto offset = maxD;
        const auto length = 2 * maxD + 2;
        std::vector<int64_t> v1( static_cast<size_t>( length ), -1 );
        std::vector<int64_t> v2( static_cast<size_t>( length ), -1 );
        v1[ static_cast<size_t>( offset + 1 ) ] = 0;
        v2[ static_cast<size_t>( offset + 1 ) ] = 0;
        const auto delta = n - m;
        const bool front = ( delta % 2 ) != 0;
        int64_t k1start = 0;
        int64_t k1end = 0;
        int64_t k2start = 0;
        int64_t k2end = 0;
        const auto at = []( std::vector<int64_t>& v, int64_t i ) -> int64_t& {
            return v[ static_cast<size_t>( i ) ];
        };
        const auto equal = [ this, a0, b0 ]( int64_t x, int64_t y ) {
            return a_[ a0 + static_cast<size_t>( x ) ] == b_[ b0 + static_cast<size_t>( y ) ];
        };
        for ( int64_t d = 0; d < maxD; ++d ) {
            if ( cancel_ || workLeft_ == 0 ) {
                outOfWork = true;
                return {};
            }
            // Forward
            for ( auto k1 = -d + k1start; k1 <= d - k1end; k1 += 2 ) {
                const auto k1Offset = offset + k1;
                int64_t x1 = ( k1 == -d || ( k1 != d && at( v1, k1Offset - 1 ) < at( v1, k1Offset + 1 ) ) )
                                 ? at( v1, k1Offset + 1 )
                                 : at( v1, k1Offset - 1 ) + 1;
                int64_t y1 = x1 - k1;
                const auto from = x1;
                while ( x1 < n && y1 < m && equal( x1, y1 ) ) {
                    ++x1;
                    ++y1;
                }
                spend( static_cast<uint64_t>( x1 - from ) + 1 );
                at( v1, k1Offset ) = x1;
                if ( x1 > n ) {
                    k1end += 2;
                }
                else if ( y1 > m ) {
                    k1start += 2;
                }
                else if ( front ) {
                    const auto k2Offset = offset + delta - k1;
                    if ( k2Offset >= 0 && k2Offset < length && at( v2, k2Offset ) != -1 ) {
                        if ( x1 >= n - at( v2, k2Offset ) ) {
                            return std::make_pair( a0 + static_cast<size_t>( x1 ),
                                                   b0 + static_cast<size_t>( y1 ) );
                        }
                    }
                }
            }
            // Backward
            for ( auto k2 = -d + k2start; k2 <= d - k2end; k2 += 2 ) {
                const auto k2Offset = offset + k2;
                int64_t x2 = ( k2 == -d || ( k2 != d && at( v2, k2Offset - 1 ) < at( v2, k2Offset + 1 ) ) )
                                 ? at( v2, k2Offset + 1 )
                                 : at( v2, k2Offset - 1 ) + 1;
                int64_t y2 = x2 - k2;
                const auto from = x2;
                while ( x2 < n && y2 < m && equal( n - x2 - 1, m - y2 - 1 ) ) {
                    ++x2;
                    ++y2;
                }
                spend( static_cast<uint64_t>( x2 - from ) + 1 );
                at( v2, k2Offset ) = x2;
                if ( x2 > n ) {
                    k2end += 2;
                }
                else if ( y2 > m ) {
                    k2start += 2;
                }
                else if ( !front ) {
                    const auto k1Offset = offset + delta - k2;
                    if ( k1Offset >= 0 && k1Offset < length && at( v1, k1Offset ) != -1 ) {
                        const auto x1 = at( v1, k1Offset );
                        const auto y1 = offset + x1 - k1Offset;
                        if ( x1 >= n - x2 ) {
                            return std::make_pair( a0 + static_cast<size_t>( x1 ),
                                                   b0 + static_cast<size_t>( y1 ) );
                        }
                    }
                }
            }
        }
        return {};
    }

    void spend( uint64_t work )
    {
        workLeft_ -= std::min( workLeft_, work );
    }

    const std::vector<uint64_t>& a_;
    const std::vector<uint64_t>& b_;
    AtomicFlag& cancel_;
    uint64_t workLeft_ = MaxDiffWork;
};

Diff diffKeyed( Keyed& keyed, AtomicFlag& cancel )
{
    Diff result;
    Differ differ( keyed.a, keyed.b, cancel );
    differ.steps.reserve( std::max( keyed.a.size(), keyed.b.size() ) );
    differ.run( 0, keyed.a.size(), 0, keyed.b.size(), 0 );
    if ( cancel ) {
        result.cancelled = true;
        return result;
    }
    // The hashes are no longer needed
    keyed.a = {};
    keyed.b = {};
    result.steps = std::move( differ.steps );
    result.unpairedLines = differ.unpaired;
    result.firstLines = std::move( keyed.aLines );
    result.secondLines = std::move( keyed.bLines );
    return result;
}

// The lines of the scope added to one side, up to MaxDiffLines; the number left out
uint64_t addLines( const LogData& logData, const Scope& scope, bool first,
                   const DiffOptions& options, AtomicFlag& cancel, Keyed& keyed )
{
    ReaderAttached reader( logData );
    const auto bounds = boundsOf( logData, scope );
    uint64_t added = 0;
    uint64_t leftOut = 0;
    forRuns( scope, bounds.begin, bounds.end, [ & ]( uint64_t start, uint64_t count ) {
        if ( cancel ) {
            return;
        }
        if ( added >= MaxDiffLines ) {
            leftOut += count;
            return;
        }
        const auto kept = std::min( count, MaxDiffLines - added );
        leftOut += count - kept;
        const auto lines = logData.getLines( LineNumber( start ), LinesCount( kept ) );
        for ( size_t i = 0; i < lines.size(); ++i ) {
            keyed.add( first, lines[ i ], LineNumber( start + i ), options );
        }
        added += lines.size();
    } );
    return leftOut;
}

} // namespace

QString diffKey( QStringView line, const DiffOptions& options )
{
    if ( !options.ignoreTimes && !options.ignoreNumbers ) {
        return line.toString();
    }
    QString key;
    key.reserve( line.size() );
    forParts( line, options, [ &key ]( QChar part, int, int ) { key += part; } );
    return key;
}

std::vector<DiffLine> Diff::lines() const
{
    std::vector<DiffLine> lines;
    lines.reserve( steps.size() );
    size_t x = 0;
    size_t y = 0;
    for ( const auto step : steps ) {
        switch ( step ) {
        case DiffStep::Same:
            lines.push_back( { DiffLine::Op::Same, firstLines[ x++ ], secondLines[ y++ ] } );
            break;
        case DiffStep::Removed:
            lines.push_back( { DiffLine::Op::Removed, firstLines[ x++ ], {} } );
            break;
        case DiffStep::Added:
            lines.push_back( { DiffLine::Op::Added, {}, secondLines[ y++ ] } );
            break;
        }
    }
    return lines;
}

Diff diff( const LogData& first, const Scope& firstScope, const LogData& second,
           const Scope& secondScope, const DiffOptions& options, AtomicFlag& cancel )
{
    Keyed keyed;
    const auto firstLeftOut = addLines( first, firstScope, true, options, cancel, keyed );
    const auto secondLeftOut = addLines( second, secondScope, false, options, cancel, keyed );
    if ( cancel ) {
        Diff result;
        result.cancelled = true;
        return result;
    }
    auto result = diffKeyed( keyed, cancel );
    result.firstLeftOut = firstLeftOut;
    result.secondLeftOut = secondLeftOut;
    return result;
}

Diff diff( const std::vector<QString>& first, const std::vector<QString>& second,
           const DiffOptions& options, AtomicFlag& cancel )
{
    Keyed keyed;
    const auto firstKept = std::min<size_t>( first.size(), MaxDiffLines );
    const auto secondKept = std::min<size_t>( second.size(), MaxDiffLines );
    for ( size_t i = 0; i < firstKept; ++i ) {
        keyed.add( true, first[ i ], LineNumber( i ), options );
    }
    for ( size_t i = 0; i < secondKept; ++i ) {
        keyed.add( false, second[ i ], LineNumber( i ), options );
    }
    auto result = diffKeyed( keyed, cancel );
    result.firstLeftOut = first.size() - firstKept;
    result.secondLeftOut = second.size() - secondKept;
    return result;
}

LineChanges changedParts( QStringView first, QStringView second, const DiffOptions& options )
{
    struct Part {
        QChar key;
        int start;
        int length;
    };
    const auto partsOf = [ &options ]( QStringView line ) {
        std::vector<Part> parts;
        parts.reserve( static_cast<size_t>( line.size() ) );
        forParts( line, options,
                  [ &parts ]( QChar key, int start, int length ) {
                      parts.push_back( { key, start, length } );
                  } );
        return parts;
    };
    const auto a = partsOf( first );
    const auto b = partsOf( second );

    // Whole parts of a line changed: from its first part compared to its last one
    const auto whole = []( const std::vector<Part>& parts ) {
        std::vector<std::pair<int, int>> ranges;
        if ( !parts.empty() ) {
            const auto from = parts.front().start;
            ranges.emplace_back( from, parts.back().start + parts.back().length - from );
        }
        return ranges;
    };
    // Bounds the time, as it runs for the rows shown: about a millisecond a pair of lines
    // at most
    constexpr size_t MaxParts = 2000;
    constexpr int MaxChanges = 200;
    LineChanges changes;
    std::optional<std::vector<Step>> steps;
    if ( a.size() <= MaxParts && b.size() <= MaxParts ) {
        steps = myers(
            static_cast<int>( a.size() ), static_cast<int>( b.size() ),
            [ &a, &b ]( int x, int y ) {
                return a[ static_cast<size_t>( x ) ].key == b[ static_cast<size_t>( y ) ].key;
            },
            MaxChanges, nullptr );
    }
    if ( !steps ) {
        changes.first = whole( a );
        changes.second = whole( b );
        return changes;
    }

    // Short runs alike between changes are changes too, as in the semantic clean up of
    // diff-match-patch: "auth timeout" against "retry request" is one change, not letters
    // scattered over both. A run alike goes when it is no longer than the changes on each
    // side of it.
    {
        auto& all = *steps;
        // The runs of steps: [begin, end) and whether they are alike
        struct Run {
            size_t begin;
            size_t end;
            bool same;
        };
        bool absorbed = true;
        while ( absorbed ) {
            absorbed = false;
            std::vector<Run> runs;
            for ( size_t i = 0; i < all.size(); ) {
                const auto same = all[ i ] == Step::Same;
                auto j = i;
                while ( j < all.size() && ( all[ j ] == Step::Same ) == same ) {
                    ++j;
                }
                runs.push_back( { i, j, same } );
                i = j;
            }
            // The larger side of a run of changes: what is removed or what is added
            const auto changeSize = [ &all ]( const Run& run ) {
                size_t removed = 0;
                size_t added = 0;
                for ( auto i = run.begin; i < run.end; ++i ) {
                    ( all[ i ] == Step::Removed ? removed : added ) += 1;
                }
                return std::max( removed, added );
            };
            for ( size_t r = 1; r + 1 < runs.size(); ++r ) {
                const auto& run = runs[ r ];
                const auto length = run.end - run.begin;
                if ( run.same && length <= changeSize( runs[ r - 1 ] )
                     && length <= changeSize( runs[ r + 1 ] ) ) {
                    // Each step alike becomes a removed and an added one
                    std::vector<Step> replaced( length, Step::Removed );
                    replaced.insert( replaced.end(), length, Step::Added );
                    all.erase( all.begin() + static_cast<std::ptrdiff_t>( run.begin ),
                               all.begin() + static_cast<std::ptrdiff_t>( run.end ) );
                    all.insert( all.begin() + static_cast<std::ptrdiff_t>( run.begin ),
                                replaced.begin(), replaced.end() );
                    absorbed = true;
                    break;
                }
            }
        }
    }

    // The parts changed, the ones next to each other in the line in one range
    const auto addPart = []( std::vector<std::pair<int, int>>& ranges, const Part& part ) {
        if ( !ranges.empty() && ranges.back().first + ranges.back().second == part.start ) {
            ranges.back().second = part.start + part.length - ranges.back().first;
        }
        else {
            ranges.emplace_back( part.start, part.length );
        }
    };
    size_t x = 0;
    size_t y = 0;
    for ( const auto step : *steps ) {
        switch ( step ) {
        case Step::Same:
            ++x;
            ++y;
            break;
        case Step::Removed:
            addPart( changes.first, a[ x ] );
            ++x;
            break;
        case Step::Added:
            addPart( changes.second, b[ y ] );
            ++y;
            break;
        }
    }
    return changes;
}

} // namespace templates
