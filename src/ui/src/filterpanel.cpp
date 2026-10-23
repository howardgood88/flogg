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
#include <utility>

#include <QAction>
#include <QCursor>
#include <QDir>
#include <QDrag>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleOption>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include "configuration.h"
#include "filtereditdialog.h"
#include "log.h"
#include "persistentinfo.h"
#include "shortcuts.h"

#include "filterpanel.h"

namespace {

constexpr int IdRole = Qt::UserRole;
constexpr int SuspendedRole = Qt::UserRole + 1;

constexpr int PatternColumn = 0;
constexpr int HitsColumn = 1;
constexpr int DescriptionColumn = 2;

constexpr auto CurrentFileKey = "filterPanel/currentFile";
constexpr auto RecentFilesKey = "filterPanel/recentFiles";
constexpr auto HeaderStateKey = "filterPanel/headerState";
constexpr int MaxRecentFiles = 8;
constexpr auto FileFilter = "flogg filters (*.flogg-filters.json);;All files (*)";

QString defaultFiltersFile()
{
    const auto dir
        = QStandardPaths::writableLocation( QStandardPaths::GenericDataLocation ) + "/flogg";
    QDir{}.mkpath( dir );
    return dir + "/filters.flogg-filters.json";
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

// Tree that also accepts drops below its last row. The horizontal position of the
// cursor chooses the level: the end of the top level list or of one of the last groups.
class FilterTreeWidget : public QTreeWidget {
  public:
    using QTreeWidget::QTreeWidget;

  protected:
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
        const auto rootOffset = rootIsDecorated() ? indentation() : 0;
        const auto level
            = std::clamp( ( pos.x() + horizontalOffset() - rootOffset ) / indentation(), 0,
                          static_cast<int>( parents.size() ) - 1 );

        tailParent_ = parents[ static_cast<size_t>( level ) ];

        const auto left = rootOffset + level * indentation() - horizontalOffset();
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

FilterPanel::FilterPanel( QWidget* parent )
    : QWidget( parent )
    , snapshot_( std::make_shared<FilterSnapshot>() )
{
    setupUi();

    changeTimer_.setSingleShot( true );
    changeTimer_.setInterval( 150 );
    connect( &changeTimer_, &QTimer::timeout, this, &FilterPanel::publishChange );

    auto file = settings().value( CurrentFileKey ).toString();
    if ( file.isEmpty() ) {
        file = defaultFiltersFile();
    }
    loadFile( file );
}

void FilterPanel::setupUi()
{
    tree_ = new FilterTreeWidget;
    tree_->setColumnCount( 3 );
    tree_->setHeaderLabels( { tr( "Filter" ), tr( "Hits" ), tr( "Description" ) } );
    tree_->setSelectionMode( QAbstractItemView::ExtendedSelection );
    tree_->setDragDropMode( QAbstractItemView::InternalMove );
    tree_->setDefaultDropAction( Qt::MoveAction );
    tree_->setContextMenuPolicy( Qt::CustomContextMenu );
    tree_->setUniformRowHeights( true );

    // Each column is resized by its own right edge only, a wider panel just shows more room
    auto* header = tree_->header();
    header->setSectionResizeMode( QHeaderView::Interactive );
    header->setStretchLastSection( false );
    header->setSectionsMovable( false );
    tree_->setColumnWidth( PatternColumn, 220 );
    tree_->setColumnWidth( HitsColumn, 60 );
    tree_->setColumnWidth( DescriptionColumn, 200 );
    header->restoreState( settings().value( HeaderStateKey ).toByteArray() );
    header->setContextMenuPolicy( Qt::CustomContextMenu );
    connect( header, &QWidget::customContextMenuRequested, this, &FilterPanel::showHeaderMenu );
    connect( header, &QHeaderView::sectionResized, this, &FilterPanel::saveHeaderState );

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
        for ( const auto& file : recentFiles ) {
            recentMenu->addAction( QDir::toNativeSeparators( file ), this,
                                   [ this, file ] { loadFile( file ); } );
        }
        recentMenu->setEnabled( !recentFiles.isEmpty() );
    } );
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
    moveUpAction_ = new QAction( arrowIcon( true, palette() ), tr( "Move up" ), this );
    moveUpAction_->setShortcutContext( Qt::WidgetShortcut );
    connect( moveUpAction_, &QAction::triggered, this, [ this ] { moveCurrent( true ); } );
    addArrowButton( moveUpAction_ );
    tree_->addAction( moveUpAction_ );

    moveDownAction_ = new QAction( arrowIcon( false, palette() ), tr( "Move down" ), this );
    moveDownAction_->setShortcutContext( Qt::WidgetShortcut );
    connect( moveDownAction_, &QAction::triggered, this, [ this ] { moveCurrent( false ); } );
    addArrowButton( moveDownAction_ );
    tree_->addAction( moveDownAction_ );

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

    fileLabel_ = new QLabel;
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

    QMenu menu( this );
    for ( int column = 0; column < tree_->columnCount(); ++column ) {
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
    const auto path = QFileDialog::getOpenFileName(
        this, tr( "Open filters" ), QFileInfo( currentFile_ ).absolutePath(), tr( FileFilter ) );
    if ( !path.isEmpty() ) {
        loadFile( path );
    }
}

void FilterPanel::saveFileAs()
{
    auto path = QFileDialog::getSaveFileName( this, tr( "Save filters as" ), currentFile_,
                                              tr( FileFilter ) );
    if ( path.isEmpty() ) {
        return;
    }

    if ( !path.endsWith( ".json" ) ) {
        path.append( ".flogg-filters.json" );
    }

    currentFile_ = path;
    settings().setValue( CurrentFileKey, currentFile_ );
    addRecentFile( currentFile_ );
    saveCurrentFile();
    updateFileLabel();
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

void FilterPanel::publishChange()
{
    loading_ = true;
    for ( int i = 0; i < tree_->topLevelItemCount(); ++i ) {
        updateSuspendedLook( tree_->topLevelItem( i ), false );
    }
    loading_ = false;

    const auto root = buildTree();

    auto snapshot = std::make_shared<FilterSnapshot>();
    snapshot->revision = ++revision_;
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
    snapshot->highlighters
        = std::make_shared<HighlighterSet>( filters::buildHighlighterSet( effective ) );

    snapshot_ = std::move( snapshot );

    saveCurrentFile();

    Q_EMIT filtersChanged();
}

void FilterPanel::setRoot( const FilterNode& root )
{
    loading_ = true;

    tree_->clear();
    nodes_.clear();

    for ( const auto& child : root.children ) {
        tree_->addTopLevelItem( createItem( child ) );
    }
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
        item->setIcon( PatternColumn, style()->standardIcon( QStyle::SP_DirIcon ) );
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
        const MultiRegularExpression expression{ { filters::toPattern( node ) } };
        const auto isSlow = node.useRegex && !expression.isFullyAccelerated();
        item->setIcon( PatternColumn, isSlow
                                          ? style()->standardIcon( QStyle::SP_MessageBoxWarning )
                                          : QIcon{} );

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
        item->setToolTip( PatternColumn, toolTip );
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

void FilterPanel::loadFile( const QString& path )
{
    QString error;
    auto root = QFileInfo::exists( path ) ? filters::loadFromFile( path, &error )
                                          : std::make_optional( FilterNode::makeGroup( "root" ) );

    if ( !root ) {
        LOG_ERROR << "Failed to load filters from " << path.toStdString() << ": "
                  << error.toStdString();
        QMessageBox::warning( this, tr( "Open filters" ),
                              tr( "Can't load filters from %1: %2" ).arg( path, error ) );
        return;
    }

    currentFile_ = path;
    settings().setValue( CurrentFileKey, currentFile_ );
    addRecentFile( currentFile_ );

    setRoot( *root );
    updateFileLabel();

    // Publish right away, file loading should not wait for the debounce timer
    changeTimer_.stop();
    publishChange();
}

void FilterPanel::saveCurrentFile()
{
    if ( currentFile_.isEmpty() ) {
        return;
    }

    QString error;
    if ( !filters::saveToFile( buildTree(), currentFile_, &error ) ) {
        LOG_ERROR << "Failed to save filters to " << currentFile_.toStdString() << ": "
                  << error.toStdString();
    }
}

void FilterPanel::setHitCounts( const FilterHits& hits )
{
    hits_ = hits;
    updateHitCounts();
}

void FilterPanel::updateHitCounts()
{
    const auto wasLoading = loading_;
    loading_ = true;

    QTreeWidgetItemIterator it( tree_ );
    while ( *it ) {
        auto* item = *it;
        const auto hits = hits_.find( item->data( PatternColumn, IdRole ).toString() );
        item->setText( HitsColumn, hits != hits_.end() ? QString::number( *hits ) : QString{} );
        item->setTextAlignment( HitsColumn, Qt::AlignRight | Qt::AlignVCenter );
        ++it;
    }

    loading_ = wasLoading;
}

void FilterPanel::addRecentFile( const QString& path )
{
    auto recentFiles = settings().value( RecentFilesKey ).toStringList();
    recentFiles.removeAll( path );
    recentFiles.prepend( path );
    while ( recentFiles.size() > MaxRecentFiles ) {
        recentFiles.removeLast();
    }
    settings().setValue( RecentFilesKey, recentFiles );
}

void FilterPanel::updateFileLabel()
{
    fileLabel_->setText( QFileInfo( currentFile_ ).fileName() );
    fileLabel_->setToolTip( QDir::toNativeSeparators( currentFile_ ) );
}
