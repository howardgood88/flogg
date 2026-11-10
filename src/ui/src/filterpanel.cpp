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

#include <algorithm>
#include <functional>
#include <optional>
#include <utility>

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDrag>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleOption>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QtConcurrent>
#include <QVBoxLayout>

#include "configuration.h"
#include "filtereditdialog.h"
#include "log.h"
#include "persistentinfo.h"
#include "shortcuts.h"
#include "styles.h"

#include "filterpanel.h"

namespace {

constexpr int IdRole = Qt::UserRole;
constexpr int SuspendedRole = Qt::UserRole + 1;
// Tool tip of the pattern column without the filter number
constexpr int PropertiesRole = Qt::UserRole + 2;

constexpr int PatternColumn = 0;
constexpr int HitsColumn = 1;
constexpr int DescriptionColumn = 2;
// Filter number (priority order), shown first like line numbers
constexpr int NumberColumn = 3;

constexpr auto CurrentFileKey = "filterPanel/currentFile";
constexpr auto RecentFilesKey = "filterPanel/recentFiles";
constexpr auto HeaderStateKey = "filterPanel/headerState";
constexpr int MaxRecentFiles = 8;
constexpr auto FileFilter = "flogg filters (*.flogg-filters.json);;All files (*)";

QString defaultFiltersFile()
{
    // Next to the settings when they are kept in a file of their own
    auto dir = PersistentInfo::portableDirectory();
    if ( dir.isEmpty() ) {
        dir = QStandardPaths::writableLocation( QStandardPaths::GenericDataLocation ) + "/flogg";
    }
    QDir{}.mkpath( dir );
    return dir + "/filters.flogg-filters.json";
}

QString& startupFile()
{
    static QString path;
    return path;
}

// Revisions are unique among all the panels: a tab can get the snapshots of several panels
// (shared filters, moved tab)
uint64_t nextRevision()
{
    static uint64_t revision = 0;
    return ++revision;
}

// Compares the filters only, every tree built from the panel gets a new root
QByteArray serialize( const FilterNode& root )
{
    auto filtersOnly = FilterNode::makeGroup( {} );
    filtersOnly.id.clear();
    filtersOnly.children = root.children;
    return QJsonDocument( filters::toJson( filtersOnly ) ).toJson( QJsonDocument::Compact );
}

// Filter files in the folder of portable settings are remembered relative to it,
// so that the folder still works when it is moved or copied elsewhere
QString toRememberedPath( const QString& path )
{
    const auto dir = PersistentInfo::portableDirectory();
    if ( dir.isEmpty() ) {
        return path;
    }

    const auto relativePath = QDir( dir ).relativeFilePath( path );
    if ( QDir::isAbsolutePath( relativePath ) || relativePath == ".."
         || relativePath.startsWith( "../" ) ) {
        return path;
    }
    return relativePath;
}

QString fromRememberedPath( const QString& path )
{
    const auto dir = PersistentInfo::portableDirectory();
    if ( dir.isEmpty() || path.isEmpty() || QDir::isAbsolutePath( path ) ) {
        return path;
    }
    return QDir::cleanPath( QDir( dir ).absoluteFilePath( path ) );
}

QSettings& settings()
{
    return PersistentInfo::getSettings( app_settings{} );
}

bool isGroupItem( const QTreeWidgetItem* item )
{
    return item->flags().testFlag( Qt::ItemIsDropEnabled );
}

// Thin arrow drawn with the text colour, matching the text only tool buttons
QIcon arrowIcon( bool up, const QPalette& palette )
{
    constexpr int Size = 12;
    constexpr qreal Scale = 2;

    QIcon icon;
    for ( const auto mode : { QIcon::Normal, QIcon::Disabled } ) {
        QPixmap pixmap( static_cast<int>( Size * Scale ), static_cast<int>( Size * Scale ) );
        pixmap.setDevicePixelRatio( Scale );
        pixmap.fill( Qt::transparent );

        QPainter painter( &pixmap );
        painter.setRenderHint( QPainter::Antialiasing );
        const auto group = mode == QIcon::Disabled ? QPalette::Disabled : QPalette::Active;
        painter.setPen( QPen( palette.color( group, QPalette::ButtonText ), 1.3, Qt::SolidLine,
                              Qt::RoundCap, Qt::RoundJoin ) );

        const qreal tip = up ? 2 : Size - 2;
        const qreal tail = up ? Size - 2 : 2;
        const qreal head = up ? 4 : -4;
        const qreal middle = Size / 2.0;
        painter.drawLine( QPointF( middle, tip ), QPointF( middle, tail ) );
        painter.drawPolyline( QPolygonF{ { middle - 3.5, tip + head },
                                         { middle, tip },
                                         { middle + 3.5, tip + head } } );
        painter.end();

        icon.addPixmap( pixmap, mode );
    }
    return icon;
}

bool isSameOrChild( const QTreeWidgetItem* item, const QTreeWidgetItem* ancestor )
{
    for ( ; item; item = item->parent() ) {
        if ( item == ancestor ) {
            return true;
        }
    }
    return false;
}

// Lays out widgets left to right, wrapping to a new row when the width runs out
// Shows "..." at the end of a text too long instead of widening the panel
class ElidedLabel : public QLabel {
  public:
    void setFullText( const QString& text )
    {
        fullText_ = text;
        updateText();
    }

    QSize minimumSizeHint() const override
    {
        return { 0, QLabel::minimumSizeHint().height() };
    }

    QSize sizeHint() const override
    {
        return { 0, QLabel::sizeHint().height() };
    }

  protected:
    void resizeEvent( QResizeEvent* event ) override
    {
        QLabel::resizeEvent( event );
        updateText();
    }

  private:
    void updateText()
    {
        const auto width = contentsRect().width();
        setText( fontMetrics().elidedText( fullText_, Qt::ElideRight, std::max( width, 0 ) ) );
    }

    QString fullText_;
};

class FlowLayout : public QLayout {
  public:
    FlowLayout()
    {
        setContentsMargins( 0, 0, 0, 0 );
        setSpacing( 0 );
    }

    ~FlowLayout() override
    {
        while ( auto* item = takeAt( 0 ) ) {
            delete item;
        }
    }

    void addItem( QLayoutItem* item ) override
    {
        items_.push_back( item );
    }

    int count() const override
    {
        return static_cast<int>( items_.size() );
    }

    QLayoutItem* itemAt( int index ) const override
    {
        return index >= 0 && index < count() ? items_[ static_cast<size_t>( index ) ] : nullptr;
    }

    QLayoutItem* takeAt( int index ) override
    {
        if ( index < 0 || index >= count() ) {
            return nullptr;
        }
        auto* item = items_[ static_cast<size_t>( index ) ];
        items_.erase( items_.begin() + index );
        return item;
    }

    Qt::Orientations expandingDirections() const override
    {
        return {};
    }

    bool hasHeightForWidth() const override
    {
        return true;
    }

    int heightForWidth( int width ) const override
    {
        return doLayout( QRect( 0, 0, width, 0 ), false );
    }

    void setGeometry( const QRect& rect ) override
    {
        QLayout::setGeometry( rect );
        doLayout( rect, true );
    }

    QSize sizeHint() const override
    {
        return minimumSize();
    }

    QSize minimumSize() const override
    {
        QSize size;
        for ( const auto* item : items_ ) {
            size = size.expandedTo( item->minimumSize() );
        }
        const auto margins = contentsMargins();
        return size + QSize( margins.left() + margins.right(), margins.top() + margins.bottom() );
    }

  private:
    int doLayout( const QRect& rect, bool apply ) const
    {
        const auto area = rect.marginsRemoved( contentsMargins() );
        int x = area.x();
        int y = area.y();
        int rowHeight = 0;

        for ( auto* item : items_ ) {
            const auto size = item->sizeHint();
            if ( x + size.width() > area.right() + 1 && rowHeight > 0 ) {
                x = area.x();
                y += rowHeight + spacing();
                rowHeight = 0;
            }
            if ( apply ) {
                item->setGeometry( QRect( QPoint( x, y ), size ) );
            }
            x += size.width() + spacing();
            rowHeight = std::max( rowHeight, size.height() );
        }

        return y + rowHeight - rect.y() + contentsMargins().bottom();
    }

    std::vector<QLayoutItem*> items_;
};

// Filters have no expand arrow: their check box takes the place of the arrow, so that it
// lines up with the arrows of the groups next to them instead of with their folder icons
class FilterItemDelegate : public QStyledItemDelegate {
  public:
    explicit FilterItemDelegate( QTreeView* tree )
        : QStyledItemDelegate( tree )
        , tree_( tree )
    {
    }

    void paint( QPainter* painter, const QStyleOptionViewItem& option,
                const QModelIndex& index ) const override
    {
        auto itemOption = alignedOption( option, index );
        initStyleOption( &itemOption, index );
        const auto* widget = itemOption.widget;
        auto* style = widget ? widget->style() : QApplication::style();

        // Rows are selected as a whole, the current column is not marked
        itemOption.state &= ~QStyle::State_HasFocus;

        // A selected row has the fixed colours of the selected lines of the log views, in
        // place of the filter colours
        const bool selected = itemOption.state.testFlag( QStyle::State_Selected );
        if ( selected ) {
            itemOption.state &= ~QStyle::State_Selected;
            itemOption.backgroundBrush = QBrush{};
            painter->fillRect( itemOption.rect, StyleManager::selectionBackColor() );
            for ( const auto role : { QPalette::Text, QPalette::HighlightedText } ) {
                itemOption.palette.setColor( QPalette::Active, role,
                                             StyleManager::selectionTextColor() );
                itemOption.palette.setColor( QPalette::Inactive, role,
                                             StyleManager::selectionTextColor() );
            }
        }

        if ( !itemOption.features.testFlag( QStyleOptionViewItem::HasCheckIndicator ) ) {
            style->drawControl( QStyle::CE_ItemViewItem, &itemOption, painter, widget );
            return;
        }

        // The style draws the check box with the text colour, which is the filter colour:
        // draw the item with the palette of the tree, then the style draws the text again
        // clipped to the text area, like for the groups. A filter colour is kept when selected.
        const auto textRect
            = style->subElementRect( QStyle::SE_ItemViewItemText, &itemOption, widget );

        auto boxOption = itemOption;
        boxOption.text.clear();
        boxOption.palette = tree_->palette();
        style->drawControl( QStyle::CE_ItemViewItem, &boxOption, painter, widget );

        if ( !selected && index.data( Qt::ForegroundRole ).isValid() ) {
            itemOption.palette.setBrush( QPalette::HighlightedText,
                                         itemOption.palette.brush( QPalette::Text ) );
        }
        painter->save();
        painter->setClipRect( textRect, Qt::IntersectClip );
        style->drawControl( QStyle::CE_ItemViewItem, &itemOption, painter, widget );
        painter->restore();
    }

  protected:
    // Check box clicks are tested against the moved rectangle
    bool editorEvent( QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option,
                      const QModelIndex& index ) override
    {
        return QStyledItemDelegate::editorEvent( event, model, alignedOption( option, index ),
                                                 index );
    }

