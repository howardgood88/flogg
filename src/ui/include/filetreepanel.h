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

#ifndef KLOGG_FILETREEPANEL_H
#define KLOGG_FILETREEPANEL_H

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include <QDir>
#include <QKeySequence>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QWidget>

#include "dirsearch.h"

class QFileSystemModel;
class QLineEdit;
class QMenu;
class QModelIndex;
class QStackedWidget;
class QToolButton;
class QTreeView;
class FileTreeModel;
class FileTreeFilter;
class FileTreeIndex;
class QFutureWatcherBase;
class QSplitter;
class DirSearchPanel;

// Order of the rows of the tree, the folders first
enum class FileTreeSort {
    NameAscending,
    NameDescending,
    NewestFirst,
    OldestFirst,
    LargestFirst,
    SmallestFirst,
    Type,
};

// What "Copy ..." puts in the clipboard (one line per path)
enum class FileTreeCopy { Path, RelativePath, Name };

// The folder of a window shown as a tree, its files are opened in tabs. A window has at
// most one folder. The tree reads the folders on demand (only the expanded ones), in the
// thread of QFileSystemModel, nothing is read before a folder is set.
class FileTreePanel : public QWidget {
    Q_OBJECT

  public:
    explicit FileTreePanel( QWidget* parent = nullptr );
    ~FileTreePanel() override;

    // Empty closes the folder
    void setRootFolder( const QString& folder );
    QString rootFolder() const
    {
        return rootFolder_;
    }

    // The files open in the window, shown in bold
    void setOpenFiles( const std::vector<QString>& files );
    // Selects the file of the current tab when it is in the folder
    void setCurrentFile( const QString& file );

    // Folders opened before, the most recent first
    static QStringList recentFolders();

    // The hidden files and folders, the choice is kept in the settings
    bool showHidden() const
    {
        return showHidden_;
    }
    void setShowHidden( bool show );

    // The files open in every window, a file open (or a folder holding one) is not
    // renamed. The files of this window without it.
    void setOpenFilesProvider( std::function<QStringList()> provider );
    bool isInUse( const QString& path ) const;
    // Edits the name of the current file or folder
    void renameCurrent();

    // Kept in the settings
    FileTreeSort sortOrder() const
    {
        return sortOrder_;
    }
    void setSortOrder( FileTreeSort order );

    static QString copyText( FileTreeCopy kind, const QString& root, const QStringList& paths );

    // After asking, refused when one of them is open in a tab
    void moveToTrash( const QStringList& paths );
    // For the tests: answers the question, moves a path (QFile::moveToTrash by default)
    void setTrashHandlers( std::function<bool( const QStringList& )> confirm,
                           std::function<bool( const QString& )> trash );

    // Opens the search under the tree for the folder (the root when empty), made when first
    // used
    void searchInFolder( const QString& folder );
    void closeSearch();
    // Null before the first search
    DirSearchPanel* searchPanel() const
    {
        return searchPanel_;
    }
    // The files with changes not saved in the edit mode, not replaced in
    void setModifiedFilesProvider( std::function<QStringList()> provider );
    // Where the results of the search are written to be opened in a tab
    void setTemporaryFolder( const QString& folder );

    // Helpers for the tests
    QTreeView* treeView() const
    {
        return tree_;
    }
    void setNameFilter( const QString& text );
    // The file names of the filter, a text without a wildcard matches a part of the name
    static QStringList nameFilters( const QString& text );
    // The filters as one pattern (case insensitive), empty for none
    static QRegularExpression namePattern( const QString& text );
    // The folder is being indexed for the name filter (once, when the filter is used)
    bool isIndexing() const;
    bool isIndexed() const;
    // Lists the folder again in the index, as when the disk tells it changed
    void refreshIndexedFolder( const QString& folder );
    // Path of an index of the tree view, index of a path
    QString pathAt( const QModelIndex& index ) const;
    QModelIndex indexOf( const QString& path ) const;

  Q_SIGNALS:
    void openFilesRequested( const QStringList& files );
    void openFolderRequested();
    void rootFolderChanged( const QString& folder );
    void renameFailed( const QString& message );
    void trashFailed( const QString& message );
    // A result of the search, the line from 0; in the edit mode with its find bar when asked
    void openSearchResult( const QString& path, qint64 line, bool editMode,
                           const dirsearch::Query& query );
    // The file in the edit mode with its find bar (the text of the search, if any)
    void searchInFileRequested( const QString& path, const dirsearch::Query& query );
    // Written by a replacement of the search
    void filesChangedBySearch( const QStringList& paths );

  protected:
    void dragEnterEvent( QDragEnterEvent* event ) override;
    void dropEvent( QDropEvent* event ) override;
    bool eventFilter( QObject* watched, QEvent* event ) override;
    void changeEvent( QEvent* event ) override;

  private:
    void showContextMenu( QPoint pos );
    void openSelected();
    void applyNameFilter();
    void updateRecentMenu();
    static void addRecentFolder( const QString& folder );
    QStringList selectedFiles() const;
    bool isDirAt( const QModelIndex& index ) const;
    void ensureIndex();
    QDir::Filters modelFilter() const;
    void addShowHiddenAction( QMenu* menu );
    void addSortActions( QMenu* menu );
    void showWarning( const QString& title, const QString& message );
    // The selected paths when the index is selected, else its path
    QStringList targetsAt( const QModelIndex& index ) const;
    static QString trashText();
    static QKeySequence trashShortcut();
    void dropIndex();

    QString rootFolder_;
    QString currentFile_;
    // Normalized paths, shared with the model (implicitly)
    QSet<QString> openFiles_;
    std::function<QStringList()> openFilesProvider_;
    bool showHidden_ = false;
    FileTreeSort sortOrder_ = FileTreeSort::NameAscending;
    std::function<bool( const QStringList& )> confirmTrash_;
    std::function<bool( const QString& )> trash_;

    QStackedWidget* stack_ = nullptr;
    QToolButton* upButton_ = nullptr;
    QToolButton* folderButton_ = nullptr;
    QToolButton* sortButton_ = nullptr;
    QToolButton* searchButton_ = nullptr;
    QLineEdit* filterEdit_ = nullptr;
    QTreeView* tree_ = nullptr;
    QSplitter* splitter_ = nullptr;
    DirSearchPanel* searchPanel_ = nullptr;
    std::function<QStringList()> modifiedFilesProvider_;
    QString temporaryFolder_;
    // Created with the first folder
    FileTreeModel* model_ = nullptr;
    FileTreeFilter* proxy_ = nullptr;
    // The files and folders under the root, made when the name filter is used
    std::unique_ptr<FileTreeIndex> index_;
    QFutureWatcherBase* indexWatcher_ = nullptr;
    std::shared_ptr<std::atomic_bool> indexCancelled_;
    // Too many entries to index, the folders are not filtered
    bool indexTooLarge_ = false;
};

#endif
