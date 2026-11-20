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

#ifndef KLOGG_RENDERVIEW_H
#define KLOGG_RENDERVIEW_H

#include <QAbstractItemModel>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QPointer>
#include <QTextCursor>
#include <QWidget>

#include <atomic>
#include <functional>
#include <limits>
#include <optional>
#include <memory>
#include <vector>

#include "renderformat.h"
#include "rendermodels.h"

class LogData;
class LogFilteredData;
class MatchHighlightDelegate;
class QCheckBox;
class QAbstractItemView;
class QLineEdit;
class QToolButton;
class QComboBox;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTableView;
class QTextBrowser;
class QTextCodec;
class QTextDocument;
class QTimer;
class QTreeView;

namespace render {

// Result of reading and parsing a whole document in the loading thread
struct Document {
    RenderFormat format = RenderFormat::None;
    QString error;
    // Line (from 1) of the error, 0 when none
    int errorLine = 0;
    // Markdown and HTML, made in the loading thread and handed to the UI thread
    std::shared_ptr<QTextDocument> textDocument;
    bool needsBrowser = false;
    QJsonDocument json;
    std::shared_ptr<const XmlTree> xml;
};

// Text of the file decoded with the codec, empty with the error set when the file is
// larger than the limit or can not be read. Without an encoding chosen by the user, valid
// UTF-8 is read as UTF-8 (preferUtf8).
QString readText( const QString& fileName, QTextCodec* codec, qint64 sizeLimit, QString* error,
                  bool preferUtf8 = false );

// Largest file of the format parsed as a whole: the limit of the options, a quarter of it
// for Markdown and HTML
qint64 sizeLimit( RenderFormat format );

// Reads and parses the file, run in a thread of the pool
Document loadDocument( const QString& fileName, QTextCodec* codec, RenderFormat format,
                       qint64 sizeLimit, bool preferUtf8 = false );

// Matches of a document: start and length in its raw text (the positions of the cursors)
constexpr size_t MaxDocumentMatches = 10000;
struct DocumentSearch {
    std::vector<std::pair<int, int>> matches;
    // More than MaxDocumentMatches
    bool cut = false;
};
// The raw text is QTextDocument::toRawText(), its blocks are searched one by one
DocumentSearch findInDocument( const QString& rawText, const TextMatcher& matcher,
                               const std::atomic_bool* cancelled = nullptr );

// Matches of a search of a tree
struct TreeSearch {
    std::vector<JsonMatch> json;
    std::vector<XmlMatch> xml;
};

} // namespace render

// Render mode of a tab: the file shown as a document (Markdown, HTML), a tree (JSON, XML)
// or a table (CSV, TSV, JSON Lines) instead of its lines. Made when the mode is entered,
// deleted when it is left.
class RenderView : public QWidget {
    Q_OBJECT

  public:
    // Lines of the blocks the matches of a table are counted by
    static constexpr quint64 CountBlockLines = 4096;

    // The file is the one of the log data, attached by the time the view is started
    RenderView( std::shared_ptr<LogData> logData, RenderFormat format, QWidget* parent = nullptr );
    ~RenderView() override;

    // The format shown, the detected one when it was asked to detect it
    RenderFormat format() const
    {
        return format_;
    }

    // Starts reading the file, once it is loaded in the log data
    void start();
    bool isStarted() const
    {
        return started_;
    }

    // Opens the Markdown (made into a page) or the HTML file in the web browser
    void openInBrowser();

    // Copies what is selected (Edit > Copy, Cmd+C: the window takes the key)
    void copy();

    // Shows the section (#anchor of a link), once the document is shown
    void scrollToAnchor( const QString& anchor );

    // The user chose the encoding of the file: kept, valid UTF-8 is not read as UTF-8
    void setEncodingChosen( bool chosen )
    {
        encodingChosen_ = chosen;
    }

    // Search of the text of the find bar: the document, the tree or the lines of the table.
    // The search starts with the first find after the text (or an option) has changed, the
    // next ones go from match to match, wrapping around.
    void showFindBar();
    void hideFindBar();
    void setFindText( const QString& text, bool caseSensitive = false, bool regex = false );
    void findNext();
    void findPrevious();
    // "3 / 120", "No match"...
    QString findStatus() const;
    QString findText() const;
    bool isSearching() const;

    // For the tests
    QWidget* currentViewer() const;
    bool isLoadingDocument() const;
    QString message() const;
    QAbstractItemModel* treeModel() const;
    LineTableModel* tableModel() const;
    QTextDocument* textDocument() const;
    QTableView* table() const
    {
        return table_;
    }

  Q_SIGNALS:
    void formatChanged( RenderFormat format );
    // The user wants the line (from 0) in the log view
    void showLineRequested( qint64 line );
    // A link of the document to a file, and to a section of it (#anchor), or empty
    void openFileRequested( const QString& fileName, const QString& anchor );

  protected:
    bool eventFilter( QObject* watched, QEvent* event ) override;
    void changeEvent( QEvent* event ) override;

  private:
    // The images of the document fit the width of the view
    void findImages();
    void fitImages();
    void showFormat( RenderFormat format );
    void showDocument( render::Document document );
    void showMessage( const QString& text, bool warning = false );
    void showLineTable();
    void logDataLoaded();
    void logDataChanged();
    void copySelectedCells();
    void showTreeMenu( const QPoint& position );
    void updateButtons();

