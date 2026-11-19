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

#include <QJsonDocument>
#include <QJsonObject>

#include "renderformat.h"
#include "rendermodels.h"
#include "renderview.h"

using namespace render;

TEST_CASE( "Render format from the file name", "[render]" )
{
    CHECK( formatFromFileName( "/a/README.md" ) == RenderFormat::Markdown );
    CHECK( formatFromFileName( "notes.MARKDOWN" ) == RenderFormat::Markdown );
    CHECK( formatFromFileName( "data.json" ) == RenderFormat::Json );
    CHECK( formatFromFileName( "network.HAR" ) == RenderFormat::Json );
    CHECK( formatFromFileName( "analysis.ipynb" ) == RenderFormat::Json );
    CHECK( formatFromFileName( "events.jsonl" ) == RenderFormat::JsonLines );
    CHECK( formatFromFileName( "events.ndjson" ) == RenderFormat::JsonLines );
    CHECK( formatFromFileName( "table.csv" ) == RenderFormat::Csv );
    CHECK( formatFromFileName( "table.tsv" ) == RenderFormat::Tsv );
    CHECK( formatFromFileName( "pom.xml" ) == RenderFormat::Xml );
    CHECK( formatFromFileName( "index.htm" ) == RenderFormat::Html );
    CHECK( formatFromFileName( "app.log" ) == RenderFormat::None );
    CHECK( formatFromFileName( "noextension" ) == RenderFormat::None );
}

TEST_CASE( "Render format from the content", "[render]" )
{
    CHECK( formatFromContent( "{\"a\": 1,\n \"b\": [1, 2]}\n" ) == RenderFormat::Json );
    CHECK( formatFromContent( "[1, 2, 3]" ) == RenderFormat::Json );
    CHECK( formatFromContent( "{\"a\":1}\n{\"a\":2}\n{\"b\":3}\n" ) == RenderFormat::JsonLines );
    CHECK( formatFromContent( "\xef\xbb\xbf<?xml version=\"1.0\"?><a/>" ) == RenderFormat::Xml );
    CHECK( formatFromContent( "<!DOCTYPE html><html><body></body></html>" ) == RenderFormat::Html );
    CHECK( formatFromContent( "2026-01-01 INFO started\n" ) == RenderFormat::None );
    CHECK( formatFromContent( "   \n" ) == RenderFormat::None );

    // The extension wins
    CHECK( detectFormat( "x.csv", "{\"a\":1}\n{\"a\":2}\n" ) == RenderFormat::Csv );
    CHECK( detectFormat( "x.log", "{\"a\":1}\n{\"a\":2}\n" ) == RenderFormat::JsonLines );
}

TEST_CASE( "Render format ids", "[render]" )
{
    for ( const auto format : allFormats() ) {
        CHECK( formatFromId( formatId( format ) ) == format );
        CHECK_FALSE( formatName( format ).isEmpty() );
    }
    CHECK( formatFromId( "auto" ) == RenderFormat::None );
    CHECK( isLineFormat( RenderFormat::Csv ) );
    CHECK( isLineFormat( RenderFormat::JsonLines ) );
    CHECK_FALSE( isLineFormat( RenderFormat::Json ) );
}

TEST_CASE( "CSV fields with quotes", "[render]" )
{
    CHECK( splitDelimited( u"a,b,c", ',' ) == QStringList{ "a", "b", "c" } );
    CHECK( splitDelimited( u"a,,c,", ',' ) == QStringList{ "a", "", "c", "" } );
    CHECK( splitDelimited( u"\"a,b\",c", ',' ) == QStringList{ "a,b", "c" } );
    CHECK( splitDelimited( u"\"say \"\"hi\"\"\",x", ',' ) == QStringList{ "say \"hi\"", "x" } );
    // A quote inside a field that does not start with one is kept
    CHECK( splitDelimited( u"5\" disk,x", ',' ) == QStringList{ "5\" disk", "x" } );
    CHECK( splitDelimited( u"a\tb\r", '\t' ) == QStringList{ "a", "b" } );
    CHECK( splitDelimited( u"", ',' ) == QStringList{ "" } );
}

