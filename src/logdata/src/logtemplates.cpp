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
            if ( ratio >= 2.0 ) {
                compared.status = Compared::Status::More;
            }
            else if ( ratio <= 0.5 ) {
                compared.status = Compared::Status::Fewer;
            }
            score = std::abs( std::log( ratio ) );
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

// Templates of the lines of the scope as ids, and the line of each
bool templateIds( const LogData& logData, const Scope& scope, AtomicFlag& cancel,
                  std::unordered_map<QString, uint32_t>& ids, std::vector<uint32_t>& lineIds,
                  std::vector<LineNumber>& lineNumbers )
{
    ReaderAttached reader( logData );
    const auto bounds = boundsOf( logData, scope );
    forRuns( scope, bounds.begin, bounds.end, [ & ]( uint64_t start, uint64_t count ) {
        if ( cancel ) {
            return;
        }
        const auto lines = logData.getLines( LineNumber( start ), LinesCount( count ) );
        for ( size_t i = 0; i < lines.size(); ++i ) {
            const auto id = ids.emplace( maskLine( lines[ i ] ),
                                         static_cast<uint32_t>( ids.size() ) )
                                .first->second;
            lineIds.push_back( id );
            lineNumbers.emplace_back( start + i );
        }
    } );
    return !cancel;
}

} // namespace

Diff diff( const LogData& first, const Scope& firstScope, const LogData& second,
           const Scope& secondScope, AtomicFlag& cancel )
{
    Diff result;
    if ( firstScope.size( first ) > MaxDiffLines || secondScope.size( second ) > MaxDiffLines ) {
        result.error = translate( "Line by line comparison is limited to %L1 lines on each side: "
                                  "select fewer lines or use the filtered view" )
                           .arg( MaxDiffLines );
        return result;
    }

    std::unordered_map<QString, uint32_t> ids;
    std::vector<uint32_t> a;
    std::vector<uint32_t> b;
    std::vector<LineNumber> aLines;
    std::vector<LineNumber> bLines;
    if ( !templateIds( first, firstScope, cancel, ids, a, aLines )
         || !templateIds( second, secondScope, cancel, ids, b, bLines ) ) {
        result.cancelled = true;
        return result;
    }

    // Same start and end are taken out of the comparison
    size_t prefix = 0;
    while ( prefix < a.size() && prefix < b.size() && a[ prefix ] == b[ prefix ] ) {
        ++prefix;
    }
    size_t suffix = 0;
    while ( suffix < a.size() - prefix && suffix < b.size() - prefix
            && a[ a.size() - 1 - suffix ] == b[ b.size() - 1 - suffix ] ) {
        ++suffix;
    }

    const auto n = static_cast<int>( a.size() - prefix - suffix );
    const auto m = static_cast<int>( b.size() - prefix - suffix );
    const auto at = [ prefix ]( const std::vector<uint32_t>& ids, int i ) {
        return ids[ prefix + static_cast<size_t>( i ) ];
    };
    const auto lineOf = [ prefix ]( const std::vector<LineNumber>& lines, int i ) {
        return lines[ prefix + static_cast<size_t>( i ) ];
    };

    // Myers: the furthest x on each diagonal for each number of changes d, only the
    // diagonals reached kept (memory in d squared)
    const auto maxD = qMin( n + m, MaxDiffDistance );
    const auto offset = maxD + 1;
    std::vector<int> furthest( static_cast<size_t>( 2 * maxD + 3 ), 0 );
    const auto v = [ &furthest, offset ]( int k ) -> int& {
        return furthest[ static_cast<size_t>( offset + k ) ];
    };
    std::vector<std::vector<int>> trace;
    int distance = -1;
    for ( int d = 0; d <= maxD && distance < 0; ++d ) {
        if ( cancel ) {
            result.cancelled = true;
            return result;
        }
        for ( int k = -d; k <= d; k += 2 ) {
            int x = ( k == -d || ( k != d && v( k - 1 ) < v( k + 1 ) ) ) ? v( k + 1 )
                                                                          : v( k - 1 ) + 1;
            int y = x - k;
            while ( x < n && y < m && at( a, x ) == at( b, y ) ) {
                ++x;
                ++y;
            }
            v( k ) = x;
            if ( x >= n && y >= m ) {
                distance = d;
            }
        }
        trace.emplace_back( furthest.begin() + offset - d, furthest.begin() + offset + d + 1 );
    }
    if ( distance < 0 ) {
        result.error = translate( "The lines differ too much to be compared line by line "
                                  "(more than %L1 changes)" )
                           .arg( MaxDiffDistance );
        return result;
    }

    // Back from the end
    std::vector<DiffLine> middle;
    int x = n;
    int y = m;
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
            middle.push_back( { DiffLine::Op::Same, lineOf( aLines, x ), lineOf( bLines, y ) } );
        }
        if ( x == previousX ) {
            --y;
            middle.push_back( { DiffLine::Op::Added, {}, lineOf( bLines, y ) } );
        }
        else {
            --x;
            middle.push_back( { DiffLine::Op::Removed, lineOf( aLines, x ), {} } );
        }
    }
    while ( x > 0 && y > 0 ) {
        --x;
        --y;
        middle.push_back( { DiffLine::Op::Same, lineOf( aLines, x ), lineOf( bLines, y ) } );
    }
    std::reverse( middle.begin(), middle.end() );

    result.lines.reserve( prefix + middle.size() + suffix );
    for ( size_t i = 0; i < prefix; ++i ) {
        result.lines.push_back( { DiffLine::Op::Same, aLines[ i ], bLines[ i ] } );
    }
    result.lines.insert( result.lines.end(), middle.begin(), middle.end() );
    for ( size_t i = 0; i < suffix; ++i ) {
        result.lines.push_back( { DiffLine::Op::Same, aLines[ a.size() - suffix + i ],
                                  bLines[ b.size() - suffix + i ] } );
    }
    return result;
}

} // namespace templates
