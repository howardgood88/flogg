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

#include "filetreepanel.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <optional>

#include <QApplication>
#include <QClipboard>
#include <QDirIterator>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QHash>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QActionGroup>
#include <QCollator>
#include <QLocale>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

#include "dirsearchpanel.h"
#include "paneldock.h"
#include "log.h"
#include "openfilehelper.h"
#include "persistentinfo.h"

namespace {
constexpr auto RecentFoldersKey = "fileTree/recentFolders";
constexpr auto MaxRecentFolders = 10;
constexpr auto ShowHiddenKey = "fileTree/showHidden";
constexpr auto SortKey = "fileTree/sort";
// Past this number of files and folders under the root, the folders without a matching
// file are not hidden (the index takes about 100 bytes per entry)
constexpr size_t MaxIndexEntries = 100'000;

QString normalized( const QString& path )
{
    return QDir::cleanPath( QDir::fromNativeSeparators( path ) );
}

// The part of a path under the root starts with this
QString rootPrefix( const QString& root )
{
    return root.endsWith( '/' ) ? root : root + '/';
}

} // namespace

// Files and folders directly in a folder, the hidden ones on demand, the links to folders
// are not followed (as in the model)
namespace {
void listFolder( const QString& path, bool hidden, QStringList& files, QStringList& subfolders )
{
    QDirIterator it( path, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot
                               | ( hidden ? QDir::Hidden : QDir::Filters{} ) );
    while ( it.hasNext() ) {
        const auto info = it.nextFileInfo();
        if ( info.isDir() ) {
            if ( !info.isSymLink() ) {
                subfolders.append( info.filePath() );
            }
        }
        else {
            files.append( info.fileName() );
        }
    }
}

bool isAsciiDigit( QChar c )
{
    return c >= u'0' && c <= u'9';
}

// Natural order: app.log.2 before app.log.10. The numbers are compared here, the text
// between them with the collator: QCollator::setNumericMode is not supported without ICU
// (the Linux builds of Qt).
int naturalCompare( const QCollator& collator, QStringView left, QStringView right )
{
    qsizetype i = 0;
    qsizetype j = 0;
    while ( i < left.size() && j < right.size() ) {
        const auto leftDigits = isAsciiDigit( left[ i ] );
        const auto rightDigits = isAsciiDigit( right[ j ] );
        auto leftEnd = i;
        while ( leftEnd < left.size() && isAsciiDigit( left[ leftEnd ] ) == leftDigits ) {
            ++leftEnd;
        }
        auto rightEnd = j;
        while ( rightEnd < right.size() && isAsciiDigit( right[ rightEnd ] ) == rightDigits ) {
            ++rightEnd;
        }
        auto leftPart = left.sliced( i, leftEnd - i );
        auto rightPart = right.sliced( j, rightEnd - j );

        int order = 0;
        if ( leftDigits && rightDigits ) {
            // By value: the leading zeros out, then the longer is larger
            while ( leftPart.size() > 1 && leftPart.front() == u'0' ) {
                leftPart = leftPart.sliced( 1 );
            }
            while ( rightPart.size() > 1 && rightPart.front() == u'0' ) {
                rightPart = rightPart.sliced( 1 );
            }
            order = leftPart.size() != rightPart.size()
                        ? ( leftPart.size() < rightPart.size() ? -1 : 1 )
                        : leftPart.compare( rightPart );
        }
        else {
            order = collator.compare( leftPart, rightPart );
        }
        if ( order != 0 ) {
            return order;
        }
        i = leftEnd;
        j = rightEnd;
    }
    if ( i < left.size() ) {
        return 1;
    }
    return j < right.size() ? -1 : 0;
}
} // namespace

// Every folder under the root with the names of its files. Built once in a worker thread,
// then kept up to date by watching every folder: a changed folder is listed again (only
// that one). The name filter looks for the folders here, without reading the disk.
class FileTreeIndex {
  public:
    struct Folder {
        QString path;
        QStringList files;
        std::vector<int> subfolders;
        int parent = -1;
        bool alive = true;
    };

    // Nothing when cancelled or past MaxIndexEntries
    static std::optional<std::vector<Folder>> build( const QString& root, bool hidden,
                                                     const std::atomic_bool& cancelled )
    {
        std::vector<Folder> folders;
        folders.push_back( Folder{ root, {}, {}, -1, true } );
        size_t entries = 0;
        for ( size_t id = 0; id < folders.size(); ++id ) {
            if ( cancelled.load() ) {
                return {};
            }
            QStringList files;
            QStringList subfolders;
            listFolder( folders[ id ].path, hidden, files, subfolders );
            entries += static_cast<size_t>( files.size() + subfolders.size() );
            if ( entries > MaxIndexEntries ) {
                LOG_WARNING << "File tree: more than " << MaxIndexEntries << " entries in "
                            << root.toStdString() << ", folders are not filtered";
                return {};
            }
            folders[ id ].files = std::move( files );
            for ( const auto& subfolder : subfolders ) {
                folders[ id ].subfolders.push_back( static_cast<int>( folders.size() ) );
                folders.push_back( Folder{ subfolder, {}, {}, static_cast<int>( id ), true } );
            }
        }
        return folders;
    }

    // changed is called (once per event loop pass) after folders changed on the disk
    FileTreeIndex( std::vector<Folder> folders, bool hidden, std::function<void()> changed )
        : folders_( std::move( folders ) )
        , hidden_( hidden )
        , changed_( std::move( changed ) )
    {
        QStringList paths;
        for ( int id = 0; id < static_cast<int>( folders_.size() ); ++id ) {
            ids_.insert( folders_[ id ].path, id );
            paths.append( folders_[ id ].path );
            entries_ += static_cast<size_t>( folders_[ id ].files.size() ) + 1;
        }
        watcher_.addPaths( paths );
        QObject::connect( &watcher_, &QFileSystemWatcher::directoryChanged, &watcher_,
                          [ this ]( const QString& path ) { refresh( path ); } );
        notifyTimer_.setSingleShot( true );
        notifyTimer_.setInterval( 0 );
        QObject::connect( &notifyTimer_, &QTimer::timeout, &watcher_, [ this ] { changed_(); } );
    }

    // Too many entries since it was built
    bool isOverflowed() const
    {
        return entries_ > MaxIndexEntries;
    }

    size_t entries() const
    {
        return entries_;
    }

    // The folders (but the root) holding a matching file, also deeper
    QSet<QString> matchingFolders( const QRegularExpression& pattern ) const
    {
        QSet<QString> matching;
        std::vector<char> marked( folders_.size(), 0 );
        for ( size_t id = 1; id < folders_.size(); ++id ) {
            const auto& folder = folders_[ id ];
            if ( !folder.alive || marked[ id ] ) {
                continue;
            }
            const auto hasMatch
                = std::any_of( folder.files.cbegin(), folder.files.cend(),
                               [ &pattern ]( const auto& file ) {
                                   return pattern.match( file ).hasMatch();
                               } );
            if ( !hasMatch ) {
                continue;
            }
            for ( auto parent = static_cast<int>( id ); parent > 0 && !marked[ parent ];
                  parent = folders_[ parent ].parent ) {
                marked[ parent ] = 1;
                matching.insert( folders_[ parent ].path );
            }
        }
        return matching;
    }

