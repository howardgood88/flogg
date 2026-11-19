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

#include "rendermodels.h"

#include <QBrush>
#include <QFont>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <QRegularExpression>
#include <QXmlStreamReader>

#include <algorithm>
#include <functional>
#include <limits>

#include "abstractlogdata.h"
#include "linetypes.h"

namespace {

QString compactJson( const QJsonValue& value )
{
    switch ( value.type() ) {
    case QJsonValue::Object:
        return QString::fromUtf8( QJsonDocument( value.toObject() ).toJson( QJsonDocument::Compact ) );
    case QJsonValue::Array:
        return QString::fromUtf8( QJsonDocument( value.toArray() ).toJson( QJsonDocument::Compact ) );
    case QJsonValue::String: {
        // Quoted and escaped like in the document
        const auto array
            = QJsonDocument( QJsonArray{ value } ).toJson( QJsonDocument::Compact );
        return QString::fromUtf8( array.mid( 1, array.size() - 2 ) );
    }
    case QJsonValue::Double:
        return value.toVariant().toString();
    case QJsonValue::Bool:
        return value.toBool() ? QStringLiteral( "true" ) : QStringLiteral( "false" );
    case QJsonValue::Null:
        return QStringLiteral( "null" );
    case QJsonValue::Undefined:
        break;
    }
    return {};
}

QString typeName( const QJsonValue& value )
{
    switch ( value.type() ) {
    case QJsonValue::Object:
        return QStringLiteral( "object" );
    case QJsonValue::Array:
        return QStringLiteral( "array" );
    case QJsonValue::String:
        return QStringLiteral( "string" );
    case QJsonValue::Double:
        return QStringLiteral( "number" );
    case QJsonValue::Bool:
        return QStringLiteral( "boolean" );
    case QJsonValue::Null:
        return QStringLiteral( "null" );
    case QJsonValue::Undefined:
        break;
    }
    return {};
}

QString elided( QString text, int maxLength )
{
    if ( text.size() > maxLength ) {
        text.truncate( maxLength );
        text.append( QChar( 0x2026 ) );
    }
    return text;
}

// Cells shown on one line
QString singleLine( QString text )
{
    text.replace( '\n', QChar( 0x21b5 ) );
    text.remove( '\r' );
    return text;
}

// Text of a scalar JSON value searched, empty for objects and arrays
QString scalarText( const QJsonValue& value )
{
    if ( value.isString() ) {
        return value.toString();
    }
    if ( value.isObject() || value.isArray() || value.isUndefined() ) {
        return {};
    }
    return compactJson( value );
}

} // namespace

/*
 * TextMatcher
 */
TextMatcher::TextMatcher( const QString& text, bool caseSensitive, bool regex )
    : text_( text )
    , caseSensitive_( caseSensitive )
    , regex_( regex )
{
    if ( regex_ ) {
        expression_.setPattern( text_ );
        expression_.setPatternOptions( caseSensitive_
                                           ? QRegularExpression::NoPatternOption
                                           : QRegularExpression::CaseInsensitiveOption );
    }
}

bool TextMatcher::isValid() const
{
    return !regex_ || expression_.isValid();
}

bool TextMatcher::matches( const QString& text ) const
{
    if ( text_.isEmpty() || text.isEmpty() ) {
        return false;
    }
    if ( regex_ ) {
        return expression_.isValid() && expression_.match( text ).hasMatch();
    }
    return text.contains( text_, caseSensitive_ ? Qt::CaseSensitive : Qt::CaseInsensitive );
}