  private:
    QStyleOptionViewItem alignedOption( const QStyleOptionViewItem& option,
                                        const QModelIndex& index ) const
    {
        auto aligned = option;
        if ( index.column() == PatternColumn && !index.flags().testFlag( Qt::ItemIsDropEnabled ) ) {
            aligned.rect.setLeft( aligned.rect.left() - tree_->indentation() );
        }
        return aligned;
    }

    QTreeView* tree_;
};

// Tree that also accepts drops below its last row. The horizontal position of the
// cursor chooses the level: the end of the top level list or of one of the last groups.
class FilterTreeWidget : public QTreeWidget {
  public:
    using QTreeWidget::QTreeWidget;

  protected:
    // The style does not fill selected rows: the delegate and the branches use the fixed
    // selection colour of the log views
    void initViewItemOption( QStyleOptionViewItem* option ) const override
    {
        QTreeWidget::initViewItemOption( option );
        option->palette.setColor( QPalette::Highlight, Qt::transparent );
    }

    void drawBranches( QPainter* painter, const QRect& rect,
                       const QModelIndex& index ) const override
    {
        // Under the expand arrow
        if ( selectionModel()->isSelected( index ) ) {
            painter->fillRect( rect, StyleManager::selectionBackColor() );
        }
        QTreeWidget::drawBranches( painter, rect, index );
    }

    // All drops are handled inside the tree (QTreeWidget moves the items itself and so does
    // the drop below the last row), so unlike QAbstractItemView never remove the dragged
    // items once the drag is done.
    void startDrag( Qt::DropActions ) override
    {
        auto indexes = selectedIndexes();
        indexes.erase( std::remove_if( indexes.begin(), indexes.end(),
                                       []( const QModelIndex& index ) {
                                           return !index.flags().testFlag( Qt::ItemIsDragEnabled );
                                       } ),
                       indexes.end() );
        auto* data = indexes.isEmpty() ? nullptr : model()->mimeData( indexes );
        if ( !data ) {
            return;
        }

        QRect rect;
        for ( const auto& index : indexes ) {
            rect |= visualRect( index );
        }
        rect &= viewport()->rect();

        auto* drag = new QDrag( this );
        drag->setMimeData( data );
        if ( !rect.isEmpty() ) {
            drag->setPixmap( viewport()->grab( rect ) );
            drag->setHotSpot( viewport()->mapFromGlobal( QCursor::pos() ) - rect.topLeft() );
        }
        drag->exec( Qt::MoveAction );
    }

    void dragMoveEvent( QDragMoveEvent* event ) override
    {
        QTreeWidget::dragMoveEvent( event );

        updateTailDrop( event->position().toPoint() );
        if ( tailParent_ ) {
            event->setDropAction( Qt::MoveAction );
            event->accept();
        }
        viewport()->update();
    }

    void dragLeaveEvent( QDragLeaveEvent* event ) override
    {
        QTreeWidget::dragLeaveEvent( event );
        tailParent_ = nullptr;
        viewport()->update();
    }

    void dropEvent( QDropEvent* event ) override
    {
        auto* parent = std::exchange( tailParent_, nullptr );
        if ( !parent || event->source() != this ) {
            QTreeWidget::dropEvent( event );
            return;
        }

        moveSelectedItemsTo( parent );
        event->acceptProposedAction();
        viewport()->update();
    }

    void paintEvent( QPaintEvent* event ) override
    {
        QTreeWidget::paintEvent( event );

        if ( !tailParent_ ) {
            return;
        }

        // Same indicator as the one drawn by Qt between rows
        QStyleOption option;
        option.initFrom( this );
        option.rect = tailIndicator_;
        QPainter painter( viewport() );
        style()->drawPrimitive( QStyle::PE_IndicatorItemViewItemDrop, &option, &painter, this );
    }

  private:
    // Parents an item dropped below the last row can be appended to, from the top level
    // to the deepest group ending the tree
    std::vector<QTreeWidgetItem*> tailParents() const
    {
        std::vector<QTreeWidgetItem*> parents{ invisibleRootItem() };

        auto* item = topLevelItemCount() > 0 ? topLevelItem( topLevelItemCount() - 1 ) : nullptr;
        while ( item && isGroupItem( item ) && !isDragged( item ) ) {
            if ( item->childCount() > 0 && !item->isExpanded() ) {
                break;
            }
            parents.push_back( item );
            item = item->childCount() > 0 ? item->child( item->childCount() - 1 ) : nullptr;
        }

        return parents;
    }

    QTreeWidgetItem* lastVisibleItem() const
    {
        auto* item = topLevelItemCount() > 0 ? topLevelItem( topLevelItemCount() - 1 ) : nullptr;
        while ( item && item->isExpanded() && item->childCount() > 0 ) {
            item = item->child( item->childCount() - 1 );
        }
        return item;
    }

    bool isDragged( const QTreeWidgetItem* item ) const
    {
        for ( ; item; item = item->parent() ) {
            if ( item->isSelected() ) {
                return true;
            }
        }
        return false;
    }

    void updateTailDrop( const QPoint& pos )
    {
        tailParent_ = nullptr;

        const auto* last = lastVisibleItem();
        if ( !last ) {
            return;
        }

        const auto lastRow = visualItemRect( last );
        if ( pos.y() <= lastRow.bottom() ) {
            return;
        }

        const auto parents = tailParents();
        // The tree column is not the first one, it follows the number column
        const auto treeLeft = header()->sectionViewportPosition( treePosition() );
        const auto rootOffset = treeLeft + ( rootIsDecorated() ? indentation() : 0 );
        const auto level = std::clamp( ( pos.x() - rootOffset ) / indentation(), 0,
                                       static_cast<int>( parents.size() ) - 1 );

        tailParent_ = parents[ static_cast<size_t>( level ) ];

        const auto left = rootOffset + level * indentation();
        tailIndicator_ = QRect( left, lastRow.bottom(), viewport()->width() - left, 0 );
    }

    void moveSelectedItemsTo( QTreeWidgetItem* parent )
    {
        // Top most selected items in tree order, their children move with them
        std::vector<QTreeWidgetItem*> moved;
        for ( QTreeWidgetItemIterator it( this, QTreeWidgetItemIterator::Selected ); *it; ++it ) {
            if ( !isDragged( ( *it )->parent() ) ) {
                moved.push_back( *it );
            }
        }

        for ( auto* item : moved ) {
            std::vector<QTreeWidgetItem*> expanded;
            for ( QTreeWidgetItemIterator it( item ); *it && isSameOrChild( *it, item ); ++it ) {
                if ( ( *it )->isExpanded() ) {
                    expanded.push_back( *it );
                }
            }

            auto* from = item->parent() ? item->parent() : invisibleRootItem();
            from->takeChild( from->indexOfChild( item ) );
            parent->addChild( item );

            for ( auto* group : expanded ) {
                group->setExpanded( true );
            }
            item->setSelected( true );
        }

        if ( parent != invisibleRootItem() ) {
            parent->setExpanded( true );
        }
    }

    QTreeWidgetItem* tailParent_ = nullptr;
    QRect tailIndicator_;
};

} // namespace

QIcon moveArrowIcon( bool up, const QPalette& palette )
{
    return arrowIcon( up, palette );
}

QIcon sharedFiltersIcon( const QColor& color )
{
    QIcon icon;
    for ( const auto scale : { 1, 2, 3 } ) {
        QPixmap pixmap( 16 * scale, 16 * scale );
        pixmap.setDevicePixelRatio( scale );
        pixmap.fill( Qt::transparent );

        QPainter painter( &pixmap );
        painter.setRenderHint( QPainter::Antialiasing );
        QPen pen( color, 1.7 );
        painter.setPen( pen );
        painter.setBrush( Qt::NoBrush );

        // Two links crossing the icon diagonally, overlapping in the middle
        painter.translate( 8, 8 );
        painter.rotate( -45 );
        painter.drawRoundedRect( QRectF( -7.4, -2.7, 9.0, 5.4 ), 2.7, 2.7 );
        painter.drawRoundedRect( QRectF( -1.6, -2.7, 9.0, 5.4 ), 2.7, 2.7 );
        painter.end();

        icon.addPixmap( pixmap );
    }
    return icon;
}

FilterFiles& FilterFiles::instance()
{
    static auto* const filterFiles = new FilterFiles;
    return *filterFiles;
}

bool FilterFiles::isSameFile( const QString& lhs, const QString& rhs )
{
    return !lhs.isEmpty() && !rhs.isEmpty() && QFileInfo( lhs ) == QFileInfo( rhs );
}

bool FilterFiles::follow( FilterTabState& state, const QString& path, const QByteArray& json )
{
    if ( !state.initialized || !isSameFile( state.file, path ) || state.savedJson == json ) {
        return false;
    }

    if ( FilterPanel::isModified( state ) ) {
        state.fileChanged = true;
        return true;
    }

    // Applied again when the tab is shown
    state.root = filters::fromJson( QJsonDocument::fromJson( json ).object() );
    state.savedJson = json;
    state.json = json;
    state.snapshot.reset();
    state.fileChanged = false;
    return true;
}

SharedFilters& SharedFilters::instance()
{
    static auto* const sharedFilters = [] {
        auto* filters = new SharedFilters;
        // Saved by a tab using the file on its own
        connect( &FilterFiles::instance(), &FilterFiles::saved, filters,
                 [ filters ]( const QString& path, const QByteArray& json ) {
                     if ( FilterFiles::follow( filters->state_, path, json ) ) {
                         Q_EMIT filters->changed( nullptr );
                     }
                 } );
        return filters;
    }();
    return *sharedFilters;
}

void SharedFilters::setState( FilterTabState state, const QObject* source )
{
    state_ = std::move( state );
    state_.initialized = true;
    state_.shared = false;
    Q_EMIT changed( source );
}

void SharedFilters::setUser( QWidget* tab, bool isUser )
{
    // Called on every tab switch, the views follow actual changes only
    const auto wasUser = this->isUser( tab );
    users_.removeIf( [ tab ]( const auto& user ) { return user.isNull() || user == tab; } );
    if ( isUser && tab ) {
        users_.append( tab );
        if ( !wasUser ) {
            // Also when the tab is destroyed without leaving first
            connect( tab, &QObject::destroyed, this, &SharedFilters::usersChanged,
                     Qt::UniqueConnection );
        }
    }
    if ( wasUser != ( isUser && tab ) ) {
        Q_EMIT usersChanged();
    }

    // Shared filters left by their last user are dropped (it was asked to save them): the
    // next tab shared starts them again from its own
    if ( users_.isEmpty() && state_.initialized ) {
        state_ = {};
    }
}

bool SharedFilters::isUser( const QWidget* tab ) const
{
    return std::any_of( users_.begin(), users_.end(),
                        [ tab ]( const auto& user ) { return user == tab; } );
}