    // Lists the folder again, for the tests
    void refresh( const QString& path )
    {
        const auto found = ids_.find( path );
        if ( found == ids_.end() ) {
            return;
        }
        const auto id = found.value();
        if ( id != 0 && !QFileInfo( path ).isDir() ) {
            // Gone with its content
            removeFolder( id );
            notifyTimer_.start();
            return;
        }

        QStringList files;
        QStringList subfolders;
        listFolder( path, hidden_, files, subfolders );

        const QSet<QString> current( subfolders.cbegin(), subfolders.cend() );
        // A file written to (not added or removed) may be told as a change of its folder:
        // nothing to filter again
        const auto sameSubfolders
            = current.size() == static_cast<qsizetype>( folders_[ id ].subfolders.size() )
              && std::all_of( folders_[ id ].subfolders.cbegin(), folders_[ id ].subfolders.cend(),
                              [ & ]( int subfolder ) {
                                  return current.contains( folders_[ subfolder ].path );
                              } );
        if ( sameSubfolders
             && QSet<QString>( files.cbegin(), files.cend() )
                    == QSet<QString>( folders_[ id ].files.cbegin(), folders_[ id ].files.cend() ) ) {
            return;
        }

        entries_ = entries_ - static_cast<size_t>( folders_[ id ].files.size() )
                   + static_cast<size_t>( files.size() );
        folders_[ id ].files = std::move( files );

        std::vector<int> kept;
        for ( const auto subfolder : folders_[ id ].subfolders ) {
            if ( current.contains( folders_[ subfolder ].path ) ) {
                kept.push_back( subfolder );
            }
            else {
                removeFolder( subfolder );
            }
        }
        folders_[ id ].subfolders = std::move( kept );
        for ( const auto& subfolder : subfolders ) {
            if ( !ids_.contains( subfolder ) ) {
                addFolder( id, subfolder );
            }
        }
        notifyTimer_.start();
    }

  private:
    // Adds the folder and its content (a new folder, usually small)
    void addFolder( int parent, const QString& path )
    {
        std::vector<std::pair<int, QString>> pending{ { parent, path } };
        while ( !pending.empty() && !isOverflowed() ) {
            auto [ parentId, folderPath ] = pending.back();
            pending.pop_back();

            const auto id = static_cast<int>( folders_.size() );
            QStringList files;
            QStringList subfolders;
            listFolder( folderPath, hidden_, files, subfolders );
            entries_ += static_cast<size_t>( files.size() ) + 1;
            folders_.push_back( Folder{ folderPath, std::move( files ), {}, parentId, true } );
            folders_[ parentId ].subfolders.push_back( id );
            ids_.insert( folderPath, id );
            watcher_.addPath( folderPath );
            for ( const auto& subfolder : subfolders ) {
                pending.emplace_back( id, subfolder );
            }
        }
    }

    // The entries stay (not alive) until the index is built again
    void removeFolder( int id )
    {
        std::vector<int> pending{ id };
        while ( !pending.empty() ) {
            auto& folder = folders_[ pending.back() ];
            pending.pop_back();
            if ( !folder.alive ) {
                continue;
            }
            folder.alive = false;
            entries_ -= static_cast<size_t>( folder.files.size() ) + 1;
            folder.files.clear();
            ids_.remove( folder.path );
            watcher_.removePath( folder.path );
            pending.insert( pending.end(), folder.subfolders.cbegin(), folder.subfolders.cend() );
            folder.subfolders.clear();
        }
    }

    std::vector<Folder> folders_;
    bool hidden_;
    QHash<QString, int> ids_;
    size_t entries_ = 0;
    QFileSystemWatcher watcher_;
    QTimer notifyTimer_;
    std::function<void()> changed_;
};

// Hides the files out of the name filter and the folders (under the root) without a
// matching file. The folders are known once scanned, all of them are shown before.
class FileTreeFilter : public QSortFilterProxyModel {
  public:
    FileTreeFilter( QFileSystemModel* source, FileTreeSort order, QObject* parent )
        : QSortFilterProxyModel( parent )
        , source_( source )
        , order_( order )
    {
        // Natural order (app.log.2 before app.log.10) in naturalCompare
        collator_.setCaseSensitivity( Qt::CaseInsensitive );
        setSourceModel( source );
        // The direction is in lessThan
        sort( 0, Qt::AscendingOrder );

        // The model drops the rows of the removed paths once the disk tells it
        connect( source, &QAbstractItemModel::rowsAboutToBeRemoved, this,
                 [ this ]( const QModelIndex& parent, int first, int last ) {
                     if ( removed_.isEmpty() ) {
                         return;
                     }
                     for ( int row = first; row <= last; ++row ) {
                         removed_.remove( source_->filePath( source_->index( row, 0, parent ) ) );
                     }
                 } );
    }

    void setSortOrder( FileTreeSort order )
    {
        order_ = order;
        invalidate();
    }

    // Hidden at once, before the model sees them removed. A path back on the disk (or
    // never dropped by the model) is shown again after a while.
    void hideRemoved( const QStringList& paths )
    {
        for ( const auto& path : paths ) {
            removed_.insert( path );
        }
        invalidateRowsFilter();
        QTimer::singleShot( 5000, this, [ this, paths ] {
            auto changed = false;
            for ( const auto& path : paths ) {
                if ( removed_.contains( path ) && QFileInfo::exists( path ) ) {
                    removed_.remove( path );
                    changed = true;
                }
            }
            if ( changed ) {
                invalidateRowsFilter();
            }
        } );
    }

    QFileSystemModel* fileSystem() const
    {
        return source_;
    }

    // An empty pattern shows everything, without folders all of them are shown
    void setFilter( const QRegularExpression& pattern, const QString& root,
                    std::optional<QSet<QString>> folders )
    {
        pattern_ = pattern;
        prefix_ = rootPrefix( root );
        folders_ = std::move( folders );
        invalidateRowsFilter();
    }

    bool isActive() const
    {
        return !pattern_.pattern().isEmpty();
    }

    bool isShownFolder( const QString& folder ) const
    {
        return !folders_ || !folder.startsWith( prefix_ ) || folders_->contains( folder );
    }

    bool matches( const QString& fileName ) const
    {
        return pattern_.match( fileName ).hasMatch();
    }

  protected:
    bool filterAcceptsRow( int row, const QModelIndex& parent ) const override
    {
        const auto index = source_->index( row, 0, parent );
        if ( !removed_.isEmpty() && removed_.contains( source_->filePath( index ) ) ) {
            return false;
        }
        if ( !isActive() ) {
            return true;
        }
        if ( source_->isDir( index ) ) {
            // The root and its parents are always kept
            return isShownFolder( source_->filePath( index ) );
        }
        return matches( source_->fileName( index ) );
    }