std::vector<std::pair<int, int>> TextMatcher::matchRanges( const QString& text ) const
{
    std::vector<std::pair<int, int>> ranges;
    if ( text_.isEmpty() || text.isEmpty() ) {
        return ranges;
    }
    if ( regex_ ) {
        if ( !expression_.isValid() ) {
            return ranges;
        }
        auto it = expression_.globalMatch( text );
        while ( it.hasNext() ) {
            const auto match = it.next();
            if ( match.capturedLength() > 0 ) {
                ranges.emplace_back( static_cast<int>( match.capturedStart() ),
                                     static_cast<int>( match.capturedLength() ) );
            }
        }
        return ranges;
    }
    const auto sensitivity = caseSensitive_ ? Qt::CaseSensitive : Qt::CaseInsensitive;
    for ( auto from = text.indexOf( text_, 0, sensitivity ); from >= 0;
          from = text.indexOf( text_, from + text_.size(), sensitivity ) ) {
        ranges.emplace_back( static_cast<int>( from ), static_cast<int>( text_.size() ) );
    }
    return ranges;
}

namespace render {

QString displayText( const QString& text )
{
    // Most cells are shown as they are, shared without a copy
    if ( text.size() <= MaxCellLength && !text.contains( '\n' ) && !text.contains( '\r' ) ) {
        return text;
    }
    return elided( singleLine( text ), MaxCellLength );
}

QString singleLineText( const QString& text )
{
    return singleLine( text );
}

bool needsCellScan( const TextMatcher& matcher )
{
    if ( !matcher.isRegex() ) {
        return false;
    }
    // Anchors (also in a class, a few more cell scans do no harm), looking around,
    // references to groups: not the same on the line and on a cell
    static const QRegularExpression cellSyntax(
        QStringLiteral( R"([\^$]|\\[AzZbBG1-9]|\(\?<?[=!])" ) );
    return cellSyntax.match( matcher.text() ).hasMatch();
}

QStringList LineFormat::cells( const QString& line ) const
{
    if ( format == RenderFormat::JsonLines ) {
        return jsonLineCells( line, columns );
    }
    auto lineCells = splitDelimited( line, delimiter );
    // More fields than columns: the rest stays in the last column
    if ( lineCells.size() > columns.size() && !columns.isEmpty() ) {
        const auto last = columns.size() - 1;
        const auto rest = lineCells.mid( last ).join( delimiter );
        lineCells = lineCells.mid( 0, last );
        lineCells.append( rest );
    }
    return lineCells;
}

int LineFormat::countMatches( const QString& line, const TextMatcher& matcher,
                              bool cellScan ) const
{
    // Most lines do not match at all: the cells are made only for the others
    if ( !cellScan && !matcher.matches( line ) ) {
        return 0;
    }
    int count = 0;
    for ( const auto& cell : cells( line ) ) {
        count += static_cast<int>( matcher.matchRanges( displayText( cell ) ).size() );
    }
    return count;
}

std::vector<JsonMatch> findInJson( const QJsonValue& root, const TextMatcher& matcher,
                                   const std::atomic_bool* cancelled )
{
    std::vector<JsonMatch> matches;
    std::vector<int> rows;

    // Depth first, children in the order of the model (the keys sorted by QJsonObject)
    const std::function<bool( const QJsonValue& )> visit = [ & ]( const QJsonValue& value ) {
        const auto child = [ & ]( int row, const QString& key, const QJsonValue& childValue ) {
            if ( cancelled && cancelled->load( std::memory_order_relaxed ) ) {
                return false;
            }
            rows.push_back( row );
            // Every occurrence in the text shown, like the cells of the tree
            const auto add = [ & ]( int column, const QString& text ) {
                const auto ranges = matcher.matchRanges( displayText( text ) );
                for ( int occurrence = 0; occurrence < static_cast<int>( ranges.size() )
                                          && matches.size() < MaxTreeMatches;
                      ++occurrence ) {
                    matches.push_back( { rows, column, occurrence } );
                }
            };
            if ( !key.isEmpty() ) {
                add( JsonTreeModel::KeyColumn, key );
            }
            add( JsonTreeModel::ValueColumn, scalarText( childValue ) );
            const auto goOn = matches.size() < MaxTreeMatches && visit( childValue );
            rows.pop_back();
            return goOn;
        };

        if ( value.isObject() ) {
            const auto object = value.toObject();
            int row = 0;
            for ( auto it = object.begin(); it != object.end(); ++it ) {
                if ( !child( row++, it.key(), it.value() ) ) {
                    return false;
                }
            }
        }
        else if ( value.isArray() ) {
            const auto array = value.toArray();
            for ( int row = 0; row < array.size(); ++row ) {
                if ( !child( row, QString{}, array.at( row ) ) ) {
                    return false;
                }
            }
        }
        return true;
    };

    visit( root );
    return matches;
}

std::vector<XmlMatch> findInXml( const XmlTree& tree, const TextMatcher& matcher,
                                 const std::atomic_bool* cancelled )
{
    // The nodes are in the order of the document
    std::vector<XmlMatch> matches;
    for ( size_t id = 1; id < tree.nodes.size(); ++id ) {
        if ( ( id & 0xfff ) == 0 && cancelled && cancelled->load( std::memory_order_relaxed ) ) {
            break;
        }
        const auto& node = tree.nodes[ id ];
        // Every occurrence in the text shown in the two cells
        for ( const auto column : { XmlTreeModel::NodeColumn, XmlTreeModel::ValueColumn } ) {
            const auto text = XmlTreeModel::cellText( node, column );
            if ( text.isNull() ) {
                continue;
            }
            const auto ranges = matcher.matchRanges( text );
            for ( int occurrence = 0; occurrence < static_cast<int>( ranges.size() )
                                      && matches.size() < MaxTreeMatches;
                  ++occurrence ) {
                matches.push_back( { static_cast<int>( id ), column, occurrence } );
            }
        }
        if ( matches.size() >= MaxTreeMatches ) {
            break;
        }
    }
    return matches;
}

} // namespace render