    void find( bool backward );
    // The text or the options changed, or what is shown: the next find searches again
    void resetSearch();
    void searchDocument( const TextMatcher& matcher );
    void showDocumentMatches( const render::DocumentSearch& search );
    // Highlights of the matches of the document around what is shown
    void updateDocumentHighlights();
    void searchTree( const TextMatcher& matcher );
    void searchTable( const TextMatcher& matcher );
    void stepDocument( bool backward );
    void stepTree( bool backward );
    void stepTable( bool backward );
    void tableSearchProgressed( int progress );
    void setFindStatus( const QString& status, bool warning = false );
    // The match shown like the selected text of a document, without selecting its cell
    void showCurrentMatch( QAbstractItemView* view, const QModelIndex& index, int occurrence );
    // Count of the matches of the table, from the block on (the lines appended)
    void startCounting( quint64 fromBlock );
    void countProgressed( int begin, int end );
    // Matches in the lines before the line, empty until the blocks before it are counted
    std::optional<quint64> matchesBeforeLine( quint64 line );
    // Matches of each line of a block (kept for one block)
    const std::vector<int>& lineCountsOfBlock( quint64 block );
    void updateTableStatus();
    // The column is widened and scrolled to show the match, a long cell cuts its text
    void revealOccurrence( QAbstractItemView* view, const QModelIndex& index, int occurrence );
    // Text selected in what is shown, searched by the find bar when it is opened
    QString selectedText() const;
    // Index of the first match of the tree at or after the start of the cell
    std::pair<size_t, bool> treeMatchPosition( const QModelIndex& current ) const;

    // Next row with a match in its cells after (before) the row, -1 for none
    struct RowSearch {
        enum State { Found, Pending, Limit, None } state = None;
        int row = -1;
        quint64 checked = 0;
    };
    // From the lines found by the search of the lines, checked in their cells
    RowSearch nextRowBySearch( int fromRow, bool backward,
                               const std::function<bool( int )>& hasMatch );
    // From the counts of the blocks, every cell scanned (regular expressions with anchors)
    RowSearch nextRowByBlocks( int fromRow, bool backward );

    std::shared_ptr<LogData> logData_;
    QString fileName_;
    RenderFormat format_;
    bool started_ = false;
    bool fileChanged_ = false;
    bool loadingDocument_ = false;
    int errorLine_ = 0;

    QComboBox* formatBox_;
    QCheckBox* headerBox_;
    QLabel* messageLabel_;
    QPushButton* showLineButton_;
    QPushButton* browserButton_;
    QPushButton* reloadButton_;
    QStackedWidget* pages_;
    QLabel* placeholder_;
    QTextBrowser* browser_;
    QTreeView* tree_;
    QWidget* tablePage_;
    QTableView* table_;

    // Kept for the browser while it is shown, deleted after it (QTextBrowser does not own it)
    std::shared_ptr<QTextDocument> textDocument_;
    struct Image {
        int position = 0;
        int length = 0;
        qreal indent = 0;
    };
    std::vector<Image> images_;
    QString pendingAnchor_;
    bool encodingChosen_ = false;
    QTimer* imageFitTimer_ = nullptr;
    QPointer<QAbstractItemModel> treeModel_;
    QPointer<LineTableModel> tableModel_;
    QFutureWatcher<render::Document>* watcher_;
    std::shared_ptr<const XmlTree> xmlTree_;

    QWidget* findBar_;
    QLineEdit* findEdit_;
    QToolButton* caseButton_;
    QToolButton* regexButton_;
    QLabel* findStatus_;
    QToolButton* previousButton_ = nullptr;
    QToolButton* nextButton_ = nullptr;

    // Search done for the find bar, none until the first find
    std::optional<TextMatcher> searched_;
    bool pendingBackward_ = false;
    // Matches of the document, in their order
    std::vector<QTextCursor> documentMatches_;
    bool documentMatchesCut_ = false;
    render::TreeSearch treeMatches_;
    int treeMatch_ = -1;
    QFutureWatcher<render::TreeSearch>* treeSearchWatcher_;
    bool treeSearchRunning_ = false;
    MatchHighlightDelegate* highlightDelegate_;
    std::shared_ptr<std::atomic_bool> treeSearchCancelled_;
    // Search of the lines of the table, by the search threads of flogg
    std::unique_ptr<LogFilteredData> tableSearch_;
    bool tableSearchDone_ = false;
    // The current match of the table: its cell and occurrence in the cell
    QPersistentModelIndex tableMatch_;
    int tableOccurrence_ = 0;
    // The cells are scanned instead of the lines searched
    bool tableCellScan_ = false;
    // A step waits for the blocks before the next match to be counted
    bool pendingTableStep_ = false;
    QPersistentModelIndex shownTreeMatch_;
    // Matches of each block of lines, -1 while not counted
    static constexpr quint64 NoBlock = std::numeric_limits<quint64>::max();
    std::vector<int> blockCounts_;
    quint64 countedLines_ = 0;
    quint64 countFirstBlock_ = 0;
    bool countPending_ = false;
    bool counting_ = false;
    QFutureWatcher<int>* countWatcher_;
    QFutureWatcher<render::DocumentSearch>* documentSearchWatcher_;
    bool documentSearchRunning_ = false;
    // Matches of each line of one block, for the rank of the current match
    quint64 lineCountsBlock_ = NoBlock;
    std::vector<int> lineCounts_;
};

#endif