    // The folders first, then by the order, by name when equal
    bool lessThan( const QModelIndex& left, const QModelIndex& right ) const override
    {
        const auto leftDir = source_->isDir( left );
        const auto rightDir = source_->isDir( right );
        if ( leftDir != rightDir ) {
            return leftDir;
        }
        const auto byName = [ & ] {
            return naturalCompare( collator_, source_->fileName( left ), source_->fileName( right ) );
        };
        switch ( order_ ) {
        case FileTreeSort::NameDescending:
            return byName() > 0;
        case FileTreeSort::NewestFirst:
        case FileTreeSort::OldestFirst: {
            const auto leftTime = source_->lastModified( left );
            const auto rightTime = source_->lastModified( right );
            if ( leftTime != rightTime ) {
                return ( order_ == FileTreeSort::NewestFirst ) == ( leftTime > rightTime );
            }
            break;
        }
        case FileTreeSort::LargestFirst:
        case FileTreeSort::SmallestFirst: {
            // The folders by name
            const auto leftSize = leftDir ? 0 : source_->size( left );
            const auto rightSize = rightDir ? 0 : source_->size( right );
            if ( leftSize != rightSize ) {
                return ( order_ == FileTreeSort::LargestFirst ) == ( leftSize > rightSize );
            }
            break;
        }
        case FileTreeSort::Type: {
            const auto typeOrder
                = naturalCompare( collator_, QFileInfo( source_->fileName( left ) ).suffix(),
                                     QFileInfo( source_->fileName( right ) ).suffix() );
            if ( typeOrder != 0 ) {
                return typeOrder < 0;
            }
            break;
        }
        case FileTreeSort::NameAscending:
            break;
        }
        return byName() < 0;
    }

  private:
    QFileSystemModel* source_;
    FileTreeSort order_;
    QCollator collator_;
    QSet<QString> removed_;
    QRegularExpression pattern_;
    QString prefix_;
    std::optional<QSet<QString>> folders_;
};

// The file system with the open files in bold. The font is looked up for the drawn rows
// only, with one hash lookup each.
class FileTreeModel : public QFileSystemModel {
  public:
    using QFileSystemModel::QFileSystemModel;

    void setOpenFiles( QSet<QString> files )
    {
        openFiles_ = std::move( files );
    }

    // Paths that can't be renamed (open in a tab, or a folder holding one), and the
    // message of a refused rename
    std::function<bool( const QString& )> isInUse;
    std::function<void( const QString& )> renameFailed;

    // Renamed in the tree, nothing else: the files dragged out are copied (never moved by
    // the drop target), no drop on the tree
    Qt::DropActions supportedDragActions() const override
    {
        return Qt::CopyAction;
    }

    Qt::ItemFlags flags( const QModelIndex& index ) const override
    {
        return QFileSystemModel::flags( index ) & ~Qt::ItemIsDropEnabled;
    }

    bool setData( const QModelIndex& index, const QVariant& value, int role ) override
    {
        if ( role != Qt::EditRole || !index.isValid() || index.column() != 0 ) {
            return false;
        }
        const auto oldName = fileName( index );
        const auto newName = value.toString();
        if ( newName == oldName ) {
            return false;
        }

        const auto path = filePath( index );
        const auto folder = path.left( path.lastIndexOf( '/' ) );
        if ( newName.trimmed().isEmpty() || newName == "." || newName == ".."
             || newName.contains( '/' ) || newName.contains( QDir::separator() ) ) {
            renameFailed( QFileSystemModel::tr( "\"%1\" is not a valid name." ).arg( newName ) );
            return false;
        }
        // A change of case only is the same file on a case insensitive file system
        if ( QFileInfo::exists( folder + '/' + newName )
             && newName.compare( oldName, Qt::CaseInsensitive ) != 0 ) {
            renameFailed( QFileSystemModel::tr( "\"%1\" already exists." ).arg( newName ) );
            return false;
        }
        if ( isInUse( path ) ) {
            renameFailed( QFileSystemModel::tr( "\"%1\" is open in a tab, close it before "
                                                "renaming." )
                              .arg( oldName ) );
            return false;
        }
        if ( !QFileSystemModel::setData( index, value, role ) ) {
            renameFailed( QFileSystemModel::tr( "Can't rename \"%1\" to \"%2\"." )
                              .arg( oldName, newName ) );
            return false;
        }
        LOG_INFO << "File tree renamed " << path.toStdString() << " to "
                 << newName.toStdString();
        return true;
    }

    QVariant data( const QModelIndex& index, int role ) const override
    {
        // From the information the model keeps, when hovered only. On one line: Qt places
        // it as the other tool tips of flogg (beside a large cursor on macOS, below it
        // for a taller tip).
        if ( role == Qt::ToolTipRole && index.column() == 0 ) {
            // The name is on the row already
            QStringList parts;
            if ( !isDir( index ) ) {
                parts.append( QLocale().formattedDataSize( size( index ) ) );
            }
            const auto modified = lastModified( index );
            if ( modified.isValid() ) {
                parts.append( modified.toString( "yyyy-MM-dd HH:mm:ss" ) );
            }
            return parts.join( QString::fromUtf8( "  \u00B7  " ) );
        }
        if ( role == Qt::FontRole && !openFiles_.isEmpty() && index.column() == 0
             && openFiles_.contains( filePath( index ) ) ) {
            QFont font;
            font.setBold( true );
            return font;
        }
        return QFileSystemModel::data( index, role );
    }

  private:
    QSet<QString> openFiles_;
};

// The editor of a name selects it without the extension, as file managers do
class FileTreeDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

    // Enter saves the name, without going on to the tree (where it opens the file)
    bool eventFilter( QObject* editor, QEvent* event ) override
    {
        const auto handled = QStyledItemDelegate::eventFilter( editor, event );
        if ( event->type() == QEvent::KeyPress ) {
            const auto key = static_cast<QKeyEvent*>( event )->key();
            if ( key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Escape ) {
                return true;
            }
        }
        return handled;
    }

    void setEditorData( QWidget* editor, const QModelIndex& index ) const override
    {
        QStyledItemDelegate::setEditorData( editor, index );
        auto* lineEdit = qobject_cast<QLineEdit*>( editor );
        if ( !lineEdit ) {
            return;
        }
        const auto name = lineEdit->text();
        // FileInfoRole is Qt 6.8+, the releases are built with Qt 6.7 (once per rename)
        const auto isFolder
            = QFileInfo( index.data( QFileSystemModel::FilePathRole ).toString() ).isDir();
        const auto dot = name.lastIndexOf( '.' );
        // A leading dot is part of the name (.profile). After the view selects all of it.
        const auto length = isFolder || dot <= 0 ? name.size() : dot;
        QTimer::singleShot( 0, lineEdit, [ lineEdit, length ] { lineEdit->setSelection( 0, length ); } );
    }
};