/*
 * JsonTreeModel
 */
JsonTreeModel::JsonTreeModel( QJsonValue root, QObject* parent )
    : QAbstractItemModel( parent )
{
    Node rootNode;
    rootNode.value = std::move( root );
    nodes_.push_back( std::move( rootNode ) );
}

int JsonTreeModel::nodeId( const QModelIndex& index ) const
{
    return index.isValid() ? static_cast<int>( index.internalId() ) : 0;
}

const JsonTreeModel::Node& JsonTreeModel::node( const QModelIndex& index ) const
{
    return nodes_[ static_cast<size_t>( nodeId( index ) ) ];
}

void JsonTreeModel::populate( int id ) const
{
    auto* current = &nodes_[ static_cast<size_t>( id ) ];
    if ( current->populated ) {
        return;
    }
    current->populated = true;

    // Copies of the values are shared with the document, not deep copies
    std::vector<Node> children;
    const auto value = current->value;
    if ( value.isObject() ) {
        const auto object = value.toObject();
        children.reserve( static_cast<size_t>( object.size() ) );
        for ( auto it = object.begin(); it != object.end(); ++it ) {
            Node child;
            child.key = it.key();
            child.value = it.value();
            children.push_back( std::move( child ) );
        }
    }
    else if ( value.isArray() ) {
        const auto array = value.toArray();
        children.reserve( static_cast<size_t>( array.size() ) );
        for ( int i = 0; i < array.size(); ++i ) {
            Node child;
            child.arrayIndex = i;
            child.value = array.at( i );
            children.push_back( std::move( child ) );
        }
    }

    std::vector<int> ids;
    ids.reserve( children.size() );
    int row = 0;
    for ( auto& child : children ) {
        child.parent = id;
        child.row = row++;
        ids.push_back( static_cast<int>( nodes_.size() ) );
        nodes_.push_back( std::move( child ) );
    }
    // The deque does not move its elements when growing at the end
    nodes_[ static_cast<size_t>( id ) ].children = std::move( ids );
}

QModelIndex JsonTreeModel::index( int row, int column, const QModelIndex& parent ) const
{
    if ( row < 0 || column < 0 || column >= ColumnCount ) {
        return {};
    }
    const auto parentId = nodeId( parent );
    populate( parentId );
    const auto& children = nodes_[ static_cast<size_t>( parentId ) ].children;
    if ( row >= static_cast<int>( children.size() ) ) {
        return {};
    }
    return createIndex( row, column, static_cast<quintptr>( children[ static_cast<size_t>( row ) ] ) );
}

