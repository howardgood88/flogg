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

#ifndef KLOGG_RENDERMODELS_H
#define KLOGG_RENDERMODELS_H

#include <QAbstractItemModel>
#include <QAbstractTableModel>
#include <QJsonValue>
#include <QRegularExpression>
#include <QStringList>

#include <atomic>
#include <deque>
#include <memory>
#include <vector>

#include "renderformat.h"

class AbstractLogData;

// Text of a cell that the search of the render mode looks at, invalid for the cells it
// does not (types, sizes of objects...). It is the text shown, highlighted where it matches.
constexpr int SearchTextRole = Qt::UserRole + 1;
// The cells with a text can be opened (double click, F2) in a read only editor to select a
// part of it: Qt::EditRole is the whole text, on one line.

// Text searched in the render mode: a part of the text, or a regular expression
class TextMatcher {
  public:
    TextMatcher() = default;
    TextMatcher( const QString& text, bool caseSensitive, bool regex );

    bool isEmpty() const
    {
        return text_.isEmpty();
    }
    // False for a regular expression that does not compile
    bool isValid() const;
    bool matches( const QString& text ) const;
    // Start and length of the matches in the text
    std::vector<std::pair<int, int>> matchRanges( const QString& text ) const;

    const QString& text() const
    {
        return text_;
    }
    bool isCaseSensitive() const
    {
        return caseSensitive_;
    }
    bool isRegex() const
    {
        return regex_;
    }

  private:
    QString text_;
    bool caseSensitive_ = false;
    bool regex_ = false;
    QRegularExpression expression_;
};

namespace render {

// Longest text shown in a cell, the search looks at the text shown
constexpr int MaxCellLength = 500;
// Text of a cell as shown: on one line, shortened
QString displayText( const QString& text );
// The whole text of a cell on one line, for the editor to select a part of it
QString singleLineText( const QString& text );

// The search of a regular expression with anchors or looking around a match: the line as
// a whole may not match where a cell does, the cells are searched one by one
bool needsCellScan( const TextMatcher& matcher );

// How the lines of a table are cut in cells, a copy is used by the counting threads
struct LineFormat {
    RenderFormat format = RenderFormat::Csv;
    QChar delimiter = ',';
    // Keys of JSON Lines, names of the columns of CSV and TSV
    QStringList columns;

    QStringList cells( const QString& line ) const;
    // Matches in the text shown in the cells of the line. The line is looked at first,
    // the cells are made only for the lines matching, unless every cell is scanned.
    int countMatches( const QString& line, const TextMatcher& matcher,
                      bool cellScan = false ) const;
};

} // namespace render

// Tree of a JSON document. The children of a node are made when the view first asks for
// them, a large document costs only the nodes that were expanded.
class JsonTreeModel : public QAbstractItemModel {
    Q_OBJECT

  public:
    enum Column { KeyColumn, ValueColumn, TypeColumn, ColumnCount };

    explicit JsonTreeModel( QJsonValue root, QObject* parent = nullptr );

    QModelIndex index( int row, int column, const QModelIndex& parent = {} ) const override;
    QModelIndex parent( const QModelIndex& child ) const override;
    int rowCount( const QModelIndex& parent = {} ) const override;
    int columnCount( const QModelIndex& parent = {} ) const override;
    QVariant data( const QModelIndex& index, int role = Qt::DisplayRole ) const override;
    QVariant headerData( int section, Qt::Orientation orientation,
                         int role = Qt::DisplayRole ) const override;
    bool hasChildren( const QModelIndex& parent = {} ) const override;
    Qt::ItemFlags flags( const QModelIndex& index ) const override;

    // JSON text of the value (compact for objects and arrays), and its path ($.a[2].b)
    QString valueText( const QModelIndex& index ) const;
    QString path( const QModelIndex& index ) const;

    const QJsonValue& root() const
    {
        return nodes_.front().value;
    }
    // Index of the node at the rows from the root, made on the way
    QModelIndex indexOfPath( const std::vector<int>& rows, int column = 0 ) const;

  private:
    struct Node {
        QString key;
        // Index in the array, -1 for the members of an object and the root
        int arrayIndex = -1;
        QJsonValue value;
        int parent = -1;
        int row = 0;
        std::vector<int> children;
        bool populated = false;
    };

    const Node& node( const QModelIndex& index ) const;
    int nodeId( const QModelIndex& index ) const;
    void populate( int id ) const;

    // The root (id 0) is not shown, its children are the top level rows
    mutable std::deque<Node> nodes_;
};

