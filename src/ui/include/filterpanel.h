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

#ifndef KLOGG_FILTERPANEL_H
#define KLOGG_FILTERPANEL_H

#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include <QByteArray>
#include <QHash>
#include <QIcon>
#include <QKeySequence>
#include <QList>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QWidget>

#include "filtertree.h"

class QAction;
class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

// Everything the log views need to know about the current filters.
struct FilterSnapshot {
    // Increases on every change, used to apply filters lazily to background tabs
    uint64_t revision = 0;

    // Effective filters, in priority order
    std::vector<FilterNode> filters;

    // Empty pattern when there are no effective filters
    RegularExpressionPattern searchPattern;

    std::shared_ptr<const HighlighterSet> highlighters;
};

// Filters of one tab, kept with the tab while another tab is shown
struct FilterTabState {
    // False until the tab is shown the first time, it then gets the default filter file
    // (or the one set in file)
    bool initialized = false;

    // The tab uses the shared filters (SharedFilters), the filters below are its own ones,
    // used again when it leaves the shared mode
    bool shared = false;

    // File loaded or saved last
    QString file;
    FilterNode root;

    // Filters as loaded from or saved to the file, the tab has unsaved changes when the
    // filters differ
    QByteArray savedJson;

    // Filters of the tab serialized like savedJson, empty when not computed yet
    QByteArray json;

    // Another tab saved the file while this one had unsaved changes
    bool fileChanged = false;

    // Shared filters copied from the filters of a tab: they keep its file until their first
    // change, then they become untitled so that saving them does not overwrite it
    bool copyOfFile = false;

    // Empty when the filters still have to be applied
    std::shared_ptr<const FilterSnapshot> snapshot;

    // How the tree was left: ids of the collapsed groups and of the selected items
    QStringList collapsedGroups;
    QStringList selectedItems;
    QString currentItem;
    int scrollPosition = 0;

    // Hit counts shown when the tab was left
    QHash<QString, qint64> hits;
};

// Number of matching lines per filter id
using FilterHits = QHash<QString, qint64>;

// Filter files saved by a panel: the tabs using the same file without unsaved changes take
// the saved filters, the others are marked as changed elsewhere.
class FilterFiles : public QObject {
    Q_OBJECT

  public:
    static FilterFiles& instance();

    static bool isSameFile( const QString& lhs, const QString& rhs );

    // json is serialized like FilterTabState::savedJson
    void notifySaved( const QString& path, const QByteArray& json, const QObject* source )
    {
        Q_EMIT saved( path, json, source );
    }

    // Applies a save to the state of a tab in the background, true when it changed
    static bool follow( FilterTabState& state, const QString& path, const QByteArray& json );

  Q_SIGNALS:
    void saved( const QString& path, const QByteArray& json, const QObject* source );

  private:
    FilterFiles() = default;
};

// Filters with unsaved changes that are lost when their tabs are closed
struct UnsavedFilters {
    // What the filters belong to, e.g. "Shared filters" or "Filters of a.log"
    QString name;
    // Empty for untitled filters
    QString file;
    // The shared filters, listed once when several windows are closed
    bool shared = false;
    // False when they were not saved (cancelled or failed)
    std::function<bool()> save;
    // Reverts them to their file, the session must not keep the discarded changes
    std::function<void()> discard;
};

// Lists the unsaved filters in one question, saves the ones selected and discards the others:
// false when the user cancels or a save fails (nothing is discarded then)
bool confirmUnsavedFilters( QWidget* parent, const QString& text,
                            const std::vector<UnsavedFilters>& filters );

// Marks what uses the shared filters: two links of a chain, in the given colour
QIcon sharedFiltersIcon( const QColor& color );

// Filters shared by the tabs in shared mode, in every window. The filter panels showing such
// a tab publish their changes here right away (also unsaved ones) and take the changes of
// the others; tabs in the background get them when they are shown, as any filter change.
class SharedFilters : public QObject {
    Q_OBJECT

  public:
    static SharedFilters& instance();

    // False until a tab enters the shared mode (or a session with shared filters is restored)
    bool isInitialized() const
    {
        return state_.initialized;
    }

    // File, filters and snapshot, without the tree view of any tab
    const FilterTabState& state() const
    {
        return state_;
    }
    // source is the panel publishing the change, it is not notified
    void setState( FilterTabState state, const QObject* source );