QModelIndex JsonTreeModel::parent( const QModelIndex& child ) const
{
    if ( !child.isValid() ) {
        return {};
    }
    const auto parentId = node( child ).parent;
    if ( parentId <= 0 ) {
        return {};
    }
    const auto& parentNode = nodes_[ static_cast<size_t>( parentId ) ];
    return createIndex( parentNode.row, 0, static_cast<quintptr>( parentId ) );
}

int JsonTreeModel::rowCount( const QModelIndex& parent ) const
{
    if ( parent.column() > 0 ) {
        return 0;
    }
    const auto id = nodeId( parent );
    populate( id );
    return static_cast<int>( nodes_[ static_cast<size_t>( id ) ].children.size() );
}

int JsonTreeModel::columnCount( const QModelIndex& ) const
{
    return ColumnCount;
}

bool JsonTreeModel::hasChildren( const QModelIndex& parent ) const
{
    if ( parent.column() > 0 ) {
        return false;
    }
    const auto& value = node( parent ).value;
    if ( value.isObject() ) {
        return !value.toObject().isEmpty();
    }
    if ( value.isArray() ) {
        return !value.toArray().isEmpty();
    }
    return false;
}

QVariant JsonTreeModel::data( const QModelIndex& index, int role ) const
{
    if ( !index.isValid() ) {
        return {};
    }
    const auto& current = node( index );

    if ( role == Qt::DisplayRole ) {
        switch ( index.column() ) {
        case KeyColumn:
            return current.arrayIndex >= 0 ? QStringLiteral( "[%1]" ).arg( current.arrayIndex )
                                           : render::displayText( current.key );
        case ValueColumn:
            if ( current.value.isObject() ) {
                return QStringLiteral( "{%1}" ).arg( current.value.toObject().size() );
            }
            if ( current.value.isArray() ) {
                return QStringLiteral( "[%1]" ).arg( current.value.toArray().size() );
            }
            return render::displayText( scalarText( current.value ) );
        case TypeColumn:
            return typeName( current.value );
        default:
            break;
        }
    }
    else if ( role == Qt::EditRole ) {
        if ( index.column() == KeyColumn && current.arrayIndex < 0 ) {
            return render::singleLineText( current.key );
        }
        if ( index.column() == ValueColumn && !current.value.isObject()
             && !current.value.isArray() ) {
            return render::singleLineText( scalarText( current.value ) );
        }
        return {};
    }
    else if ( role == SearchTextRole ) {
        // The keys of objects and the scalar values, like the search
        const auto searched = ( index.column() == KeyColumn && current.arrayIndex < 0 )
                              || ( index.column() == ValueColumn && !current.value.isObject()
                                   && !current.value.isArray() );
        return searched ? data( index, Qt::DisplayRole ) : QVariant{};
    }
    else if ( role == Qt::ToolTipRole && index.column() == ValueColumn
              && current.value.isString() ) {
        return elided( current.value.toString(), 5000 );
    }
    else if ( role == Qt::ForegroundRole && index.column() == ValueColumn ) {
        const auto palette = QGuiApplication::palette();
        if ( current.value.isObject() || current.value.isArray() || current.value.isNull() ) {
            return palette.brush( QPalette::Disabled, QPalette::Text );
        }
    }
    return {};
}

QVariant JsonTreeModel::headerData( int section, Qt::Orientation orientation, int role ) const
{
    if ( orientation != Qt::Horizontal || role != Qt::DisplayRole ) {
        return {};
    }
    switch ( section ) {
    case KeyColumn:
        return tr( "Key" );
    case ValueColumn:
        return tr( "Value" );
    case TypeColumn:
        return tr( "Type" );
    default:
        break;
    }
    return {};
}