FileTreePanel::FileTreePanel( QWidget* parent )
    : QWidget( parent )
{
    setAcceptDrops( true );
    showHidden_ = PersistentInfo::getSettings( app_settings{} ).value( ShowHiddenKey, false ).toBool();
    sortOrder_ = static_cast<FileTreeSort>(
        std::clamp( PersistentInfo::getSettings( app_settings{} ).value( SortKey, 0 ).toInt(),
                    static_cast<int>( FileTreeSort::NameAscending ),
                    static_cast<int>( FileTreeSort::Type ) ) );

    upButton_ = new QToolButton;
    upButton_->setIcon( style()->standardIcon( QStyle::SP_FileDialogToParent ) );
    upButton_->setToolTip( tr( "Parent folder" ) );
    upButton_->setAutoRaise( true );
    connect( upButton_, &QToolButton::clicked, this, [ this ] {
        QDir dir( rootFolder_ );
        if ( dir.cdUp() ) {
            setRootFolder( dir.absolutePath() );
        }
    } );

    folderButton_ = new QToolButton;
    folderButton_->setPopupMode( QToolButton::InstantPopup );
    folderButton_->setToolButtonStyle( Qt::ToolButtonTextOnly );
    folderButton_->setAutoRaise( true );
    folderButton_->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Preferred );
    // The arrow of the menu at the middle of the right side (at the bottom by default)
    folderButton_->setStyleSheet(
        "QToolButton { padding-right: 14px; }"
        "QToolButton::menu-indicator { subcontrol-origin: padding;"
        " subcontrol-position: center right; }" );
    auto* recentMenu = new QMenu( folderButton_ );
    folderButton_->setMenu( recentMenu );
    connect( recentMenu, &QMenu::aboutToShow, this, &FileTreePanel::updateRecentMenu );

    sortButton_ = new QToolButton;
    sortButton_->setText( QString::fromUtf8( "\u21C5" ) );
    sortButton_->setToolTip( tr( "Sort" ) );
    sortButton_->setPopupMode( QToolButton::InstantPopup );
    sortButton_->setAutoRaise( true );
    sortButton_->setStyleSheet( "QToolButton::menu-indicator { image: none; }" );
    auto* sortMenu = new QMenu( sortButton_ );
    sortButton_->setMenu( sortMenu );
    connect( sortMenu, &QMenu::aboutToShow, this, [ this, sortMenu ] {
        sortMenu->clear();
        addSortActions( sortMenu );
    } );

    // The root folder is searched also when the tree has no room left to click
    searchButton_ = new QToolButton;
    searchButton_->setIcon( panelicons::search( palette().color( QPalette::WindowText ) ) );
    searchButton_->setToolTip( tr( "Search in Folder" ) );
    searchButton_->setAutoRaise( true );
    connect( searchButton_, &QToolButton::clicked, this, [ this ] { searchInFolder( {} ); } );

    auto* header = new QHBoxLayout;
    header->setContentsMargins( 0, 0, 0, 0 );
    header->setSpacing( 2 );
    header->addWidget( upButton_ );
    header->addWidget( folderButton_, 1 );
    header->addWidget( searchButton_ );
    header->addWidget( sortButton_ );

    filterEdit_ = new QLineEdit;
    filterEdit_->setPlaceholderText( tr( "Filter file names (e.g. *.log)" ) );
    filterEdit_->setClearButtonEnabled( true );
    filterEdit_->setToolTip( tr( "Shows the files matching one of the patterns (separated by "
                                 "spaces or ';'); a pattern without * or ? matches a part "
                                 "of the name. Folders without a matching file are hidden." ) );
    // Filtered on each key, in memory once the folder is indexed
    connect( filterEdit_, &QLineEdit::textChanged, this, &FileTreePanel::applyNameFilter );
    // Indexed when about to be used
    filterEdit_->installEventFilter( this );

    tree_ = new QTreeView;
    tree_->setHeaderHidden( true );
    tree_->setUniformRowHeights( true );
    tree_->setSelectionMode( QAbstractItemView::ExtendedSelection );
    // Renamed from the menu or with F2 only, Enter opens
    tree_->setEditTriggers( QAbstractItemView::NoEditTriggers );
    tree_->setItemDelegate( new FileTreeDelegate( tree_ ) );
    // Files dragged to the tabs open there, drops on the tree go to the panel
    tree_->setDragDropMode( QAbstractItemView::DragOnly );
    tree_->setContextMenuPolicy( Qt::CustomContextMenu );
    tree_->installEventFilter( this );
    connect( tree_, &QTreeView::customContextMenuRequested, this,
             &FileTreePanel::showContextMenu );
    connect( tree_, &QTreeView::doubleClicked, this, [ this ]( const QModelIndex& index ) {
        if ( model_ && !isDirAt( index ) ) {
            Q_EMIT openFilesRequested( { pathAt( index ) } );
        }
    } );

    auto* treePage = new QWidget;
    auto* treeLayout = new QVBoxLayout( treePage );
    treeLayout->setContentsMargins( 4, 4, 4, 4 );
    treeLayout->setSpacing( 4 );
    treeLayout->addLayout( header );
    treeLayout->addWidget( filterEdit_ );
    // The search goes under the tree when used
    splitter_ = new QSplitter( Qt::Vertical );
    splitter_->setChildrenCollapsible( false );
    splitter_->addWidget( tree_ );
    treeLayout->addWidget( splitter_, 1 );

    auto* emptyPage = new QWidget;
    auto* emptyLayout = new QVBoxLayout( emptyPage );
    auto* emptyLabel = new QLabel( tr( "Drop a folder here to browse its files" ) );
    emptyLabel->setAlignment( Qt::AlignCenter );
    emptyLabel->setWordWrap( true );
    auto* openButton = new QPushButton( tr( "Open Folder..." ) );
    connect( openButton, &QPushButton::clicked, this, &FileTreePanel::openFolderRequested );
    auto* recentButton = new QPushButton( tr( "Recent Folders" ) );
    recentButton->setMenu( recentMenu );
    emptyLayout->addStretch();
    emptyLayout->addWidget( emptyLabel );
    emptyLayout->addWidget( openButton, 0, Qt::AlignHCenter );
    emptyLayout->addWidget( recentButton, 0, Qt::AlignHCenter );
    emptyLayout->addStretch();

    stack_ = new QStackedWidget;
    stack_->addWidget( emptyPage );
    stack_->addWidget( treePage );

    auto* layout = new QVBoxLayout( this );
    layout->setContentsMargins( 0, 0, 0, 0 );
    layout->addWidget( stack_ );
}

FileTreePanel::~FileTreePanel()
{
    dropIndex();
}

QString FileTreePanel::pathAt( const QModelIndex& index ) const
{
    return model_->filePath( proxy_->mapToSource( index ) );
}

bool FileTreePanel::isDirAt( const QModelIndex& index ) const
{
    return model_->isDir( proxy_->mapToSource( index ) );
}

QModelIndex FileTreePanel::indexOf( const QString& path ) const
{
    return proxy_->mapFromSource( model_->index( path ) );
}

QDir::Filters FileTreePanel::modelFilter() const
{
    return QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Drives
           | ( showHidden_ ? QDir::Hidden : QDir::Filters{} );
}