    // Tabs in shared mode, a tab is dropped when it is destroyed
    void setUser( QWidget* tab, bool isUser );
    bool isUser( const QWidget* tab ) const;
    // Whether another tab uses the shared filters, not counting the tabs of closingWindow
    bool hasOtherUsers( const QWidget* tab, const QWidget* closingWindow = nullptr ) const;
    // Whether a tab not closing uses the shared filters
    bool hasUsersLeft( const std::function<bool( const QWidget* )>& isClosing ) const;

    // Saves the shared filters (asks for a file when they are untitled), for when no panel
    // shows them
    bool save( QWidget* parent );
    // Reverts the shared filters to their file
    void discardChanges();

    void notifyHighlightersCompiled( const HighlighterSet* highlighters );

    // Forgets the shared filters and their users
    void clear()
    {
        state_ = {};
        users_.clear();
    }

  Q_SIGNALS:
    void changed( const QObject* source );
    // A tab (or a panel without tab) started or stopped using the shared filters
    void usersChanged();
    void highlightersCompiled( const HighlighterSet* highlighters );

  private:
    SharedFilters() = default;

    FilterTabState state_;
    QList<QPointer<QWidget>> users_;
};

enum class FilterSearchMode {
    // include filters are or-ed, exclude filters are and-not-ed
    Add,
    // every filter is and-not-ed
    Exclude,
    Replace,
};

// Tree of filters and filter groups of the current tab of a window. Changes are only
// written to the filter file when saved.
class FilterPanel : public QWidget {
    Q_OBJECT

  public:
    explicit FilterPanel( QWidget* parent = nullptr );

    // Filter file loaded by the panels created from now on (instead of the remembered one),
    // without remembering it
    static void setStartupFile( const QString& path );

    std::shared_ptr<const FilterSnapshot> snapshot() const
    {
        return snapshot_;
    }

    // Opens the edit dialog for a new filter, prefilled with the passed text
    void addFilterFromText( const QString& text );

    QString currentFile() const
    {
        return currentFile_;
    }

    // Hit counts of the current tab, filters without a count are shown blank
    void setHitCounts( const FilterHits& hits );

    // Filters shown in the panel, to keep with their tab when another tab is shown
    FilterTabState tabState();

    // Shows the filters of a tab
    void setTabState( const FilterTabState& state );

    // State of a tab restored from the session, compared to the file as it is now. The session
    // keeps the filters only when they had unsaved changes, without them the tab gets the
    // file as it is now.
    static FilterTabState restoredTabState( const QString& file,
                                            const std::optional<FilterNode>& root );

    static bool isModified( const FilterTabState& state );
    bool isModified() const;
    // The own filters of the tab, hidden while it uses the shared filters, are changed
    bool isPrivateModified() const
    {
        return shared_ && isModified( privateState_ );
    }

    // Writes the filters of a state to its file (asks for one when it is untitled, title of
    // that dialog), without notifying FilterFiles: false when cancelled or failed
    static bool saveState( FilterTabState& state, QWidget* parent, const QString& title );
    // Saves the own filters of the tab hidden while it uses the shared filters
    bool savePrivateFilters( const QString& tabName );

    // Reverts the filters of a state to its file
    static void discardChanges( FilterTabState& state );
    // Reverts the filters shown to their file
    void discardChanges();
    void discardPrivateChanges();

    // Applies a change still waiting to be published
    void flushChange();

    // Asks to save the changed filters before they are closed: false when the user cancels,
    // discarded changes are reverted to the file
    bool confirmClose( const QString& tabName );

    // Tab whose filters are shown, its use of the shared filters follows the shared mode
    void setTab( QWidget* tab );
    // What uses the shared filters for this panel: its tab, or the panel when no file is open
    QWidget* sharedUser();

    bool isSharedMode() const
    {
        return shared_;
    }
    // The tab uses the shared filters, or its own ones again: false when the user cancels
    // saving the changed filters left
    bool setSharedMode( bool shared );

    // Loads the filters of the tab from a file (asks to save the changed ones first, unless
    // confirm is false), without making it the default one
    bool openFiltersFile( const QString& path, bool confirm = true );

    // Adds filters at the end of the tree and applies them right away
    void appendFilters( const std::vector<FilterNode>& filters );

    // Reads the settings of the panel again after they were replaced
    void reloadSettings();

    // Applies the configured keyboard shortcuts
    void updateShortcuts();

  Q_SIGNALS:
    void filtersChanged();
    // The highlighters of the published filters are compiled (in the background)
    void highlightersCompiled();
    void searchFragmentRequested( const std::vector<FilterNode>& filters, FilterSearchMode mode );
    void findRequested( const std::vector<FilterNode>& filters, bool backward );

  public Q_SLOTS:
    // Saves the filters shown, asks for a file when they are untitled
    bool saveFile();