bool SharedFilters::hasOtherUsers( const QWidget* tab, const QWidget* closingWindow ) const
{
    return hasUsersLeft( [ & ]( const QWidget* user ) {
        return user == tab || ( closingWindow && user->window() == closingWindow );
    } );
}

bool SharedFilters::hasUsersLeft( const std::function<bool( const QWidget* )>& isClosing ) const
{
    return std::any_of( users_.begin(), users_.end(), [ & ]( const auto& user ) {
        return !user.isNull() && !isClosing( user.data() );
    } );
}

bool SharedFilters::save( QWidget* parent )
{
    auto state = state_;
    if ( !FilterPanel::saveState( state, parent, tr( "Save shared filters as" ) ) ) {
        return false;
    }
    setState( std::move( state ), nullptr );
    FilterFiles::instance().notifySaved( state_.file, state_.savedJson, this );
    return true;
}

void SharedFilters::discardChanges()
{
    auto state = state_;
    FilterPanel::discardChanges( state );
    setState( std::move( state ), nullptr );
}

bool confirmUnsavedFilters( QWidget* parent, const QString& text,
                            const std::vector<UnsavedFilters>& filters )
{
    if ( filters.empty() ) {
        return true;
    }

    const auto fileName = []( const UnsavedFilters& filter ) {
        return filter.file.isEmpty() ? QObject::tr( "untitled" )
                                     : QFileInfo( filter.file ).fileName();
    };

    // One file to save: the plain question, like closing the shared filters
    if ( filters.size() == 1 ) {
        const auto& filter = filters.front();
        const auto answer = QMessageBox::question(
            parent, QObject::tr( "Unsaved filters" ),
            QObject::tr( "%1 (%2) have changed. Save them?" ).arg( filter.name, fileName( filter ) ),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save );
        if ( answer == QMessageBox::Save ) {
            return filter.save();
        }
        if ( answer == QMessageBox::Discard ) {
            filter.discard();
            return true;
        }
        return false;
    }

    QDialog dialog( parent );
    dialog.setObjectName( "unsavedFilters" );
    dialog.setWindowTitle( QObject::tr( "Unsaved filters" ) );

    auto* list = new QTreeWidget( &dialog );
    list->setRootIsDecorated( false );
    list->setHeaderLabels( { QObject::tr( "Filters" ), QObject::tr( "File" ) } );
    for ( const auto& filter : filters ) {
        auto* item = new QTreeWidgetItem( list );
        item->setText( 0, filter.name );
        item->setText( 1, fileName( filter ) );
        item->setToolTip( 1, QDir::toNativeSeparators( filter.file ) );
        item->setCheckState( 0, Qt::Checked );
    }
    list->resizeColumnToContents( 0 );

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Discard | QDialogButtonBox::Cancel, &dialog );
    buttons->button( QDialogButtonBox::Save )->setText( QObject::tr( "Save selected" ) );
    buttons->button( QDialogButtonBox::Discard )->setText( QObject::tr( "Discard all" ) );
    buttons->button( QDialogButtonBox::Save )->setDefault( true );

    auto discard = false;
    QObject::connect( buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept );
    QObject::connect( buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject );
    QObject::connect( buttons->button( QDialogButtonBox::Discard ), &QPushButton::clicked,
                      &dialog, [ & ] {
                          discard = true;
                          dialog.accept();
                      } );

    auto* layout = new QVBoxLayout( &dialog );
    layout->addWidget( new QLabel( text, &dialog ) );
    layout->addWidget( list );
    layout->addWidget( buttons );

    if ( dialog.exec() != QDialog::Accepted ) {
        return false;
    }

    const auto isSelected = [ & ]( size_t i ) {
        return !discard
               && list->topLevelItem( static_cast<int>( i ) )->checkState( 0 ) == Qt::Checked;
    };
    // In the order shown, the first one not saved stops the closing
    for ( size_t i = 0; i < filters.size(); ++i ) {
        if ( isSelected( i ) && !filters[ i ].save() ) {
            return false;
        }
    }
    for ( size_t i = 0; i < filters.size(); ++i ) {
        if ( !isSelected( i ) ) {
            filters[ i ].discard();
        }
    }
    return true;
}

void SharedFilters::notifyHighlightersCompiled( const HighlighterSet* highlighters )
{
    Q_EMIT highlightersCompiled( highlighters );
}

FilterPanel::FilterPanel( QWidget* parent )
    : QWidget( parent )
    , snapshot_( std::make_shared<FilterSnapshot>() )
{
    groupIcon_ = style()->standardIcon( QStyle::SP_DirIcon );
    slowIcon_ = style()->standardIcon( QStyle::SP_MessageBoxWarning );

    setupUi();

    changeTimer_.setSingleShot( true );
    changeTimer_.setInterval( 150 );
    connect( &changeTimer_, &QTimer::timeout, this, &FilterPanel::publishChange );

    // Changes of the shared filters made in another panel
    auto& sharedFilters = SharedFilters::instance();
    connect( &sharedFilters, &SharedFilters::changed, this, [ this ]( const QObject* source ) {
        if ( !shared_ || source == this ) {
            return;
        }
        FilterTabState view;
        saveTreeView( view );
        showSharedFilters( view );
        if ( !snapshot_ ) {
            publishChange();
        }
        else {
            Q_EMIT filtersChanged();
        }
    } );

    // The file of the shown filters saved by another tab
    connect( &FilterFiles::instance(), &FilterFiles::saved, this,
             [ this ]( const QString& path, const QByteArray& json, const QObject* source ) {
                 if ( source == this ) {
                     return;
                 }
                 if ( shared_ ) {
                     // The shared filters follow in SharedFilters, the own ones of the tab here
                     if ( FilterFiles::follow( privateState_, path, json ) ) {
                         updateFileLabel();
                     }
                     return;
                 }
                 if ( !FilterFiles::isSameFile( currentFile_, path ) || json == savedJson_ ) {
                     return;
                 }

                 flushChange();
                 if ( isModified() ) {
                     fileChanged_ = true;
                     updateFileLabel();
                     return;
                 }

                 FilterTabState view;
                 saveTreeView( view );
                 savedJson_ = json;
                 setRoot( filters::fromJson( QJsonDocument::fromJson( json ).object() ) );
                 restoreTreeView( view );
                 publishChange();
             } );
    connect( &sharedFilters, &SharedFilters::highlightersCompiled, this,
             [ this ]( const HighlighterSet* highlighters ) {
                 if ( shared_ && snapshot_ && snapshot_->highlighters.get() == highlighters ) {
                     Q_EMIT highlightersCompiled();
                 }
             } );

    loadFile( defaultFile(), startupFile().isEmpty() );
}

// File of the tabs shown for the first time
QString FilterPanel::defaultFile()
{
    if ( !startupFile().isEmpty() ) {
        return startupFile();
    }

    auto file = fromRememberedPath( settings().value( CurrentFileKey ).toString() );
    if ( file.isEmpty() ) {
        file = defaultFiltersFile();
    }
    return file;
}

void FilterPanel::setStartupFile( const QString& path )
{
    startupFile() = path;
}

bool FilterPanel::matchesPatterns( const QString& patterns, const QString& logFile )
{
    const QFileInfo info( logFile );
    const auto name = info.fileName();
    const auto path = QDir::fromNativeSeparators( info.absoluteFilePath() );

    const auto list = patterns.split( ';', Qt::SkipEmptyParts );
    for ( const auto& item : list ) {
        // Folders may be written with a backslash on every system
        const auto pattern = item.trimmed().replace( '\\', '/' );
        if ( pattern.isEmpty() ) {
            continue;
        }

        // A pattern with a folder matches the whole path, its '*' also crosses folders
        const auto withFolder = pattern.contains( '/' );
#if QT_VERSION >= QT_VERSION_CHECK( 6, 6, 0 )
        const auto options = withFolder ? QRegularExpression::NonPathWildcardConversion
                                        : QRegularExpression::DefaultWildcardConversion;
        const QRegularExpression regex(
            QRegularExpression::wildcardToRegularExpression( pattern, options ),
            QRegularExpression::CaseInsensitiveOption );
#else
        const QRegularExpression regex( QRegularExpression::wildcardToRegularExpression( pattern ),
                                        QRegularExpression::CaseInsensitiveOption );
#endif
        if ( regex.match( withFolder ? path : name ).hasMatch() ) {
            return true;
        }
    }
    return false;
}

QString FilterPanel::ruleFileFor( const QString& logFile )
{
    // A few patterns per file opened, nothing when there are no rules
    const auto& rules = Configuration::get().filterFileRules();
    for ( const auto& rule : rules ) {
        if ( !rule.file.isEmpty() && matchesPatterns( rule.patterns, logFile ) ) {
            return fromRememberedPath( rule.file );
        }
    }
    return {};
}

QString FilterPanel::rememberedPath( const QString& path )
{
    return toRememberedPath( path );
}

QString FilterPanel::pathFromRemembered( const QString& path )
{
    return fromRememberedPath( path );
}