QString JsonTreeModel::valueText( const QModelIndex& index ) const
{
    const auto& value = node( index ).value;
    return value.isString() ? value.toString() : compactJson( value );
}

Qt::ItemFlags JsonTreeModel::flags( const QModelIndex& index ) const
{
    auto itemFlags = QAbstractItemModel::flags( index );
    // A double click on the key of an object or an array expands it
    if ( index.isValid() && data( index, Qt::EditRole ).isValid()
         && ( index.column() != KeyColumn || !hasChildren( index ) ) ) {
        itemFlags |= Qt::ItemIsEditable;
    }
    return itemFlags;
}

QModelIndex JsonTreeModel::indexOfPath( const std::vector<int>& rows, int column ) const
{
    QModelIndex current;
    for ( size_t i = 0; i < rows.size(); ++i ) {
        current = index( rows[ i ], i + 1 == rows.size() ? column : 0, current );
        if ( !current.isValid() ) {
            return {};
        }
    }
    return current;
}

QString JsonTreeModel::path( const QModelIndex& index ) const
{
    static const QRegularExpression identifier( QStringLiteral( "^[A-Za-z_$][A-Za-z0-9_$]*$" ) );

    QStringList parts;
    for ( auto id = nodeId( index ); id > 0; id = nodes_[ static_cast<size_t>( id ) ].parent ) {
        const auto& current = nodes_[ static_cast<size_t>( id ) ];
        if ( current.arrayIndex >= 0 ) {
            parts.prepend( QStringLiteral( "[%1]" ).arg( current.arrayIndex ) );
        }
        else if ( identifier.match( current.key ).hasMatch() ) {
            parts.prepend( QStringLiteral( "." ) + current.key );
        }
        else {
            parts.prepend( QStringLiteral( "[%1]" ).arg( compactJson( QJsonValue( current.key ) ) ) );
        }
    }
    return QStringLiteral( "$" ) + parts.join( QString{} );
}

/*
 * XmlTree
 */
std::shared_ptr<XmlTree> XmlTree::parse( const QString& text )
{
    auto tree = std::make_shared<XmlTree>();
    auto& nodes = tree->nodes;
    nodes.emplace_back(); // the document

    const auto add = [ &nodes ]( int parent, Node node ) {
        node.parent = parent;
        node.row = static_cast<int>( nodes[ static_cast<size_t>( parent ) ].children.size() );
        const auto id = static_cast<int>( nodes.size() );
        nodes[ static_cast<size_t>( parent ) ].children.push_back( id );
        nodes.push_back( std::move( node ) );
        return id;
    };

    QXmlStreamReader reader( text );
    int current = 0;
    while ( !reader.atEnd() ) {
        switch ( reader.readNext() ) {
        case QXmlStreamReader::StartElement: {
            Node node;
            node.kind = Node::Kind::Element;
            node.name = reader.qualifiedName().toString();
            QStringList attributes;
            for ( const auto& attribute : reader.attributes() ) {
                attributes.append( QStringLiteral( "%1=\"%2\"" )
                                       .arg( attribute.qualifiedName().toString(),
                                             attribute.value().toString() ) );
            }
            for ( const auto& ns : reader.namespaceDeclarations() ) {
                const auto prefix = ns.prefix().toString();
                attributes.prepend( QStringLiteral( "%1=\"%2\"" )
                                        .arg( prefix.isEmpty() ? QStringLiteral( "xmlns" )
                                                               : QStringLiteral( "xmlns:" ) + prefix,
                                              ns.namespaceUri().toString() ) );
            }
            node.value = attributes.join( ' ' );
            current = add( current, std::move( node ) );
            break;
        }
        case QXmlStreamReader::EndElement: {
            // An element with only a text and no attribute shows the text on its row
            auto& element = nodes[ static_cast<size_t>( current ) ];
            if ( element.children.size() == 1 && element.value.isEmpty()
                 && element.children.front() == static_cast<int>( nodes.size() ) - 1
                 && nodes.back().kind == Node::Kind::Text ) {
                element.value = std::move( nodes.back().value );
                element.children.clear();
                nodes.pop_back();
            }
            current = nodes[ static_cast<size_t>( current ) ].parent;
            break;
        }
        case QXmlStreamReader::Characters: {
            if ( reader.isWhitespace() ) {
                break;
            }
            Node node;
            node.kind = reader.isCDATA() ? Node::Kind::CData : Node::Kind::Text;
            node.value = reader.text().toString().trimmed();
            add( current, std::move( node ) );
            break;
        }
        case QXmlStreamReader::Comment: {
            Node node;
            node.kind = Node::Kind::Comment;
            node.value = reader.text().toString().trimmed();
            add( current, std::move( node ) );
            break;
        }
        case QXmlStreamReader::ProcessingInstruction: {
            Node node;
            node.kind = Node::Kind::ProcessingInstruction;
            node.name = reader.processingInstructionTarget().toString();
            node.value = reader.processingInstructionData().toString();
            add( current, std::move( node ) );
            break;
        }
        case QXmlStreamReader::DTD: {
            Node node;
            node.kind = Node::Kind::Dtd;
            node.name = QStringLiteral( "!DOCTYPE" );
            node.value = reader.text().toString();
            add( current, std::move( node ) );
            break;
        }
        default:
            break;
        }
    }

    if ( reader.hasError() ) {
        tree->error = reader.errorString();
        tree->errorLine = static_cast<int>( reader.lineNumber() );
        tree->errorColumn = static_cast<int>( reader.columnNumber() );
    }
    return tree;
}