TEST_CASE( "CSV separator guessed from the lines", "[render]" )
{
    CHECK( guessDelimiter( { "a,b,c", "1,2,3", "4,5,6" } ) == ',' );
    CHECK( guessDelimiter( { "a;b;c", "1,5;2;3", "4;5;6" } ) == ';' );
    CHECK( guessDelimiter( { "a\tb", "x, y\tz", "1\t2" } ) == '\t' );
    CHECK( guessDelimiter( { "a|b|c", "1|2|3" } ) == '|' );
    CHECK( guessDelimiter( { "no separator" } ) == ',' );
}

TEST_CASE( "JSON Lines columns", "[render]" )
{
    const QStringList lines{ R"({"time":"t1","level":"info","msg":"a"})",
                             R"({"time":"t2","msg":"b","extra":{"k":1}})", "not json",
                             R"({"level":"warn","count":3,"ok":true,"none":null})" };
    const auto keys = jsonLinesKeys( lines );
    // In the order of the lines, not sorted
    CHECK( keys == QStringList{ "time", "level", "msg", "extra", "count", "ok", "none" } );

    CHECK( jsonLineCells( lines.at( 1 ), keys )
           == QStringList{ "t2", "", "b", R"({"k":1})", "", "", "" } );
    CHECK( jsonLineCells( lines.at( 3 ), keys )
           == QStringList{ "", "warn", "", "", "3", "true", "null" } );
    CHECK( jsonLineCells( "not json", keys ) == QStringList{ "not json" } );
}

TEST_CASE( "Markdown parts shown by the browser", "[render]" )
{
    CHECK( needsBrowser( "# Title\n\n```mermaid\ngraph TD; A-->B\n```\n" ) );
    CHECK( needsBrowser( "Energy $$E = mc^2$$" ) );
    CHECK( needsBrowser( "Inline \\(x^2\\)" ) );
    CHECK_FALSE( needsBrowser( "# Title\n\n```cpp\nint a;\n```\nCosts $5." ) );

    const auto page = markdownBrowserPage( "a </script><script>alert(1)</script>", "A & B",
                                           "/tmp/some dir" );
    // The text can not end the script it is in
    CHECK_FALSE( page.contains( "a </script>" ) );
    CHECK( page.contains( "a <\\/script>" ) );
    CHECK( page.contains( "<title>A &amp; B</title>" ) );
    CHECK( page.contains( "<base href=\"file:///tmp/some%20dir/\">" ) );
    CHECK( page.contains( "mermaid@11.4.1" ) );
}

TEST_CASE( "Markdown made ready for Qt", "[render]" )
{
    CHECK( prepareMarkdown( "a<br>b" ) == "a<br />b" );
    CHECK( prepareMarkdown( "a<BR>b<br/>c<br />d" ) == "a<BR />b<br />c<br />d" );
    CHECK( prepareMarkdown( R"(<img src="a.png" width="300">)" )
           == R"(<img src="a.png" width="300" />)" );
    CHECK( prepareMarkdown( "<hr>" ) == "<hr />" );
    CHECK( prepareMarkdown( "<kbd>Ctrl</kbd>" ) == "<code>Ctrl</code>" );
    // Not the code
    CHECK( prepareMarkdown( "`<br>` and <br>" ) == "`<br>` and <br />" );
    CHECK( prepareMarkdown( "``a `<br>` b`` <br>" ) == "``a `<br>` b`` <br />" );
    CHECK( prepareMarkdown( "```html\n<br>\n```\n<br>" ) == "```html\n<br>\n```\n<br />" );
    CHECK( prepareMarkdown( "~~~\n<img src=x>\n~~~" ) == "~~~\n<img src=x>\n~~~" );
    // Not other elements
    CHECK( prepareMarkdown( "<b>x</b> <brand>" ) == "<b>x</b> <brand>" );
}