void FilterPanel::setupUi()
{
    tree_ = new FilterTreeWidget;
    tree_->setItemDelegate( new FilterItemDelegate( tree_ ) );
    tree_->setColumnCount( 4 );
    tree_->setHeaderLabels( { tr( "Filter" ), tr( "Hits" ), tr( "Description" ), tr( "#" ) } );
    tree_->setSelectionMode( QAbstractItemView::ExtendedSelection );
    tree_->setDragDropMode( QAbstractItemView::InternalMove );
    tree_->setDefaultDropAction( Qt::MoveAction );
    tree_->setContextMenuPolicy( Qt::CustomContextMenu );
    tree_->setUniformRowHeights( true );
    // Double click edits the item; the view would toggle a group after the dialog closes
    tree_->setExpandsOnDoubleClick( false );

    // Each column is resized by its own right edge only, a wider panel just shows more room
    auto* header = tree_->header();
    header->setSectionResizeMode( QHeaderView::Interactive );
    header->setStretchLastSection( false );
    header->setSectionsMovable( false );
    tree_->setColumnWidth( PatternColumn, 220 );
    tree_->setColumnWidth( HitsColumn, 60 );
    tree_->setColumnWidth( DescriptionColumn, 200 );
    tree_->setColumnWidth( NumberColumn, 32 );
    header->moveSection( header->visualIndex( NumberColumn ), 0 );
    restoreHeaderState();
    header->setContextMenuPolicy( Qt::CustomContextMenu );
    connect( header, &QWidget::customContextMenuRequested, this, &FilterPanel::showHeaderMenu );
    connect( header, &QHeaderView::sectionResized, this, &FilterPanel::saveHeaderState );

    // Save has the same default key as the focus of the search input, the panel takes it first
    tree_->installEventFilter( this );

    connect( tree_, &QTreeWidget::itemChanged, this, &FilterPanel::onTreeChanged );
    connect( tree_, &QTreeWidget::itemDoubleClicked, this, &FilterPanel::editCurrent );
    connect( tree_, &QWidget::customContextMenuRequested, this, &FilterPanel::showContextMenu );
    connect( tree_, &QTreeWidget::itemSelectionChanged, this, &FilterPanel::updateActions );
    connect( tree_, &QTreeWidget::currentItemChanged, this, &FilterPanel::updateActions );
    connect( tree_->model(), &QAbstractItemModel::rowsInserted, this,
             &FilterPanel::onTreeChanged );
    connect( tree_->model(), &QAbstractItemModel::rowsRemoved, this,
             &FilterPanel::onTreeChanged );

    auto* toolBar = new FlowLayout;
    const auto addButton = [ this, toolBar ]( QAction* action ) {
        auto* button = new QToolButton( this );
        button->setAutoRaise( true );
        button->setDefaultAction( action );
        toolBar->addWidget( button );
        return button;
    };

    auto* addFilterButtonAction = new QAction( tr( "+ Filter" ), this );
    addFilterButtonAction->setToolTip( tr( "Add a filter" ) );
    connect( addFilterButtonAction, &QAction::triggered, this, &FilterPanel::addFilter );
    const auto textButtonHeight = addButton( addFilterButtonAction )->sizeHint().height();

    // Small and close together, as tall as the text buttons to stay aligned with them
    const auto addArrowButton = [ addButton, textButtonHeight ]( QAction* action ) {
        auto* button = addButton( action );
        button->setIconSize( QSize( 12, 12 ) );
        button->setStyleSheet( "QToolButton { padding: 0px 1px; }" );
        button->setFixedHeight( textButtonHeight );
        return button;
    };

    auto* addGroupAction = new QAction( tr( "+ Group" ), this );
    connect( addGroupAction, &QAction::triggered, this, &FilterPanel::addGroup );
    addButton( addGroupAction );

    // Text follows the selection, see updateActions()
    suspendAction_ = new QAction( tr( "Suspend" ), this );
    suspendAction_->setToolTip(
        tr( "Temporarily disable/enable the selected filters or groups without changing their "
            "check state" ) );
    connect( suspendAction_, &QAction::triggered, this, &FilterPanel::toggleSuspendSelected );

    auto* fileMenu = new QMenu( this );
    fileMenu->addAction( tr( "Open filters..." ), this, &FilterPanel::openFile );
    auto* recentMenu = fileMenu->addMenu( tr( "Recent filters" ) );
    connect( recentMenu, &QMenu::aboutToShow, this, [ this, recentMenu ] {
        recentMenu->clear();
        const auto recentFiles = settings().value( RecentFilesKey ).toStringList();
        for ( const auto& rememberedFile : recentFiles ) {
            const auto file = fromRememberedPath( rememberedFile );
            recentMenu->addAction( QDir::toNativeSeparators( file ), this, [ this, file ] {
                if ( confirmClose( {} ) ) {
                    loadFile( file );
                }
            } );
        }
        recentMenu->setEnabled( !recentFiles.isEmpty() );
    } );
    saveAction_ = fileMenu->addAction( tr( "Save filters" ), this, &FilterPanel::saveFile );
    fileMenu->addAction( tr( "Save filters as..." ), this, &FilterPanel::saveFileAs );
    fileMenu->addSeparator();
    fileMenu->addAction( tr( "Import TextAnalysisTool filters (.tat)..." ), this,
                         &FilterPanel::importTatFile );
    fileMenu->addAction( tr( "Import klogg predefined filters and highlighters..." ), this,
                         &FilterPanel::importKloggSettings );
    auto* fileAction = new QAction( tr( "File" ), this );
    fileAction->setMenu( fileMenu );
    connect( fileAction, &QAction::triggered, this, &FilterPanel::openFile );
    addButton( fileAction )->setPopupMode( QToolButton::MenuButtonPopup );

    // Moving across the edge of a group moves in or out of it
    moveUpAction_ = new QAction( tr( "Move up" ), this );
    moveUpAction_->setShortcutContext( Qt::WidgetShortcut );
    connect( moveUpAction_, &QAction::triggered, this, [ this ] { moveCurrent( true ); } );
    addArrowButton( moveUpAction_ );
    tree_->addAction( moveUpAction_ );

    moveDownAction_ = new QAction( tr( "Move down" ), this );
    moveDownAction_->setShortcutContext( Qt::WidgetShortcut );
    connect( moveDownAction_, &QAction::triggered, this, [ this ] { moveCurrent( false ); } );
    addArrowButton( moveDownAction_ );
    tree_->addAction( moveDownAction_ );

    sharedAction_ = new QAction( tr( "Shared" ), this );
    sharedAction_->setCheckable( true );
    sharedAction_->setToolTip(
        tr( "Use the filters shared by every tab in shared mode, in all the windows: a change "
            "made in one of them, saved or not, shows in all of them" ) );
    connect( sharedAction_, &QAction::triggered, this, [ this ]( bool checked ) {
        setSharedMode( checked );
        updateSharedAction();
    } );
    loadIcons();
    auto* sharedButton = addButton( sharedAction_ );
    sharedButton->setToolButtonStyle( Qt::ToolButtonTextBesideIcon );
    // As tall as the other text buttons
    sharedButton->setIconSize( QSize( 14, 14 ) );
    sharedButton->setFixedHeight( textButtonHeight );

    findNextAction_ = new QAction( tr( "Find next match of filter" ), this );
    findNextAction_->setShortcutContext( Qt::WindowShortcut );
    connect( findNextAction_, &QAction::triggered, this, [ this ] { requestFind( false ); } );
    addAction( findNextAction_ );

    findPreviousAction_ = new QAction( tr( "Find previous match of filter" ), this );
    findPreviousAction_->setShortcutContext( Qt::WindowShortcut );
    connect( findPreviousAction_, &QAction::triggered, this, [ this ] { requestFind( true ); } );
    addAction( findPreviousAction_ );

    deleteAction_ = new QAction( tr( "Delete" ), this );
    deleteAction_->setShortcutContext( Qt::WidgetShortcut );
    connect( deleteAction_, &QAction::triggered, this, &FilterPanel::deleteSelected );
    tree_->addAction( deleteAction_ );

    addFilterAction_ = new QAction( tr( "Add filter..." ), this );
    addFilterAction_->setShortcutContext( Qt::WidgetShortcut );
    connect( addFilterAction_, &QAction::triggered, this, &FilterPanel::addFilter );
    tree_->addAction( addFilterAction_ );

    editAction_ = new QAction( tr( "Edit..." ), this );
    editAction_->setShortcutContext( Qt::WidgetShortcut );
    connect( editAction_, &QAction::triggered, this, &FilterPanel::editCurrent );
    tree_->addAction( editAction_ );

    fileLabel_ = new ElidedLabel;
    fileLabel_->setTextInteractionFlags( Qt::TextSelectableByMouse );
    fileLabel_->setContentsMargins( 4, 0, 4, 0 );

    auto* layout = new QVBoxLayout;
    layout->setContentsMargins( 0, 0, 0, 0 );
    layout->setSpacing( 2 );
    layout->addLayout( toolBar );
    layout->addWidget( tree_ );
    layout->addWidget( fileLabel_ );
    setLayout( layout );

    updateShortcuts();
    updateActions();
}

void FilterPanel::updateShortcuts()
{
    const auto& shortcuts = Configuration::get().shortcuts();
    const auto setShortcuts = [ &shortcuts ]( QAction* action, const std::string& actionName ) {
        action->setShortcuts( ShortcutAction::shortcutKeys( actionName, shortcuts ) );
    };

    setShortcuts( findNextAction_, ShortcutAction::FilterPanelFindNext );
    setShortcuts( findPreviousAction_, ShortcutAction::FilterPanelFindPrevious );
    setShortcuts( addFilterAction_, ShortcutAction::FilterPanelAddFilter );
    setShortcuts( editAction_, ShortcutAction::FilterPanelEdit );
    setShortcuts( deleteAction_, ShortcutAction::FilterPanelDelete );
    setShortcuts( moveUpAction_, ShortcutAction::FilterPanelMoveUp );
    setShortcuts( moveDownAction_, ShortcutAction::FilterPanelMoveDown );

    saveKeys_ = ShortcutAction::shortcutKeys( ShortcutAction::FilterPanelSave, shortcuts );
    ShortcutAction::showInMenu( saveAction_, ShortcutAction::FilterPanelSave, shortcuts );

    // Tool tips of the tool bar buttons show the shortcut
    for ( auto* action : { moveUpAction_, moveDownAction_ } ) {
        const auto shortcut = action->shortcut().toString( QKeySequence::NativeText );
        action->setToolTip( shortcut.isEmpty() ? action->text()
                                               : QString( "%1 (%2)" ).arg( action->text(),
                                                                           shortcut ) );
    }
}

void FilterPanel::saveHeaderState()
{
    settings().setValue( HeaderStateKey, tree_->header()->saveState() );
}

void FilterPanel::showHeaderMenu( const QPoint& pos )
{
    auto* header = tree_->header();

    // In the order the columns are shown
    QMenu menu( this );
    for ( int visual = 0; visual < header->count(); ++visual ) {
        const auto column = header->logicalIndex( visual );
        auto* action = menu.addAction( tree_->headerItem()->text( column ) );
        action->setCheckable( true );
        action->setChecked( !header->isSectionHidden( column ) );
        // The filter column holds the tree and the check boxes
        action->setEnabled( column != PatternColumn );
        connect( action, &QAction::toggled, this, [ this, header, column ]( bool visible ) {
            header->setSectionHidden( column, !visible );
            saveHeaderState();
        } );
    }

    menu.exec( header->mapToGlobal( pos ) );
}

void FilterPanel::addFilterFromText( const QString& text )
{
    auto node = FilterNode::makeFilter( text );
    FilterEditDialog dialog( node, this );
    if ( dialog.exec() != QDialog::Accepted ) {
        return;
    }

    node = dialog.filter();
    insertItem( createItem( node ) );
}

void FilterPanel::addFilter()
{
    addFilterFromText( {} );
}

void FilterPanel::addGroup()
{
    bool ok = false;
    const auto name = QInputDialog::getText( this, tr( "New group" ), tr( "Group name:" ),
                                             QLineEdit::Normal, {}, &ok );
    if ( !ok || name.isEmpty() ) {
        return;
    }

    insertItem( createItem( FilterNode::makeGroup( name ) ) );
}

void FilterPanel::editCurrent()
{
    auto* item = tree_->currentItem();
    if ( !item ) {
        return;
    }

    auto& node = nodes_.at( item->data( PatternColumn, IdRole ).toString() );
    if ( node.isGroup() ) {
        bool ok = false;
        const auto name = QInputDialog::getText( this, tr( "Rename group" ), tr( "Group name:" ),
                                                 QLineEdit::Normal, node.name, &ok );
        if ( !ok || name.isEmpty() ) {
            return;
        }
        node.name = name;
    }
    else {
        FilterEditDialog dialog( node, this );
        if ( dialog.exec() != QDialog::Accepted ) {
            return;
        }
        node = dialog.filter();
    }

    updateItem( item );
    scheduleChange();
}

