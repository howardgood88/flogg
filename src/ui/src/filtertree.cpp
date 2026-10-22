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

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringList>
#include <QXmlStreamReader>

#include "log.h"
#include "uuid.h"

#include "filtertree.h"

FilterNode FilterNode::makeGroup( const QString& name )
{
    FilterNode node;
    node.kind = Kind::Group;
    node.id = generateIdFromUuid();
    node.name = name;
    return node;
}

FilterNode FilterNode::makeFilter( const QString& pattern )
{
    FilterNode node;
    node.kind = Kind::Filter;
    node.id = generateIdFromUuid();
    node.pattern = pattern;
    return node;
}

namespace filters {

namespace {

QString colorToString( const QColor& color )
{
    return color.isValid() ? color.name( QColor::HexArgb ) : QString{};
}

QColor colorFromString( const QString& color )
{
    return color.isEmpty() ? QColor{} : QColor( color );
}

QJsonObject nodeToJson( const FilterNode& node )
{
    QJsonObject json;
    json[ "id" ] = node.id;

    if ( node.isGroup() ) {
        json[ "type" ] = "group";
        json[ "name" ] = node.name;
        if ( node.children.empty() ) {
            json[ "enabled" ] = node.enabled;
        }

        QJsonArray children;
        for ( const auto& child : node.children ) {
            children.append( nodeToJson( child ) );
        }
        json[ "children" ] = children;
    }
    else {
        json[ "type" ] = "filter";
        json[ "enabled" ] = node.enabled;
        json[ "pattern" ] = node.pattern;
        json[ "description" ] = node.name;
        json[ "regex" ] = node.useRegex;
        json[ "caseSensitive" ] = node.caseSensitive;
        json[ "exclude" ] = node.exclude;
        json[ "foreColor" ] = colorToString( node.foreColor );
        json[ "backColor" ] = colorToString( node.backColor );
        json[ "colorMode" ]
            = node.colorMode == FilterNode::ColorMode::MatchOnly ? "match" : "line";
    }

    return json;
}

FilterNode nodeFromJson( const QJsonObject& json )
{
    FilterNode node;
    node.id = json[ "id" ].toString();
    if ( node.id.isEmpty() ) {
        node.id = generateIdFromUuid();
    }

    if ( json[ "type" ].toString() == "group" ) {
        node.kind = FilterNode::Kind::Group;
        node.name = json[ "name" ].toString();
        node.enabled = json[ "enabled" ].toBool( true );

        const auto children = json[ "children" ].toArray();
        node.children.reserve( static_cast<size_t>( children.size() ) );
        for ( const auto& child : children ) {
            node.children.push_back( nodeFromJson( child.toObject() ) );
        }
    }
    else {
        node.kind = FilterNode::Kind::Filter;
        node.enabled = json[ "enabled" ].toBool( true );
        node.pattern = json[ "pattern" ].toString();
        node.name = json[ "description" ].toString();
        node.useRegex = json[ "regex" ].toBool( false );
        node.caseSensitive = json[ "caseSensitive" ].toBool( false );
        node.exclude = json[ "exclude" ].toBool( false );
        node.foreColor = colorFromString( json[ "foreColor" ].toString() );
        node.backColor = colorFromString( json[ "backColor" ].toString() );
        node.colorMode = json[ "colorMode" ].toString() == "match"
                             ? FilterNode::ColorMode::MatchOnly
                             : FilterNode::ColorMode::WholeLine;
    }

    return node;
}

void collectEffective( const FilterNode& node, std::vector<const FilterNode*>& result )
{
    if ( node.suspended ) {
        return;
    }

    if ( !node.isGroup() ) {
        if ( node.enabled && !node.pattern.isEmpty() ) {
            result.push_back( &node );
        }
        return;
    }

    for ( const auto& child : node.children ) {
        collectEffective( child, result );
    }
}

QString joinSubPatterns( const std::vector<const FilterNode*>& filters )
{
    QStringList subPatterns;
    for ( const auto* filter : filters ) {
        subPatterns.append( toBooleanSubPattern( *filter ) );
    }

    const auto joined = subPatterns.join( " or " );
    return subPatterns.size() > 1 ? "(" + joined + ")" : joined;
}

} // namespace

QJsonObject toJson( const FilterNode& root )
{
    QJsonObject json;
    json[ "version" ] = FileFormatVersion;
    json[ "root" ] = nodeToJson( root );
    return json;
}

FilterNode fromJson( const QJsonObject& json )
{
    auto root = nodeFromJson( json[ "root" ].toObject() );
    root.kind = FilterNode::Kind::Group;
    return root;
}

bool saveToFile( const FilterNode& root, const QString& path, QString* errorString )
{
    QSaveFile file( path );
    if ( !file.open( QIODevice::WriteOnly ) ) {
        if ( errorString ) {
            *errorString = file.errorString();
        }
        return false;
    }

    file.write( QJsonDocument( toJson( root ) ).toJson( QJsonDocument::Indented ) );

    if ( !file.commit() ) {
        if ( errorString ) {
            *errorString = file.errorString();
        }
        return false;
    }

    return true;
}

std::optional<FilterNode> loadFromFile( const QString& path, QString* errorString )
{
    QFile file( path );
    if ( !file.open( QIODevice::ReadOnly ) ) {
        if ( errorString ) {
            *errorString = file.errorString();
        }
        return {};
    }

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson( file.readAll(), &parseError );
    if ( document.isNull() || !document.isObject() ) {
        if ( errorString ) {
            *errorString = parseError.errorString();
        }
        return {};
    }

    return fromJson( document.object() );
}

FilterNode importKloggSettings( QSettings& settings )
{
    auto imported = FilterNode::makeGroup( QObject::tr( "Imported from klogg" ) );

    auto predefined = FilterNode::makeGroup( QObject::tr( "Predefined filters" ) );
    settings.beginGroup( "PredefinedFiltersCollection" );
    const auto nbPredefined = settings.beginReadArray( "filters" );
    for ( int i = 0; i < nbPredefined; ++i ) {
        settings.setArrayIndex( i );
        auto filter = FilterNode::makeFilter( settings.value( "filter" ).toString() );
        filter.name = settings.value( "name" ).toString();
        filter.useRegex = settings.value( "regex", true ).toBool();
        filter.enabled = false;
        if ( !filter.pattern.isEmpty() ) {
            predefined.children.push_back( std::move( filter ) );
        }
    }
    settings.endArray();
    settings.endGroup();

    if ( !predefined.children.empty() ) {
        imported.children.push_back( std::move( predefined ) );
    }

    settings.beginGroup( "HighlighterSetCollection" );
    const auto activeSets = settings.value( "active_sets" ).toStringList();
    const auto nbSets = settings.beginReadArray( "sets" );
    for ( int i = 0; i < nbSets; ++i ) {
        settings.setArrayIndex( i );
        settings.beginGroup( "HighlighterSet" );

        const auto setId = settings.value( "id" ).toString();
        auto group = FilterNode::makeGroup(
            settings.value( "name", QObject::tr( "Highlighters" ) ).toString() );

        const auto nbHighlighters = settings.beginReadArray( "highlighters" );
        for ( int j = 0; j < nbHighlighters; ++j ) {
            settings.setArrayIndex( j );
            auto filter = FilterNode::makeFilter( settings.value( "regexp" ).toString() );
            filter.caseSensitive = !settings.value( "ignore_case", false ).toBool();
            filter.useRegex = settings.value( "use_regex", true ).toBool();
            filter.colorMode = settings.value( "match_only", false ).toBool()
                                   ? FilterNode::ColorMode::MatchOnly
                                   : FilterNode::ColorMode::WholeLine;
            filter.foreColor = colorFromString( settings.value( "fore_colour" ).toString() );
            filter.backColor = colorFromString( settings.value( "back_colour" ).toString() );
            filter.enabled = activeSets.contains( setId );
            if ( !filter.pattern.isEmpty() ) {
                group.children.push_back( std::move( filter ) );
            }
        }
        settings.endArray();
        settings.endGroup();

        if ( !group.children.empty() ) {
            imported.children.push_back( std::move( group ) );
        }
    }
    settings.endArray();
    settings.endGroup();

    return imported;
}

std::optional<FilterNode> importTatFile( const QString& path, QString* errorString )
{
    QFile file( path );
    if ( !file.open( QIODevice::ReadOnly ) ) {
        if ( errorString ) {
            *errorString = file.errorString();
        }
        return {};
    }

    QString parseError;
    auto group = importTat( file.readAll(), QFileInfo( path ).completeBaseName(), &parseError );
    if ( !parseError.isEmpty() ) {
        if ( errorString ) {
            *errorString = parseError;
        }
        return {};
    }

    return group;
}

FilterNode importTat( const QByteArray& xml, const QString& groupName, QString* errorString )
{
    const auto isYes = []( const QStringView value ) {
        return value.compare( QLatin1String( "y" ), Qt::CaseInsensitive ) == 0
               || value.compare( QLatin1String( "true" ), Qt::CaseInsensitive ) == 0;
    };

    // Colors are stored as RRGGBB hex digits, sometimes with a leading '#'
    const auto parseColor = []( QStringView value ) {
        if ( value.startsWith( '#' ) ) {
            value = value.mid( 1 );
        }
        bool ok = false;
        const auto rgb = value.toUInt( &ok, 16 );
        return ok ? QColor( QRgb( rgb ) ) : QColor{};
    };

    auto group = FilterNode::makeGroup( groupName );

    QXmlStreamReader reader( xml );
    while ( !reader.atEnd() ) {
        if ( reader.readNext() != QXmlStreamReader::StartElement
             || reader.name() != QLatin1String( "filter" ) ) {
            continue;
        }

        const auto attributes = reader.attributes();
        const auto type = attributes.value( "type" );
        if ( !type.isEmpty() && type != QLatin1String( "matches_text" ) ) {
            LOG_WARNING << "Skipping unsupported TAT filter type " << type.toString().toStdString();
            continue;
        }

        auto filter = FilterNode::makeFilter( attributes.value( "text" ).toString() );
        filter.enabled = isYes( attributes.value( "enabled" ) );
        filter.exclude = isYes( attributes.value( "excluding" ) );
        filter.useRegex = isYes( attributes.value( "regex" ) );
        filter.caseSensitive = isYes( attributes.value( "case_sensitive" ) );
        filter.name = attributes.value( "description" ).toString();
        filter.foreColor = parseColor( attributes.value( "foreColor" ) );
        filter.backColor = parseColor( attributes.value( "backColor" ) );

        group.children.push_back( std::move( filter ) );
    }

    if ( reader.hasError() && errorString ) {
        *errorString = reader.errorString();
    }

    return group;
}

std::vector<const FilterNode*> effectiveFilters( const FilterNode& root )
{
    std::vector<const FilterNode*> result;
    collectEffective( root, result );
    return result;
}

std::vector<const FilterNode*> filtersForSearch( const FilterNode& node )
{
    if ( !node.isGroup() ) {
        return node.pattern.isEmpty() ? std::vector<const FilterNode*>{}
                                      : std::vector<const FilterNode*>{ &node };
    }

    std::vector<const FilterNode*> result;
    for ( const auto& child : node.children ) {
        collectEffective( child, result );
    }
    return result;
}

RegularExpressionPattern toPattern( const FilterNode& filter )
{
    constexpr auto Inverse = false;
    constexpr auto Boolean = false;
    return RegularExpressionPattern( filter.pattern, filter.caseSensitive, Inverse, Boolean,
                                     !filter.useRegex );
}

QString toRegex( const FilterNode& filter )
{
    const auto regex
        = filter.useRegex ? filter.pattern : QRegularExpression::escape( filter.pattern );
    return QString( filter.caseSensitive ? "(?-i:%1)" : "(?i:%1)" ).arg( regex );
}

QString toAnyOfRegex( const std::vector<const FilterNode*>& filters )
{
    QStringList regexes;
    for ( const auto* filter : filters ) {
        regexes.append( toRegex( *filter ) );
    }
    return regexes.join( '|' );
}

QString toBooleanSubPattern( const FilterNode& filter )
{
    // Wrapping in a group keeps the case setting local and makes sure the closing quote
    // is never preceded by a backslash (which the boolean parser treats as an escaped quote)
    auto regex = toRegex( filter );
    regex.replace( "\"", "\\\"" );

    return "\"" + regex + "\"";
}

QString buildBooleanExpression( const std::vector<const FilterNode*>& filters )
{
    std::vector<const FilterNode*> includes;
    std::vector<const FilterNode*> excludes;
    for ( const auto* filter : filters ) {
        ( filter->exclude ? excludes : includes ).push_back( filter );
    }

    QString expression = joinSubPatterns( includes );

    if ( !excludes.empty() ) {
        if ( !expression.isEmpty() ) {
            expression.append( " and " );
        }
        expression.append( "not(" ).append( joinSubPatterns( excludes ) ).append( ")" );
    }

    return expression;
}

RegularExpressionPattern buildSearchPattern( const std::vector<const FilterNode*>& filters )
{
    constexpr auto CaseSensitive = true;
    constexpr auto Inverse = false;
    constexpr auto Boolean = true;
    constexpr auto PlainText = false;
    return RegularExpressionPattern( buildBooleanExpression( filters ), CaseSensitive, Inverse,
                                     Boolean, PlainText );
}

QColor displayForeColor( const FilterNode& filter )
{
    if ( filter.foreColor.isValid() || !filter.backColor.isValid() ) {
        return filter.foreColor;
    }

    const auto& back = filter.backColor;
    const auto luma = 0.299 * back.redF() + 0.587 * back.greenF() + 0.114 * back.blueF();
    return luma > 0.5 ? QColor( Qt::black ) : QColor( Qt::white );
}

HighlighterSet buildHighlighterSet( const std::vector<const FilterNode*>& filters )
{
    QList<Highlighter> highlighters;
    for ( const auto* filter : filters ) {
        if ( filter->exclude || !( filter->foreColor.isValid() || filter->backColor.isValid() ) ) {
            continue;
        }

        Highlighter highlighter( filter->pattern, !filter->caseSensitive,
                                 filter->colorMode == FilterNode::ColorMode::MatchOnly,
                                 displayForeColor( *filter ), filter->backColor );
        highlighter.setUseRegex( filter->useRegex );
        highlighters.append( highlighter );
    }

    return HighlighterSet::fromHighlighters( "filters", highlighters );
}

} // namespace filters
