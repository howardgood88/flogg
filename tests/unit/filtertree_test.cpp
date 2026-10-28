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

#include <catch2/catch.hpp>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>

#include "filtertree.h"
#include "regularexpression.h"

namespace {

FilterNode filter( const QString& pattern, bool enabled = true, bool exclude = false )
{
    auto node = FilterNode::makeFilter( pattern );
    node.enabled = enabled;
    node.exclude = exclude;
    return node;
}

QStringList patterns( const std::vector<const FilterNode*>& filters )
{
    QStringList result;
    for ( const auto* f : filters ) {
        result.append( f->pattern );
    }
    return result;
}

bool matches( const std::vector<const FilterNode*>& filters, std::string_view line )
{
    RegularExpression expression( filters::buildSearchPattern( filters ) );
    REQUIRE( expression.isValid() );
    return expression.createMatcher()->hasMatch( line );
}

} // namespace

SCENARIO( "Effective filters of a filter tree", "[filtertree]" )
{
    auto root = FilterNode::makeGroup( "root" );

    auto common = FilterNode::makeGroup( "Common" );
    common.children.push_back( filter( "ERROR" ) );
    common.children.push_back( filter( "crash", false ) );

    auto wifi = FilterNode::makeGroup( "WiFi" );
    wifi.children.push_back( filter( "wlan0" ) );
    wifi.children.push_back( filter( "scan" ) );

    root.children.push_back( common );
    root.children.push_back( wifi );
    root.children.push_back( filter( "top" ) );

    WHEN( "Nothing is suspended" )
    {
        THEN( "Checked filters are returned in tree order" )
        {
            REQUIRE( patterns( filters::effectiveFilters( root ) )
                     == QStringList{ "ERROR", "wlan0", "scan", "top" } );
        }
    }

    WHEN( "A group is suspended" )
    {
        root.children[ 1 ].suspended = true;

        THEN( "Its filters are skipped but keep their check state" )
        {
            REQUIRE( patterns( filters::effectiveFilters( root ) )
                     == QStringList{ "ERROR", "top" } );
            REQUIRE( root.children[ 1 ].children[ 0 ].enabled );
        }

        THEN( "Adding the group to the search uses its effective filters" )
        {
            REQUIRE( patterns( filters::filtersForSearch( root.children[ 1 ] ) )
                     == QStringList{ "wlan0", "scan" } );
        }
    }

    WHEN( "A single filter is suspended" )
    {
        root.children[ 0 ].children[ 0 ].suspended = true;

        THEN( "Only this filter is skipped" )
        {
            REQUIRE( patterns( filters::effectiveFilters( root ) )
                     == QStringList{ "wlan0", "scan", "top" } );
        }
    }
}

SCENARIO( "Filters converted to a boolean search", "[filtertree]" )
{
    auto error = filter( "error" );
    auto wlan = filter( "wlan0" );
    auto heartbeat = filter( "heartbeat", true, true );

    WHEN( "Using include filters" )
    {
        const std::vector<const FilterNode*> filters{ &error, &wlan };
        REQUIRE( matches( filters, "an error happened" ) );
        REQUIRE( matches( filters, "wlan0 up" ) );
        REQUIRE_FALSE( matches( filters, "all good" ) );
    }

    WHEN( "Using include and exclude filters" )
    {
        const std::vector<const FilterNode*> filters{ &error, &wlan, &heartbeat };
        REQUIRE( matches( filters, "wlan0 up" ) );
        REQUIRE_FALSE( matches( filters, "wlan0 heartbeat" ) );
        REQUIRE_FALSE( matches( filters, "all good" ) );
    }

    WHEN( "Using only exclude filters" )
    {
        const std::vector<const FilterNode*> filters{ &heartbeat };
        REQUIRE( matches( filters, "all good" ) );
        REQUIRE_FALSE( matches( filters, "heartbeat" ) );
    }

    WHEN( "Filters have different case sensitivity" )
    {
        auto sensitive = filter( "Error" );
        sensitive.caseSensitive = true;
        auto insensitive = filter( "warning" );

        const std::vector<const FilterNode*> filters{ &sensitive, &insensitive };
        REQUIRE( matches( filters, "Error" ) );
        REQUIRE_FALSE( matches( filters, "ERROR" ) );
        REQUIRE( matches( filters, "WARNING" ) );
    }

    WHEN( "Plain text filters contain special characters" )
    {
        auto special = filter( R"(a "quoted" [x] (y) C:\)" );
        const std::vector<const FilterNode*> filters{ &special };
        REQUIRE( matches( filters, R"(prefix a "quoted" [x] (y) C:\ suffix)" ) );
        REQUIRE_FALSE( matches( filters, R"(a quoted x y C:)" ) );
    }

    WHEN( "Regex filters are used" )
    {
        auto regex = filter( R"(id=\d+)" );
        regex.useRegex = true;
        const std::vector<const FilterNode*> filters{ &regex };
        REQUIRE( matches( filters, "id=42" ) );
        REQUIRE_FALSE( matches( filters, "id=x" ) );
    }

    WHEN( "Looking for any of the filters with a single regex" )
    {
        auto sensitive = filter( "Error" );
        sensitive.caseSensitive = true;
        const std::vector<const FilterNode*> filters{ &sensitive, &wlan };
        const QRegularExpression regex( filters::toAnyOfRegex( filters ) );
        REQUIRE( regex.isValid() );
        REQUIRE( regex.match( "Error" ).hasMatch() );
        REQUIRE_FALSE( regex.match( "ERROR" ).hasMatch() );
        REQUIRE( regex.match( "WLAN0" ).hasMatch() );
    }
}

