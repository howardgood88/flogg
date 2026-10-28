/*
 * Copyright (C) 2009, 2010, 2011 Nicolas Bonnefon and other contributors
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

// This file implements classes Highlighter and HighlighterSet

#include <algorithm>
#include <iterator>
#include <qcolor.h>
#include <qnamespace.h>
#include <random>
#include <utility>

#include <QApplication>
#include <QSettings>

#include <simdutf.h>

#include "containers.h"
#include "crc32.h"
#include "linetypes.h"
#include "log.h"
#include "regularexpression.h"
#include "regularexpressionpattern.h"
#include "uuid.h"

#include "highlighterset.h"

QRegularExpression::PatternOptions getPatternOptions( bool ignoreCase )
{
    QRegularExpression::PatternOptions options = QRegularExpression::UseUnicodePropertiesOption;

    if ( ignoreCase ) {
        options |= QRegularExpression::CaseInsensitiveOption;
    }
    return options;
}

Highlighter::Highlighter( const QString& pattern, bool ignoreCase, bool onlyMatch,
                          const QColor& foreColor, const QColor& backColor )
    : regexp_( pattern, getPatternOptions( ignoreCase ) )
    , highlightOnlyMatch_( onlyMatch )
    , color_{ foreColor, backColor }
{
    LOG_DEBUG << "New Highlighter, fore: " << color_.foreColor.name()
              << " back: " << color_.backColor.name();
}

QString Highlighter::pattern() const
{
    return regexp_.pattern();
}

void Highlighter::setPattern( const QString& pattern )
{
    regexp_.setPattern( pattern );
}

bool Highlighter::ignoreCase() const
{
    return regexp_.patternOptions().testFlag( QRegularExpression::CaseInsensitiveOption );
}

void Highlighter::setIgnoreCase( bool ignoreCase )
{
    regexp_.setPatternOptions( getPatternOptions( ignoreCase ) );
}

bool Highlighter::useRegex() const
{
    return useRegex_;
}

void Highlighter::setUseRegex( bool useRegex )
{
    useRegex_ = useRegex;
}

bool Highlighter::highlightOnlyMatch() const
{
    return highlightOnlyMatch_;
}

void Highlighter::setHighlightOnlyMatch( bool onlyMatch )
{
    highlightOnlyMatch_ = onlyMatch;
}

bool Highlighter::variateColors() const
{
    return variateColors_;
}
void Highlighter::setVariateColors( bool variateColors )
{
    variateColors_ = variateColors;
}

int Highlighter::colorVariance() const
{
    return colorVariance_;
}

void Highlighter::setColorVariance( int colorVariance )
{
    colorVariance_ = colorVariance;
}

const QColor& Highlighter::foreColor() const
{
    return color_.foreColor;
}

void Highlighter::setForeColor( const QColor& foreColor )
{
    color_.foreColor = foreColor;
}

const QColor& Highlighter::backColor() const
{
    return color_.backColor;
}

void Highlighter::setBackColor( const QColor& backColor )
{
    color_.backColor = backColor;
}

std::pair<QColor, QColor> Highlighter::vairateColors( const QString& match ) const
{
    if ( !( variateColors_ && highlightOnlyMatch_ ) ) {
        return std::make_pair( color_.foreColor, color_.backColor );
    }

    std::uniform_int_distribution<int> colorDistribution( 100 - colorVariance_,
                                                          100 + colorVariance_ );

    std::minstd_rand0 generator( Crc32::calculate( match.toUtf8() ) );
    const auto factor = colorDistribution( generator );

    return std::make_pair( color_.foreColor.darker( factor ), color_.backColor.darker( factor ) );
}

RegularExpressionPattern Highlighter::expressionPattern() const
{
    RegularExpressionPattern result;
    result.pattern
        = useRegex_ ? regexp_.pattern() : QRegularExpression::escape( regexp_.pattern() );
    result.isCaseSensitive = !ignoreCase();
    result.isPrefilter = true;

    return result;
}

void Highlighter::compile() const
{
    const auto pattern
        = useRegex_ ? regexp_.pattern() : QRegularExpression::escape( regexp_.pattern() );

    optimizedRegexp_ = QRegularExpression( pattern, regexp_.patternOptions() );
    optimizedRegexp_->optimize();
}

bool Highlighter::matchLine( const QString& line, klogg::vector<HighlightedMatch>& matches ) const
{
    matches.clear();

    if ( !optimizedRegexp_ ) {
        compile();
    }

    QRegularExpressionMatchIterator matchIterator = optimizedRegexp_->globalMatch( line );

    while ( matchIterator.hasNext() ) {
        QRegularExpressionMatch match = matchIterator.next();
        if ( optimizedRegexp_->captureCount() > 0 ) {
            matches.reserve( static_cast<size_t>( match.lastCapturedIndex() ) );
            for ( int i = 1; i <= match.lastCapturedIndex(); ++i ) {

                const auto colors = vairateColors( match.captured( i ) );

                matches.emplace_back( LineColumn{ match.capturedStart( i ) },
                                      LineLength{ match.capturedLength( i ) }, colors.first,
                                      colors.second );
            }
        }
        else {
            const auto colors = vairateColors( match.captured( 0 ) );

            matches.emplace_back( LineColumn{ match.capturedStart( 0 ) },
                                  LineLength{ match.capturedLength( 0 ) }, colors.first,
                                  colors.second );
        }
    }

    return ( !matches.empty() );
}

HighlighterSet HighlighterSet::fromHighlighters( const QString& name,
                                                 const QList<Highlighter>& highlighters )
{
    HighlighterSet set{ name };
    set.highlighterList_ = highlighters;
    return set;
}

HighlighterSet::HighlighterSet( const QString& name )
    : name_( name )
{
}

QString HighlighterSet::name() const
{
    return name_;
}

bool HighlighterSet::isEmpty() const
{
    return highlighterList_.isEmpty();
}

void HighlighterSet::compile() const
{
    std::call_once( compiled_->once, [ this ] {
        klogg::vector<RegularExpressionPattern> patterns(
            static_cast<size_t>( highlighterList_.size() ) );
        std::transform( highlighterList_.begin(), highlighterList_.end(), patterns.begin(),
                        []( const Highlighter& hl ) { return hl.expressionPattern(); } );

        compiled_->expression = std::make_shared<MultiRegularExpression>( patterns );
        compiled_->done = true;
    } );
}

bool HighlighterSet::isCompiled() const
{
    return compiled_->done;
}

HighlighterMatchType HighlighterSet::matchLine( const QString& line,
                                                HighlightedMatchRanges& matches ) const
{
    const auto matcher = createMatcher();
    if ( !matcher ) {
        return HighlighterMatchType::NoMatch;
    }

    return matchLine( line, matches, *matcher );
}

std::unique_ptr<MultiPatternMatcher> HighlighterSet::createMatcher() const
{
    if ( highlighterList_.empty() ) {
        return {};
    }

    compile();
    return compiled_->expression->createMatcher();
}

HighlighterMatchType HighlighterSet::matchLine( const QString& line,
                                                HighlightedMatchRanges& matches,
                                                const MultiPatternMatcher& matcher ) const
{
    if ( highlighterList_.empty() ) {
        return HighlighterMatchType::NoMatch;
    }

    klogg::vector<char> utf8Data( static_cast<size_t>( line.size() * 4 ) );
    const auto resultSize
        = simdutf::convert_utf16_to_utf8( reinterpret_cast<const char16_t*>( line.utf16() ),
                                          static_cast<size_t>( line.size() ), utf8Data.data() );

    // One char per highlighter, without copying the patterns for each drawn line
    const auto matchedPatterns
        = matcher.matchAll( std::string_view{ utf8Data.data(), resultSize } );
    if ( matchedPatterns.size() < static_cast<size_t>( highlighterList_.size() ) ) {
        // No result per pattern (matcher without a usable database)
        return HighlighterMatchType::NoMatch;
    }

    auto matchType = HighlighterMatchType::NoMatch;

    for ( int index = static_cast<int>( highlighterList_.size() ) - 1; index >= 0; --index ) {
        const Highlighter& hl = highlighterList_[ index ];
        if ( !matchedPatterns[ static_cast<size_t>( index ) ] ) {
            continue;
        }

        klogg::vector<HighlightedMatch> thisMatches;
        if ( !hl.matchLine( line, thisMatches ) ) {
            continue;
        }

        if (hl.highlightOnlyMatch()) {
            if ( matchType != HighlighterMatchType::LineMatch ) {
                matchType = HighlighterMatchType::WordMatch;
            }
            matches.addMatches( thisMatches );
        }
        else {
            matchType = HighlighterMatchType::LineMatch;
            matches.clear();
            matches.addMatch(
                { 0_lcol, LineLength{ line.size() }, hl.foreColor(), hl.backColor() } );
        }
    }

    return matchType;
}

QList<QuickHighlighter> HighlighterSetCollection::quickHighlighters() const
{
    return quickHighlighters_;
}

void HighlighterSetCollection::setQuickHighlighters(
    const QList<QuickHighlighter>& quickHighlighters )
{
    quickHighlighters_ = quickHighlighters;
}

void HighlighterSetCollection::saveToStorage( QSettings& settings ) const
{
    LOG_INFO << "HighlighterSetCollection::saveToStorage, v" << HighlighterSetCollection_VERSION;

    settings.beginGroup( "HighlighterSetCollection" );
    settings.setValue( "version", HighlighterSetCollection_VERSION );
    settings.remove( "quick" );
    settings.beginWriteArray( "quick" );
    for ( int i = 0; i < quickHighlighters_.size(); ++i ) {
        settings.setArrayIndex( i );
        settings.setValue( "name", quickHighlighters_[ i ].name );
        settings.setValue( "fore_colour",
                           quickHighlighters_[ i ].color.foreColor.name( QColor::HexArgb ) );
        settings.setValue( "back_colour",
                           quickHighlighters_[ i ].color.backColor.name( QColor::HexArgb ) );
        settings.setValue( "cycle", quickHighlighters_[ i ].useInCycle );
    }
    settings.endArray();
    settings.endGroup();
}

void HighlighterSetCollection::retrieveFromStorage( QSettings& settings )
{
    LOG_DEBUG << "HighlighterSetCollection::retrieveFromStorage";

    quickHighlighters_.clear();

    if ( settings.contains( "HighlighterSetCollection/version" ) ) {
        settings.beginGroup( "HighlighterSetCollection" );
        if ( settings.value( "version" ).toInt() <= HighlighterSetCollection_VERSION ) {
            const auto size = settings.beginReadArray( "quick" );
            for ( int i = 0; i < size; ++i ) {
                settings.setArrayIndex( i );
                QuickHighlighter quickHighlighter;
                quickHighlighter.color.foreColor
                    = QColor( settings.value( "fore_colour" ).toString() );
                quickHighlighter.color.backColor
                    = QColor( settings.value( "back_colour" ).toString() );
                quickHighlighter.useInCycle = settings.value( "cycle", true ).toBool();

                quickHighlighter.name
                    = settings.value( "name", QString( "Color label %1" ).arg( i + 1 ) ).toString();

                quickHighlighters_.append( std::move( quickHighlighter ) );
            }
            settings.endArray();
        }
        else {
            LOG_ERROR << "Unknown version of HighlighterSetCollection, ignoring it...";
        }

        settings.endGroup();
    }

    QList<QuickHighlighter> defaultLabels;
    defaultLabels.append( { QApplication::tr( "Color label 1" ),
                            { QColor{ "#001e80" }, QColor{ "#a1b7ff" } },
                            true } );
    defaultLabels.append( { QApplication::tr( "Color label 2" ),
                            { QColor{ "#80005D" }, QColor{ "#ffa1c6" } },
                            true } );
    defaultLabels.append( { QApplication::tr( "Color label 3" ),
                            { QColor{ "#0f8000" }, QColor{ "#acffa1" } },
                            true } );
    defaultLabels.append( { QApplication::tr( "Color label 4" ),
                            { QColor{ "#806000" }, QColor{ "#ffe8a1" } },
                            true } );
    defaultLabels.append( { QApplication::tr( "Color label 5" ),
                            { QColor{ "#420080" }, QColor{ "#d2a1ff" } },
                            true } );
    defaultLabels.append( { QApplication::tr( "Color label 6" ),
                            { QColor{ "#007f80" }, QColor{ "#a1feff" } },
                            true } );
    defaultLabels.append( { QApplication::tr( "Color label 7" ),
                            { QColor{ "#004e80" }, QColor{ "#a1dbff" } },
                            true } );
    defaultLabels.append( { QApplication::tr( "Color label 8" ),
                            { QColor{ "#120080" }, QColor{ "#a29ccf" } },
                            true } );
    defaultLabels.append( { QApplication::tr( "Color label 9" ), { QColor{}, Qt::gray }, true } );

    if ( quickHighlighters_.size() < defaultLabels.size() ) {
        LOG_WARNING << "Got " << quickHighlighters_.size() << " quick highlighters";
        std::copy( defaultLabels.begin() + quickHighlighters_.size(), defaultLabels.end(),
                   std::back_inserter( quickHighlighters_ ) );
    }
    else if ( quickHighlighters_.size() > defaultLabels.size() ) {
        LOG_WARNING << "Got " << quickHighlighters_.size() << " quick highlighters";

        quickHighlighters_.erase( quickHighlighters_.begin() + defaultLabels.size(),
                                  quickHighlighters_.end() );
    }

    LOG_INFO << "Loaded " << quickHighlighters_.size() << " color labels";
}