void FileTreePanel::setShowHidden( bool show )
{
    if ( show == showHidden_ ) {
        return;
    }
    showHidden_ = show;
    if ( searchPanel_ ) {
        searchPanel_->setShowHidden( show );
    }
    PersistentInfo::getSettings( app_settings{} ).setValue( ShowHiddenKey, show );
    if ( model_ ) {
        model_->setFilter( modelFilter() );
    }
    // Indexed again with or without the hidden files when the name filter is used
    dropIndex();
    applyNameFilter();
}

void FileTreePanel::setOpenFilesProvider( std::function<QStringList()> provider )
{
    openFilesProvider_ = std::move( provider );
}

bool FileTreePanel::isInUse( const QString& path ) const
{
    const auto files = openFilesProvider_ ? openFilesProvider_()
                                          : QStringList( openFiles_.cbegin(), openFiles_.cend() );
    const auto prefix = rootPrefix( path );
    return std::any_of( files.cbegin(), files.cend(), [ & ]( const auto& file ) {
        const auto openFile = normalized( file );
        return openFile == path || openFile.startsWith( prefix );
    } );
}

void FileTreePanel::renameCurrent()
{
    const auto index = tree_->currentIndex();
    if ( !model_ || !index.isValid() ) {
        return;
    }
    // Refused before editing rather than after
    const auto path = pathAt( index );
    if ( isInUse( path ) ) {
        model_->renameFailed( tr( "\"%1\" is open in a tab, close it before renaming." )
                                  .arg( QFileInfo( path ).fileName() ) );
        return;
    }
    tree_->edit( index );
}

void FileTreePanel::showWarning( const QString& title, const QString& message )
{
    auto* box = new QMessageBox( QMessageBox::Warning, title, message, QMessageBox::Ok, this );
    box->setAttribute( Qt::WA_DeleteOnClose );
    // Not modal to the event loop (an editor may be closing)
    box->open();
}

void FileTreePanel::setSortOrder( FileTreeSort order )
{
    if ( order == sortOrder_ ) {
        return;
    }
    sortOrder_ = order;
    PersistentInfo::getSettings( app_settings{} ).setValue( SortKey, static_cast<int>( order ) );
    if ( proxy_ ) {
        proxy_->setSortOrder( order );
    }
}

void FileTreePanel::addSortActions( QMenu* menu )
{
    const std::pair<FileTreeSort, QString> orders[] = {
        { FileTreeSort::NameAscending, tr( "Name (A to Z)" ) },
        { FileTreeSort::NameDescending, tr( "Name (Z to A)" ) },
        { FileTreeSort::NewestFirst, tr( "Date Modified (Newest First)" ) },
        { FileTreeSort::OldestFirst, tr( "Date Modified (Oldest First)" ) },
        { FileTreeSort::LargestFirst, tr( "Size (Largest First)" ) },
        { FileTreeSort::SmallestFirst, tr( "Size (Smallest First)" ) },
        { FileTreeSort::Type, tr( "Type" ) },
    };
    auto* group = new QActionGroup( menu );
    for ( const auto& [ order, text ] : orders ) {
        auto* action = menu->addAction( text );
        action->setCheckable( true );
        action->setChecked( order == sortOrder_ );
        group->addAction( action );
        connect( action, &QAction::triggered, this, [ this, order = order ] { setSortOrder( order ); } );
    }
}

QString FileTreePanel::copyText( FileTreeCopy kind, const QString& root, const QStringList& paths )
{
    QStringList lines;
    for ( const auto& path : paths ) {
        switch ( kind ) {
        case FileTreeCopy::Path:
            lines.append( QDir::toNativeSeparators( path ) );
            break;
        case FileTreeCopy::Name:
            lines.append( QFileInfo( path ).fileName() );
            break;
        case FileTreeCopy::RelativePath:
            lines.append( QDir::toNativeSeparators( QDir( root ).relativeFilePath( path ) ) );
            break;
        }
    }
    return lines.join( '\n' );
}

QStringList FileTreePanel::targetsAt( const QModelIndex& index ) const
{
    QStringList paths;
    if ( !model_ || !index.isValid() ) {
        return paths;
    }
    // The selection when the clicked row is in it
    if ( tree_->selectionModel()->isRowSelected( index.row(), index.parent() ) ) {
        const auto rows = tree_->selectionModel()->selectedRows( 0 );
        for ( const auto& row : rows ) {
            paths.append( pathAt( row ) );
        }
    }
    else {
        paths.append( pathAt( index ) );
    }
    return paths;
}

void FileTreePanel::setTrashHandlers( std::function<bool( const QStringList& )> confirm,
                                      std::function<bool( const QString& )> trash )
{
    confirmTrash_ = std::move( confirm );
    trash_ = std::move( trash );
}

QString FileTreePanel::trashText()
{
    // The same on every system (Windows calls it the Recycle Bin)
    return tr( "Move to Trash" );
}

QKeySequence FileTreePanel::trashShortcut()
{
#ifdef Q_OS_MAC
    // Cmd+Backspace as in the Finder
    return QKeySequence( Qt::CTRL | Qt::Key_Backspace );
#else
    return QKeySequence( Qt::Key_Delete );
#endif
}

void FileTreePanel::moveToTrash( const QStringList& paths )
{
    if ( paths.isEmpty() ) {
        return;
    }
    QStringList inUse;
    for ( const auto& path : paths ) {
        if ( isInUse( path ) ) {
            inUse.append( QFileInfo( path ).fileName() );
        }
    }
    if ( !inUse.isEmpty() ) {
        const auto message = tr( "Open in a tab, close it first: %1" ).arg( inUse.join( ", " ) );
        Q_EMIT trashFailed( message );
        showWarning( trashText(), message );
        return;
    }

    const auto confirmed = confirmTrash_ ? confirmTrash_( paths ) : [ & ] {
        QStringList names;
        for ( const auto& path : paths.mid( 0, 10 ) ) {
            names.append( QFileInfo( path ).fileName() );
        }
        if ( paths.size() > 10 ) {
            names.append( tr( "and %1 more" ).arg( paths.size() - 10 ) );
        }
        const auto question = paths.size() == 1
                                  ? tr( "Move \"%1\" to the trash?" ).arg( names.front() )
                                  : tr( "Move these %1 items to the trash?\n\n%2" )
                                        .arg( paths.size() )
                                        .arg( names.join( '\n' ) );
        return QMessageBox::question( this, trashText(), question,
                                      QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes )
               == QMessageBox::Yes;
    }();
    if ( !confirmed ) {
        return;
    }

    QStringList moved;
    QStringList failed;
    for ( const auto& path : paths ) {
        const auto done = trash_ ? trash_( path ) : QFile::moveToTrash( path );
        if ( done ) {
            LOG_INFO << "File tree moved " << path.toStdString() << " to the trash";
            moved.append( path );
        }
        else {
            failed.append( QFileInfo( path ).fileName() );
        }
    }
    if ( proxy_ && !moved.isEmpty() ) {
        proxy_->hideRemoved( moved );
    }
    if ( !failed.isEmpty() ) {
        const auto message = tr( "Can't move to the trash: %1" ).arg( failed.join( ", " ) );
        Q_EMIT trashFailed( message );
        showWarning( trashText(), message );
    }
}

