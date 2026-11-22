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

#include "loganalysis.h"

#include <algorithm>
#include <deque>
#include <map>
#include <queue>
#include <random>
#include <unordered_map>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <tbb/blocked_range.h>
#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>

#include "logdata.h"
#include "logfiltereddataworker.h"
#include "timestamp.h"

namespace analysis {

namespace {

QString translate( const char* text )
{
    return QCoreApplication::translate( "analysis", text );
}

QRegularExpression compile( const QString& pattern, bool caseSensitive )
{
    QRegularExpression::PatternOptions options = QRegularExpression::UseUnicodePropertiesOption;
    if ( !caseSensitive ) {
        options |= QRegularExpression::CaseInsensitiveOption;
    }
    return QRegularExpression( pattern, options );
}

// Lines read at once when the matching lines follow each other
constexpr uint64_t MaxRunLines = 1024;

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

QString captureKey( const QRegularExpression& regex, const QString& text )
{
    if ( regex.captureCount() <= 0 ) {
        return {};
    }
    const auto match = regex.match( text );
    if ( !match.hasMatch() ) {
        return {};
    }
    QStringList parts;
    for ( int i = 1; i <= regex.captureCount(); ++i ) {
        parts.append( match.captured( i ) );
    }
    return parts.join( QChar( 0x1f ) );
}

namespace {

// A line matching some of the patterns, with what the analyses need from it, worked out in
// parallel before the lines are handled in order
struct Candidate {
    LineNumber line;
    QString text;
    std::vector<uint32_t> patterns;
    // The capture groups of each matched pattern (empty for the others)
    std::vector<QString> keys;
    timestamps::LineTimes times;