TEST_CASE( "Markdown details and footnotes", "[render]" )
{
    CHECK( prepareMarkdown( "<details>\n<summary>More</summary>\n\nText\n</details>" )
           == QStringLiteral( "\n<b>\u25BC More</b>\n\nText\n" ) );

    const auto prepared = prepareMarkdown( "A[^b] and B[^a] and A again[^b].\n\n"
                                           "[^a]: First defined.\n"
                                           "[^b]: Second defined\n"
                                           "    on two lines.\n"
                                           "Not a [^missing] one.\n"
                                           "```\n[^a] in code\n```\n" );
    // Numbered in the order of the references
    CHECK( prepared.contains( "A<sup><a name=\"fnref-b\"></a>[1](#fn-b)</sup> and "
                              "B<sup><a name=\"fnref-a\"></a>[2](#fn-a)</sup>" ) );
    CHECK( prepared.contains( "again<sup><a name=\"fnref-b\"></a>[1](#fn-b)</sup>" ) );
    CHECK( prepared.contains( "Not a [^missing] one." ) );
    CHECK( prepared.contains( "```\n[^a] in code\n```" ) );
    CHECK_FALSE( prepared.contains( "[^a]: First" ) );
    CHECK( prepared.contains( QStringLiteral( "1. <a name=\"fn-b\">\u200B</a>Second defined on two lines. "
                                              "[\u21A9](#fnref-b)" ) ) );
    CHECK( prepared.contains( QStringLiteral( "2. <a name=\"fn-a\">\u200B</a>First defined. [\u21A9](#fnref-a)" ) ) );
}

TEST_CASE( "Line and column of an offset", "[render]" )
{
    const QByteArray text = "ab\ncd\n\xc3\xa9x";
    CHECK( lineAndColumn( text, 0 ) == std::make_pair( 1, 1 ) );
    CHECK( lineAndColumn( text, 4 ) == std::make_pair( 2, 2 ) );
    // Columns count characters, not bytes
    CHECK( lineAndColumn( text, 8 ) == std::make_pair( 3, 2 ) );
    CHECK( lineAndColumn( text, 100 ) == std::make_pair( 3, 3 ) );
}

TEST_CASE( "LRU cache keeps the last entries", "[render]" )
{
    LruCache<int, QString> cache( 2 );
    cache.insert( 1, "one" );
    cache.insert( 2, "two" );
    REQUIRE( cache.find( 1 ) );
    // 2 is the least recently used
    cache.insert( 3, "three" );
    CHECK( cache.size() == 2 );
    CHECK( cache.find( 2 ) == nullptr );
    CHECK( *cache.find( 1 ) == "one" );
    CHECK( *cache.find( 3 ) == "three" );
    cache.insert( 3, "THREE" );
    CHECK( *cache.find( 3 ) == "THREE" );
    cache.clear();
    CHECK( cache.size() == 0 );
}