/*
 * XmlTreeModel
 */
XmlTreeModel::XmlTreeModel( std::shared_ptr<const XmlTree> tree, QObject* parent )
    : QAbstractItemModel( parent )
    , tree_( std::move( tree ) )
{
}

QModelIndex XmlTreeModel::index( int row, int column, const QModelIndex& parent ) const
{
    if ( row < 0 || column < 0 || column >= ColumnCount ) {
        return {};
    }
    const auto parentId = parent.isValid() ? parent.internalId() : 0;
    const auto& children = tree_->nodes[ parentId ].children;
    if ( row >= static_cast<int>( children.size() ) ) {
        return {};
    }
    return createIndex( row, column, static_cast<quintptr>( children[ static_cast<size_t>( row ) ] ) );
}

QModelIndex XmlTreeModel::parent( const QModelIndex& child ) const
{
    if ( !child.isValid() ) {
        return {};
    }
    const auto parentId = tree_->nodes[ child.internalId() ].parent;
    if ( parentId <= 0 ) {
        return {};
    }
    return createIndex( tree_->nodes[ static_cast<size_t>( parentId ) ].row, 0,
                        static_cast<quintptr>( parentId ) );
}

int XmlTreeModel::rowCount( const QModelIndex& parent ) const
{
    if ( parent.column() > 0 ) {
        return 0;
    }
    const auto id = parent.isValid() ? parent.internalId() : 0;
    return static_cast<int>( tree_->nodes[ id ].children.size() );
}

int XmlTreeModel::columnCount( const QModelIndex& ) const
{
    return ColumnCount;
}

QVariant XmlTreeModel::data( const QModelIndex& index, int role ) const
{
    if ( !index.isValid() ) {
        return {};
    }
    const auto& node = tree_->nodes[ index.internalId() ];
    using Kind = XmlTree::Node::Kind;

    if ( role == Qt::DisplayRole ) {
        const auto text = cellText( node, index.column() );
        if ( !text.isNull() ) {
            return text;
        }
        // Labels of the nodes without a name
        switch ( node.kind ) {
        case Kind::Text:
            return tr( "text" );
        case Kind::CData:
            return QStringLiteral( "CDATA" );
        case Kind::Comment:
            return tr( "comment" );
        default:
            return {};
        }
    }
    if ( role == Qt::EditRole ) {
        if ( index.column() == ValueColumn ) {
            return render::singleLineText( node.value );
        }
        const auto text = cellText( node, index.column() );
        return text.isNull() ? QVariant{} : QVariant( text );
    }
    if ( role == SearchTextRole ) {
        // The names and values, not the labels of the text and comment nodes
        const auto text = cellText( node, index.column() );
        return text.isNull() ? QVariant{} : QVariant( text );
    }
    if ( role == Qt::ToolTipRole && index.column() == ValueColumn ) {
        return elided( node.value, 5000 );
    }
    if ( role == Qt::ForegroundRole && node.kind != Kind::Element && node.kind != Kind::Text
         && node.kind != Kind::CData ) {
        return QGuiApplication::palette().brush( QPalette::Disabled, QPalette::Text );
    }
    return {};
}

