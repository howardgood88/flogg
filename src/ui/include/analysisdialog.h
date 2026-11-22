/*
 * Copyright (C) 2026 flogg contributors
 *
 * This file is part of flogg.
 *
 * flogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * flogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with flogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef FLOGG_ANALYSISDIALOG_H
#define FLOGG_ANALYSISDIALOG_H

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <QAbstractItemModel>
#include <QHash>
#include <QDialog>
#include <QFutureWatcher>
#include <QPointer>

#include "atomicflag.h"
#include "linetypes.h"
#include "loganalysis.h"
#include "logtemplates.h"

class CrawlerWidget;
class LogData;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTabWidget;
class QTextBrowser;
class QToolButton;
class QTreeView;
class QTreeWidget;
class QTreeWidgetItem;

// The templates compared, filtered by their status; under each template its first lines
// in each file
class CompareModel : public QAbstractItemModel {
  public:
    enum Column { Status, Template, FirstCount, SecondCount, FirstLine, SecondLine, NbColumns };
    enum Filter { All, Changed, OnlyFirst, OnlySecond, More, Fewer };
    // Reads a line of a side (0 the first file, 1 the other) for the rows of the lines
    using LineReader = std::function<QString( int side, LineNumber line )>;

    using QAbstractItemModel::QAbstractItemModel;

    // The lines of each side give the share shown next to the counts
    void setResults( std::vector<templates::Compared> rows, bool hasSecond,
                     uint64_t firstLines = 0, uint64_t secondLines = 0 );
    void setFilter( Filter filter );
    // The names of the files compared, for the titles and the changes
    void setNames( const QString& first, const QString& second );
    void setLineReader( LineReader reader )
    {
        reader_ = std::move( reader );
    }
    // The template of a top row
    const templates::Compared& compared( int row ) const
    {
        return all_[ shown_[ static_cast<size_t>( row ) ] ];
    }
    // The side and the line of a row under a template, nothing for the others
    std::optional<std::pair<int, LineNumber>> lineAt( const QModelIndex& index ) const;

    QModelIndex index( int row, int column, const QModelIndex& parent = {} ) const override;
    QModelIndex parent( const QModelIndex& child ) const override;
    int rowCount( const QModelIndex& parent = {} ) const override;
    int columnCount( const QModelIndex& parent = {} ) const override;
    QVariant data( const QModelIndex& index, int role ) const override;
    QVariant headerData( int section, Qt::Orientation orientation, int role ) const override;

  private:
    // A row under a template: a line of a side, or the count of the lines not listed
    struct Child {
        int side = 0;
        std::optional<LineNumber> line;
        uint64_t more = 0;
    };
    Child childAt( int row, int child ) const;
    int childCount( int row ) const;

    std::vector<templates::Compared> all_;
    std::vector<size_t> shown_;
    bool hasSecond_ = false;
    uint64_t firstLines_ = 0;
    uint64_t secondLines_ = 0;
    QString firstName_;
    QString secondName_;
    Filter filter_ = All;
    LineReader reader_;
    // The texts of the lines shown, read once
    mutable QHash<quint64, QString> texts_;
};

// Analyses of the log of the current tab, in the background: the time between pairs of
// events, the rules of known problems, the comparison with another tab. One runs at a time;
// the window keeps the log data of the tabs it reads until it is done, so closing a tab
// does not stop it from being read.
class AnalysisDialog : public QDialog {
    Q_OBJECT

  public:
    struct Tab {
        QString name;
        QPointer<CrawlerWidget> crawler;
    };

    struct Context {
        std::function<CrawlerWidget*()> current;
        std::function<std::vector<Tab>()> tabs;
        // Brings the tab to the front before its lines are shown
        std::function<void( CrawlerWidget* )> activate;
    };

    enum Page { Durations, Rules, Compare };

    AnalysisDialog( Context context, QWidget* parent = nullptr );
    ~AnalysisDialog() override;

    void showPage( Page page );
    bool isRunning() const
    {
        return watcher_ != nullptr;
    }

    // Run on the current tab, as the buttons do
    void runDurations( const QString& startPattern, const QString& endPattern );
    void runRules( const QString& rulesFile );
    // The rules of the file in the editor, with a new rule matching the text when given
    void editRules( const QString& textOfNewRule = {} );
    void runCompare( bool lineByLine );

    // For the tests
    QTreeWidget* durationTable() const
    {
        return durationTable_;
    }
    QTreeWidget* rulesTree() const
    {
        return rulesTree_;
    }
    QTreeWidget* diffTree() const
    {
        return diffTree_;
    }
    CompareModel* compareModel() const
    {
        return compareModel_;
    }
    QComboBox* otherTab() const
    {
        return otherTab_;
    }
    QComboBox* firstTab() const
    {
        return firstTab_;
    }
    QComboBox* durationTab() const
    {
        return durationTab_;
    }
    QComboBox* rulesTab() const
    {
        return rulesTab_;
    }
    QToolButton* helpButton() const
    {
        return helpButton_;
    }
    QDialog* helpWindow() const
    {
        return help_;
    }
    QTreeView* compareView() const
    {
        return compareView_;
    }
    QLabel* ruleDetails() const
    {
        return ruleDetails_;
    }
    // The message of the last analysis
    QString message() const;

  protected:
    void showEvent( QShowEvent* event ) override;
    void closeEvent( QCloseEvent* event ) override;
    void changeEvent( QEvent* event ) override;

  private:
    QWidget* createDurationsPage();
    QWidget* createRulesPage();
    QWidget* createComparePage();

    // Runs `work` in the background, then `done` here unless the window is gone
    template <typename Result>
    void start( std::function<Result( AtomicFlag&, const analysis::Progress& )> work,
                std::function<void( const Result& )> done,
                std::vector<std::shared_ptr<LogData>> keep );
    void stop();
    void setRunning( bool running );
    // The message of the page that started the last analysis
    void setMessage( const QString& text, bool error = false );
    void showMessageOf( int page );
    // Whether `page` can start an analysis now; its messages go to it
    bool startsOn( Page page );

    // The tab chosen in the list, told when there is none or it is loading
    CrawlerWidget* chosenCrawler( QComboBox* tabs, const QString& side = {} );
    void refreshTabs();
    // The tabs in the list: the one chosen before while it is open, the current one otherwise
    void fillTabs( QComboBox* list, const std::vector<Tab>& tabs, CrawlerWidget* current );
    // How to use a page, with examples, shown by the "?" next to Stop for the page shown
    void setHelp( Page page, const QString& title, const QString& html );
    void showHelp();
    std::optional<templates::Scope> scopeOf( CrawlerWidget* crawler, int kind,
                                             QString* error ) const;
    void showLinesOf( const QPointer<CrawlerWidget>& crawler, LineNumber first, LineNumber last );

    Context context_;
    QTabWidget* pages_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton* stop_ = nullptr;
    QToolButton* helpButton_ = nullptr;
    std::array<std::pair<QString, QString>, 3> helps_;
    QPointer<QDialog> help_;
    QTextBrowser* helpText_ = nullptr;
    QLabel* message_ = nullptr;
    std::vector<QPushButton*> runButtons_;

    QComboBox* durationTab_ = nullptr;
    QLineEdit* startEdit_ = nullptr;
    QLineEdit* endEdit_ = nullptr;
    QCheckBox* durationCase_ = nullptr;
    QCheckBox* durationRegex_ = nullptr;
    QLabel* durationSummary_ = nullptr;
    QTreeWidget* durationTable_ = nullptr;
    QPointer<CrawlerWidget> durationCrawler_;

    QComboBox* rulesTab_ = nullptr;
    QLineEdit* rulesPath_ = nullptr;
    QTreeWidget* rulesTree_ = nullptr;
    QLabel* ruleDetails_ = nullptr;
    // The rules of the results shown, for their details
    std::vector<analysis::Rule> shownRules_;
    QPointer<CrawlerWidget> rulesCrawler_;

    QComboBox* firstTab_ = nullptr;
    QComboBox* firstScope_ = nullptr;
    QComboBox* otherTab_ = nullptr;
    QComboBox* secondScope_ = nullptr;
    QCheckBox* merge_ = nullptr;
    QComboBox* statusFilter_ = nullptr;
    QComboBox* compareMode_ = nullptr;
    QWidget* filterRow_ = nullptr;
    QStackedWidget* compareResults_ = nullptr;
    QTreeView* compareView_ = nullptr;
    CompareModel* compareModel_ = nullptr;
    QTreeWidget* diffTree_ = nullptr;
    QPointer<CrawlerWidget> firstCrawler_;
    QPointer<CrawlerWidget> secondCrawler_;

    std::array<std::pair<QString, bool>, 3> messages_;
    int messagePage_ = 0;

    std::shared_ptr<AtomicFlag> cancel_;
    QFutureWatcherBase* watcher_ = nullptr;
    std::vector<std::shared_ptr<LogData>> keep_;
};

#endif