TEST_CASE( "JSON tree model", "[render]" )
{
    const auto document = QJsonDocument::fromJson(
        R"({"name":"flogg","tags":["log","fast"],"nested":{"my key":1.5,"none":null}})" );
    JsonTreeModel model( document.object() );

    // QJsonObject sorts the keys
    REQUIRE( model.rowCount() == 3 );
    const auto name = model.index( 0, JsonTreeModel::KeyColumn );
    CHECK( name.data().toString() == "name" );
    CHECK( model.index( 0, JsonTreeModel::ValueColumn ).data().toString() == "flogg" );
    CHECK( model.index( 0, JsonTreeModel::TypeColumn ).data().toString() == "string" );

    const auto nested = model.index( 1, 0 );
    CHECK( nested.data().toString() == "nested" );
    CHECK( model.index( 1, JsonTreeModel::ValueColumn ).data().toString() == "{2}" );
    CHECK( model.hasChildren( nested ) );
    REQUIRE( model.rowCount( nested ) == 2 );
    const auto myKey = model.index( 0, 0, nested );
    CHECK( model.parent( myKey ) == nested );
    CHECK( model.path( myKey ) == "$.nested[\"my key\"]" );
    CHECK( model.valueText( myKey ) == "1.5" );

    const auto tags = model.index( 2, 0 );
    REQUIRE( model.rowCount( tags ) == 2 );
    const auto second = model.index( 1, 0, tags );
    CHECK( second.data().toString() == "[1]" );
    CHECK( model.path( second ) == "$.tags[1]" );
    CHECK( model.valueText( tags ) == R"(["log","fast"])" );
    CHECK_FALSE( model.hasChildren( second ) );
    CHECK( model.rowCount( second ) == 0 );
}

TEST_CASE( "XML tree", "[render]" )
{
    const auto tree = XmlTree::parse(
        R"(<?xml version="1.0"?><!-- top --><root xmlns:x="urn:x" a="1"><x:item>text</x:item><empty/></root>)" );
    CHECK( tree->error.isEmpty() );

    XmlTreeModel model( tree );
    // The comment and the root element
    REQUIRE( model.rowCount() == 2 );
    CHECK( model.index( 0, XmlTreeModel::NodeColumn ).data().toString() == "comment" );
    const auto root = model.index( 1, XmlTreeModel::NodeColumn );
    CHECK( root.data().toString() == "<root>" );
    CHECK( model.index( 1, XmlTreeModel::ValueColumn ).data().toString()
           == R"(xmlns:x="urn:x" a="1")" );
    REQUIRE( model.rowCount( root ) == 2 );
    const auto item = model.index( 0, 0, root );
    CHECK( item.data().toString() == "<x:item>" );
    CHECK( model.parent( item ) == root );
    // Its only text is on its row
    CHECK( model.rowCount( item ) == 0 );
    CHECK( model.index( 0, XmlTreeModel::ValueColumn, root ).data().toString() == "text" );
    // With attributes the text stays a child
    const auto withAttribute = XmlTree::parse( R"(<a k="v">text</a>)" );
    REQUIRE( withAttribute->nodes.size() == 3 );
    CHECK( withAttribute->nodes.at( 2 ).value == "text" );

    const auto broken = XmlTree::parse( "<a>\n<b></a>" );
    CHECK_FALSE( broken->error.isEmpty() );
    CHECK( broken->errorLine == 2 );
    // What was read before the error is kept
    CHECK( broken->nodes.size() > 1 );
}

TEST_CASE( "Text matcher of the render search", "[render]" )
{
    const TextMatcher plain( "Err", false, false );
    CHECK( plain.isValid() );
    CHECK( plain.matches( "an error" ) );
    CHECK_FALSE( plain.matches( "fine" ) );
    CHECK_FALSE( plain.matches( "" ) );
    CHECK_FALSE( TextMatcher( "Err", true, false ).matches( "an error" ) );
    // A plain text is not a regular expression
    CHECK_FALSE( TextMatcher( "a.c", false, false ).matches( "abc" ) );
    CHECK( TextMatcher( "a.c", false, true ).matches( "ABC" ) );
    CHECK_FALSE( TextMatcher( "a.c", true, true ).matches( "ABC" ) );
    CHECK_FALSE( TextMatcher( "a(", false, true ).isValid() );
    CHECK( TextMatcher().isEmpty() );
}