bool FileTreePanel::isIndexing() const
{
    return indexWatcher_ != nullptr;
}

bool FileTreePanel::isIndexed() const
{
    return index_ != nullptr;
}

void FileTreePanel::refreshIndexedFolder( const QString& folder )
{
    if ( index_ ) {
        index_->refresh( normalized( folder ) );
    }
}

void FileTreePanel::dropIndex()
{
    if ( indexCancelled_ ) {
        indexCancelled_->store( true );
        indexCancelled_.reset();
    }
    if ( indexWatcher_ ) {
        // The worker ends on its own, its result is dropped
        indexWatcher_->disconnect( this );
        indexWatcher_->deleteLater();
        indexWatcher_ = nullptr;
    }
    index_.reset();
    indexTooLarge_ = false;
}

void FileTreePanel::ensureIndex()
{
    if ( rootFolder_.isEmpty() || index_ || indexWatcher_ || indexTooLarge_ ) {
        return;
    }

    using Folders = std::optional<std::vector<FileTreeIndex::Folder>>;
    indexCancelled_ = std::make_shared<std::atomic_bool>( false );
    auto* indexWatcher = new QFutureWatcher<Folders>( this );
    indexWatcher_ = indexWatcher;
    const auto root = rootFolder_;
    const auto hidden = showHidden_;
    connect( indexWatcher, &QFutureWatcher<Folders>::finished, this, [ this, root, hidden ] {
        auto* watcher = static_cast<QFutureWatcher<Folders>*>( indexWatcher_ );
        auto folders = watcher->result();
        watcher->deleteLater();
        indexWatcher_ = nullptr;
        indexCancelled_.reset();
        if ( root != rootFolder_ || hidden != showHidden_ ) {
            return;
        }
        if ( !folders ) {
            indexTooLarge_ = true;
            return;
        }
        LOG_INFO << "File tree indexed " << folders->size() << " folders of "
                 << root.toStdString();
        index_ = std::make_unique<FileTreeIndex>( std::move( *folders ), hidden, [ this ] {
            if ( index_->isOverflowed() ) {
                // Not from the timer of the index itself
                QTimer::singleShot( 0, this, [ this ] {
                    if ( index_ && index_->isOverflowed() ) {
                        LOG_WARNING << "File tree: too many entries, folders are not filtered";
                        index_.reset();
                        indexTooLarge_ = true;
                        applyNameFilter();
                    }
                } );
                return;
            }
            applyNameFilter();
        } );
        applyNameFilter();
    } );
    indexWatcher->setFuture( QtConcurrent::run(
        [ root, hidden, cancelled = indexCancelled_ ] {
            return FileTreeIndex::build( root, hidden, *cancelled );
        } ) );
}

void FileTreePanel::setRootFolder( const QString& folder )
{
    const auto path = folder.isEmpty() ? QString{} : normalized( QFileInfo( folder ).absoluteFilePath() );
    if ( path == rootFolder_ ) {
        return;
    }
    rootFolder_ = path;

    // A search of a folder out of the new root ends
    if ( searchPanel_ && !searchPanel_->isHidden()
         && ( path.isEmpty()
              || !( searchPanel_->folder() + '/' ).startsWith( path.endsWith( '/' ) ? path : path + '/' ) ) ) {
        closeSearch();
    }

    if ( path.isEmpty() ) {
        // Drops the watched folders and the nodes read so far
        dropIndex();
        if ( model_ ) {
            tree_->setModel( nullptr );
            delete proxy_;
            proxy_ = nullptr;
            delete model_;
            model_ = nullptr;
        }
        stack_->setCurrentIndex( 0 );
        Q_EMIT rootFolderChanged( path );
        return;
    }

    LOG_INFO << "File tree folder " << path.toStdString();

    if ( !model_ ) {
        model_ = new FileTreeModel( this );
        // Not read only for the renames, the other changes are not offered
        model_->setReadOnly( false );
        model_->setFilter( modelFilter() );
        model_->isInUse = [ this ]( const QString& path ) { return isInUse( path ); };
        model_->renameFailed = [ this ]( const QString& message ) {
            Q_EMIT renameFailed( message );
            showWarning( tr( "Rename" ), message );
        };
        // Hides the files out of the filter instead of graying them
        model_->setNameFilterDisables( false );
        model_->setOpenFiles( openFiles_ );
        proxy_ = new FileTreeFilter( model_, sortOrder_, this );
        tree_->setModel( proxy_ );
        for ( int column = 1; column < model_->columnCount(); ++column ) {
            tree_->hideColumn( column );
        }
    }

    // The previous folder is no longer watched
    model_->setRootPath( path );
    dropIndex();
    if ( !filterEdit_->text().trimmed().isEmpty() || filterEdit_->hasFocus() ) {
        ensureIndex();
    }
    applyNameFilter();
    tree_->setRootIndex( indexOf( path ) );

    folderButton_->setText( QFileInfo( path ).fileName().isEmpty()
                                ? QDir::toNativeSeparators( path )
                                : QFileInfo( path ).fileName() );
    folderButton_->setToolTip( QDir::toNativeSeparators( path ) );
    upButton_->setEnabled( !QDir( path ).isRoot() );
    stack_->setCurrentIndex( 1 );

    addRecentFolder( path );
    if ( !currentFile_.isEmpty() ) {
        setCurrentFile( currentFile_ );
    }
    Q_EMIT rootFolderChanged( path );
}

void FileTreePanel::setOpenFiles( const std::vector<QString>& files )
{
    openFiles_.clear();
    openFiles_.reserve( static_cast<qsizetype>( files.size() ) );
    for ( const auto& file : files ) {
        openFiles_.insert( normalized( file ) );
    }
    if ( model_ ) {
        model_->setOpenFiles( openFiles_ );
        tree_->viewport()->update();
    }
}

void FileTreePanel::setCurrentFile( const QString& file )
{
    currentFile_ = normalized( file );
    if ( !model_ || currentFile_.isEmpty()
         || !currentFile_.startsWith( rootFolder_.endsWith( '/' ) ? rootFolder_
                                                                  : rootFolder_ + '/' ) ) {
        return;
    }

    const auto index = indexOf( currentFile_ );
    if ( index.isValid() ) {
        // Expands the parents of the file
        tree_->setCurrentIndex( index );
        tree_->scrollTo( index );
    }
}

void FileTreePanel::addShowHiddenAction( QMenu* menu )
{
    auto* action = menu->addAction( tr( "Show Hidden Files" ) );
    action->setCheckable( true );
    action->setChecked( showHidden_ );
    action->setShortcut( QKeySequence( Qt::CTRL | Qt::SHIFT | Qt::Key_Period ) );
    action->setShortcutVisibleInContextMenu( true );
    connect( action, &QAction::toggled, this, &FileTreePanel::setShowHidden );
}

QStringList FileTreePanel::recentFolders()
{
    return PersistentInfo::getSettings( app_settings{} ).value( RecentFoldersKey ).toStringList();
}