Qt::ItemFlags XmlTreeModel::flags( const QModelIndex& index ) const
{
    auto itemFlags = QAbstractItemModel::flags( index );
    if ( !index.isValid() ) {
        return itemFlags;
    }
    const auto& node = tree_->nodes[ index.internalId() ];
    // A double click on an element with children expands it
    const auto expands = index.column() == NodeColumn && !node.children.empty();
    if ( !expands && data( index, Qt::EditRole ).isValid() ) {
        itemFlags |= Qt::ItemIsEditable;
    }
    return itemFlags;
}

QString XmlTreeModel::cellText( const XmlTree::Node& node, int column )
{
    using Kind = XmlTree::Node::Kind;
    if ( column == ValueColumn ) {
        return render::displayText( node.value );
    }
    switch ( node.kind ) {
    case Kind::Element:
        return QStringLiteral( "<%1>" ).arg( node.name );
    case Kind::ProcessingInstruction:
        return QStringLiteral( "<?%1?>" ).arg( node.name );
    case Kind::Dtd:
        return node.name;
    default:
        return {};
    }
}

QModelIndex XmlTreeModel::indexOfNode( int id, int column ) const
{
    if ( id <= 0 || id >= static_cast<int>( tree_->nodes.size() ) ) {
        return {};
    }
    return createIndex( tree_->nodes[ static_cast<size_t>( id ) ].row, column,
                        static_cast<quintptr>( id ) );
}

QVariant XmlTreeModel::headerData( int section, Qt::Orientation orientation, int role ) const
{
    if ( orientation != Qt::Horizontal || role != Qt::DisplayRole ) {
        return {};
    }
    return section == NodeColumn ? tr( "Node" ) : tr( "Attributes / text" );
}

/*
 * LineTableModel
 */
LineTableModel::LineTableModel( const AbstractLogData* logData, RenderFormat format, bool header,
                                QObject* parent )
    : QAbstractTableModel( parent )
    , logData_( logData )
    , header_( header && format != RenderFormat::JsonLines )
{
    format_.format = format;
    if ( format == RenderFormat::Tsv ) {
        format_.delimiter = '\t';
    }
    readColumns();
    rows_ = rowsOfData();
}

QStringList LineTableModel::sampleLines( int count ) const
{
    const auto available = logData_->getNbLine().get();
    const auto lines = std::min<quint64>( available, static_cast<quint64>( count ) );
    QStringList sample;
    if ( lines == 0 ) {
        return sample;
    }
    const auto read = logData_->getLines( LineNumber( 0 ), LinesCount( static_cast<LinesCount::UnderlyingType>( lines ) ) );
    sample.reserve( static_cast<qsizetype>( read.size() ) );
    for ( const auto& line : read ) {
        sample.append( line );
    }
    return sample;
}