    bool has( uint32_t pattern ) const
    {
        return std::find( patterns.begin(), patterns.end(), pattern ) != patterns.end();
    }
    const QString& key( uint32_t pattern ) const
    {
        return keys[ pattern ];
    }
};

constexpr size_t BatchLines = 32768;

using BatchVisitor = std::function<void( std::vector<Candidate>& batch )>;

// Searches the patterns (one pass, like the filters), reads the matching lines in batches,
// works out their time stamps and capture groups in parallel when `prepare` is set, then
// hands each batch over in the order of the lines
bool scanCandidates( const LogData& logData, const std::vector<RegularExpressionPattern>& patterns,
                     bool prepare, AtomicFlag& cancel, const Progress& progress,
                     const BatchVisitor& visit, QString* error )
{
    for ( const auto& pattern : patterns ) {
        const auto regex = static_cast<QRegularExpression>( pattern );
        if ( !regex.isValid() ) {
            if ( error ) {
                *error = translate( "Invalid regular expression %1: %2" )
                             .arg( pattern.pattern, regex.errorString() );
            }
            return false;
        }
    }
    if ( patterns.empty() ) {
        return true;
    }

    ReaderAttached reader( logData );
    const auto nbLines = logData.getNbLine();

    // One pass over the file for all the patterns, like the filters
    MultiSearchData searchData;
    searchData.reset( 0, patterns.size() );
    {
        klogg::vector<RegularExpressionPattern> searched( patterns.begin(), patterns.end() );
        MultiSearchOperation operation( logData, cancel, searched, 0_lnum,
                                        LineNumber( nbLines.get() ), 0 );
        if ( progress ) {
            QObject::connect( &operation, &MultiSearchOperation::searchProgressed,
                              [ &progress ]( LinesCount, int percent, LineNumber ) {
                                  progress( percent * 4 / 10 );
                              } );
        }
        operation.run( searchData );
    }
    if ( cancel ) {
        return false;
    }

    const auto results = searchData.takeCurrentResults();
    SearchResultArray all;
    for ( const auto& matches : results.newMatches ) {
        all |= matches;
    }

    // Each thread its own expressions: their first match compiles them
    tbb::enumerable_thread_specific<std::vector<QRegularExpression>> threadRegexes( [ & ] {
        std::vector<QRegularExpression> regexes;
        for ( const auto& pattern : patterns ) {
            regexes.push_back( compile( pattern.pattern, pattern.isCaseSensitive ) );
        }
        return regexes;
    } );

    const auto total = all.cardinality();
    uint64_t done = 0;
    int reported = 40;
    std::vector<Candidate> batch;
    batch.reserve( BatchLines );

    const auto handOver = [ & ]() {
        if ( batch.empty() ) {
            return;
        }
        if ( prepare ) {
            tbb::parallel_for( tbb::blocked_range<size_t>( 0, batch.size(), 256 ),
                               [ & ]( const tbb::blocked_range<size_t>& range ) {
                                   auto& regexes = threadRegexes.local();
                                   for ( auto i = range.begin(); i != range.end(); ++i ) {
                                       auto& candidate = batch[ i ];
                                       candidate.times = timestamps::parseLine( candidate.text );
                                       candidate.keys.resize( patterns.size() );
                                       for ( const auto p : candidate.patterns ) {
                                           candidate.keys[ p ]
                                               = captureKey( regexes[ p ], candidate.text );
                                       }
                                   }
                               } );
        }
        visit( batch );
        done += batch.size();
        batch.clear();
        if ( progress && total > 0 ) {
            const auto percent = 40 + static_cast<int>( done * 60 / total );
            if ( percent > reported ) {
                reported = percent;
                progress( percent );
            }
        }
    };

    // Only the matching lines are read, in runs of lines that follow each other
    uint64_t runStart = 0;
    uint64_t runLength = 0;
    const auto read = [ & ]() {
        if ( runLength == 0 ) {
            return;
        }
        auto lines = logData.getLines( LineNumber( runStart ), LinesCount( runLength ) );
        for ( size_t i = 0; i < lines.size(); ++i ) {
            Candidate candidate;
            candidate.line = LineNumber( runStart + i );
            candidate.text = std::move( lines[ i ] );
            for ( size_t p = 0; p < results.newMatches.size(); ++p ) {
                if ( results.newMatches[ p ].contains( runStart + i ) ) {
                    candidate.patterns.push_back( static_cast<uint32_t>( p ) );
                }
            }
            batch.push_back( std::move( candidate ) );
        }
        runLength = 0;
        if ( batch.size() >= BatchLines ) {
            handOver();
        }
    };

    for ( const auto line : all ) {
        if ( runLength > 0 && ( line != runStart + runLength || runLength >= MaxRunLines ) ) {
            read();
            if ( cancel ) {
                return false;
            }
        }
        if ( runLength == 0 ) {
            runStart = line;
        }
        ++runLength;
    }
    read();
    handOver();
    return !cancel;
}

} // namespace

bool scanMatches( const LogData& logData, const std::vector<RegularExpressionPattern>& patterns,
                  AtomicFlag& cancel, const Progress& progress, const LineVisitor& visit,
                  QString* error )
{
    return scanCandidates( logData, patterns, false, cancel, progress,
                           [ &visit ]( std::vector<Candidate>& batch ) {
                               for ( const auto& candidate : batch ) {
                                   visit( candidate.line, candidate.text, candidate.patterns );
                               }
                           },
                           error );
}

//
// Durations
//

DurationResult measureDurations( const LogData& logData, const DurationSpec& spec,
                                 AtomicFlag& cancel, const Progress& progress )
{
    DurationResult result;
    if ( spec.startPattern.isEmpty() || spec.endPattern.isEmpty() ) {
        result.error = translate( "Both a start and an end pattern are needed" );
        return result;
    }

    struct Open {
        LineNumber line;
        timestamps::LineTimes times;
    };
    // Keys without an open start are taken out, so the map stays as small as the starts
    // still open
    std::unordered_map<QString, std::vector<Open>> open;
    size_t openCount = 0;
    uint64_t overflow = 0;

    std::vector<int64_t> samples;
    std::mt19937_64 random( 42 );
    double sum = 0;
    int fractionDigits = 0;

    const auto slower = []( const DurationPair& a, const DurationPair& b ) { return a.us > b.us; };
    std::priority_queue<DurationPair, std::vector<DurationPair>, decltype( slower )> slowest(
        slower );

    const auto visitOne = [ & ]( Candidate& candidate ) {
        const auto line = candidate.line;
        const auto isStart = candidate.has( 0 );
        const auto isEnd = candidate.has( 1 );

        // A line matching both ends a pair before starting one
        if ( isEnd ) {
            const auto& key = candidate.key( 1 );
            auto queue = open.find( key );
            if ( queue == open.end() || queue->second.empty() ) {
                ++result.unmatchedEnds;
            }
            else {
                // The latest start: a start never ended (a chip that did not wake up)
                // stays open instead of shifting every pair after it
                const auto start = queue->second.back();
                queue->second.pop_back();
                if ( queue->second.empty() ) {
                    open.erase( queue );
                }
                --openCount;
                const auto difference
                    = timestamps::difference( start.times, candidate.times, spec.timeFormat );
                if ( !difference.us ) {
                    ++result.withoutTime;
                }
                else if ( *difference.us < 0 ) {
                    ++result.negative;
                }
                else {
                    const auto us = *difference.us;
                    fractionDigits
                        = qMax( fractionDigits, qMax( difference.first->fractionDigits,
                                                      difference.second->fractionDigits ) );
                    if ( result.pairs == 0 ) {
                        result.minUs = us;
                        result.maxUs = us;
                    }
                    result.minUs = qMin( result.minUs, us );
                    result.maxUs = qMax( result.maxUs, us );
                    sum += static_cast<double>( us );
                    ++result.pairs;

                    // Reservoir sampling keeps the memory bounded
                    if ( samples.size() < MaxDurationSamples ) {
                        samples.push_back( us );
                    }
                    else {
                        std::uniform_int_distribution<uint64_t> pick( 0, result.pairs - 1 );
                        const auto index = pick( random );
                        if ( index < MaxDurationSamples ) {
                            samples[ index ] = us;
                        }
                    }

                    DurationPair pair{ start.line, line, us, key };
                    if ( slowest.size() < MaxSlowestPairs ) {
                        slowest.push( std::move( pair ) );
                    }
                    else if ( us > slowest.top().us ) {
                        slowest.pop();
                        slowest.push( std::move( pair ) );
                    }
                }
            }
        }

        if ( isStart ) {
            if ( openCount >= MaxOpenStarts ) {
                ++overflow;
                return;
            }
            open[ candidate.key( 0 ) ].push_back( { line, std::move( candidate.times ) } );
            ++openCount;
        }
    };

    if ( !scanCandidates( logData,
                       { RegularExpressionPattern( spec.startPattern, spec.caseSensitive, false,
                                                   false, false ),
                         RegularExpressionPattern( spec.endPattern, spec.caseSensitive, false,
                                                   false, false ) },
                       true, cancel, progress,
                       [ &visitOne ]( std::vector<Candidate>& batch ) {
                           for ( auto& candidate : batch ) {
                               visitOne( candidate );
                           }
                       },
                       &result.error ) ) {
        result.cancelled = result.error.isEmpty();
        return result;
    }

    result.openStarts = openCount + overflow;
    for ( const auto& [ key, starts ] : open ) {
        for ( const auto& start : starts ) {
            result.openStartLines.push_back( start.line );
        }
    }
    std::sort( result.openStartLines.begin(), result.openStartLines.end() );
    if ( result.openStartLines.size() > 100 ) {
        result.openStartLines.resize( 100 );
    }

    while ( !slowest.empty() ) {
        result.slowest.push_back( slowest.top() );
        slowest.pop();
    }
    std::reverse( result.slowest.begin(), result.slowest.end() );

    if ( result.pairs > 0 ) {
        result.meanUs = sum / static_cast<double>( result.pairs );
        result.sampled = result.pairs > samples.size();
        std::sort( samples.begin(), samples.end() );
        const auto at = [ &samples ]( double fraction ) {
            const auto index = static_cast<size_t>( fraction * static_cast<double>( samples.size() - 1 ) );
            return samples[ index ];
        };
        result.p50Us = at( 0.5 );
        result.p90Us = at( 0.9 );
        result.p99Us = at( 0.99 );
    }
    result.fractionDigits = fractionDigits;
    return result;
}

//
// Rules
//

RuleSet parseRules( const QByteArray& json )
{
    RuleSet set;
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson( json, &parseError );
    if ( document.isNull() ) {
        set.error = translate( "Not a rule file: %1" ).arg( parseError.errorString() );
        return set;
    }

    static const std::map<QString, Rule::Type> types = {
        { "match", Rule::Type::Match },
        { "sequence", Rule::Type::Sequence },
        { "absence", Rule::Type::Absence },
        { "count", Rule::Type::Count },
    };

    for ( const auto& value : document.object().value( "rules" ).toArray() ) {
        const auto object = value.toObject();
        Rule rule;
        rule.name = object.value( "name" ).toString();
        rule.description = object.value( "description" ).toString();
        rule.link = object.value( "link" ).toString();
        rule.severity = object.value( "severity" ).toString( "warning" );
        rule.caseSensitive = object.value( "caseSensitive" ).toBool( false );
        if ( object.contains( "withinSeconds" ) ) {
            rule.withinSeconds = object.value( "withinSeconds" ).toDouble();
        }
        rule.count = object.value( "count" ).toInt( 2 );
        for ( const auto& pattern : object.value( "patterns" ).toArray() ) {
            rule.patterns.append( pattern.toString() );
        }
        if ( object.contains( "pattern" ) ) {
            rule.patterns.prepend( object.value( "pattern" ).toString() );
        }

        const auto typeName = object.value( "type" ).toString( "match" ).toLower();
        const auto type = types.find( typeName );
        const auto name = rule.name.isEmpty() ? QString::number( set.rules.size() + 1 ) : rule.name;
        if ( type == types.end() ) {
            set.error = translate( "Rule %1: unknown type %2" ).arg( name, typeName );
            return set;
        }
        rule.type = type->second;

        const auto needed = rule.type == Rule::Type::Sequence ? 2
                            : rule.type == Rule::Type::Absence ? 2
                                                               : 1;
        if ( rule.patterns.size() < needed
             || ( rule.type == Rule::Type::Absence && rule.patterns.size() != 2 ) ) {
            set.error = translate( "Rule %1: %2 pattern(s) needed" )
                            .arg( name )
                            .arg( rule.type == Rule::Type::Sequence ? translate( "2 or more" )
                                                                    : QString::number( needed ) );
            return set;
        }
        for ( const auto& pattern : rule.patterns ) {
            const auto regex = compile( pattern, rule.caseSensitive );
            if ( pattern.isEmpty() || !regex.isValid() ) {
                set.error = translate( "Rule %1: invalid regular expression %2" )
                                .arg( name, pattern );
                return set;
            }
        }
        if ( rule.type == Rule::Type::Count && rule.count < 1 ) {
            set.error = translate( "Rule %1: the count must be 1 or more" ).arg( name );
            return set;
        }
        set.rules.push_back( std::move( rule ) );
    }

    if ( set.rules.empty() ) {
        set.error = translate( "No rules in the file" );
    }
    return set;
}

QString ruleTypeName( Rule::Type type )
{
    switch ( type ) {
    case Rule::Type::Match:
        return "match";
    case Rule::Type::Sequence:
        return "sequence";
    case Rule::Type::Absence:
        return "absence";
    case Rule::Type::Count:
        return "count";
    }
    return "match";
}

QString checkRule( const Rule& rule )
{
    const auto needed = rule.type == Rule::Type::Sequence || rule.type == Rule::Type::Absence ? 2
                                                                                               : 1;
    if ( rule.patterns.size() < needed ) {
        return translate( "%1 pattern(s) needed" )
            .arg( rule.type == Rule::Type::Sequence ? translate( "2 or more" )
                                                    : QString::number( needed ) );
    }
    if ( rule.type == Rule::Type::Absence && rule.patterns.size() != 2 ) {
        return translate( "An absence takes 2 patterns" );
    }
    if ( ( rule.type == Rule::Type::Match || rule.type == Rule::Type::Count )
         && rule.patterns.size() != 1 ) {
        return translate( "This kind of rule takes 1 pattern" );
    }
    for ( const auto& pattern : rule.patterns ) {
        const auto regex = compile( pattern, rule.caseSensitive );
        if ( pattern.isEmpty() || !regex.isValid() ) {
            return translate( "Invalid regular expression %1: %2" )
                .arg( pattern, regex.errorString() );
        }
    }
    if ( rule.type == Rule::Type::Count && rule.count < 1 ) {
        return translate( "The count must be 1 or more" );
    }
    return {};
}

QByteArray rulesToJson( const std::vector<Rule>& rules )
{
    QJsonArray array;
    for ( const auto& rule : rules ) {
        QJsonObject object;
        object.insert( "name", rule.name );
        if ( !rule.description.isEmpty() ) {
            object.insert( "description", rule.description );
        }
        object.insert( "type", ruleTypeName( rule.type ) );
        object.insert( "severity", rule.severity.isEmpty() ? QString( "warning" ) : rule.severity );
        object.insert( "patterns", QJsonArray::fromStringList( rule.patterns ) );
        if ( rule.caseSensitive ) {
            object.insert( "caseSensitive", true );
        }
        if ( rule.withinSeconds ) {
            object.insert( "withinSeconds", *rule.withinSeconds );
        }
        if ( rule.type == Rule::Type::Count ) {
            object.insert( "count", rule.count );
        }
        if ( !rule.link.isEmpty() ) {
            object.insert( "link", rule.link );
        }
        array.append( object );
    }
    return QJsonDocument( QJsonObject{ { "rules", array } } ).toJson( QJsonDocument::Indented );
}

QByteArray exampleRules()
{
    return R"json({
  "rules": [
    {
      "name": "Crash",
      "description": "An exception that was not caught",
      "type": "match",
      "severity": "error",
      "patterns": [ "FATAL EXCEPTION|Unhandled exception|Segmentation fault" ]
    },
    {
      "name": "Connection never established",
      "description": "Connecting did not succeed within 5 seconds. The groups in parentheses must be the same in both lines (here the connection id).",
      "type": "absence",
      "severity": "error",
      "patterns": [ "connect start id=(\\d+)", "connected id=(\\d+)" ],
      "withinSeconds": 5
    },
    {
      "name": "Retry storm",
      "description": "At least 3 retries of the same request within 60 seconds",
      "type": "count",
      "severity": "warning",
      "patterns": [ "retry request=(\\w+)" ],
      "count": 3,
      "withinSeconds": 60
    },
    {
      "name": "Login then logout right away",
      "description": "The lines in this order for the same user within 2 seconds",
      "type": "sequence",
      "severity": "info",
      "patterns": [ "login user=(\\w+)", "logout user=(\\w+)" ],
      "withinSeconds": 2,
      "link": "https://example.com/wiki/login-loop"
    }
  ]
}
)json";
}

