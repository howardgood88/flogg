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

#ifndef KLOGG_FILTERTREE_H
#define KLOGG_FILTERTREE_H

#include <optional>
#include <vector>

#include <QColor>
#include <QSettings>
#include <QJsonObject>
#include <QString>

#include "highlighterset.h"
#include "regularexpressionpattern.h"

// A node of the user maintained filter tree: either a group (folder) or a filter.
// The tree drives both the filtered view and the colouring of the log views.
struct FilterNode {
    enum class Kind { Group, Filter };
    enum class ColorMode { WholeLine, MatchOnly };

    Kind kind = Kind::Filter;
    QString id;

    // Group name, or free text description of a filter
    QString name;

    // Check state of a filter. Groups have no check box (they are switched with
    // all their filters), their stored value is unused.
    bool enabled = true;

    // Temporarily disabled without touching the check state.
    // Runtime only, never written to a filter file.
    bool suspended = false;

    // Filter properties
    QString pattern;
    bool useRegex = false;
    bool caseSensitive = false;
    bool exclude = false;
    QColor foreColor;
    QColor backColor;
    ColorMode colorMode = ColorMode::WholeLine;

    std::vector<FilterNode> children;

    static FilterNode makeGroup( const QString& name );
    static FilterNode makeFilter( const QString& pattern );

    bool isGroup() const
    {
        return kind == Kind::Group;
    }
};

namespace filters {

constexpr int FileFormatVersion = 1;

QJsonObject toJson( const FilterNode& root );
FilterNode fromJson( const QJsonObject& json );

bool saveToFile( const FilterNode& root, const QString& path, QString* errorString = nullptr );
std::optional<FilterNode> loadFromFile( const QString& path, QString* errorString = nullptr );

// Reads klogg predefined filters and highlighter sets (from klogg settings) into a group,
// predefined filters are imported unchecked, highlighters of active sets checked.
FilterNode importKloggSettings( QSettings& settings );

// Reads the filters of a TextAnalysisTool.NET .tat file into a group named after the file
std::optional<FilterNode> importTatFile( const QString& path, QString* errorString = nullptr );
FilterNode importTat( const QByteArray& xml, const QString& groupName,
                      QString* errorString = nullptr );

// Filters that are checked and not suspended (neither themselves nor any of their parent
// groups), in priority order: the topmost filter of the tree comes first.
std::vector<const FilterNode*> effectiveFilters( const FilterNode& root );

// Filters to use when the given node is added to the search:
// a filter is used as is, a group stands for its effective filters.
std::vector<const FilterNode*> filtersForSearch( const FilterNode& node );

// Pattern searched for a filter (the exclude flag is not part of it)
RegularExpressionPattern toPattern( const FilterNode& filter );

// Regular expression matching the filter pattern with the filter case sensitivity
// encoded in it (independent of any case insensitive option of the matcher)
QString toRegex( const FilterNode& filter );

// Single regular expression matching any of the filters (ignoring the exclude flag)
QString toAnyOfRegex( const std::vector<const FilterNode*>& filters );

// Converts a filter to a quoted sub-pattern of a klogg boolean expression.
// Case sensitivity is encoded in the pattern, so the result does not depend
// on the match case setting of the search.
QString toBooleanSubPattern( const FilterNode& filter );

// ("inc1" or "inc2") and not ("exc1" or "exc2"), empty if there are no filters
QString buildBooleanExpression( const std::vector<const FilterNode*>& filters );

RegularExpressionPattern buildSearchPattern( const std::vector<const FilterNode*>& filters );

// Text colour used to draw a filter: its own one, or black/white contrasting with
// its background when only a background is set (invalid if neither is set)
QColor displayForeColor( const FilterNode& filter );

// Highlighters for the include filters that have a colour, in priority order
HighlighterSet buildHighlighterSet( const std::vector<const FilterNode*>& filters );

} // namespace filters

#endif
