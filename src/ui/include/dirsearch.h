/*
 * Copyright (C) 2026 flogg contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * klogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef KLOGG_DIRSEARCH_H
#define KLOGG_DIRSEARCH_H

#include <QByteArray>
#include <QIODevice>
#include <QDateTime>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <vector>

// Search and replace in the files of a folder (the file tree panel), on demand, in a thread
// of the pool. The files are read in blocks of whole lines in their encoding, nothing is
// kept of them but the lines shown in the results (a bounded number).
namespace dirsearch {

struct Query {
    QString text;
    bool caseSensitive = false;
    bool regex = false;

    // Invalid when the regular expression is
    QRegularExpression pattern() const;
};

// A line with a match, as shown in the results
struct LineMatch {
    // From 0
    qint64 line = 0;
    // The line, shortened around the first match when long
    QString preview;
    // Start and length of the matches in the preview
    std::vector<std::pair<int, int>> ranges;
};

struct FileResult {
    QString path;
    // Lines kept for the results (all of them up to the limit)
    std::vector<LineMatch> lines;
    // Every match of the file, also those past the limit
    qint64 matchCount = 0;
    qint64 lineCount = 0;
    // When the file was searched: a replace in some lines checks it did not change
    qint64 size = 0;
    QDateTime modified;
};

// Most lines kept for the results of a search, the matches past it are only counted
constexpr qint64 MaxResultLines = 10000;
// Longest preview of a line
constexpr int MaxPreviewLength = 250;

// The files under the folder (recursively) whose name matches the pattern (empty for all),
// sorted by path. The hidden files and folders only when asked.
QStringList listFiles( const QString& folder, const QRegularExpression& namePattern,
                       bool showHidden, const std::atomic_bool* cancelled = nullptr );

// Searches a file, false when it can not be read, is binary or the search was cancelled
// (error left empty then). At most maxLines lines are kept in the result.
bool searchFile( const QString& path, const QRegularExpression& pattern, qint64 maxLines,
                 FileResult* result, QString* error, const std::atomic_bool* cancelled = nullptr );

// Replaces the matches of a file (all of them, or those of the lines given), writing it in
// its encoding with its byte order mark and line ends, atomically (a temporary file renamed
// over it). Lines not changed are written byte for byte. A replacement with \0 to \9 uses
// the groups of a regular expression. The number of matches replaced, -1 on an error.
qint64 replaceInFile( const QString& path, const QRegularExpression& pattern,
                      const QString& replacement, bool expandGroups,
                      const QSet<qint64>* lines, QString* error,
                      const std::atomic_bool* cancelled = nullptr );

// Writes every line with a match of the files, in UTF-8, as grep -n does: the path
// (relative to the root), the line number (from 1) and the line. False on an error of the
// output or when cancelled; the files that can not be read are left out.
bool exportMatches( const QStringList& files, const QString& root,
                    const QRegularExpression& pattern, QIODevice* output, QString* error,
                    const std::atomic_bool* cancelled = nullptr,
                    std::atomic_int* filesDone = nullptr );

// The replacement of one match: \0 to \9 are its groups, \\ a backslash
QString expandReplacement( const QRegularExpressionMatch& match, const QString& replacement );

} // namespace dirsearch

#endif