  private Q_SLOTS:
    void addFilter();
    void addGroup();
    void editCurrent();
    void deleteSelected();
    void toggleSuspendSelected();
    void toggleSuspendAll();
    void setAllEnabled( bool enabled );
    // Filters are checked or unchecked, groups stand for all their filters
    void setSelectedEnabled( bool enabled );
    void moveCurrent( bool up );
    void openFile();
    bool saveFileAs();
    void importTatFile();
    void importKloggSettings();
    void showContextMenu( const QPoint& pos );
    void showHeaderMenu( const QPoint& pos );
    void requestSearchFragment( FilterSearchMode mode );
    void requestFind( bool backward );
    void onTreeChanged();

  private:
    void setupUi();
    static QString defaultFile();
    void setRoot( const FilterNode& root );
    FilterNode buildTree() const;
    FilterNode buildNode( const QTreeWidgetItem* item ) const;
    QTreeWidgetItem* createItem( const FilterNode& node );
    void updateItem( QTreeWidgetItem* item );
    void updateSuspendedLook( QTreeWidgetItem* item, bool parentSuspended );
    bool selectionSuspended() const;
    bool anySuspended() const;
    void updateActions();
    void moveItem( QTreeWidgetItem* item, QTreeWidgetItem* newParent, int index );
    void saveHeaderState();
    QTreeWidgetItem* insertionParent() const;
    void insertItem( QTreeWidgetItem* item );
    std::vector<FilterNode> selectedFiltersForSearch() const;

    bool eventFilter( QObject* watched, QEvent* event ) override;
    void changeEvent( QEvent* event ) override;
    // Icons drawn in the colours of the style
    void loadIcons();

    void scheduleChange();
    void publishChange();
    void updateItems();
    void updateLook();
    bool loadFile( const QString& path, bool remember = true );
    // checkFile: asks before overwriting a file changed since it was loaded
    bool saveCurrentFile( bool checkFile = true );
    void updateFileLabel();
    void restoreHeaderState();
    void saveTreeView( FilterTabState& state ) const;
    void restoreTreeView( const FilterTabState& state );
    static void addRecentFile( const QString& path );
    // The own filters of the tab kept aside while it is shared
    void setPrivateState( FilterTabState state );
    // Publishes the filters shown to the shared filters
    void pushShared( const FilterNode& root );
    // The filters shown become the shared ones, see FilterTabState::copyOfFile
    void seedShared();
    void detachSharedCopy();
    // Shows the shared filters, keeping the tree view
    void showSharedFilters( const FilterTabState& view );
    void updateSharedAction();
    void updateHitCounts();
    void updateHitCounts( const QList<QTreeWidgetItem*>& items );
    // Numbers the filters in priority order (FilterNode::number)
    void numberFilters();
    void numberFilters( const QList<QTreeWidgetItem*>& items );
    QList<QTreeWidgetItem*> topLevelItems() const;
    void updateNumberToolTip( QTreeWidgetItem* item, const FilterNode& node );

  private:
    QTreeWidget* tree_;
    QLabel* fileLabel_ = nullptr;
    QTimer changeTimer_;

    QAction* suspendAction_;
    QAction* findNextAction_;
    QAction* findPreviousAction_;
    QAction* addFilterAction_;
    QAction* editAction_;
    QAction* deleteAction_;
    QAction* moveUpAction_ = nullptr;
    QAction* moveDownAction_ = nullptr;
    QAction* saveAction_;
    QAction* sharedAction_ = nullptr;
    QList<QKeySequence> saveKeys_;

    // Per item filter properties (the tree structure and check states
    // are held by the QTreeWidget itself)
    std::unordered_map<QString, FilterNode> nodes_;

    // Whether a regex filter is fully handled by the fast regex engine, by case flag and
    // pattern: compiling it is too slow to repeat for every item on each tab switch
    QHash<QString, bool> acceleratedPatterns_;

    // Style icons are slow to get on Windows, fetched once
    QIcon groupIcon_;
    QIcon slowIcon_;

    QString currentFile_;
    QByteArray savedJson_;
    // Tree as last published, serialized like savedJson_
    QByteArray treeJson_;
    FilterHits hits_;
    bool loading_ = false;
    std::shared_ptr<const FilterSnapshot> snapshot_;

    // The shown tab uses the shared filters, its own ones are kept aside
    bool shared_ = false;
    FilterTabState privateState_;
    // See FilterTabState::fileChanged and FilterTabState::copyOfFile
    bool fileChanged_ = false;
    bool sharedCopy_ = false;
    QPointer<QWidget> tab_;
};

#endif