void LineTableModel::readColumns()
{
    auto& columns = format_.columns;
    columns.clear();
    const auto sample = sampleLines( SampleLines );

    if ( format_.format == RenderFormat::JsonLines ) {
        columns = render::jsonLinesKeys( sample );
        if ( columns.isEmpty() ) {
            columns.append( tr( "Line" ) );
        }
        return;
    }

    if ( format_.format == RenderFormat::Csv ) {
        format_.delimiter = render::guessDelimiter( sample.mid( 0, 20 ) );
    }

    qsizetype fields = 1;
    for ( const auto& line : sample ) {
        fields = std::max( fields, render::splitDelimited( line, format_.delimiter ).size() );
    }
    if ( header_ && !sample.isEmpty() ) {
        columns = render::splitDelimited( sample.first(), format_.delimiter );
    }
    for ( auto column = columns.size(); column < fields; ++column ) {
        columns.append( QString::number( column + 1 ) );
    }
}

int LineTableModel::rowsOfData() const
{
    const auto lines = logData_->getNbLine().get();
    const auto headerLines = header_ ? 1u : 0u;
    const auto rows = lines > headerLines ? lines - headerLines : 0u;
    return static_cast<int>( std::min<quint64>( rows, std::numeric_limits<int>::max() ) );
}

void LineTableModel::refresh()
{
    const auto rows = rowsOfData();
    if ( rows == rows_ ) {
        return;
    }
    if ( rows > rows_ ) {
        beginInsertRows( {}, rows_, rows - 1 );
        rows_ = rows;
        endInsertRows();
        return;
    }

    // Truncated: the lines are not the same any more
    beginResetModel();
    cache_.clear();
    readColumns();
    rows_ = rows;
    endResetModel();
}

qint64 LineTableModel::lineOfRow( int row ) const
{
    return static_cast<qint64>( row ) + ( header_ ? 1 : 0 );
}

QString LineTableModel::lineText( int row ) const
{
    return logData_->getLineString(
        LineNumber( static_cast<LineNumber::UnderlyingType>( lineOfRow( row ) ) ) );
}

int LineTableModel::rowOfLine( qint64 line ) const
{
    return static_cast<int>( line - ( header_ ? 1 : 0 ) );
}

QString LineTableModel::cellText( int row, int column ) const
{
    const auto& rowCells = cells( row );
    return column < rowCells.size() ? rowCells.at( column ) : QString{};
}

QString LineTableModel::cellDisplayText( int row, int column ) const
{
    return render::displayText( cellText( row, column ) );
}

const QStringList& LineTableModel::cells( int row ) const
{
    if ( const auto* cached = cache_.find( row ) ) {
        return *cached;
    }

    return cache_.insert( row, format_.cells( lineText( row ) ) );
}

int LineTableModel::rowCount( const QModelIndex& parent ) const
{
    return parent.isValid() ? 0 : rows_;
}

int LineTableModel::columnCount( const QModelIndex& parent ) const
{
    return parent.isValid() ? 0 : static_cast<int>( format_.columns.size() );
}

QVariant LineTableModel::data( const QModelIndex& index, int role ) const
{
    if ( !index.isValid()
         || ( role != Qt::DisplayRole && role != Qt::ToolTipRole && role != SearchTextRole
              && role != Qt::EditRole ) ) {
        return {};
    }
    const auto& rowCells = cells( index.row() );
    if ( index.column() >= rowCells.size() ) {
        return {};
    }
    const auto& cell = rowCells.at( index.column() );
    if ( role == Qt::ToolTipRole ) {
        return cell.size() > 40 ? elided( cell, 5000 ) : QVariant{};
    }
    if ( role == Qt::EditRole ) {
        return render::singleLineText( cell );
    }
    return render::displayText( cell );
}

Qt::ItemFlags LineTableModel::flags( const QModelIndex& index ) const
{
    return QAbstractTableModel::flags( index ) | ( index.isValid() ? Qt::ItemIsEditable : Qt::NoItemFlags );
}

QVariant LineTableModel::headerData( int section, Qt::Orientation orientation, int role ) const
{
    if ( role != Qt::DisplayRole ) {
        return {};
    }
    if ( orientation == Qt::Vertical ) {
        // Line numbers of the file, like the log view
        return QString::number( lineOfRow( section ) + 1 );
    }
    return section < format_.columns.size() ? format_.columns.at( section ) : QVariant{};
}