void FileTreePanel::addRecentFolder( const QString& folder )
{
    auto folders = recentFolders();
    folders.removeAll( folder );
    folders.prepend( folder );
    while ( folders.size() > MaxRecentFolders ) {
        folders.removeLast();
    }
    PersistentInfo::getSettings( app_settings{} ).setValue( RecentFoldersKey, folders );
}

void FileTreePanel::updateRecentMenu()
{
    auto* menu = folderButton_->menu();
    menu->clear();
    // Groups separated by a line: recent folders, open, close, view
    const auto startGroup = [ menu ] {
        if ( !menu->isEmpty() ) {
            menu->addSeparator();
        }
    };

    const auto folders = recentFolders();
    for ( const auto& folder : folders ) {
        if ( folder == rootFolder_ ) {
            continue;
        }
        auto* action = menu->addAction( QDir::toNativeSeparators( folder ) );
        connect( action, &QAction::triggered, this, [ this, folder ] { setRootFolder( folder ); } );
    }

    startGroup();
    connect( menu->addAction( tr( "Open Folder..." ) ), &QAction::triggered, this,
             &FileTreePanel::openFolderRequested );
    if ( !rootFolder_.isEmpty() ) {
        connect( menu->addAction( tr( "Reveal in File Manager" ) ), &QAction::triggered, this,
                 [ this ] { showPathInFileExplorer( rootFolder_ ); } );
    }

    const auto hasRecent = folders.size() > ( folders.contains( rootFolder_ ) ? 1 : 0 );
    if ( !rootFolder_.isEmpty() || hasRecent ) {
        startGroup();
        if ( !rootFolder_.isEmpty() ) {
            connect( menu->addAction( tr( "Close Folder" ) ), &QAction::triggered, this,
                     [ this ] { setRootFolder( {} ); } );
        }
        if ( hasRecent ) {
            connect( menu->addAction( tr( "Clear Recent Folders" ) ), &QAction::triggered, this,
                     [] { PersistentInfo::getSettings( app_settings{} ).remove( RecentFoldersKey ); } );
        }
    }

    if ( !rootFolder_.isEmpty() ) {
        startGroup();
        addShowHiddenAction( menu );
    }
}

QRegularExpression FileTreePanel::namePattern( const QString& text )
{
    QStringList parts;
    for ( const auto& filter : nameFilters( text ) ) {
        parts.append( "(?:"
                      + QRegularExpression::wildcardToRegularExpression(
                          filter, QRegularExpression::NonPathWildcardConversion )
                      + ")" );
    }
    return QRegularExpression( parts.join( '|' ), QRegularExpression::CaseInsensitiveOption );
}

QStringList FileTreePanel::nameFilters( const QString& text )
{
    QStringList filters;
    const auto parts = text.split( QRegularExpression( "[\\s;,]+" ), Qt::SkipEmptyParts );
    for ( const auto& part : parts ) {
        if ( part.contains( '*' ) || part.contains( '?' ) || part.contains( '[' ) ) {
            filters.append( part );
        }
        else {
            filters.append( '*' + part + '*' );
        }
    }
    return filters;
}

void FileTreePanel::setNameFilter( const QString& text )
{
    filterEdit_->setText( text );
    applyNameFilter();
}

void FileTreePanel::applyNameFilter()
{
    if ( !model_ ) {
        return;
    }

    const auto pattern = namePattern( filterEdit_->text() );
    std::optional<QSet<QString>> folders;
    if ( !pattern.pattern().isEmpty() ) {
        ensureIndex();
        // Before the index is ready the files are filtered, every folder is shown
        if ( index_ ) {
            folders = index_->matchingFolders( pattern );
        }
    }
    proxy_->setFilter( pattern, rootFolder_, std::move( folders ) );
}

QStringList FileTreePanel::selectedFiles() const
{
    QStringList files;
    if ( !model_ ) {
        return files;
    }
    const auto rows = tree_->selectionModel()->selectedRows( 0 );
    for ( const auto& index : rows ) {
        if ( !isDirAt( index ) ) {
            files.append( pathAt( index ) );
        }
    }
    return files;
}

QStringList FileTreePanel::selectedFilesInViewOrder() const
{
    if ( !model_ ) {
        return {};
    }
    // Rows from the top of the tree, compared level by level
    const auto position = []( QModelIndex index ) {
        std::vector<int> rows;
        while ( index.isValid() ) {
            rows.push_back( index.row() );
            index = index.parent();
        }
        std::reverse( rows.begin(), rows.end() );
        return rows;
    };
    std::vector<std::pair<std::vector<int>, QString>> files;
    for ( const auto& index : tree_->selectionModel()->selectedRows( 0 ) ) {
        if ( !isDirAt( index ) ) {
            files.emplace_back( position( index ), pathAt( index ) );
        }
    }
    std::sort( files.begin(), files.end() );
    QStringList paths;
    for ( const auto& file : files ) {
        paths.append( file.second );
    }
    return paths;
}

void FileTreePanel::openSelected()
{
    const auto files = selectedFiles();
    if ( !files.isEmpty() ) {
        Q_EMIT openFilesRequested( files );
    }
}

bool FileTreePanel::eventFilter( QObject* watched, QEvent* event )
{
    if ( watched == filterEdit_ && event->type() == QEvent::FocusIn ) {
        ensureIndex();
    }

    if ( watched == tree_ && event->type() == QEvent::KeyPress ) {
        const auto* keyEvent = static_cast<QKeyEvent*>( event );
        if ( keyEvent->key() == Qt::Key_F2 ) {
            renameCurrent();
            return true;
        }
        if ( QKeySequence( keyEvent->keyCombination() ) == trashShortcut() ) {
            moveToTrash( targetsAt( tree_->currentIndex() ) );
            return true;
        }
        // Cmd+Shift+. as in the file dialogs of macOS (Ctrl+Shift+. elsewhere)
        if ( ( keyEvent->key() == Qt::Key_Period || keyEvent->key() == Qt::Key_Greater )
             && keyEvent->modifiers().testFlags( Qt::ControlModifier | Qt::ShiftModifier ) ) {
            setShowHidden( !showHidden_ );
            return true;
        }
        if ( keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter ) {
            const auto current = tree_->currentIndex();
            if ( model_ && current.isValid() && isDirAt( current )
                 && selectedFiles().isEmpty() ) {
                tree_->setExpanded( current, !tree_->isExpanded( current ) );
            }
            else {
                openSelected();
            }
            return true;
        }
    }
    return QWidget::eventFilter( watched, event );
}

