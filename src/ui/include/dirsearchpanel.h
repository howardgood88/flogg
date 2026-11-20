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

#ifndef KLOGG_DIRSEARCHPANEL_H
#define KLOGG_DIRSEARCHPANEL_H

#include <QFuture>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>

#include "dirsearch.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

// Search in the files of a folder of the file tree panel, with the replacement. Made when
// first used: nothing runs, nothing is kept before. The search and the replacement run in
// a thread of the pool, the results come to the tree every 100 ms.
class DirSearchPanel : public QWidget {
    Q_OBJECT

  public:
    explicit DirSearchPanel( QWidget* parent = nullptr );
    ~DirSearchPanel() override;

    // The folder searched, the paths are shown relative to the root
    void setFolder( const QString& folder, const QString& root );
    QString folder() const
    {
        return folder_;
    }
    void setShowHidden( bool show )
    {
        showHidden_ = show;
    }
    void setIncludeText( const QString& text );
    // The find field, its text selected
    void focusFind();

    dirsearch::Query query() const;
    void setQuery( const dirsearch::Query& query );
    void setReplacement( const QString& text );

    // Searches again (the last one is stopped)
    void startSearch();
    void stop();
    void clearResults();
    bool isBusy() const;

    // The selected results, a file for all its matches; the current one without a selection
    void replaceSelected();
    // After asking
    void replaceAll();

    // Writes the lines with a match of the files found (all of them, not only those shown)
    // to a file of the folder set, as grep -n does, then asks to open it
    void exportResults();
    void setExportFolder( const QString& folder )
    {
        exportFolder_ = folder;
    }

    // The files with changes not saved in the edit mode: not replaced in
    void setModifiedFilesProvider( std::function<QStringList()> provider );
    // For the tests: the question before replacing all, the warnings
    void setConfirmHandler( std::function<bool( const QString& )> confirm );
    void setWarningHandler( std::function<void( const QString& )> warn );

    // For the tests
    QTreeWidget* resultsView() const
    {
        return results_;
    }
    qint64 matchCount() const
    {
        return matchCount_;
    }
    int fileCount() const;
    QString statusText() const;

  Q_SIGNALS:
    // Opens the file at the line (from 0), in the edit mode with the find bar when asked
    void openRequested( const QString& path, qint64 line, bool editMode,
                        const dirsearch::Query& query );
    void closeRequested();
    // Files written by a replacement
    void filesChanged( const QStringList& paths );
    // The results written to a file
    void exported( const QString& path );

  protected:
    bool eventFilter( QObject* watched, QEvent* event ) override;

  private:
    struct SearchJob;
    struct ReplaceJob;
    struct ExportJob;

    void poll();
    void addResult( const dirsearch::FileResult& result );
    void updateStatus();
    void showContextMenu( QPoint pos );
    void openItem( QTreeWidgetItem* item, bool editMode );
    // Path -> lines (empty for the whole file)
    void replaceIn( const std::vector<std::pair<QString, std::optional<QSet<qint64>>>>& targets );
    void replaceFinished();
    void exportFinished();
    QStringList resultFiles() const;
    void warn( const QString& message );
    void setBusyLook( bool busy );

    QString folder_;
    QString root_;
    bool showHidden_ = false;

    QLabel* titleLabel_;
    QLineEdit* findEdit_;
    QToolButton* caseButton_;
    QToolButton* regexButton_;
    QLineEdit* replaceEdit_;
    QPushButton* replaceButton_;
    QPushButton* replaceAllButton_;
    QLineEdit* includeEdit_;
    QLabel* statusLabel_;
    QToolButton* stopButton_;
    QToolButton* exportButton_;
    QTreeWidget* results_;
    QTimer* pollTimer_;
    QTimer* typingTimer_;

    std::shared_ptr<SearchJob> search_;
    QFuture<void> searchFuture_;
    std::shared_ptr<ReplaceJob> replace_;
    QFuture<void> replaceFuture_;
    std::shared_ptr<ExportJob> export_;
    QFuture<void> exportFuture_;
    QString exportFolder_;
    // The query of the results shown
    dirsearch::Query resultsQuery_;
    qint64 matchCount_ = 0;
    // Shown under the results until the next search
    QString replaceMessage_;

    std::function<QStringList()> modifiedFilesProvider_;
    std::function<bool( const QString& )> confirm_;
    std::function<void( const QString& )> warn_;
};

#endif