SCENARIO( "Filter tree serialization", "[filtertree]" )
{
    auto root = FilterNode::makeGroup( "root" );
    auto group = FilterNode::makeGroup( "Common" );
    auto f = filter( "ERROR", false, true );
    f.useRegex = true;
    f.caseSensitive = true;
    f.foreColor = QColor( Qt::red );
    f.colorMode = FilterNode::ColorMode::MatchOnly;
    f.name = "errors";
    f.suspended = true;
    group.children.push_back( f );
    group.suspended = true;
    root.children.push_back( group );

    const auto json = QJsonDocument( filters::toJson( root ) ).toJson();
    const auto restored = filters::fromJson( QJsonDocument::fromJson( json ).object() );

    REQUIRE( restored.children.size() == 1 );
    const auto& restoredGroup = restored.children[ 0 ];
    REQUIRE( restoredGroup.isGroup() );
    REQUIRE( restoredGroup.name == "Common" );
    REQUIRE( restoredGroup.suspended );

    REQUIRE( restoredGroup.children.size() == 1 );
    const auto& restoredFilter = restoredGroup.children[ 0 ];
    REQUIRE( restoredFilter.id == f.id );
    REQUIRE( restoredFilter.pattern == "ERROR" );
    REQUIRE( restoredFilter.name == "errors" );
    REQUIRE_FALSE( restoredFilter.enabled );
    REQUIRE( restoredFilter.exclude );
    REQUIRE( restoredFilter.useRegex );
    REQUIRE( restoredFilter.caseSensitive );
    REQUIRE( restoredFilter.foreColor == QColor( Qt::red ) );
    REQUIRE_FALSE( restoredFilter.backColor.isValid() );
    REQUIRE( restoredFilter.colorMode == FilterNode::ColorMode::MatchOnly );
    REQUIRE( restoredFilter.suspended );

    // Not written when unset, older files load as not suspended
    auto plain = FilterNode::makeGroup( "root" );
    plain.children.push_back( filter( "WARN", true, false ) );
    const auto plainJson = filters::toJson( plain );
    const auto plainFilter
        = plainJson[ "root" ].toObject()[ "children" ].toArray().at( 0 ).toObject();
    REQUIRE_FALSE( plainFilter.contains( "suspended" ) );
    REQUIRE_FALSE( filters::fromJson( plainJson ).children[ 0 ].suspended );
}

SCENARIO( "TextAnalysisTool filters import", "[filtertree]" )
{
    const QByteArray tat = R"(<?xml version="1.0" encoding="utf-8" standalone="yes"?>
<TextAnalysisTool.NET version="2020-12-17" showOnlyFilteredLines="False">
  <filters>
    <filter enabled="y" excluding="n" description="errors" foreColor="ff0000" backColor="ffffff" type="matches_text" case_sensitive="n" regex="n" text="ERROR" />
    <filter enabled="n" excluding="y" foreColor="#000080" type="matches_text" case_sensitive="y" regex="y" text="heart\w+" />
  </filters>
</TextAnalysisTool.NET>
)";

    QString error;
    const auto group = filters::importTat( tat, "mine", &error );
    REQUIRE( error.isEmpty() );
    REQUIRE( group.isGroup() );
    REQUIRE( group.name == "mine" );
    REQUIRE( group.children.size() == 2 );

    const auto& first = group.children[ 0 ];
    REQUIRE( first.pattern == "ERROR" );
    REQUIRE( first.enabled );
    REQUIRE_FALSE( first.exclude );
    REQUIRE_FALSE( first.useRegex );
    REQUIRE_FALSE( first.caseSensitive );
    REQUIRE( first.name == "errors" );
    REQUIRE( first.foreColor == QColor( 0xff, 0, 0 ) );
    REQUIRE( first.backColor == QColor( 0xff, 0xff, 0xff ) );

    const auto& second = group.children[ 1 ];
    REQUIRE( second.pattern == "heart\\w+" );
    REQUIRE_FALSE( second.enabled );
    REQUIRE( second.exclude );
    REQUIRE( second.useRegex );
    REQUIRE( second.caseSensitive );
    REQUIRE( second.foreColor == QColor( 0, 0, 0x80 ) );
    REQUIRE_FALSE( second.backColor.isValid() );
}