// Tree of an XML document, read once with QXmlStreamReader in the loading thread
class XmlTree {
  public:
    struct Node {
        enum class Kind { Element, Text, CData, Comment, ProcessingInstruction, Dtd };
        Kind kind = Kind::Element;
        QString name;
        // Attributes of an element (name="value" ...), the text of the other nodes
        QString value;
        int parent = -1;
        int row = 0;
        std::vector<int> children;
    };

    // Empty with the error set when the document is not well formed
    static std::shared_ptr<XmlTree> parse( const QString& text );

    std::vector<Node> nodes;
    QString error;
    int errorLine = 0;
    int errorColumn = 0;
};

namespace render {

// Searches of the trees, in the order of the rows, stopped at the limit or when cancelled
constexpr size_t MaxTreeMatches = 100000;

// A match of a JSON tree: the rows from the root, the column (key or value) and the
// occurrence in the text of the cell, every occurrence is a match like in a document
struct JsonMatch {
    std::vector<int> rows;
    int column = 0;
    int occurrence = 0;
};
std::vector<JsonMatch> findInJson( const QJsonValue& root, const TextMatcher& matcher,
                                   const std::atomic_bool* cancelled = nullptr );

// A match of an XML tree: the node, the column (name or value) and the occurrence
struct XmlMatch {
    int node = 0;
    int column = 0;
    int occurrence = 0;
};
std::vector<XmlMatch> findInXml( const XmlTree& tree, const TextMatcher& matcher,
                                 const std::atomic_bool* cancelled = nullptr );

} // namespace render

class XmlTreeModel : public QAbstractItemModel {
    Q_OBJECT

  public:
    enum Column { NodeColumn, ValueColumn, ColumnCount };

    explicit XmlTreeModel( std::shared_ptr<const XmlTree> tree, QObject* parent = nullptr );

    QModelIndex index( int row, int column, const QModelIndex& parent = {} ) const override;
    QModelIndex parent( const QModelIndex& child ) const override;
    int rowCount( const QModelIndex& parent = {} ) const override;
    int columnCount( const QModelIndex& parent = {} ) const override;
    QVariant data( const QModelIndex& index, int role = Qt::DisplayRole ) const override;
    QVariant headerData( int section, Qt::Orientation orientation,
                         int role = Qt::DisplayRole ) const override;

    Qt::ItemFlags flags( const QModelIndex& index ) const override;

    QModelIndex indexOfNode( int id, int column = 0 ) const;

    // Text shown in the cell, null for the labels of the text nodes (not searched)
    static QString cellText( const XmlTree::Node& node, int column );

  private:
    // The root (node 0) is the document, not shown
    std::shared_ptr<const XmlTree> tree_;
};

// Table over the lines of the log data: CSV, TSV or JSON Lines. Rows are read from the log
// data when the view shows them, the last parsed rows are cached.
class LineTableModel : public QAbstractTableModel {
    Q_OBJECT

  public:
    static constexpr size_t CachedRows = 256;
    static constexpr int SampleLines = 1000;

    LineTableModel( const AbstractLogData* logData, RenderFormat format, bool header,
                    QObject* parent = nullptr );

    int rowCount( const QModelIndex& parent = {} ) const override;
    int columnCount( const QModelIndex& parent = {} ) const override;
    QVariant data( const QModelIndex& index, int role = Qt::DisplayRole ) const override;
    QVariant headerData( int section, Qt::Orientation orientation,
                         int role = Qt::DisplayRole ) const override;
    Qt::ItemFlags flags( const QModelIndex& index ) const override;

    // The log data has changed: rows appended, or the file truncated
    void refresh();

    bool hasHeader() const
    {
        return header_;
    }
    QChar delimiter() const
    {
        return format_.delimiter;
    }
    const render::LineFormat& lineFormat() const
    {
        return format_;
    }

    // Line of the log data shown by the row
    qint64 lineOfRow( int row ) const;
    // Text of the line of the row
    QString lineText( int row ) const;
    // Row showing the line, -1 for the header
    int rowOfLine( qint64 line ) const;
    // Whole text of a cell, not shortened, and the text shown
    QString cellText( int row, int column ) const;
    QString cellDisplayText( int row, int column ) const;

  private:
    QStringList sampleLines( int count ) const;
    void readColumns();
    const QStringList& cells( int row ) const;
    int rowsOfData() const;

    const AbstractLogData* logData_;
    render::LineFormat format_;
    bool header_;
    int rows_ = 0;
    mutable render::LruCache<int, QStringList> cache_{ CachedRows };
};

#endif
