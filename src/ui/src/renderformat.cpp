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

#include "renderformat.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <algorithm>
#include <map>

namespace render {

const std::vector<RenderFormat>& allFormats()
{
    static const std::vector<RenderFormat> formats{
        RenderFormat::Markdown, RenderFormat::Json, RenderFormat::JsonLines, RenderFormat::Csv,
        RenderFormat::Tsv,      RenderFormat::Xml,  RenderFormat::Html,
    };
    return formats;
}

QString formatId( RenderFormat format )
{
    switch ( format ) {
    case RenderFormat::Markdown:
        return QStringLiteral( "markdown" );
    case RenderFormat::Json:
        return QStringLiteral( "json" );
    case RenderFormat::JsonLines:
        return QStringLiteral( "jsonl" );
    case RenderFormat::Csv:
        return QStringLiteral( "csv" );
    case RenderFormat::Tsv:
        return QStringLiteral( "tsv" );
    case RenderFormat::Xml:
        return QStringLiteral( "xml" );
    case RenderFormat::Html:
        return QStringLiteral( "html" );
    case RenderFormat::None:
        break;
    }
    return {};
}

RenderFormat formatFromId( const QString& id )
{
    for ( const auto format : allFormats() ) {
        if ( formatId( format ) == id ) {
            return format;
        }
    }
    return RenderFormat::None;
}

QString formatName( RenderFormat format )
{
    switch ( format ) {
    case RenderFormat::Markdown:
        return QStringLiteral( "Markdown" );
    case RenderFormat::Json:
        return QStringLiteral( "JSON" );
    case RenderFormat::JsonLines:
        return QStringLiteral( "JSON Lines" );
    case RenderFormat::Csv:
        return QStringLiteral( "CSV" );
    case RenderFormat::Tsv:
        return QStringLiteral( "TSV" );
    case RenderFormat::Xml:
        return QStringLiteral( "XML" );
    case RenderFormat::Html:
        return QStringLiteral( "HTML" );
    case RenderFormat::None:
        break;
    }
    return {};
}

bool isLineFormat( RenderFormat format )
{
    return format == RenderFormat::Csv || format == RenderFormat::Tsv
           || format == RenderFormat::JsonLines;
}

RenderFormat formatFromFileName( const QString& fileName )
{
    static const std::map<QString, RenderFormat> extensions{
        { "md", RenderFormat::Markdown },      { "markdown", RenderFormat::Markdown },
        { "mdown", RenderFormat::Markdown },   { "mkd", RenderFormat::Markdown },
        { "json", RenderFormat::Json },        { "geojson", RenderFormat::Json },
        // Network logs of the browsers, notebooks of Jupyter: JSON on one line or nearly
        { "har", RenderFormat::Json },         { "ipynb", RenderFormat::Json },
        { "jsonl", RenderFormat::JsonLines },  { "ndjson", RenderFormat::JsonLines },
        { "csv", RenderFormat::Csv },          { "tsv", RenderFormat::Tsv },
        { "tab", RenderFormat::Tsv },          { "xml", RenderFormat::Xml },
        { "xsd", RenderFormat::Xml },          { "xsl", RenderFormat::Xml },
        { "xslt", RenderFormat::Xml },         { "svg", RenderFormat::Xml },
        { "plist", RenderFormat::Xml },        { "csproj", RenderFormat::Xml },
        { "html", RenderFormat::Html },        { "htm", RenderFormat::Html },
        { "xhtml", RenderFormat::Html },
    };

    const auto suffix = QFileInfo( fileName ).suffix().toLower();
    const auto it = extensions.find( suffix );
    return it != extensions.end() ? it->second : RenderFormat::None;
}

RenderFormat formatFromContent( const QByteArray& head )
{
    auto text = QString::fromUtf8( head );
    if ( text.startsWith( QChar( 0xfeff ) ) ) {
        text.remove( 0, 1 );
    }
    const auto trimmed = text.trimmed();
    if ( trimmed.isEmpty() ) {
        return RenderFormat::None;
    }

    if ( trimmed.startsWith( '<' ) ) {
        const auto lower = trimmed.left( 512 ).toLower();
        if ( lower.startsWith( "<!doctype html" ) || lower.startsWith( "<html" )
             || lower.contains( "<html" ) ) {
            return RenderFormat::Html;
        }
        return RenderFormat::Xml;
    }

    if ( trimmed.startsWith( '{' ) || trimmed.startsWith( '[' ) ) {
        // Several complete lines that are each an object: JSON Lines. The last line of
        // the head may be cut, it is not looked at.
        auto lines = text.split( '\n', Qt::SkipEmptyParts );
        if ( head.size() >= 4096 && lines.size() > 1 ) {
            lines.removeLast();
        }
        int objects = 0;
        for ( const auto& line : std::as_const( lines ) ) {
            const auto trimmedLine = line.trimmed();
            if ( trimmedLine.isEmpty() ) {
                continue;
            }
            if ( !QJsonDocument::fromJson( trimmedLine.toUtf8() ).isObject() ) {
                objects = 0;
                break;
            }
            ++objects;
        }
        return objects > 1 ? RenderFormat::JsonLines : RenderFormat::Json;
    }

    return RenderFormat::None;
}

RenderFormat detectFormat( const QString& fileName, const QByteArray& head )
{
    const auto byName = formatFromFileName( fileName );
    return byName != RenderFormat::None ? byName : formatFromContent( head );
}

QChar guessDelimiter( const QStringList& lines )
{
    static const QChar candidates[] = { ',', '\t', ';', '|' };

    QChar best = ',';
    int bestScore = 0;
    for ( const auto candidate : candidates ) {
        // Number of lines having the most common count of the separator
        std::map<int, int> counts;
        for ( const auto& line : lines ) {
            if ( line.isEmpty() ) {
                continue;
            }
            const auto fields = splitDelimited( line, candidate ).size();
            if ( fields > 1 ) {
                ++counts[ static_cast<int>( fields ) ];
            }
        }
        int score = 0;
        for ( const auto& count : counts ) {
            score = std::max( score, count.second );
        }
        if ( score > bestScore ) {
            bestScore = score;
            best = candidate;
        }
    }
    return best;
}

QStringList splitDelimited( QStringView line, QChar delimiter )
{
    // Without quotes: the fields between the separators
    if ( !line.contains( QLatin1Char( '"' ) ) ) {
        if ( line.endsWith( QLatin1Char( '\r' ) ) ) {
            line.chop( 1 );
        }
        QStringList plain;
        for ( const auto field : line.tokenize( delimiter ) ) {
            plain.append( field.toString() );
        }
        return plain;
    }

    QStringList fields;
    QString field;
    bool quoted = false;
    bool atFieldStart = true;

    for ( qsizetype i = 0; i < line.size(); ++i ) {
        const auto c = line[ i ];
        if ( quoted ) {
            if ( c == '"' ) {
                if ( i + 1 < line.size() && line[ i + 1 ] == '"' ) {
                    field.append( '"' );
                    ++i;
                }
                else {
                    quoted = false;
                }
            }
            else {
                field.append( c );
            }
        }
        else if ( c == delimiter ) {
            fields.append( field );
            field.clear();
            atFieldStart = true;
            continue;
        }
        else if ( c == '"' && atFieldStart ) {
            quoted = true;
        }
        else if ( c == '\r' && i + 1 == line.size() ) {
            // End of a CRLF line
        }
        else {
            field.append( c );
        }
        atFieldStart = false;
    }
    fields.append( field );
    return fields;
}

QStringList jsonLinesKeys( const QStringList& lines )
{
    QStringList keys;
    QSet<QString> seen;
    for ( const auto& line : lines ) {
        const auto document = QJsonDocument::fromJson( line.toUtf8() );
        if ( !document.isObject() ) {
            continue;
        }
        // QJsonObject sorts its keys, the order of the line is read from the text
        const auto object = document.object();
        std::vector<std::pair<qsizetype, QString>> positions;
        for ( auto it = object.begin(); it != object.end(); ++it ) {
            if ( seen.contains( it.key() ) ) {
                continue;
            }
            const auto quotedKey = QString::fromUtf8(
                QJsonDocument( QJsonArray{ it.key() } ).toJson( QJsonDocument::Compact ) );
            // ["key"] -> "key"
            const auto position
                = line.indexOf( quotedKey.mid( 1, quotedKey.size() - 2 ) + QLatin1Char( ':' ) );
            positions.emplace_back( position < 0 ? line.size() : position, it.key() );
        }
        std::sort( positions.begin(), positions.end() );
        for ( const auto& position : positions ) {
            seen.insert( position.second );
            keys.append( position.second );
        }
    }
    return keys;
}

QStringList jsonLineCells( const QString& line, const QStringList& keys )
{
    QStringList cells;
    const auto document = QJsonDocument::fromJson( line.toUtf8() );
    if ( !document.isObject() ) {
        cells.append( line );
        return cells;
    }

    const auto object = document.object();
    cells.reserve( keys.size() );
    for ( const auto& key : keys ) {
        const auto value = object.value( key );
        switch ( value.type() ) {
        case QJsonValue::Undefined:
            cells.append( QString{} );
            break;
        case QJsonValue::Null:
            cells.append( QStringLiteral( "null" ) );
            break;
        case QJsonValue::Bool:
            cells.append( value.toBool() ? QStringLiteral( "true" ) : QStringLiteral( "false" ) );
            break;
        case QJsonValue::Double:
            cells.append( value.toVariant().toString() );
            break;
        case QJsonValue::String:
            cells.append( value.toString() );
            break;
        case QJsonValue::Array:
            cells.append( QString::fromUtf8(
                QJsonDocument( value.toArray() ).toJson( QJsonDocument::Compact ) ) );
            break;
        case QJsonValue::Object:
            cells.append( QString::fromUtf8(
                QJsonDocument( value.toObject() ).toJson( QJsonDocument::Compact ) ) );
            break;
        }
    }
    return cells;
}

namespace {

} // namespace

QString prepareTextPart( const QString& text )
{
    static const QRegularExpression voidElement(
        QStringLiteral( R"RE(<(area|base|br|col|embed|hr|img|input|link|meta|source|track|wbr))RE"
                        R"RE((\s[^<>]*?)??\s*/?>)RE" ),
        QRegularExpression::CaseInsensitiveOption );
    static const QRegularExpression keyboard( QStringLiteral( "<(/?)kbd>" ),
                                              QRegularExpression::CaseInsensitiveOption );
    // Shown open: QTextBrowser can not fold it
    static const QRegularExpression details( QStringLiteral( "</?details[^>]*>" ),
                                             QRegularExpression::CaseInsensitiveOption );
    static const QRegularExpression summary( QStringLiteral( "<summary>(.*?)</summary>" ),
                                             QRegularExpression::CaseInsensitiveOption );
    auto result = text;
    result.replace( voidElement, QStringLiteral( "<\\1\\2 />" ) );
    result.replace( keyboard, QStringLiteral( "<\\1code>" ) );
    result.replace( details, QString{} );
    result.replace( summary, QStringLiteral( "<b>\u25BC \\1</b>" ) );
    return result;
}

namespace {

// Footnotes of GitHub: the definitions ([^id]: text) moved to the end, numbered in the
// order of the references, which link to them and back
struct Footnotes {
    std::map<QString, QString> texts;
    std::vector<QString> order;