void FilterPanel::deleteSelected()
{
    auto selected = tree_->selectedItems();
    if ( selected.isEmpty() ) {
        return;
    }

    const auto answer = QMessageBox::question(
        this, tr( "Delete filters" ),
        tr( "Delete %n selected item(s)? Groups are deleted with their content.", "",
            static_cast<int>( selected.size() ) ) );
    if ( answer != QMessageBox::Yes ) {
        return;
    }

    // Children of a selected group are deleted with it
    selected.erase( std::remove_if( selected.begin(), selected.end(),
                                    [ &selected ]( const QTreeWidgetItem* item ) {
                                        for ( auto* parent = item->parent(); parent;
                                              parent = parent->parent() ) {
                                            if ( selected.contains( parent ) ) {
                                                return true;
                                            }
                                        }
                                        return false;
                                    } ),
                    selected.end() );

    for ( auto* item : selected ) {
        delete item;
    }
    scheduleChange();
}

void FilterPanel::toggleSuspendSelected()
{
    const auto selected = tree_->selectedItems();
    if ( selected.isEmpty() ) {
        return;
    }

    // Suspend all unless all are already suspended
    const auto suspend = !selectionSuspended();

    loading_ = true;
    for ( auto* item : selected ) {
        item->setData( PatternColumn, SuspendedRole, suspend );
    }
    loading_ = false;

    updateActions();
    scheduleChange();
}

// Resumes everything when anything is suspended, otherwise suspends everything
void FilterPanel::toggleSuspendAll()
{
    const auto suspend = !anySuspended();

    loading_ = true;
    for ( QTreeWidgetItemIterator it( tree_ ); *it; ++it ) {
        ( *it )->setData( PatternColumn, SuspendedRole, suspend );
    }
    loading_ = false;

    updateActions();
    scheduleChange();
}

bool FilterPanel::anySuspended() const
{
    for ( QTreeWidgetItemIterator it( tree_ ); *it; ++it ) {
        if ( ( *it )->data( PatternColumn, SuspendedRole ).toBool() ) {
            return true;
        }
    }
    return false;
}

void FilterPanel::setAllEnabled( bool enabled )
{
    loading_ = true;
    for ( QTreeWidgetItemIterator it( tree_ ); *it; ++it ) {
        if ( !isGroupItem( *it ) ) {
            ( *it )->setCheckState( PatternColumn, enabled ? Qt::Checked : Qt::Unchecked );
        }
    }
    loading_ = false;

    scheduleChange();
}

void FilterPanel::setSelectedEnabled( bool enabled )
{
    loading_ = true;
    for ( auto* selected : tree_->selectedItems() ) {
        for ( QTreeWidgetItemIterator it( selected ); *it; ++it ) {
            if ( *it != selected && !isSameOrChild( *it, selected ) ) {
                break;
            }
            if ( !isGroupItem( *it ) ) {
                ( *it )->setCheckState( PatternColumn, enabled ? Qt::Checked : Qt::Unchecked );
            }
        }
    }
    loading_ = false;

    scheduleChange();
}

bool FilterPanel::selectionSuspended() const
{
    const auto selected = tree_->selectedItems();
    return !selected.isEmpty()
           && std::all_of( selected.begin(), selected.end(), []( const QTreeWidgetItem* item ) {
                  return item->data( PatternColumn, SuspendedRole ).toBool();
              } );
}

void FilterPanel::updateActions()
{
    suspendAction_->setEnabled( !tree_->selectedItems().isEmpty() );
    suspendAction_->setText( selectionSuspended() ? tr( "Resume" ) : tr( "Suspend" ) );

    const auto hasCurrent = tree_->currentItem() != nullptr;
    moveUpAction_->setEnabled( hasCurrent );
    moveDownAction_->setEnabled( hasCurrent );
}

void FilterPanel::moveCurrent( bool up )
{
    auto* item = tree_->currentItem();
    if ( !item ) {
        return;
    }

    auto* root = tree_->invisibleRootItem();
    auto* parent = item->parent() ? item->parent() : root;
    const auto siblingIndex = parent->indexOfChild( item ) + ( up ? -1 : 1 );

    if ( siblingIndex < 0 || siblingIndex >= parent->childCount() ) {
        // At the edge of a group: out of it, right before or after the group
        if ( parent == root ) {
            return;
        }
        auto* grandParent = parent->parent() ? parent->parent() : root;
        moveItem( item, grandParent, grandParent->indexOfChild( parent ) + ( up ? 0 : 1 ) );
        return;
    }

    auto* sibling = parent->child( siblingIndex );
    if ( isGroupItem( sibling ) && ( sibling->isExpanded() || sibling->childCount() == 0 ) ) {
        // Into the open group next to the item, at its near edge
        moveItem( item, sibling, up ? sibling->childCount() : 0 );
        return;
    }

    moveItem( item, parent, siblingIndex );
}

void FilterPanel::moveItem( QTreeWidgetItem* item, QTreeWidgetItem* newParent, int index )
{
    // Taking an item out of the tree forgets which groups were expanded
    std::vector<QTreeWidgetItem*> expanded;
    for ( QTreeWidgetItemIterator it( item ); *it && isSameOrChild( *it, item ); ++it ) {
        if ( ( *it )->isExpanded() ) {
            expanded.push_back( *it );
        }
    }

    auto* oldParent = item->parent() ? item->parent() : tree_->invisibleRootItem();
    oldParent->takeChild( oldParent->indexOfChild( item ) );
    newParent->insertChild( index, item );

    for ( auto* group : expanded ) {
        group->setExpanded( true );
    }
    if ( newParent != tree_->invisibleRootItem() ) {
        newParent->setExpanded( true );
    }

    tree_->setCurrentItem( item );
    tree_->scrollToItem( item );
}

void FilterPanel::openFile()
{
    if ( !confirmClose( {} ) ) {
        return;
    }

    const auto path = QFileDialog::getOpenFileName(
        this, tr( "Open filters" ), QFileInfo( currentFile_ ).absolutePath(), tr( FileFilter ) );
    if ( !path.isEmpty() ) {
        loadFile( path );
    }
}

bool FilterPanel::saveFile()
{
    return currentFile_.isEmpty() ? saveFileAs() : saveCurrentFile();
}

bool FilterPanel::saveFileAs()
{
    auto path = QFileDialog::getSaveFileName( this, tr( "Save filters as" ), currentFile_,
                                              tr( FileFilter ) );
    if ( path.isEmpty() ) {
        return false;
    }

    if ( !path.endsWith( ".json" ) ) {
        path.append( ".flogg-filters.json" );
    }

    currentFile_ = path;
    sharedCopy_ = false;
    settings().setValue( CurrentFileKey, toRememberedPath( currentFile_ ) );
    addRecentFile( currentFile_ );
    // The file dialog already asked about replacing an existing file
    return saveCurrentFile( false );
}

void FilterPanel::importTatFile()
{
    const auto path = QFileDialog::getOpenFileName( this, tr( "Import TextAnalysisTool filters" ),
                                                    {}, tr( "TextAnalysisTool filters (*.tat)" ) );
    if ( path.isEmpty() ) {
        return;
    }

    QString error;
    const auto group = filters::importTatFile( path, &error );
    if ( !group ) {
        QMessageBox::warning( this, tr( "Import failed" ),
                              tr( "Can't import %1: %2" ).arg( path, error ) );
        return;
    }

    auto* item = createItem( *group );
    tree_->addTopLevelItem( item );
    item->setExpanded( true );
    tree_->setCurrentItem( item );
    scheduleChange();
}

void FilterPanel::importKloggSettings()
{
#ifdef Q_OS_WIN
    const auto format = QSettings::IniFormat;
#else
    const auto format = QSettings::NativeFormat;
#endif

    QSettings kloggSettings( format, QSettings::UserScope, "klogg", "klogg" );
    auto group = filters::importKloggSettings( kloggSettings );

    if ( group.children.empty() ) {
        // Portable klogg keeps its settings next to the executable
        const auto path = QFileDialog::getOpenFileName(
            this, tr( "No klogg settings found, select a klogg.conf file" ), {},
            tr( "klogg settings (*.conf)" ) );
        if ( path.isEmpty() ) {
            return;
        }

        QSettings portableSettings( path, QSettings::IniFormat );
        group = filters::importKloggSettings( portableSettings );
    }

    if ( group.children.empty() ) {
        QMessageBox::information( this, tr( "Import klogg settings" ),
                                  tr( "No predefined filters or highlighters were found." ) );
        return;
    }

    auto* item = createItem( group );
    tree_->addTopLevelItem( item );
    item->setExpanded( true );
    tree_->setCurrentItem( item );
    scheduleChange();
}

void FilterPanel::showContextMenu( const QPoint& pos )
{
    QMenu menu( this );
    const auto hasSelection = !tree_->selectedItems().isEmpty();

    const auto* clickedItem = tree_->itemAt( pos );

    menu.addAction( addFilterAction_ );
    menu.addAction( tr( "Add group..." ), this, &FilterPanel::addGroup );
    if ( clickedItem ) {
        menu.addAction( editAction_ );
        menu.addAction( deleteAction_ );
        menu.addAction( moveUpAction_ );
        menu.addAction( moveDownAction_ );
    }
    menu.addSeparator();

    // Groups have no check box: enabling a group checks all the filters it contains
    if ( hasSelection ) {
        const auto onGroup = clickedItem && isGroupItem( clickedItem );
        menu.addAction( onGroup ? tr( "Enable in group" ) : tr( "Enable" ), this,
                        [ this ] { setSelectedEnabled( true ); } );
        menu.addAction( onGroup ? tr( "Disable in group" ) : tr( "Disable" ), this,
                        [ this ] { setSelectedEnabled( false ); } );
    }
    menu.addAction( tr( "Enable all" ), this, [ this ] { setAllEnabled( true ); } );
    menu.addAction( tr( "Disable all" ), this, [ this ] { setAllEnabled( false ); } );
    menu.addSeparator();

    menu.addAction( suspendAction_ );
    menu.addAction( anySuspended() ? tr( "Resume all" ) : tr( "Suspend all" ), this,
                    &FilterPanel::toggleSuspendAll );
    menu.addSeparator();

    menu.addAction( tr( "Expand all" ), tree_, &QTreeView::expandAll );
    menu.addAction( tr( "Collapse all" ), tree_, &QTreeView::collapseAll );
    menu.addSeparator();

    menu.addAction( tr( "Add to search" ), this,
                    [ this ] { requestSearchFragment( FilterSearchMode::Add ); } )
        ->setEnabled( hasSelection );
    menu.addAction( tr( "Exclude from search" ), this,
                    [ this ] { requestSearchFragment( FilterSearchMode::Exclude ); } )
        ->setEnabled( hasSelection );
    menu.addAction( tr( "Replace search" ), this,
                    [ this ] { requestSearchFragment( FilterSearchMode::Replace ); } )
        ->setEnabled( hasSelection );
    menu.addSeparator();

    menu.addAction( findNextAction_ );
    menu.addAction( findPreviousAction_ );

    menu.exec( tree_->viewport()->mapToGlobal( pos ) );
}