TEST_CASE( "Search of a JSON tree", "[render]" )
{
    const auto document = QJsonDocument::fromJson(
        R"({"name":"error log","list":[{"level":"error"},{"level":"info"}],"error":true,"n":42})" );
    JsonTreeModel model( document.object() );

    // In the order of the rows (keys sorted): error, list, n, name
    const auto matches = findInJson( model.root(), TextMatcher( "error", false, false ) );
    REQUIRE( matches.size() == 3 );
    CHECK( matches[ 0 ].rows == std::vector<int>{ 0 } );
    CHECK( matches[ 0 ].column == JsonTreeModel::KeyColumn );
    CHECK( matches[ 1 ].rows == std::vector<int>{ 1, 0, 0 } );
    CHECK( matches[ 1 ].column == JsonTreeModel::ValueColumn );
    CHECK( matches[ 2 ].rows == std::vector<int>{ 3 } );

    // A key and its value are two matches
    const auto keyAndValue = findInJson( QJsonDocument::fromJson( R"({"id":"id-1"})" ).object(),
                                         TextMatcher( "id", false, false ) );
    REQUIRE( keyAndValue.size() == 2 );
    CHECK( keyAndValue[ 0 ].column == JsonTreeModel::KeyColumn );
    CHECK( keyAndValue[ 1 ].column == JsonTreeModel::ValueColumn );

    // Every occurrence in a cell is a match, like in a document
    const auto twice = findInJson( QJsonDocument::fromJson( R"({"k":"x and x"})" ).object(),
                                   TextMatcher( "x", false, false ) );
    REQUIRE( twice.size() == 2 );
    CHECK( twice[ 0 ].occurrence == 0 );
    CHECK( twice[ 1 ].occurrence == 1 );

    // Numbers and booleans are searched as their text
    CHECK( findInJson( model.root(), TextMatcher( "42", false, false ) ).size() == 1 );
    CHECK( findInJson( model.root(), TextMatcher( "true", false, false ) ).size() == 1 );

    // The rows lead to the node in the model
    const auto index = model.indexOfPath( matches[ 1 ].rows, JsonTreeModel::ValueColumn );
    REQUIRE( index.isValid() );
    CHECK( index.data().toString() == "error" );
    CHECK( model.path( index ) == "$.list[0].level" );
    CHECK_FALSE( model.indexOfPath( { 1, 5 } ).isValid() );

    const std::atomic_bool cancelled{ true };
    CHECK( findInJson( model.root(), TextMatcher( "error", false, false ), &cancelled ).empty() );
}

TEST_CASE( "Search of an XML tree", "[render]" )
{
    const auto tree = XmlTree::parse(
        R"(<log><entry level="error">disk full</entry><entry level="info">ok</entry><!-- error --></log>)" );
    const auto matches = findInXml( *tree, TextMatcher( "error", false, false ) );
    // The attributes of the first entry, the comment
    REQUIRE( matches.size() == 2 );
    CHECK( matches[ 0 ].column == XmlTreeModel::ValueColumn );

    XmlTreeModel model( tree );
    const auto first = model.indexOfNode( matches[ 0 ].node );
    REQUIRE( first.isValid() );
    CHECK( first.data().toString() == "<entry>" );
    CHECK( model.parent( first ).data().toString() == "<log>" );
    CHECK( model.indexOfNode( matches[ 1 ].node ).data().toString() == "comment" );
    CHECK( findInXml( *tree, TextMatcher( "disk", false, false ) ).size() == 1 );

    // The name and the value of a node are two matches, like two cells
    const auto both = findInXml( *XmlTree::parse( "<item>an item</item>" ),
                                 TextMatcher( "item", false, false ) );
    REQUIRE( both.size() == 2 );
    CHECK( both[ 0 ].column == XmlTreeModel::NodeColumn );
    CHECK( both[ 1 ].column == XmlTreeModel::ValueColumn );
    // The labels of the text nodes are not names
    CHECK( findInXml( *XmlTree::parse( "<a k=\"1\">text</a>" ), TextMatcher( "text", false, false ) )
               .size()
           == 1 );
    CHECK_FALSE( model.indexOfNode( 0 ).isValid() );
}