    QString reference( const QString& id )
    {
        auto it = std::find( order.begin(), order.end(), id );
        if ( it == order.end() ) {
            order.push_back( id );
            it = order.end() - 1;
        }
        const auto number = it - order.begin() + 1;
        return QStringLiteral( "<sup><a name=\"fnref-%1\"></a>[%2](#fn-%1)</sup>" )
            .arg( id )
            .arg( number );
    }
};

QString footnoteReferences( const QString& text, Footnotes& footnotes )
{
    static const QRegularExpression reference( QStringLiteral( "\\[\\^([^\\]\\s]+)\\](?!:)" ) );
    QString result;
    qsizetype position = 0;
    auto it = reference.globalMatch( text );
    while ( it.hasNext() ) {
        const auto match = it.next();
        if ( footnotes.texts.count( match.captured( 1 ) ) == 0 ) {
            continue;
        }
        result += text.mid( position, match.capturedStart() - position );
        result += footnotes.reference( match.captured( 1 ) );
        position = match.capturedEnd();
    }
    return result + text.mid( position );
}

// The parts of a line out of its code spans changed
QString prepareLine( const QString& line, Footnotes& footnotes )
{
    const auto prepareText = [ &footnotes ]( const QString& text ) {
        return footnoteReferences( ::render::prepareTextPart( text ), footnotes );
    };
    QString result;
    qsizetype position = 0;
    while ( position < line.size() ) {
        const auto start = line.indexOf( '`', position );
        if ( start < 0 ) {
            break;
        }
        // A span ends with as many backticks as it starts with
        auto ticks = start;
        while ( ticks < line.size() && line.at( ticks ) == '`' ) {
            ++ticks;
        }
        const auto marker = line.mid( start, ticks - start );
        const auto end = line.indexOf( marker, ticks );
        if ( end < 0 ) {
            break;
        }
        result += prepareText( line.mid( position, start - position ) );
        result += line.mid( start, end + marker.size() - start );
        position = end + marker.size();
    }
    result += prepareText( line.mid( position ) );
    return result;
}

} // namespace

QString prepareMarkdown( const QString& markdown )
{
    auto lines = markdown.split( '\n' );

    // The fenced code blocks are kept as they are
    const auto inFences = [ &lines ] {
        std::vector<bool> fenced( static_cast<size_t>( lines.size() ), false );
        QString fence;
        for ( qsizetype i = 0; i < lines.size(); ++i ) {
            const auto trimmed = lines.at( i ).trimmed();
            if ( fence.isEmpty() ) {
                if ( trimmed.startsWith( QStringLiteral( "```" ) )
                     || trimmed.startsWith( QStringLiteral( "~~~" ) ) ) {
                    fence = trimmed.left( 3 );
                    fenced[ static_cast<size_t>( i ) ] = true;
                }
            }
            else {
                fenced[ static_cast<size_t>( i ) ] = true;
                if ( trimmed.startsWith( fence ) ) {
                    fence.clear();
                }
            }
        }
        return fenced;
    };

    // The definitions of the footnotes, and the indented lines going on with them
    static const QRegularExpression definition(
        QStringLiteral( "^ {0,3}\\[\\^([^\\]\\s]+)\\]:\\s?(.*)$" ) );
    Footnotes footnotes;
    {
        const auto fenced = inFences();
        QStringList kept;
        QString current;
        for ( qsizetype i = 0; i < lines.size(); ++i ) {
            const auto& line = lines.at( i );
            if ( !fenced[ static_cast<size_t>( i ) ] ) {
                if ( const auto match = definition.match( line ); match.hasMatch() ) {
                    current = match.captured( 1 );
                    footnotes.texts[ current ] = match.captured( 2 );
                    continue;
                }
                if ( !current.isEmpty()
                     && ( line.startsWith( QStringLiteral( "    " ) ) || line.startsWith( '\t' ) )
                     && !line.trimmed().isEmpty() ) {
                    footnotes.texts[ current ] += ' ' + line.trimmed();
                    continue;
                }
            }
            current.clear();
            kept.append( line );
        }
        lines = kept;
    }

    // A hard break (two spaces, a backslash at the end) is a new paragraph of Qt: a line
    // break of HTML keeps the paragraph, without its spacing
    static const QRegularExpression hardBreak( QStringLiteral( "(?:  +|(?<!\\\\)\\\\)$" ) );
    const auto fenced = inFences();
    for ( qsizetype i = 0; i < lines.size(); ++i ) {
        if ( !fenced[ static_cast<size_t>( i ) ] ) {
            auto line = prepareLine( lines.at( i ), footnotes );
            const auto nextIsText = i + 1 < lines.size() && !fenced[ static_cast<size_t>( i + 1 ) ]
                                    && !lines.at( i + 1 ).trimmed().isEmpty();
            if ( nextIsText && !line.trimmed().isEmpty() ) {
                line.replace( hardBreak, QStringLiteral( "<br />" ) );
            }
            lines[ i ] = line;
        }
    }

    // The footnotes referenced, at the end
    if ( !footnotes.order.empty() ) {
        lines.append( QString{} );
        lines.append( QStringLiteral( "---" ) );
        lines.append( QString{} );
        int number = 1;
        for ( const auto& id : footnotes.order ) {
            // The anchor needs a character: Qt drops an empty one at the start of a block
            lines.append( QStringLiteral( "%1. <a name=\"fn-%2\">\u200B</a>%3 [\u21A9](#fnref-%2)" )
                              .arg( number++ )
                              .arg( id, prepareLine( footnotes.texts.at( id ), footnotes ) ) );
        }
    }
    return lines.join( '\n' );
}

bool needsBrowser( const QString& markdown )
{
    static const QRegularExpression browserParts(
        QStringLiteral( "^\\s*(```|~~~)\\s*mermaid|\\$\\$|\\\\\\(|\\\\\\[" ),
        QRegularExpression::MultilineOption );
    return browserParts.match( markdown ).hasMatch();
}

QString markdownBrowserPage( const QString& markdown, const QString& title,
                             const QString& baseDir )
{
    // A JavaScript string: "</script" can not end the script in it
    auto source
        = QString::fromUtf8( QJsonDocument( QJsonArray{ markdown } ).toJson( QJsonDocument::Compact ) );
    source.replace( QStringLiteral( "</" ), QStringLiteral( "<\\/" ) );

    const auto base = QUrl::fromLocalFile( baseDir.endsWith( '/' ) ? baseDir : baseDir + '/' );

    // Versions are pinned so the page renders the same every time
    static const char page[] = R"PAGE(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<base href="%BASE%">
<title>%TITLE%</title>
<link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/katex@0.16.11/dist/katex.min.css">
<link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/github-markdown-css@5.8.1/github-markdown.min.css">
<link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/highlight.js@11.10.0/styles/github.min.css" media="(prefers-color-scheme: light)">
<link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/highlight.js@11.10.0/styles/github-dark.min.css" media="(prefers-color-scheme: dark)">
<style>
  body { margin: 0; background: #ffffff; }
  @media (prefers-color-scheme: dark) { body { background: #0d1117; } }
  .markdown-body { box-sizing: border-box; max-width: 980px; margin: 0 auto; padding: 32px 16px; }
  .mermaid { background: transparent; text-align: center; }
  #offline { display: none; color: #9a6700; font-family: sans-serif; padding: 8px 16px; }
</style>
</head>
<body>
<div id="offline">The rendering libraries could not be loaded (offline?), the Markdown is shown as text.</div>
<article id="content" class="markdown-body"></article>
<script src="https://cdn.jsdelivr.net/npm/markdown-it@14.1.0/dist/markdown-it.min.js"></script>
<script src="https://cdn.jsdelivr.net/npm/katex@0.16.11/dist/katex.min.js"></script>
<script src="https://cdn.jsdelivr.net/npm/markdown-it-texmath@1.0.0/texmath.min.js"></script>
<script src="https://cdn.jsdelivr.net/npm/@highlightjs/cdn-assets@11.10.0/highlight.min.js"></script>
<script src="https://cdn.jsdelivr.net/npm/mermaid@11.4.1/dist/mermaid.min.js"></script>
<script>
const source = %SOURCE%[0];
const content = document.getElementById('content');
if (typeof window.markdownit !== 'function') {
  document.getElementById('offline').style.display = 'block';
  const pre = document.createElement('pre');
  pre.textContent = source;
  content.appendChild(pre);
} else {
  const md = window.markdownit({
    html: true, linkify: true, typographer: false,
    highlight: (code, lang) => {
      if (lang && lang !== 'mermaid' && window.hljs && hljs.getLanguage(lang)) {
        try { return hljs.highlight(code, { language: lang, ignoreIllegals: true }).value; } catch (e) {}
      }
      return '';
    }
  });
  if (window.texmath && window.katex) {
    md.use(window.texmath, { engine: window.katex, delimiters: ['dollars', 'brackets'] });
  }
  content.innerHTML = md.render(source);
  for (const code of content.querySelectorAll('pre > code.language-mermaid')) {
    const diagram = document.createElement('div');
    diagram.className = 'mermaid';
    diagram.textContent = code.textContent;
    code.parentElement.replaceWith(diagram);
  }
  if (window.mermaid) {
    const dark = window.matchMedia('(prefers-color-scheme: dark)').matches;
    mermaid.initialize({ startOnLoad: false, theme: dark ? 'dark' : 'default' });
    mermaid.run({ querySelector: '.mermaid' });
  }
}
</script>
</body>
</html>
)PAGE";

    auto result = QString::fromUtf8( page );
    result.replace( QStringLiteral( "%BASE%" ), base.toString( QUrl::FullyEncoded ).toHtmlEscaped() );
    result.replace( QStringLiteral( "%TITLE%" ), title.toHtmlEscaped() );
    result.replace( QStringLiteral( "%SOURCE%" ), source );
    return result;
}

std::pair<int, int> lineAndColumn( const QByteArray& utf8, int offset )
{
    offset = std::clamp( offset, 0, static_cast<int>( utf8.size() ) );
    int line = 1;
    int lineStart = 0;
    for ( int i = 0; i < offset; ++i ) {
        if ( utf8[ i ] == '\n' ) {
            ++line;
            lineStart = i + 1;
        }
    }
    const auto column
        = QString::fromUtf8( utf8.constData() + lineStart, offset - lineStart ).size() + 1;
    return { line, static_cast<int>( column ) };
}

} // namespace render
