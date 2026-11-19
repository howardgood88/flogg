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

#ifndef KLOGG_RENDERFORMAT_H
#define KLOGG_RENDERFORMAT_H

#include <QByteArray>
#include <QChar>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <list>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

// Formats the render mode shows. Markdown, JSON, XML and HTML are parsed as a whole
// document (with a size limit), the line formats are read line by line from the log data.
enum class RenderFormat {
    None,
    Markdown,
    Json,
    JsonLines,
    Csv,
    Tsv,
    Xml,
    Html,
};

namespace render {

// Every format but None, in the order of the menus
const std::vector<RenderFormat>& allFormats();

// Name kept in the session ("markdown", "json"...), None for an unknown name
QString formatId( RenderFormat format );
RenderFormat formatFromId( const QString& id );
QString formatName( RenderFormat format );

// One row of the file is one line of the log data
bool isLineFormat( RenderFormat format );

// From the extension of the file, None when it says nothing
RenderFormat formatFromFileName( const QString& fileName );
// From the start of the file (a few KB) when the extension says nothing
RenderFormat formatFromContent( const QByteArray& head );
RenderFormat detectFormat( const QString& fileName, const QByteArray& head );

// Separator of a CSV file guessed from its first lines: the one found the same number of
// times (and at least once) on most lines, the comma when none is
QChar guessDelimiter( const QStringList& lines );
// Fields of one line, with the quotes of RFC 4180 ("a ""b""" is a "b")
QStringList splitDelimited( QStringView line, QChar delimiter );

// Keys of the objects of JSON Lines, in the order first seen
QStringList jsonLinesKeys( const QStringList& lines );
// Cells of one JSON Lines line for the keys, the text of the values (objects and arrays
// as compact JSON); the whole line in the first cell when it is not an object
QStringList jsonLineCells( const QString& line, const QStringList& keys );

// Markdown made ready for QTextDocument::setMarkdown, outside of the code:
// - the void HTML elements closed (<br> as <br />): Qt drops the rest of the document after
//   an element it waits the end of
// - <kbd> shown as code, <details> shown open with its <summary> in bold
// - the footnotes of GitHub ([^1]) numbered, linked, at the end
QString prepareMarkdown( const QString& markdown );
// The changes of prepareMarkdown on a text out of the code
QString prepareTextPart( const QString& text );

// Markdown parts QTextBrowser shows as plain code: mermaid, math
bool needsBrowser( const QString& markdown );
// Page showing the Markdown in a web browser, with markdown-it, mermaid, KaTeX and
// highlight.js from a CDN. Relative links and images resolve from baseDir.
QString markdownBrowserPage( const QString& markdown, const QString& title,
                             const QString& baseDir );

// Line and column (from 1) of a byte offset in an UTF-8 document
std::pair<int, int> lineAndColumn( const QByteArray& utf8, int offset );

// Least recently used cache of a bounded number of entries
template <typename Key, typename Value>
class LruCache {
  public:
    explicit LruCache( size_t capacity )
        : capacity_( capacity )
    {
    }

    const Value* find( const Key& key )
    {
        const auto it = index_.find( key );
        if ( it == index_.end() ) {
            return nullptr;
        }
        entries_.splice( entries_.begin(), entries_, it->second );
        return &it->second->second;
    }

    const Value& insert( const Key& key, Value value )
    {
        if ( const auto it = index_.find( key ); it != index_.end() ) {
            it->second->second = std::move( value );
            entries_.splice( entries_.begin(), entries_, it->second );
            return it->second->second;
        }
        entries_.emplace_front( key, std::move( value ) );
        index_.emplace( key, entries_.begin() );
        if ( entries_.size() > capacity_ ) {
            index_.erase( entries_.back().first );
            entries_.pop_back();
        }
        return entries_.front().second;
    }

    void clear()
    {
        index_.clear();
        entries_.clear();
    }

    size_t size() const
    {
        return entries_.size();
    }

  private:
    using Entries = std::list<std::pair<Key, Value>>;
    size_t capacity_;
    Entries entries_;
    std::unordered_map<Key, typename Entries::iterator> index_;
};

} // namespace render

#endif