SCENARIO( "klogg settings import", "[filtertree]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    QSettings settings( dir.filePath( "klogg.conf" ), QSettings::IniFormat );

    settings.beginGroup( "PredefinedFiltersCollection" );
    settings.setValue( "version", 2 );
    settings.beginWriteArray( "filters" );
    settings.setArrayIndex( 0 );
    settings.setValue( "name", "errors" );
    settings.setValue( "filter", "ERROR|FATAL" );
    settings.setValue( "regex", true );
    settings.endArray();
    settings.endGroup();

    settings.beginGroup( "HighlighterSetCollection" );
    settings.setValue( "version", 2 );
    settings.setValue( "active_sets", QStringList{ "set-1" } );
    settings.beginWriteArray( "sets" );
    settings.setArrayIndex( 0 );
    settings.beginGroup( "HighlighterSet" );
    settings.setValue( "version", 3 );
    settings.setValue( "name", "Network" );
    settings.setValue( "id", "set-1" );
    settings.beginWriteArray( "highlighters" );
    settings.setArrayIndex( 0 );
    settings.setValue( "regexp", "wlan\\d" );
    settings.setValue( "ignore_case", true );
    settings.setValue( "match_only", true );
    settings.setValue( "use_regex", true );
    settings.setValue( "fore_colour", "#ff0000ff" );
    settings.setValue( "back_colour", "#ffffff00" );
    settings.endArray();
    settings.endGroup();
    settings.endArray();
    settings.endGroup();
    settings.sync();

    const auto imported = filters::importKloggSettings( settings );
    REQUIRE( imported.children.size() == 2 );

    const auto& predefined = imported.children[ 0 ];
    REQUIRE( predefined.children.size() == 1 );
    REQUIRE( predefined.children[ 0 ].pattern == "ERROR|FATAL" );
    REQUIRE( predefined.children[ 0 ].name == "errors" );
    REQUIRE( predefined.children[ 0 ].useRegex );
    REQUIRE_FALSE( predefined.children[ 0 ].enabled );

    const auto& network = imported.children[ 1 ];
    REQUIRE( network.name == "Network" );
    REQUIRE( network.children.size() == 1 );
    const auto& wlan = network.children[ 0 ];
    REQUIRE( wlan.pattern == "wlan\\d" );
    REQUIRE( wlan.enabled );
    REQUIRE_FALSE( wlan.caseSensitive );
    REQUIRE( wlan.colorMode == FilterNode::ColorMode::MatchOnly );
    REQUIRE( wlan.foreColor == QColor( Qt::blue ) );
    REQUIRE( wlan.backColor == QColor( Qt::yellow ) );
}

SCENARIO( "Filter text colour", "[filtertree]" )
{
    auto node = filter( "x" );

    WHEN( "Only a light background is set" )
    {
        node.backColor = QColor( "#ccffcc" );
        THEN( "Black text is used" )
        {
            REQUIRE( filters::displayForeColor( node ) == QColor( Qt::black ) );
        }
    }

    WHEN( "Only a dark background is set" )
    {
        node.backColor = QColor( "#cc0000" );
        THEN( "White text is used" )
        {
            REQUIRE( filters::displayForeColor( node ) == QColor( Qt::white ) );
        }
    }

    WHEN( "A text colour is set" )
    {
        node.foreColor = QColor( Qt::blue );
        node.backColor = QColor( "#ccffcc" );
        THEN( "It is kept" )
        {
            REQUIRE( filters::displayForeColor( node ) == QColor( Qt::blue ) );
        }
    }

    WHEN( "No colour is set" )
    {
        THEN( "The line colour is kept" )
        {
            REQUIRE_FALSE( filters::displayForeColor( node ).isValid() );
        }
    }
}
