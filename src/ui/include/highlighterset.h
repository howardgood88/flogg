/*
 * Copyright (C) 2009, 2010 Nicolas Bonnefon and other contributors
 *
 * This file is part of glogg.
 *
 * glogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * glogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with glogg.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Copyright (C) 2019 Anton Filimonov and other contributors
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

#ifndef highlighterSet_H
#define highlighterSet_H

#include <QColor>
#include <QMetaType>
#include <QRegularExpression>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <qcolor.h>
#include <qregularexpression.h>

#include "containers.h"
#include "highlightedmatch.h"
#include "persistable.h"
#include "regularexpression.h"
#include "regularexpressionpattern.h"

struct HighlightColor {
    QColor foreColor;
    QColor backColor;
};

// Represents a filter, i.e. a regexp and the colors matching text
// should be rendered in.
class Highlighter {
  public:
    // Construct an uninitialized Highlighter (when reading from a config file)
    Highlighter() = default;
    Highlighter( const QString& pattern, bool ignoreCase, bool onlyMatch, const QColor& foreColor,
                 const QColor& backColor );

    bool matchLine( const QString& line, klogg::vector<HighlightedMatch>& matches ) const;

    // Accessor functions
    QString pattern() const;
    void setPattern( const QString& pattern );
    bool ignoreCase() const;
    void setIgnoreCase( bool ignoreCase );

    bool highlightOnlyMatch() const;
    void setHighlightOnlyMatch( bool onlyMatch );

    bool useRegex() const;
    void setUseRegex( bool useRegex );

    bool variateColors() const;
    void setVariateColors( bool variateColors );

    int colorVariance() const;
    void setColorVariance( int colorVariance );

    const QColor& foreColor() const;
    void setForeColor( const QColor& foreColor );
    const QColor& backColor() const;
    void setBackColor( const QColor& backColor );

    RegularExpressionPattern expressionPattern() const;

    void compile() const;

  private:
    std::pair<QColor, QColor> vairateColors( const QString& match ) const;

  private:
    QRegularExpression regexp_;
    
    mutable std::optional<QRegularExpression> optimizedRegexp_;

    bool useRegex_ = true;
    bool highlightOnlyMatch_ = false;
    bool variateColors_ = false;
    int colorVariance_ = 15;

    HighlightColor color_;
};

enum class HighlighterMatchType { NoMatch, WordMatch, LineMatch };

// Represents an ordered set of highlighters to be applied to each line displayed,
// the first highlighter has the highest priority.
class HighlighterSet {
  public:
    static HighlighterSet fromHighlighters( const QString& name,
                                            const QList<Highlighter>& highlighters );

    HighlighterSet() = default;

    QString name() const;

    // Returns weither the passed line match a filter of the set,
    // if so, it returns the fore/back colors the line should use.
    HighlighterMatchType matchLine( const QString& line,
                                    HighlightedMatchRanges& matches ) const;

    // Same as above, reusing a matcher created once for many lines
    // (creating a matcher clones the hyperscan scratch space).
    std::unique_ptr<MultiPatternMatcher> createMatcher() const;
    HighlighterMatchType matchLine( const QString& line, HighlightedMatchRanges& matches,
                                    const MultiPatternMatcher& matcher ) const;

    bool isEmpty() const;

    // Compiles the set once, can be called from another thread: the filter panel
    // compiles new sets in the background, the views wait for it to be done
    void compile() const;
    bool isCompiled() const;

  private:
    explicit HighlighterSet( const QString& name );

  private:
    QString name_;
    QList<Highlighter> highlighterList_;

    // Shared by the copies of the set, they have the same highlighters
    struct Compiled {
        std::once_flag once;
        std::shared_ptr<MultiRegularExpression> expression;
        std::atomic<bool> done{ false };
    };
    std::shared_ptr<Compiled> compiled_ = std::make_shared<Compiled>();
};

struct QuickHighlighter {
    QString name;
    HighlightColor color;
    bool useInCycle;
};

// Color labels configuration (the user defined colouring rules are in the filter panel)
class HighlighterSetCollection final : public Persistable<HighlighterSetCollection> {
  public:
    static const char* persistableName()
    {
        return "HighlighterSetCollection";
    }

    QList<QuickHighlighter> quickHighlighters() const;
    void setQuickHighlighters( const QList<QuickHighlighter>& quickHighlighters );

    // Reads/writes the current config in the QSettings object passed
    void saveToStorage( QSettings& settings ) const;
    void retrieveFromStorage( QSettings& settings );

  private:
    static constexpr int HighlighterSetCollection_VERSION = 2;

    QList<QuickHighlighter> quickHighlighters_;
};

#endif