void FilterPanel::requestSearchFragment( FilterSearchMode mode )
{
    const auto filters = selectedFiltersForSearch();
    if ( !filters.empty() ) {
        Q_EMIT searchFragmentRequested( filters, mode );
    }
}

void FilterPanel::requestFind( bool backward )
{
    const auto filters = selectedFiltersForSearch();
    if ( !filters.empty() ) {
        Q_EMIT findRequested( filters, backward );
    }
}

std::vector<FilterNode> FilterPanel::selectedFiltersForSearch() const
{
    std::vector<FilterNode> result;
    auto selected = tree_->selectedItems();
    if ( selected.isEmpty() && tree_->currentItem() ) {
        selected.append( tree_->currentItem() );
    }

    // Keep tree order
    std::sort( selected.begin(), selected.end(),
               [ this ]( QTreeWidgetItem* lhs, QTreeWidgetItem* rhs ) {
                   return tree_->indexFromItem( lhs ) < tree_->indexFromItem( rhs );
               } );

    for ( const auto* item : selected ) {
        const auto node = buildNode( item );
        for ( const auto* filter : filters::filtersForSearch( node ) ) {
            const auto alreadyAdded
                = std::any_of( result.begin(), result.end(),
                               [ filter ]( const FilterNode& f ) { return f.id == filter->id; } );
            if ( !alreadyAdded ) {
                result.push_back( *filter );
            }
        }
    }

    return result;
}

void FilterPanel::onTreeChanged()
{
    if ( loading_ ) {
        return;
    }

    scheduleChange();
}

void FilterPanel::scheduleChange()
{
    changeTimer_.start();
}

void FilterPanel::flushChange()
{
    if ( changeTimer_.isActive() ) {
        changeTimer_.stop();
        publishChange();
    }
}

void FilterPanel::updateItems()
{
    loading_ = true;
    for ( int i = 0; i < tree_->topLevelItemCount(); ++i ) {
        updateSuspendedLook( tree_->topLevelItem( i ), false );
    }
    numberFilters();
    loading_ = false;
}

void FilterPanel::updateLook()
{
    updateItems();
    updateFileLabel();
}

void FilterPanel::publishChange()
{
    // Before building the tree: its nodes carry the numbers the tool tips of the views show
    updateItems();

    const auto root = buildTree();
    treeJson_ = serialize( root );
    if ( treeJson_ == savedJson_ ) {
        // Unchanged filters share the saved copy instead of holding their own
        treeJson_ = savedJson_;
    }
    else if ( shared_ && sharedCopy_ ) {
        detachSharedCopy();
    }

    updateFileLabel();

    auto snapshot = std::make_shared<FilterSnapshot>();
    snapshot->revision = nextRevision();
    for ( const auto* filter : filters::effectiveFilters( root ) ) {
        snapshot->filters.push_back( *filter );
    }

    std::vector<const FilterNode*> effective;
    effective.reserve( snapshot->filters.size() );
    for ( const auto& filter : snapshot->filters ) {
        effective.push_back( &filter );
    }

    if ( !effective.empty() ) {
        snapshot->searchPattern = filters::buildSearchPattern( effective );
    }
    const auto highlighters
        = std::make_shared<HighlighterSet>( filters::buildHighlighterSet( effective ) );
    snapshot->highlighters = highlighters;

    snapshot_ = std::move( snapshot );

    // Compiling hundreds of highlighters takes tens of ms, the views keep drawing with the
    // previous ones meanwhile instead of compiling them on the UI thread when they paint
    if ( !highlighters->isEmpty() ) {
        auto* watcher = new QFutureWatcher<void>( this );
        connect( watcher, &QFutureWatcher<void>::finished, this,
                 [ this, watcher, compiled = highlighters.get() ] {
                     watcher->deleteLater();
                     Q_EMIT highlightersCompiled();
                     // The other panels showing the shared filters use the same highlighters
                     SharedFilters::instance().notifyHighlightersCompiled( compiled );
                 } );
        watcher->setFuture( QtConcurrent::run( [ highlighters ] { highlighters->compile(); } ) );
    }

    if ( shared_ ) {
        pushShared( root );
    }

    Q_EMIT filtersChanged();
}

FilterTabState FilterPanel::tabState()
{
    flushChange();

    FilterTabState state;
    if ( shared_ ) {
        // The shared filters stay in SharedFilters
        state = privateState_;
        state.shared = true;
        state.collapsedGroups.clear();
        state.selectedItems.clear();
        state.currentItem.clear();
    }
    else {
        state.file = currentFile_;
        state.root = buildTree();
        state.savedJson = savedJson_;
        state.json = treeJson_;
        state.snapshot = snapshot_;
        state.fileChanged = fileChanged_;
    }
    state.initialized = true;
    state.hits = hits_;
    saveTreeView( state );
    return state;
}

void FilterPanel::saveTreeView( FilterTabState& state ) const
{
    for ( QTreeWidgetItemIterator it( tree_ ); *it; ++it ) {
        const auto id = ( *it )->data( PatternColumn, IdRole ).toString();
        if ( isGroupItem( *it ) && !( *it )->isExpanded() ) {
            state.collapsedGroups.append( id );
        }
        if ( ( *it )->isSelected() ) {
            state.selectedItems.append( id );
        }
    }
    if ( tree_->currentItem() ) {
        state.currentItem = tree_->currentItem()->data( PatternColumn, IdRole ).toString();
    }
    state.scrollPosition = tree_->verticalScrollBar()->value();
}

void FilterPanel::restoreTreeView( const FilterTabState& state )
{
    const auto wasLoading = loading_;
    loading_ = true;

    tree_->clearSelection();
    for ( QTreeWidgetItemIterator it( tree_ ); *it; ++it ) {
        const auto id = ( *it )->data( PatternColumn, IdRole ).toString();
        if ( state.collapsedGroups.contains( id ) ) {
            ( *it )->setExpanded( false );
        }
        if ( id == state.currentItem ) {
            tree_->setCurrentItem( *it, PatternColumn, QItemSelectionModel::NoUpdate );
        }
        if ( state.selectedItems.contains( id ) ) {
            ( *it )->setSelected( true );
        }
    }
    // The scroll range follows the collapsed groups only once the items are laid out
    tree_->doItemsLayout();
    tree_->verticalScrollBar()->setValue( state.scrollPosition );

    loading_ = wasLoading;
}

void FilterPanel::setTabState( const FilterTabState& state )
{
    changeTimer_.stop();

    // Shown while the tree is built, until the tab gets its own counts
    hits_ = state.hits;

    if ( state.shared && state.initialized ) {
        auto own = state;
        own.shared = false;
        if ( !SharedFilters::instance().isInitialized() ) {
            // Nothing to share yet: the filters of the tab start the shared ones
            setTabState( own );
            shared_ = true;
            setPrivateState( std::move( own ) );
            updateSharedAction();
            seedShared();
            return;
        }

        shared_ = true;
        setPrivateState( std::move( own ) );
        updateSharedAction();
        showSharedFilters( state );
        if ( !snapshot_ ) {
            publishChange();
        }
        return;
    }

    shared_ = false;
    sharedCopy_ = false;
    updateSharedAction();
    privateState_ = {};

    if ( !state.initialized ) {
        const auto file = state.file.isEmpty() ? defaultFile() : state.file;
        if ( !loadFile( file, false ) ) {
            // The filters of the previous tab must not stay
            currentFile_ = defaultFile();
            savedJson_ = serialize( FilterNode::makeGroup( "root" ) );
            setRoot( FilterNode::makeGroup( "root" ) );
            publishChange();
        }
        return;
    }

    currentFile_ = state.file;
    savedJson_ = state.savedJson;
    treeJson_ = state.json.isEmpty() ? serialize( state.root ) : state.json;
    fileChanged_ = state.fileChanged;
    setRoot( state.root );
    restoreTreeView( state );

    if ( state.snapshot && !shared_ ) {
        // The tab already has these filters applied, setRoot gave the items their look
        snapshot_ = state.snapshot;
        updateFileLabel();
    }
    else {
        publishChange();
    }
}

FilterTabState FilterPanel::restoredTabState( const QString& file,
                                              const std::optional<FilterNode>& root )
{
    FilterTabState state;
    state.initialized = true;
    state.file = file;

    const auto saved = ( QFileInfo::exists( file ) ? filters::loadFromFile( file )
                                                   : FilterNode::makeGroup( "root" ) )
                           .value_or( FilterNode::makeGroup( "root" ) );
    state.root = root.value_or( saved );
    state.savedJson = serialize( saved );
    return state;
}

bool FilterPanel::isModified( const FilterTabState& state )
{
    return state.initialized
           && ( state.json.isEmpty() ? serialize( state.root ) : state.json ) != state.savedJson;
}

bool FilterPanel::isModified() const
{
    // The tree as last published, unless a change is still pending
    return ( changeTimer_.isActive() ? serialize( buildTree() ) : treeJson_ ) != savedJson_;
}

bool FilterPanel::confirmClose( const QString& tabName )
{
    flushChange();
    if ( !isModified() ) {
        return true;
    }

    const auto fileName
        = currentFile_.isEmpty() ? tr( "untitled" ) : QFileInfo( currentFile_ ).fileName();
    const auto question
        = tabName.isEmpty()
              ? tr( "The filters of %1 have changed. Save them?" ).arg( fileName )
              : tr( "The filters of %1 (used by %2) have changed. Save them?" )
                    .arg( fileName, tabName );

    const auto answer = QMessageBox::question(
        this, tr( "Unsaved filters" ), question,
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save );

    if ( answer == QMessageBox::Save ) {
        return saveFile();
    }
    if ( answer == QMessageBox::Discard ) {
        discardChanges();
        return true;
    }
    return false;
}

void FilterPanel::reloadSettings()
{
    restoreHeaderState();
    updateShortcuts();
}

void FilterPanel::restoreHeaderState()
{
    auto* header = tree_->header();
    const auto headerState = settings().value( HeaderStateKey ).toByteArray();
    // A state saved before the number column existed would hide it or put it last
    if ( !headerState.isEmpty() && header->restoreState( headerState )
         && header->visualIndex( NumberColumn ) != 0 ) {
        header->moveSection( header->visualIndex( NumberColumn ), 0 );
        header->setSectionHidden( NumberColumn, false );
        header->resizeSection( NumberColumn, 32 );
    }
}

void FilterPanel::loadIcons()
{
    moveUpAction_->setIcon( arrowIcon( true, palette() ) );
    moveDownAction_->setIcon( arrowIcon( false, palette() ) );
    sharedAction_->setIcon(
        sharedFiltersIcon( palette().color( QPalette::Active, QPalette::Highlight ) ) );
}