void FileTreePanel::showContextMenu( QPoint pos )
{
    if ( !model_ ) {
        return;
    }
    const auto index = tree_->indexAt( pos );
    const auto path = index.isValid() ? pathAt( index ) : rootFolder_;
    const auto isDir = !index.isValid() || isDirAt( index );
    // The selected rows when the clicked one is selected, the root out of the rows
    const auto targets = index.isValid() ? targetsAt( index ) : QStringList{ rootFolder_ };

    QMenu menu;
    // Open, root, reveal; then search and copy; then rename and trash
    if ( !isDir ) {
        const auto files = selectedFiles();
        auto* open = menu.addAction( files.size() > 1 ? tr( "Open %1 Files" ).arg( files.size() )
                                                      : tr( "Open" ) );
        connect( open, &QAction::triggered, this, &FileTreePanel::openSelected );
        if ( files.size() > 1 ) {
            connect( menu.addAction( tr( "Merge %1 Files..." ).arg( files.size() ) ),
                     &QAction::triggered, this,
                     [ this ] { Q_EMIT mergeFilesRequested( selectedFilesInViewOrder() ); } );
        }
    }
    else if ( index.isValid() ) {
        connect( menu.addAction( tr( "Set as Root Folder" ) ), &QAction::triggered, this,
                 [ this, path ] { setRootFolder( path ); } );
    }
    connect( menu.addAction( tr( "Reveal in File Manager" ) ), &QAction::triggered, this,
             [ path ] { showPathInFileExplorer( path ); } );
    menu.addSeparator();

    if ( !isDir ) {
        connect( menu.addAction( tr( "Search in File" ) ), &QAction::triggered, this,
                 [ this, path ] {
                     Q_EMIT searchInFileRequested(
                         path, searchPanel_ ? searchPanel_->query() : dirsearch::Query{} );
                 } );
    }
    else {
        connect( menu.addAction( tr( "Search in Folder..." ) ), &QAction::triggered, this,
                 [ this, path ] { searchInFolder( path ); } );
    }
    const auto addCopy = [ & ]( const QString& text, FileTreeCopy kind ) {
        connect( menu.addAction( text ), &QAction::triggered, this, [ this, kind, targets ] {
            QApplication::clipboard()->setText( copyText( kind, rootFolder_, targets ) );
        } );
    };
    addCopy( tr( "Copy Path" ), FileTreeCopy::Path );
    addCopy( tr( "Copy Relative Path" ), FileTreeCopy::RelativePath );
    addCopy( tr( "Copy File Name" ), FileTreeCopy::Name );

    if ( index.isValid() ) {
        menu.addSeparator();
        auto* rename = menu.addAction( tr( "Rename..." ) );
        rename->setShortcut( QKeySequence( Qt::Key_F2 ) );
        rename->setShortcutVisibleInContextMenu( true );
        rename->setEnabled( targets.size() == 1 );
        connect( rename, &QAction::triggered, this, [ this, index ] {
            tree_->setCurrentIndex( index );
            renameCurrent();
        } );
        auto* trash = menu.addAction( trashText() + "..." );
        trash->setShortcut( trashShortcut() );
        trash->setShortcutVisibleInContextMenu( true );
        connect( trash, &QAction::triggered, this, [ this, targets ] { moveToTrash( targets ); } );
    }

    menu.addSeparator();
    addSortActions( menu.addMenu( tr( "Sort By" ) ) );
    if ( !index.isValid() ) {
        connect( menu.addAction( tr( "Collapse All" ) ), &QAction::triggered, tree_,
                 &QTreeView::collapseAll );
    }
    menu.addSeparator();
    addShowHiddenAction( &menu );

    menu.exec( tree_->viewport()->mapToGlobal( pos ) );
}

void FileTreePanel::dragEnterEvent( QDragEnterEvent* event )
{
    if ( event->mimeData()->hasUrls() ) {
        event->acceptProposedAction();
    }
}

// A folder becomes the root, the files open in tabs
void FileTreePanel::dropEvent( QDropEvent* event )
{
    QStringList files;
    for ( const auto& url : event->mimeData()->urls() ) {
        const auto path = url.toLocalFile();
        if ( path.isEmpty() ) {
            continue;
        }
        if ( QFileInfo( path ).isDir() ) {
            setRootFolder( path );
        }
        else {
            files.append( path );
        }
    }
    event->acceptProposedAction();
    if ( !files.isEmpty() ) {
        Q_EMIT openFilesRequested( files );
    }
}

void FileTreePanel::searchInFolder( const QString& folder )
{
    if ( rootFolder_.isEmpty() ) {
        return;
    }
    if ( !searchPanel_ ) {
        searchPanel_ = new DirSearchPanel;
        searchPanel_->setModifiedFilesProvider( [ this ] {
            return modifiedFilesProvider_ ? modifiedFilesProvider_() : QStringList{};
        } );
        searchPanel_->setFileHandlers(
            [ this ]( const QStringList& files ) {
                if ( releaseFiles_ ) {
                    releaseFiles_( files );
                }
            },
            [ this ]( const QStringList& files ) {
                if ( restoreFiles_ ) {
                    restoreFiles_( files );
                }
            } );
        connect( searchPanel_, &DirSearchPanel::closeRequested, this, &FileTreePanel::closeSearch );
        connect( searchPanel_, &DirSearchPanel::openRequested, this,
                 &FileTreePanel::openSearchResult );
        connect( searchPanel_, &DirSearchPanel::filesChanged, this,
                 &FileTreePanel::filesChangedBySearch );
        connect( searchPanel_, &DirSearchPanel::exported, this,
                 [ this ]( const QString& path ) { Q_EMIT openFilesRequested( { path } ); } );
        searchPanel_->setExportFolder( temporaryFolder_ );
        splitter_->addWidget( searchPanel_ );
        splitter_->setStretchFactor( 0, 1 );
        splitter_->setStretchFactor( 1, 2 );
    }
    const auto wasHidden = searchPanel_->isHidden();
    const auto target = folder.isEmpty() ? rootFolder_ : folder;
    const auto changed = target != searchPanel_->folder();
    searchPanel_->setFolder( target, rootFolder_ );
    searchPanel_->setShowHidden( showHidden_ );
    if ( wasHidden || changed ) {
        // The file name filter of the tree, as a start
        searchPanel_->setIncludeText( filterEdit_->text() );
    }
    searchPanel_->show();
    if ( wasHidden ) {
        // A third of the height to the tree
        const auto height = splitter_->height();
        splitter_->setSizes( { height / 3, height - height / 3 } );
    }
    searchPanel_->focusFind();
    if ( changed || wasHidden ) {
        searchPanel_->startSearch();
    }
}

void FileTreePanel::closeSearch()
{
    if ( !searchPanel_ ) {
        return;
    }
    searchPanel_->stop();
    searchPanel_->hide();
    tree_->setFocus();
}

void FileTreePanel::setModifiedFilesProvider( std::function<QStringList()> provider )
{
    modifiedFilesProvider_ = std::move( provider );
}

void FileTreePanel::setFileHandlers( std::function<void( const QStringList& )> release,
                                     std::function<void( const QStringList& )> restore )
{
    releaseFiles_ = std::move( release );
    restoreFiles_ = std::move( restore );
}

void FileTreePanel::changeEvent( QEvent* event )
{
    if ( event->type() == QEvent::PaletteChange && searchButton_ ) {
        searchButton_->setIcon( panelicons::search( palette().color( QPalette::WindowText ) ) );
    }
    QWidget::changeEvent( event );
}

void FileTreePanel::setTemporaryFolder( const QString& folder )
{
    temporaryFolder_ = folder;
    if ( searchPanel_ ) {
        searchPanel_->setExportFolder( folder );
    }
}