namespace {

struct Instance {
    std::vector<LineNumber> lines;
    QString key;
    timestamps::LineTimes times;
    size_t step = 1;
};

bool compatible( const QString& a, const QString& b )
{
    return a.isEmpty() || b.isEmpty() || a == b;
}

// Time from `from` to `to` when both lines have a comparable time
std::optional<int64_t> elapsed( const timestamps::LineTimes& from, const timestamps::LineTimes& to,
                                std::optional<timestamps::Format> format )
{
    return timestamps::difference( from, to, format ).us;
}

// The time of the last line of the file that has one
timestamps::LineTimes endOfFileTimes( const LogData& logData )
{
    const auto nbLines = logData.getNbLine().get();
    const auto first = nbLines > 200 ? nbLines - 200 : 0;
    const auto lines = logData.getLines( LineNumber( first ), LinesCount( nbLines - first ) );
    for ( auto line = lines.rbegin(); line != lines.rend(); ++line ) {
        auto times = timestamps::parseLine( *line );
        if ( !times.isEmpty() ) {
            return times;
        }
    }
    return {};
}

} // namespace

RuleResult runRules( const LogData& logData, const std::vector<Rule>& rules, AtomicFlag& cancel,
                     const Progress& progress, std::optional<timestamps::Format> timeFormat )
{
    RuleResult result;
    result.totalFindings.assign( rules.size(), 0 );

    // Each different pattern is searched once for all the rules
    std::vector<RegularExpressionPattern> patterns;
    std::map<std::pair<QString, bool>, uint32_t> patternIndex;
    std::vector<std::vector<uint32_t>> steps( rules.size() );
    for ( size_t r = 0; r < rules.size(); ++r ) {
        for ( const auto& pattern : rules[ r ].patterns ) {
            const auto key = std::make_pair( pattern, rules[ r ].caseSensitive );
            auto index = patternIndex.find( key );
            if ( index == patternIndex.end() ) {
                index = patternIndex
                            .emplace( key, static_cast<uint32_t>( patterns.size() ) )
                            .first;
                patterns.emplace_back( pattern, rules[ r ].caseSensitive, false, false, false );
            }
            steps[ r ].push_back( index->second );
        }
    }

    std::vector<std::deque<Instance>> instances( rules.size() );
    std::vector<std::unordered_map<QString, std::deque<Instance>>> windows( rules.size() );
    std::vector<bool> countReached( rules.size(), false );
    std::vector<uint64_t> totals( rules.size(), 0 );

    const auto addFinding = [ & ]( size_t rule, std::vector<LineNumber> lines,
                                   const QString& detail ) {
        ++result.totalFindings[ rule ];
        if ( result.totalFindings[ rule ] > MaxFindingsPerRule ) {
            return;
        }
        const auto line = lines.front();
        result.findings.push_back( { rule, line, std::move( lines ), detail } );
    };

    const auto withinText = []( const Rule& rule ) {
        return timestamps::formatDuration(
            static_cast<int64_t>( *rule.withinSeconds * 1000000 ), 0, false );
    };

    const auto visit = [ & ]( const Candidate& candidate ) {
        const auto line = candidate.line;
        const auto lineTimes = [ &candidate ]() -> const timestamps::LineTimes& {
            return candidate.times;
        };
        const auto has = [ &candidate ]( uint32_t pattern ) { return candidate.has( pattern ); };
        const auto keyOf = [ &candidate ]( uint32_t pattern ) { return candidate.key( pattern ); };

        for ( size_t r = 0; r < rules.size(); ++r ) {
            const auto& rule = rules[ r ];
            const auto& ruleSteps = steps[ r ];
            const auto withinUs
                = rule.withinSeconds
                      ? std::optional<int64_t>( static_cast<int64_t>( *rule.withinSeconds * 1e6 ) )
                      : std::nullopt;
            auto& open = instances[ r ];

            // Instances past their time: absences found, sequences given up
            if ( withinUs && !open.empty() && !lineTimes().isEmpty() ) {
                while ( !open.empty() ) {
                    const auto time = elapsed( open.front().times, lineTimes(), timeFormat );
                    if ( !time || *time <= *withinUs ) {
                        break;
                    }
                    if ( rule.type == Rule::Type::Absence ) {
                        addFinding( r, open.front().lines,
                                    translate( "Not followed within %1" ).arg( withinText( rule ) ) );
                    }
                    open.pop_front();
                }
            }

            switch ( rule.type ) {
            case Rule::Type::Match:
                if ( has( ruleSteps[ 0 ] ) ) {
                    addFinding( r, { line }, {} );
                }
                break;

            case Rule::Type::Absence:
                if ( has( ruleSteps[ 1 ] ) ) {
                    const auto key = keyOf( ruleSteps[ 1 ] );
                    const auto found
                        = std::find_if( open.begin(), open.end(), [ &key ]( const auto& i ) {
                              return compatible( i.key, key );
                          } );
                    if ( found != open.end() ) {
                        open.erase( found );
                    }
                }
                if ( has( ruleSteps[ 0 ] ) && open.size() < MaxOpenInstances ) {
                    open.push_back( { { line },
                                      keyOf( ruleSteps[ 0 ] ),
                                      lineTimes() } );
                }
                break;

            case Rule::Type::Sequence: {
                // Later steps first, so one line does not move an instance twice
                for ( auto step = ruleSteps.size() - 1; step >= 1; --step ) {
                    if ( !has( ruleSteps[ step ] ) ) {
                        continue;
                    }
                    const auto key = keyOf( ruleSteps[ step ] );
                    const auto found = std::find_if(
                        open.begin(), open.end(), [ &key, step ]( const auto& i ) {
                            return i.step == step && compatible( i.key, key );
                        } );
                    if ( found == open.end() ) {
                        continue;
                    }
                    found->lines.push_back( line );
                    if ( found->key.isEmpty() ) {
                        found->key = key;
                    }
                    if ( ++found->step == ruleSteps.size() ) {
                        QString detail;
                        if ( const auto time = elapsed( found->times, lineTimes(), timeFormat ) ) {
                            detail = translate( "In %1" ).arg(
                                timestamps::formatDuration( *time, 3, false ) );
                        }
                        addFinding( r, found->lines, detail );
                        open.erase( found );
                    }
                }
                if ( has( ruleSteps[ 0 ] ) && open.size() < MaxOpenInstances ) {
                    open.push_back( { { line },
                                      keyOf( ruleSteps[ 0 ] ),
                                      lineTimes() } );
                }
                break;
            }

            case Rule::Type::Count: {
                if ( !has( ruleSteps[ 0 ] ) ) {
                    break;
                }
                const auto key = keyOf( ruleSteps[ 0 ] );
                auto& window = windows[ r ][ key ];
                window.push_back( { { line }, key, lineTimes() } );
                if ( withinUs ) {
                    while ( window.size() > 1 ) {
                        const auto time = elapsed( window.front().times, lineTimes(), timeFormat );
                        if ( !time || *time <= *withinUs ) {
                            break;
                        }
                        window.pop_front();
                    }
                }
                if ( window.size() >= static_cast<size_t>( rule.count ) ) {
                    std::vector<LineNumber> lines;
                    for ( const auto& event : window ) {
                        if ( lines.size() < 50 ) {
                            lines.push_back( event.lines.front() );
                        }
                    }
                    // The window starts again: a burst is told once
                    const auto detail
                        = withinUs ? translate( "%1 times within %2" )
                                         .arg( window.size() )
                                         .arg( withinText( rule ) )
                                   : translate( "%1 times" ).arg( window.size() );
                    window.clear();
                    if ( withinUs || !countReached[ r ] ) {
                        countReached[ r ] = true;
                        addFinding( r, std::move( lines ), detail );
                    }
                }
                break;
            }
            }
        }
    };

    if ( !scanCandidates( logData, patterns, true, cancel, progress,
                          [ &visit ]( std::vector<Candidate>& batch ) {
                              for ( const auto& candidate : batch ) {
                                  visit( candidate );
                              }
                          },
                          &result.error ) ) {
        result.cancelled = result.error.isEmpty();
        return result;
    }

    // Absences still open at the end of the file
    std::optional<timestamps::LineTimes> endTimes;
    for ( size_t r = 0; r < rules.size(); ++r ) {
        const auto& rule = rules[ r ];
        if ( rule.type != Rule::Type::Absence ) {
            continue;
        }
        for ( const auto& open : instances[ r ] ) {
            if ( !rule.withinSeconds ) {
                addFinding( r, open.lines, translate( "Never followed" ) );
                continue;
            }
            if ( !endTimes ) {
                endTimes = endOfFileTimes( logData );
            }
            const auto time = elapsed( open.times, *endTimes, timeFormat );
            const auto withinUs = static_cast<int64_t>( *rule.withinSeconds * 1e6 );
            addFinding( r, open.lines,
                        time && *time > withinUs
                            ? translate( "Not followed within %1" ).arg( withinText( rule ) )
                            : translate( "Not followed before the end of the file (less than "
                                         "%1 after)" )
                                  .arg( withinText( rule ) ) );
        }
    }

    std::stable_sort( result.findings.begin(), result.findings.end(),
                      []( const Finding& a, const Finding& b ) {
                          return a.rule != b.rule ? a.rule < b.rule : a.line < b.line;
                      } );
    return result;
}

} // namespace analysis
