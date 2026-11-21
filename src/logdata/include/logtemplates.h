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

struct Template {
    QString text;
    uint64_t count = 0;
    LineNumber first;
    LineNumber last;
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
    Status status = Status::Same;
};

// The templates of both, the most telling first: only in one, then the largest changes
// of their share of the lines. Without a second table, the templates of the first, the
// rarest first.
std::vector<Compared> compare( const Table& first, const Table* second, bool merge );

struct DiffLine {
    enum class Op { Same, Removed, Added };
    Op op = Op::Same;
    std::optional<LineNumber> first;
    std::optional<LineNumber> second;
};

struct Diff {
    std::vector<DiffLine> lines;
    QString error;
    bool cancelled = false;
};

// Line by line, on the templates of the lines, for scopes up to MaxDiffLines lines
constexpr uint64_t MaxDiffLines = 100000;
constexpr int MaxDiffDistance = 2000;
Diff diff( const LogData& first, const Scope& firstScope, const LogData& second,
           const Scope& secondScope, AtomicFlag& cancel );

} // namespace templates

#endif
