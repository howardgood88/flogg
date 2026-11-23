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

#ifndef FLOGG_LOGTEMPLATES_H
#define FLOGG_LOGTEMPLATES_H

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <QString>
#include <QStringView>

#include "atomicflag.h"
#include "linetypes.h"
#include "loganalysis.h"
#include "logfiltereddataworker.h"

class LogData;

// Templates of log lines: the time stamp taken out and the parts that change from one line
// to the next (numbers, ids, addresses, quoted text, values of key=value) replaced, so that
// lines of the same kind give the same template. Two logs are compared by their templates.
namespace templates {

// The template of a line. A few hundred nanoseconds, no regular expression.
QString maskLine( QStringView line );

// Lines of a file: all of them, a range, or a set (the filtered view)
struct Scope {
    std::optional<std::pair<LineNumber, LineNumber>> range;
    std::optional<SearchResultArray> lines;

    uint64_t size( const LogData& logData ) const;
};

// Lines kept of each template, the first ones, to list under it
constexpr size_t MaxTemplateLines = 50;

struct Template {
    QString text;
    uint64_t count = 0;
    LineNumber first;
    LineNumber last;
    // The first lines, up to MaxTemplateLines, in order
    std::vector<LineNumber> lines;
};

struct Table {
    std::vector<Template> templates;
    uint64_t lines = 0;
    // More templates than kept: the lines of the others are counted in `otherLines`
    bool truncated = false;
    uint64_t otherLines = 0;
    bool cancelled = false;
};

// Bounds the memory: a few MB of templates whatever the size of the file
constexpr size_t MaxTemplates = 100000;

// In the background, in parallel
Table build( const LogData& logData, const Scope& scope, AtomicFlag& cancel,
             const analysis::Progress& progress );

// For each template, the template it is merged into: templates of 4 words or more that
// differ by one word only (a name, a state) become one with <*> there. Up to MaxMerged.
constexpr size_t MaxMerged = 20000;
std::vector<QString> mergeSimilar( const std::vector<QString>& templates );

struct Compared {
    enum class Status { Same, OnlyFirst, OnlySecond, More, Fewer };
    QString text;
    uint64_t firstCount = 0;
    uint64_t secondCount = 0;
    std::optional<LineNumber> firstLine;
    std::optional<LineNumber> secondLine;
    // The first lines of each side, up to MaxTemplateLines, in order
    std::vector<LineNumber> firstLines;
    std::vector<LineNumber> secondLines;
    Status status = Status::Same;
};

// A template is more or less frequent when its share of the lines changes by 2 or more
// and the chance of such a split with the same share in both is below this
constexpr double SignificantChance = 0.001;

// The templates of both, the most telling first: only in one, then the largest changes
// of their share of the lines (the ones marked more or fewer first). Without a second table, the templates of the first, the
// rarest first.
std::vector<Compared> compare( const Table& first, const Table* second, bool merge );

struct DiffLine {
    enum class Op { Same, Removed, Added };
    Op op = Op::Same;
    std::optional<LineNumber> first;
    std::optional<LineNumber> second;
};

// A step from the first side to the second: a line of each alike, a line of the first only,
// a line of the second only
enum class DiffStep : uint8_t { Same, Removed, Added };

struct Diff {
    std::vector<DiffStep> steps;
    // The line of each item of a side, in order (8 bytes a line)
    std::vector<LineNumber> firstLines;
    std::vector<LineNumber> secondLines;
    // Lines of the scopes past MaxDiffLines, not compared
    uint64_t firstLeftOut = 0;
    uint64_t secondLeftOut = 0;
    // Lines of parts too different to be paired in the time given, shown as removed then
    // added
    uint64_t unpairedLines = 0;
    QString error;
    bool cancelled = false;

    // The steps with their lines, for the tests
    std::vector<DiffLine> lines() const;
};

// What a line by line comparison leaves out of the lines: the time stamps found at the
// start (as for the durations), the runs of digits (pids, counters, ids)
struct DiffOptions {
    bool ignoreTimes = false;
    bool ignoreNumbers = false;
};

// The text of a line that is compared: the line itself without options
QString diffKey( QStringView line, const DiffOptions& options );

// Line by line on the text of the lines. Up to MaxDiffLines lines of each side, the others
// left out (told in the result). The lines are kept as 64 bit hashes of their text with
// their numbers: 16 bytes a line, twice that for a while, about 64 MB at most.
constexpr uint64_t MaxDiffLines = 1000000;
// Myers on the parts without lines found once on each side, in linear space: up to this
// much work (diagonals and lines followed, about a second) in all; past it the parts left
// are shown removed and added
constexpr uint64_t MaxDiffWork = 1000000000;
Diff diff( const LogData& first, const Scope& firstScope, const LogData& second,
           const Scope& secondScope, const DiffOptions& options, AtomicFlag& cancel );
// The same on lines of text (pasted ones); the line numbers are the indexes of the lines
Diff diff( const std::vector<QString>& first, const std::vector<QString>& second,
           const DiffOptions& options, AtomicFlag& cancel );

// The parts of two lines that differ, by characters, as (start, length) in each line; the
// parts left out by the options never differ. Long or very different lines are changed
// as a whole.
struct LineChanges {
    std::vector<std::pair<int, int>> first;
    std::vector<std::pair<int, int>> second;
};
LineChanges changedParts( QStringView first, QStringView second, const DiffOptions& options );

} // namespace templates

#endif
