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

bool scanMatches( const LogData& logData, const std::vector<RegularExpressionPattern>& patterns,
                  AtomicFlag& cancel, const Progress& progress, const LineVisitor& visit,
                  QString* error )
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
                                  progress( percent * 7 / 10 );
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

    // Then only the matching lines are read, in runs of lines that follow each other
    const auto total = all.cardinality();
    uint64_t done = 0;
    int reported = 70;
    std::vector<uint32_t> matched;
    uint64_t runStart = 0;
    uint64_t runLength = 0;
    const auto flush = [ & ]() {
        if ( runLength == 0 ) {
            return;
        }
        const auto lines = logData.getLines( LineNumber( runStart ), LinesCount( runLength ) );
        for ( size_t i = 0; i < lines.size(); ++i ) {
            const auto line = runStart + i;
            matched.clear();
            for ( size_t p = 0; p < results.newMatches.size(); ++p ) {
                if ( results.newMatches[ p ].contains( line ) ) {
                    matched.push_back( static_cast<uint32_t>( p ) );
                }
            }
            visit( LineNumber( line ), lines[ i ], matched );
        }
        done += runLength;
        runLength = 0;
        if ( progress && total > 0 ) {
            const auto percent = 70 + static_cast<int>( done * 30 / total );
            if ( percent > reported ) {
                reported = percent;
                progress( percent );
            }
        }
    };

    for ( const auto line : all ) {
        if ( runLength > 0 && ( line != runStart + runLength || runLength >= MaxRunLines ) ) {
            flush();
            if ( cancel ) {
                return false;
            }
        }
        if ( runLength == 0 ) {
            runStart = line;
        }
        ++runLength;
    }
    flush();
    return !cancel;
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

    const auto startRegex = compile( spec.startPattern, spec.caseSensitive );
    const auto endRegex = compile( spec.endPattern, spec.caseSensitive );

    struct Open {
        LineNumber line;
        timestamps::LineTimes times;
    };
    std::unordered_map<QString, std::deque<Open>> open;
    size_t openCount = 0;
    uint64_t overflow = 0;

    std::vector<int64_t> samples;
    std::mt19937_64 random( 42 );
    double sum = 0;
    int fractionDigits = 0;

    const auto slower = []( const DurationPair& a, const DurationPair& b ) { return a.us > b.us; };
    std::priority_queue<DurationPair, std::vector<DurationPair>, decltype( slower )> slowest(
        slower );

    const auto visit = [ & ]( LineNumber line, const QString& text,
                              const std::vector<uint32_t>& patterns ) {
        const auto isStart
            = std::find( patterns.begin(), patterns.end(), 0u ) != patterns.end();
        const auto isEnd = std::find( patterns.begin(), patterns.end(), 1u ) != patterns.end();

        // A line matching both ends a pair before starting one
        if ( isEnd ) {
            const auto key = captureKey( endRegex, text );
            auto queue = open.find( key );
            if ( queue == open.end() || queue->second.empty() ) {
                ++result.unmatchedEnds;
            }
            else {
                const auto start = queue->second.front();
                queue->second.pop_front();
                --openCount;
                const auto difference
                    = timestamps::difference( start.times, timestamps::parseLine( text ) );
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
            open[ captureKey( startRegex, text ) ].push_back(
                { line, timestamps::parseLine( text ) } );
            ++openCount;
        }
    };

    if ( !scanMatches( logData,
                       { RegularExpressionPattern( spec.startPattern, spec.caseSensitive, false,
                                                   false, false ),
                         RegularExpressionPattern( spec.endPattern, spec.caseSensitive, false,
                                                   false, false ) },
                       cancel, progress, visit, &result.error ) ) {
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
std::optional<int64_t> elapsed( const timestamps::LineTimes& from, const timestamps::LineTimes& to )
{
    return timestamps::difference( from, to ).us;
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
                     const Progress& progress )
{
    RuleResult result;
    result.totalFindings.assign( rules.size(), 0 );

    // Each different pattern is searched once for all the rules
    std::vector<RegularExpressionPattern> patterns;
    std::vector<QRegularExpression> regexes;
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
                regexes.push_back( compile( pattern, rules[ r ].caseSensitive ) );
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

    const auto visit = [ & ]( LineNumber line, const QString& text,
                              const std::vector<uint32_t>& matched ) {
        std::optional<timestamps::LineTimes> times;
        const auto lineTimes = [ & ]() -> const timestamps::LineTimes& {
            if ( !times ) {
                times = timestamps::parseLine( text );
            }
            return *times;
        };
        const auto has = [ &matched ]( uint32_t pattern ) {
            return std::find( matched.begin(), matched.end(), pattern ) != matched.end();
        };

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
                    const auto time = elapsed( open.front().times, lineTimes() );
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
                    const auto key = captureKey( regexes[ ruleSteps[ 1 ] ], text );
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
                                      captureKey( regexes[ ruleSteps[ 0 ] ], text ),
                                      lineTimes() } );
                }
                break;

            case Rule::Type::Sequence: {
                // Later steps first, so one line does not move an instance twice
                for ( auto step = ruleSteps.size() - 1; step >= 1; --step ) {
                    if ( !has( ruleSteps[ step ] ) ) {
                        continue;
                    }
                    const auto key = captureKey( regexes[ ruleSteps[ step ] ], text );
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
                        if ( const auto time = elapsed( found->times, lineTimes() ) ) {
                            detail = translate( "In %1" ).arg(
                                timestamps::formatDuration( *time, 3, false ) );
                        }
                        addFinding( r, found->lines, detail );
                        open.erase( found );
                    }
                }
                if ( has( ruleSteps[ 0 ] ) && open.size() < MaxOpenInstances ) {
                    open.push_back( { { line },
                                      captureKey( regexes[ ruleSteps[ 0 ] ], text ),
                                      lineTimes() } );
                }
                break;
            }

            case Rule::Type::Count: {
                if ( !has( ruleSteps[ 0 ] ) ) {
                    break;
                }
                const auto key = captureKey( regexes[ ruleSteps[ 0 ] ], text );
                auto& window = windows[ r ][ key ];
                window.push_back( { { line }, key, lineTimes() } );
                if ( withinUs ) {
                    while ( window.size() > 1 ) {
                        const auto time = elapsed( window.front().times, lineTimes() );
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

    if ( !scanMatches( logData, patterns, cancel, progress, visit, &result.error ) ) {
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
            const auto time = elapsed( open.times, *endTimes );
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