void FilterPanel::changeEvent( QEvent* event )
{
    // fileLabel_ is the last widget made by setupUi
    if ( event->type() == QEvent::PaletteChange && fileLabel_ ) {
        loadIcons();
        // Suspended filters are drawn with the disabled text colour
        updateLook();
    }

    QWidget::changeEvent( event );
}

bool FilterPanel::eventFilter( QObject* watched, QEvent* event )
{
    if ( watched == tree_
         && ( event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress ) ) {
        const auto* keyEvent = static_cast<QKeyEvent*>( event );
        if ( saveKeys_.contains( QKeySequence( keyEvent->keyCombination() ) ) ) {
            event->accept();
            if ( event->type() == QEvent::KeyPress ) {
                saveFile();
            }
            return true;
        }
    }

    return QWidget::eventFilter( watched, event );
}

void FilterPanel::numberFilters()
{
    numberFilters( topLevelItems() );
}

void FilterPanel::numberFilters( const QList<QTreeWidgetItem*>& items )
{
    int number = 0;
    const std::function<void( QTreeWidgetItem* )> numberItem = [ & ]( QTreeWidgetItem* item ) {
        auto& node = nodes_.at( item->data( PatternColumn, IdRole ).toString() );
        if ( !node.isGroup() ) {
            node.number = ++number;
            updateNumberToolTip( item, node );
            item->setText( NumberColumn, QString::number( node.number ) );
            item->setTextAlignment( NumberColumn, Qt::AlignRight | Qt::AlignVCenter );
        }
        for ( int i = 0; i < item->childCount(); ++i ) {
            numberItem( item->child( i ) );
        }
    };
    for ( auto* item : items ) {
        numberItem( item );
    }
}

QList<QTreeWidgetItem*> FilterPanel::topLevelItems() const
{
    QList<QTreeWidgetItem*> items;
    items.reserve( tree_->topLevelItemCount() );
    for ( int i = 0; i < tree_->topLevelItemCount(); ++i ) {
        items.append( tree_->topLevelItem( i ) );
    }
    return items;
}

void FilterPanel::updateNumberToolTip( QTreeWidgetItem* item, const FilterNode& node )
{
    const auto properties = item->data( PatternColumn, PropertiesRole ).toString();
    item->setToolTip( PatternColumn, node.number > 0
                                         ? filters::numberText( node ) + "  " + properties
                                         : properties );
}

void FilterPanel::setRoot( const FilterNode& root )
{
    loading_ = true;

    tree_->clear();
    nodes_.clear();

    // The items get their whole look before they are added to the tree: each change of an
    // item shown in the tree is a model change with its view update (and accessibility
    // event), too slow for hundreds of filters on each tab switch
    QList<QTreeWidgetItem*> items;
    items.reserve( static_cast<int>( root.children.size() ) );
    for ( const auto& child : root.children ) {
        items.append( createItem( child ) );
    }
    for ( auto* item : items ) {
        updateSuspendedLook( item, false );
    }
    numberFilters( items );
    updateHitCounts( items );

    tree_->insertTopLevelItems( 0, items );
    tree_->expandAll();

    loading_ = false;
}

FilterNode FilterPanel::buildTree() const
{
    auto root = FilterNode::makeGroup( "root" );
    for ( int i = 0; i < tree_->topLevelItemCount(); ++i ) {
        root.children.push_back( buildNode( tree_->topLevelItem( i ) ) );
    }
    return root;
}

FilterNode FilterPanel::buildNode( const QTreeWidgetItem* item ) const
{
    auto node = nodes_.at( item->data( PatternColumn, IdRole ).toString() );
    node.suspended = item->data( PatternColumn, SuspendedRole ).toBool();
    // Groups have no check box of their own
    node.enabled = isGroupItem( item ) || item->checkState( PatternColumn ) != Qt::Unchecked;

    for ( int i = 0; i < item->childCount(); ++i ) {
        node.children.push_back( buildNode( item->child( i ) ) );
    }

    return node;
}

QTreeWidgetItem* FilterPanel::createItem( const FilterNode& node )
{
    auto* item = new QTreeWidgetItem;

    auto properties = node;
    properties.children.clear();
    nodes_[ node.id ] = properties;

    item->setData( PatternColumn, IdRole, node.id );
    item->setData( PatternColumn, SuspendedRole, node.suspended );

    if ( node.isGroup() ) {
        // No check box, to avoid switching a whole group by mistake: groups are
        // enabled or disabled from the context menu
        item->setFlags( Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsDragEnabled
                        | Qt::ItemIsDropEnabled );
        // Expand arrow even when empty, so that a new group does not look like a filter
        item->setChildIndicatorPolicy( QTreeWidgetItem::ShowIndicator );

        for ( const auto& child : node.children ) {
            item->addChild( createItem( child ) );
        }
    }
    else {
        item->setFlags( Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable
                        | Qt::ItemIsDragEnabled | Qt::ItemNeverHasChildren );
        item->setCheckState( PatternColumn, node.enabled ? Qt::Checked : Qt::Unchecked );
    }

    updateItem( item );
    return item;
}

void FilterPanel::updateItem( QTreeWidgetItem* item )
{
    const auto wasLoading = loading_;
    loading_ = true;

    const auto& node = nodes_.at( item->data( PatternColumn, IdRole ).toString() );

    if ( node.isGroup() ) {
        item->setText( PatternColumn, node.name );
        item->setIcon( PatternColumn, groupIcon_ );
        auto font = item->font( PatternColumn );
        font.setBold( true );
        item->setFont( PatternColumn, font );
    }
    else {
        item->setText( PatternColumn, ( node.exclude ? "¬ " : "" ) + node.pattern );
        // Only the first line of a long description, all of it in the tool tip
        const auto description = node.name.trimmed();
        const auto firstLine = description.section( '\n', 0, 0 ).trimmed();
        item->setText( DescriptionColumn, firstLine.size() < description.size()
                                              ? firstLine + QStringLiteral( " \u2026" )
                                              : description );
        item->setToolTip( DescriptionColumn, description );

        // Same check as the search: such patterns are verified line by line with Qt regex
        auto isSlow = false;
        if ( node.useRegex ) {
            const auto key = ( node.caseSensitive ? QStringLiteral( "1" ) : QStringLiteral( "0" ) )
                             + node.pattern;
            auto accelerated = acceleratedPatterns_.find( key );
            if ( accelerated == acceleratedPatterns_.end() ) {
                const MultiRegularExpression expression{ { filters::toPattern( node ) } };
                accelerated = acceleratedPatterns_.insert( key, expression.isFullyAccelerated() );
            }
            isSlow = !*accelerated;
        }
        item->setIcon( PatternColumn, isSlow ? slowIcon_ : QIcon{} );

        QStringList properties;
        properties.append( node.useRegex ? tr( "regex" ) : tr( "plain text" ) );
        properties.append( node.caseSensitive ? tr( "case sensitive" )
                                              : tr( "case insensitive" ) );
        if ( node.exclude ) {
            properties.append( tr( "exclude" ) );
        }
        if ( node.colorMode == FilterNode::ColorMode::MatchOnly ) {
            properties.append( tr( "color matched text only" ) );
        }
        auto toolTip = properties.join( ", " );
        if ( isSlow ) {
            toolTip.append( "\n" ).append(
                tr( "This regular expression is not supported by the fast regex engine "
                    "(e.g. back references or look-arounds), searching it is slower." ) );
        }
        item->setData( PatternColumn, PropertiesRole, toolTip );
        updateNumberToolTip( item, node );
    }

    loading_ = wasLoading;
}

void FilterPanel::updateSuspendedLook( QTreeWidgetItem* item, bool parentSuspended )
{
    const auto& node = nodes_.at( item->data( PatternColumn, IdRole ).toString() );
    const auto ownSuspended = item->data( PatternColumn, SuspendedRole ).toBool();
    const auto suspended = ownSuspended || parentSuspended;

    auto font = item->font( PatternColumn );
    font.setStrikeOut( ownSuspended );
    item->setFont( PatternColumn, font );

    if ( suspended ) {
        const auto disabledText = palette().color( QPalette::Disabled, QPalette::Text );
        item->setForeground( PatternColumn, disabledText );
        item->setForeground( DescriptionColumn, disabledText );
        item->setBackground( PatternColumn, QBrush{} );
    }
    else {
        item->setForeground( DescriptionColumn, QBrush{} );
        const auto foreColor = filters::displayForeColor( node );
        item->setForeground( PatternColumn, foreColor.isValid() ? QBrush( foreColor ) : QBrush{} );
        item->setBackground( PatternColumn,
                             node.backColor.isValid() ? QBrush( node.backColor ) : QBrush{} );
    }

    for ( int i = 0; i < item->childCount(); ++i ) {
        updateSuspendedLook( item->child( i ), suspended );
    }
}

QTreeWidgetItem* FilterPanel::insertionParent() const
{
    auto* current = tree_->currentItem();
    if ( !current ) {
        return nullptr;
    }

    const auto& node = nodes_.at( current->data( PatternColumn, IdRole ).toString() );
    return node.isGroup() ? current : current->parent();
}

void FilterPanel::insertItem( QTreeWidgetItem* item )
{
    auto* current = tree_->currentItem();
    auto* parent = insertionParent();

    if ( parent && parent == current ) {
        parent->addChild( item );
        parent->setExpanded( true );
    }
    else {
        // Insert right after the current filter
        auto* container = parent ? parent : tree_->invisibleRootItem();
        const auto index = current ? container->indexOfChild( current ) + 1
                                   : container->childCount();
        container->insertChild( index, item );
    }

    tree_->setCurrentItem( item );
    scheduleChange();
}

bool FilterPanel::loadFile( const QString& path, bool remember )
{
    QString error;
    auto root = QFileInfo::exists( path ) ? filters::loadFromFile( path, &error )
                                          : std::make_optional( FilterNode::makeGroup( "root" ) );

    if ( !root ) {
        LOG_ERROR << "Failed to load filters from " << path.toStdString() << ": "
                  << error.toStdString();
        QMessageBox::warning( this, tr( "Open filters" ),
                              tr( "Can't load filters from %1: %2" ).arg( path, error ) );
        return false;
    }

    currentFile_ = path;
    if ( remember ) {
        settings().setValue( CurrentFileKey, toRememberedPath( currentFile_ ) );
        addRecentFile( currentFile_ );
    }

    savedJson_ = serialize( *root );
    fileChanged_ = false;
    // Opened on purpose, also as shared filters
    sharedCopy_ = false;
    setRoot( *root );

    // Publish right away, file loading should not wait for the debounce timer
    changeTimer_.stop();
    publishChange();
    return true;
}

