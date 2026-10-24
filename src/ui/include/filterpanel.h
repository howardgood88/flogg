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

#include <memory>
#include <unordered_map>
#include <vector>

#include <QHash>
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

// Number of matching lines per filter id
using FilterHits = QHash<QString, qint64>;

enum class FilterSearchMode {
    // include filters are or-ed, exclude filters are and-not-ed
    Add,
    // every filter is and-not-ed
    Exclude,
    Replace,
};

// Tree of filters and filter groups, shared by all the tabs of a window.
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

    // Applies the configured keyboard shortcuts
    void updateShortcuts();

  Q_SIGNALS:
    void filtersChanged();
    void searchFragmentRequested( const std::vector<FilterNode>& filters, FilterSearchMode mode );
    void findRequested( const std::vector<FilterNode>& filters, bool backward );

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
    void saveFileAs();
    void importTatFile();
    void importKloggSettings();
    void showContextMenu( const QPoint& pos );
    void showHeaderMenu( const QPoint& pos );
    void requestSearchFragment( FilterSearchMode mode );
    void requestFind( bool backward );
    void onTreeChanged();

  private:
    void setupUi();
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

    void scheduleChange();
    void publishChange();
    void loadFile( const QString& path, bool remember = true );
    void saveCurrentFile();
    void updateFileLabel();
    void addRecentFile( const QString& path );
    void updateHitCounts();

  private:
    QTreeWidget* tree_;
    QLabel* fileLabel_;
    QTimer changeTimer_;

    QAction* suspendAction_;
    QAction* findNextAction_;
    QAction* findPreviousAction_;
    QAction* addFilterAction_;
    QAction* editAction_;
    QAction* deleteAction_;
    QAction* moveUpAction_;
    QAction* moveDownAction_;

    // Per item filter properties (the tree structure and check states
    // are held by the QTreeWidget itself)
    std::unordered_map<QString, FilterNode> nodes_;

    QString currentFile_;
    FilterHits hits_;
    bool loading_ = false;
    uint64_t revision_ = 0;
    std::shared_ptr<const FilterSnapshot> snapshot_;
};

#endif