TEST_CASE( "Matches counted in the cells of a line", "[render]" )
{
    LineFormat csv;
    csv.columns = { "a", "b" };
    const TextMatcher error( "error", false, false );
    CHECK( csv.countMatches( "error,\"error, error\"", error ) == 3 );
    CHECK( csv.countMatches( "fine,ok", error ) == 0 );

    LineFormat jsonLines;
    jsonLines.format = RenderFormat::JsonLines;
    jsonLines.columns = { "error", "msg" };
    // The keys are not cells
    CHECK( jsonLines.countMatches( R"({"error":true,"msg":"an error"})", error ) == 1 );
}

TEST_CASE( "Ranges of the matches highlighted", "[render]" )
{
    using Ranges = std::vector<std::pair<int, int>>;
    CHECK( TextMatcher( "ab", false, false ).matchRanges( "xABxab" ) == Ranges{ { 1, 2 }, { 4, 2 } } );
    CHECK( TextMatcher( "ab", true, false ).matchRanges( "xABxab" ) == Ranges{ { 4, 2 } } );
    CHECK( TextMatcher( "a+", false, true ).matchRanges( "baaxa" ) == Ranges{ { 1, 2 }, { 4, 1 } } );
    // Empty matches are not ranges
    CHECK( TextMatcher( "x*", false, true ).matchRanges( "ab" ).empty() );
    CHECK( TextMatcher( "a", false, false ).matchRanges( "" ).empty() );
}

TEST_CASE( "Cells searched in the trees", "[render]" )
{
    const auto document = QJsonDocument::fromJson( R"({"obj":{"k":"v"},"list":[3],"s":"text"})" );
    JsonTreeModel json( document.object() );
    // list, obj, s
    CHECK( json.index( 0, JsonTreeModel::KeyColumn ).data( SearchTextRole ).toString() == "list" );
    CHECK_FALSE( json.index( 0, JsonTreeModel::ValueColumn ).data( SearchTextRole ).isValid() );
    CHECK_FALSE( json.index( 0, JsonTreeModel::TypeColumn ).data( SearchTextRole ).isValid() );
    CHECK( json.index( 2, JsonTreeModel::ValueColumn ).data( SearchTextRole ).toString() == "text" );
    // Array indexes are not searched
    const auto item = json.index( 0, JsonTreeModel::KeyColumn, json.index( 0, 0 ) );
    CHECK_FALSE( item.data( SearchTextRole ).isValid() );

    XmlTreeModel xml( XmlTree::parse( "<a><!-- c --><b>t</b></a>" ) );
    const auto a = xml.index( 0, XmlTreeModel::NodeColumn );
    CHECK( a.data( SearchTextRole ).toString() == "<a>" );
    CHECK_FALSE( xml.index( 0, XmlTreeModel::NodeColumn, a ).data( SearchTextRole ).isValid() );
    CHECK( xml.index( 0, XmlTreeModel::ValueColumn, a ).data( SearchTextRole ).toString() == "c" );
}

TEST_CASE( "Search of the raw text of a document", "[render]" )
{
    // Two paragraphs, then a table cell between the frame characters
    const QString raw = QStringLiteral( "ab cab" ) + QChar( QChar::ParagraphSeparator )
                        + QStringLiteral( "xab" ) + QChar( 0xfdd0 ) + QStringLiteral( "ab" )
                        + QChar( 0xfdd1 );
    const auto search = findInDocument( raw, TextMatcher( "ab", false, false ) );
    using Matches = std::vector<std::pair<int, int>>;
    CHECK( search.matches == Matches{ { 0, 2 }, { 4, 2 }, { 8, 2 }, { 11, 2 } } );
    CHECK_FALSE( search.cut );
    // Not over two blocks
    CHECK( findInDocument( raw, TextMatcher( "b.x", false, true ) ).matches.empty() );
    CHECK( findInDocument( raw, TextMatcher( "^ab", false, true ) ).matches.size() == 2 );
}