bool FilterPanel::saveCurrentFile( bool checkFile )
{
    if ( currentFile_.isEmpty() ) {
        return false;
    }

    // Changed since it was loaded or saved here: by another tab with unsaved changes, or by
    // another program
    if ( checkFile && QFileInfo::exists( currentFile_ ) ) {
        const auto onDisk = filters::loadFromFile( currentFile_ );
        if ( !onDisk || serialize( *onDisk ) != savedJson_ ) {
            QMessageBox question( QMessageBox::Warning, tr( "Save filters" ),
                                  tr( "%1 was changed since these filters were loaded, by another "
                                      "tab or program. Overwrite it with the filters shown here, "
                                      "or reload it and drop the changes made here?" )
                                      .arg( QDir::toNativeSeparators( currentFile_ ) ),
                                  QMessageBox::Cancel, this );
            auto* overwrite = question.addButton( tr( "Overwrite" ), QMessageBox::AcceptRole );
            auto* reload = question.addButton( tr( "Reload" ), QMessageBox::DestructiveRole );
            question.setDefaultButton( QMessageBox::Cancel );
            question.exec();
            if ( question.clickedButton() == reload ) {
                loadFile( currentFile_, false );
                return false;
            }
            if ( question.clickedButton() != overwrite ) {
                return false;
            }
        }
    }

    const auto root = buildTree();
    QString error;
    if ( !filters::saveToFile( root, currentFile_, &error ) ) {
        LOG_ERROR << "Failed to save filters to " << currentFile_.toStdString() << ": "
                  << error.toStdString();
        QMessageBox::warning( this, tr( "Save filters" ),
                              tr( "Can't save filters to %1: %2" ).arg( currentFile_, error ) );
        return false;
    }

    savedJson_ = serialize( root );
    treeJson_ = savedJson_;
    fileChanged_ = false;
    if ( shared_ ) {
        pushShared( root );
    }
    updateFileLabel();
    FilterFiles::instance().notifySaved( currentFile_, savedJson_, this );
    return true;
}

void FilterPanel::setHitCounts( const FilterHits& hits )
{
    hits_ = hits;
    updateHitCounts();
}

void FilterPanel::updateHitCounts()
{
    updateHitCounts( topLevelItems() );
}

void FilterPanel::updateHitCounts( const QList<QTreeWidgetItem*>& items )
{
    const auto wasLoading = loading_;
    loading_ = true;

    // Groups show the sum of the hits of their enabled filters, blank if none has a count
    const std::function<std::optional<qint64>( QTreeWidgetItem* )> update
        = [ & ]( QTreeWidgetItem* item ) -> std::optional<qint64> {
        std::optional<qint64> count;
        if ( isGroupItem( item ) ) {
            for ( int i = 0; i < item->childCount(); ++i ) {
                const auto childCount = update( item->child( i ) );
                if ( childCount ) {
                    count = count.value_or( 0 ) + *childCount;
                }
            }
        }
        else {
            const auto hits = hits_.find( item->data( PatternColumn, IdRole ).toString() );
            if ( hits != hits_.end() && item->checkState( PatternColumn ) != Qt::Unchecked ) {
                count = *hits;
            }
        }

        item->setText( HitsColumn, count ? QString::number( *count ) : QString{} );
        item->setTextAlignment( HitsColumn, Qt::AlignRight | Qt::AlignVCenter );
        return count;
    };

    for ( auto* item : items ) {
        update( item );
    }

    loading_ = wasLoading;
}

void FilterPanel::addRecentFile( const QString& path )
{
    const auto rememberedPath = toRememberedPath( path );
    auto recentFiles = settings().value( RecentFilesKey ).toStringList();
    recentFiles.removeAll( rememberedPath );
    recentFiles.prepend( rememberedPath );
    while ( recentFiles.size() > MaxRecentFiles ) {
        recentFiles.removeLast();
    }
    settings().setValue( RecentFilesKey, recentFiles );
}

void FilterPanel::updateFileLabel()
{
    auto fileName
        = currentFile_.isEmpty() ? tr( "untitled" ) : QFileInfo( currentFile_ ).fileName();
    if ( isModified() ) {
        fileName += " *";
    }
    if ( fileChanged_ ) {
        fileName += tr( " (saved by another tab)" );
    }
    auto toolTip = QDir::toNativeSeparators( currentFile_ );
    if ( shared_ ) {
        fileName = tr( "Shared: %1" ).arg( fileName );
        if ( isModified( privateState_ ) ) {
            const auto ownName = privateState_.file.isEmpty()
                                     ? tr( "untitled" )
                                     : QFileInfo( privateState_.file ).fileName();
            fileName += tr( "  \u00b7  own: %1 *" ).arg( ownName );
            toolTip += tr( "\nThe own filters of this tab (%1) have unsaved changes, they come "
                           "back when it leaves the shared mode" )
                           .arg( ownName );
        }
    }
    static_cast<ElidedLabel*>( fileLabel_ )->setFullText( fileName );
    fileLabel_->setToolTip( toolTip );
}

void FilterPanel::setPrivateState( FilterTabState state )
{
    // Serialized once, the file label compares it on every change
    if ( state.json.isEmpty() ) {
        state.json = serialize( state.root );
    }
    privateState_ = std::move( state );
    updateFileLabel();
}

bool FilterPanel::saveState( FilterTabState& state, QWidget* parent, const QString& title )
{
    auto path = state.file;
    if ( path.isEmpty() ) {
        path = QFileDialog::getSaveFileName( parent, title, {}, tr( FileFilter ) );
        if ( path.isEmpty() ) {
            return false;
        }
        if ( !path.endsWith( ".json" ) ) {
            path.append( ".flogg-filters.json" );
        }
        addRecentFile( path );
    }
    // Changed since it was loaded or saved, by another tab or program
    else if ( QFileInfo::exists( path ) ) {
        const auto onDisk = filters::loadFromFile( path );
        if ( ( !onDisk || serialize( *onDisk ) != state.savedJson )
             && QMessageBox::warning(
                    parent, tr( "Save filters" ),
                    tr( "%1 was changed since these filters were loaded, by another tab or "
                        "program. Overwrite it?" )
                        .arg( QDir::toNativeSeparators( path ) ),
                    QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel )
                    != QMessageBox::Save ) {
            return false;
        }
    }

    QString error;
    if ( !filters::saveToFile( state.root, path, &error ) ) {
        LOG_ERROR << "Failed to save filters to " << path.toStdString() << ": "
                  << error.toStdString();
        QMessageBox::warning( parent, tr( "Save filters" ),
                              tr( "Can't save filters to %1: %2" ).arg( path, error ) );
        return false;
    }

    state.file = path;
    state.savedJson = serialize( state.root );
    state.json = state.savedJson;
    state.fileChanged = false;
    state.copyOfFile = false;
    return true;
}

void FilterPanel::discardChanges( FilterTabState& state )
{
    state.root = filters::fromJson( QJsonDocument::fromJson( state.savedJson ).object() );
    state.json = state.savedJson;
    state.snapshot.reset();
}

void FilterPanel::discardChanges()
{
    setRoot( filters::fromJson( QJsonDocument::fromJson( savedJson_ ).object() ) );
    publishChange();
}

void FilterPanel::discardPrivateChanges()
{
    auto state = privateState_;
    discardChanges( state );
    setPrivateState( std::move( state ) );
}

bool FilterPanel::savePrivateFilters( const QString& tabName )
{
    auto state = privateState_;
    if ( !saveState( state, this, tr( "Save filters of %1 as" ).arg( tabName ) ) ) {
        return false;
    }
    setPrivateState( std::move( state ) );
    FilterFiles::instance().notifySaved( privateState_.file, privateState_.savedJson, this );
    return true;
}

void FilterPanel::setTab( QWidget* tab )
{
    // Without a tab the panel itself uses the shared filters, the first tab takes over
    if ( tab && !tab_ ) {
        auto& sharedFilters = SharedFilters::instance();
        if ( shared_ ) {
            sharedFilters.setUser( tab, true );
        }
        sharedFilters.setUser( this, false );
    }
    tab_ = tab;
}

QWidget* FilterPanel::sharedUser()
{
    return tab_ ? tab_.data() : this;
}

bool FilterPanel::setSharedMode( bool shared )
{
    flushChange();
    if ( shared == shared_ ) {
        return true;
    }

    auto& sharedFilters = SharedFilters::instance();
    if ( shared ) {
        // The own filters of the tab are kept aside as they are, also unsaved changes: the
        // shared filters started here take them too (untitled), the tab gets them back when it
        // leaves. They are asked about when the tab is closed.
        auto own = tabState();
        shared_ = true;
        setPrivateState( std::move( own ) );
        if ( sharedFilters.isInitialized() ) {
            showSharedFilters( privateState_ );
            Q_EMIT filtersChanged();
        }
        else {
            seedShared();
        }
    }
    else {
        // The tab using the shared filters last is asked to save them
        if ( !sharedFilters.hasOtherUsers( sharedUser() ) && !confirmClose( {} ) ) {
            return false;
        }

        auto own = privateState_;
        own.shared = false;
        own.hits = hits_;
        saveTreeView( own );
        setTabState( own );
        Q_EMIT filtersChanged();
    }

    sharedFilters.setUser( sharedUser(), shared_ );
    updateSharedAction();
    updateFileLabel();
    return true;
}

void FilterPanel::seedShared()
{
    // The tab keeps its own copy of these filters, the shared ones leave its file when they
    // change. Changes not saved yet (restored session) leave it right away.
    sharedCopy_ = true;
    if ( isModified() ) {
        detachSharedCopy();
    }
    pushShared( buildTree() );
    updateFileLabel();
}

void FilterPanel::detachSharedCopy()
{
    // Untitled: saving the shared filters asks for a file
    sharedCopy_ = false;
    currentFile_.clear();
    fileChanged_ = false;
    savedJson_ = serialize( FilterNode::makeGroup( "root" ) );
}

void FilterPanel::pushShared( const FilterNode& root )
{
    FilterTabState state;
    state.file = currentFile_;
    state.root = root;
    state.savedJson = savedJson_;
    state.json = treeJson_;
    state.snapshot = snapshot_;
    state.fileChanged = fileChanged_;
    state.copyOfFile = sharedCopy_;
    SharedFilters::instance().setState( std::move( state ), this );
}

void FilterPanel::showSharedFilters( const FilterTabState& view )
{
    // A change still waiting here would be lost
    flushChange();

    const auto& shared = SharedFilters::instance().state();
    currentFile_ = shared.file;
    savedJson_ = shared.savedJson;
    treeJson_ = shared.json.isEmpty() ? serialize( shared.root ) : shared.json;
    fileChanged_ = shared.fileChanged;
    sharedCopy_ = shared.copyOfFile;
    setRoot( shared.root );
    restoreTreeView( view );
    snapshot_ = shared.snapshot;
    updateFileLabel();
}

void FilterPanel::updateSharedAction()
{
    const QSignalBlocker blocker( sharedAction_ );
    sharedAction_->setChecked( shared_ );
}

bool FilterPanel::openFiltersFile( const QString& path, bool confirm )
{
    return ( !confirm || confirmClose( {} ) ) && loadFile( path, false );
}

void FilterPanel::appendFilters( const std::vector<FilterNode>& filters )
{
    flushChange();
    for ( const auto& filter : filters ) {
        tree_->addTopLevelItem( createItem( filter ) );
    }
    changeTimer_.stop();
    publishChange();
}
