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

#ifndef FLOGG_LOGANALYSIS_H
#define FLOGG_LOGANALYSIS_H

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <QString>
#include <QStringList>

#include "atomicflag.h"
#include "linetypes.h"
#include "regularexpressionpattern.h"
#include "timestamp.h"

class LogData;

// Analyses run in the background on the whole file: the patterns are searched in one pass
// like the filters (hyperscan, in parallel), then only the matching lines are read again
// for their captures and time stamps. Nothing is kept once the result is built.
namespace analysis {

using Progress = std::function<void( int percent )>;

// The lines matching any of the patterns, in order, with the patterns they match.
// Returns false when cancelled or when a pattern is not valid (`error` set).
using LineVisitor = std::function<void( LineNumber line, const QString& text,
                                        const std::vector<uint32_t>& patterns )>;
bool scanMatches( const LogData& logData, const std::vector<RegularExpressionPattern>& patterns,
                  AtomicFlag& cancel, const Progress& progress, const LineVisitor& visit,
                  QString* error = nullptr );

// The capture groups of the match joined, empty without groups
QString captureKey( const QRegularExpression& regex, const QString& text );

// The pattern as a regular expression: plain text is escaped (it then has no groups)
QString toRegex( const QString& pattern, bool regex );

//
// Time between pairs of events: an end line goes with the latest start line not ended yet
// that has the same key (the capture groups of both patterns, none when they have none).
// A start followed by another one with the same key before any end stays open: it is
// told as never ended, the pairs after it are not shifted.
//

struct DurationSpec {
    QString startPattern;
    QString endPattern;
    bool caseSensitive = false;
    // The time stamps compared, the first of the line by default (see timestamps::difference)
    std::optional<timestamps::Format> timeFormat;
    // The patterns are regular expressions, plain text when false
    bool regex = true;
};

struct DurationPair {
    LineNumber start;
    LineNumber end;
    int64_t us = 0;
    QString key;
};

struct DurationResult {
    QString error;
    bool cancelled = false;
    uint64_t pairs = 0;
    // Starts never ended, ends without a start
    uint64_t openStarts = 0;
    uint64_t unmatchedEnds = 0;
    // Pairs whose lines have no time stamp of the same kind
    uint64_t withoutTime = 0;
    // Pairs ending before they start: a clock set back between them
    uint64_t negative = 0;
    // The slowest pairs, slowest first
    std::vector<DurationPair> slowest;
    // The first starts never ended
    std::vector<LineNumber> openStartLines;
    int64_t minUs = 0;
    int64_t maxUs = 0;
    double meanUs = 0;
    int64_t p50Us = 0;
    int64_t p90Us = 0;
    int64_t p99Us = 0;
    // The percentiles come from a sample of the durations
    bool sampled = false;
    int fractionDigits = 3;
};

// Bounds of the memory used: open starts, durations kept for the percentiles, pairs listed
constexpr size_t MaxOpenStarts = 100000;
constexpr size_t MaxDurationSamples = 2000000;
constexpr size_t MaxSlowestPairs = 1000;

DurationResult measureDurations( const LogData& logData, const DurationSpec& spec,
                                 AtomicFlag& cancel, const Progress& progress );

//
// Rules: known problems told by patterns, read from a JSON file shared like the filters.
//
//  match     a line matching the pattern
//  sequence  the patterns in this order (within the time, with the same key)
//  absence   the first pattern not followed by the second within the time
//  count     the pattern at least `count` times within the time (with the same key)
//

struct Rule {
    enum class Type { Match, Sequence, Absence, Count };
    QString name;
    QString description;
    // Free text shown with the results: what to do, who to ask; web addresses in it are
    // links (read from "link" too, the older name)
    QString note;
    QString severity;
    Type type = Type::Match;
    QStringList patterns;
    bool caseSensitive = false;
    // The patterns are regular expressions, plain text when false ("regex": false)
    bool regex = true;
    std::optional<double> withinSeconds;
    int count = 2;
};

struct RuleSet {
    std::vector<Rule> rules;
    QString error;
};

RuleSet parseRules( const QByteArray& json );
// The rules as a rule file, read back the same by parseRules()
QByteArray rulesToJson( const std::vector<Rule>& rules );
QString ruleTypeName( Rule::Type type );
// Why a rule can not be used, empty when it can
QString checkRule( const Rule& rule );
QByteArray exampleRules();

struct Finding {
    size_t rule = 0;
    // The line to show, then the other lines of the finding
    LineNumber line;
    std::vector<LineNumber> lines;
    QString detail;
};

struct RuleResult {
    QString error;
    bool cancelled = false;
    std::vector<Finding> findings;
    // Per rule, when it had more findings than kept
    std::vector<uint64_t> totalFindings;
};

constexpr size_t MaxFindingsPerRule = 10000;
constexpr size_t MaxOpenInstances = 100000;

RuleResult runRules( const LogData& logData, const std::vector<Rule>& rules, AtomicFlag& cancel,
                     const Progress& progress,
                     std::optional<timestamps::Format> timeFormat = {} );

} // namespace analysis

#endif
