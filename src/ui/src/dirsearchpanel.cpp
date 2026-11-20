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

#include "dirsearchpanel.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMutex>
#include <QPainter>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QTemporaryFile>
#include <QTextLayout>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <atomic>

#include "filetreepanel.h"

namespace {

enum Role {
    PathRole = Qt::UserRole + 1,
    // From 0, on the lines of a file
    LineRole,
    // QList<QPoint>: start and length of the matches of the preview
    RangesRole,
    CountRole,
    // Of a file when it was searched: a replacement in some of its lines checks it did not
    // change since
    SizeRole,
    ModifiedRole,
};

bool isLineItem( const QTreeWidgetItem* item )
{
    return item && item->parent() != nullptr;
}

// English plurals: the translations of klogg do not have the strings of flogg
QString countText( qint64 count, const char* one, const char* many )
{
    return ( count == 1 ? QCoreApplication::translate( "DirSearchPanel", one )
                        : QCoreApplication::translate( "DirSearchPanel", many ) )
        .arg( count );
}

QString matchesInFiles( qint64 matches, qint64 files )
{
    return countText( matches, "%1 match", "%1 matches" ) + QStringLiteral( " " )
           + countText( files, "in %1 file", "in %1 files" );
}

QColor matchColor()
{
    return QColor( 255, 200, 0, 120 );
}

// A file: its name, its folder dimmed and the number of matches at the right. A line: its
// number dimmed and its text, the matches on a yellow background.
class ResultDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint( QPainter* painter, const QStyleOptionViewItem& option,
                const QModelIndex& index ) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption( &opt, index );
        const auto text = opt.text;
        opt.text.clear();
        auto* style = opt.widget ? opt.widget->style() : QApplication::style();
        style->drawControl( QStyle::CE_ItemViewItem, &opt, painter, opt.widget );

        const auto rect = style->subElementRect( QStyle::SE_ItemViewItemText, &opt, opt.widget )
                              .adjusted( 2, 0, -2, 0 );
        const auto selected = opt.state.testFlag( QStyle::State_Selected );
        const auto group = opt.state.testFlag( QStyle::State_Active ) ? QPalette::Active
                                                                     : QPalette::Inactive;
        const auto textColor = opt.palette.color(
            group, selected ? QPalette::HighlightedText : QPalette::Text );
        auto dimColor = textColor;
        dimColor.setAlphaF( 0.6f );

        painter->save();
        painter->setClipRect( rect );
        const QFontMetrics metrics( opt.font );
        const auto baseline = rect.top() + ( rect.height() - metrics.height() ) / 2 + metrics.ascent();
        auto x = rect.left();

        if ( !index.parent().isValid() ) {
            // The count at the right, then the name, then the folder in what is left
            const auto count = QString::number( index.data( CountRole ).toLongLong() );
            const auto countWidth = metrics.horizontalAdvance( count ) + 8;
            painter->setPen( dimColor );
            painter->drawText( QRect( rect.right() - countWidth, rect.top(), countWidth,
                                      rect.height() ),
                               Qt::AlignRight | Qt::AlignVCenter, count );
            const auto nameRight = rect.right() - countWidth - 4;
            painter->setPen( textColor );
            const auto name = metrics.elidedText( text, Qt::ElideMiddle, nameRight - x );
            painter->drawText( x, baseline, name );
            x += metrics.horizontalAdvance( name ) + 6;
            const auto folder = index.data( Qt::ToolTipRole ).toString();
            if ( x < nameRight && !folder.isEmpty() ) {
                painter->setPen( dimColor );
                painter->drawText( x, baseline,
                                   metrics.elidedText( folder, Qt::ElideLeft, nameRight - x ) );
            }
            painter->restore();
            return;
        }

        const auto number = QString::number( index.data( LineRole ).toLongLong() + 1 );
        painter->setPen( dimColor );
        painter->drawText( x, baseline, number );
        x += metrics.horizontalAdvance( number ) + metrics.horizontalAdvance( QLatin1Char( ' ' ) ) * 2;

        // Laid out once per line drawn, the previews are short
        QTextLayout layout( text, opt.font );
        QList<QTextLayout::FormatRange> formats;
        for ( const auto& range : index.data( RangesRole ).value<QList<QPoint>>() ) {
            QTextLayout::FormatRange format;
            format.start = range.x();
            format.length = range.y();
            format.format.setBackground( matchColor() );
            formats.append( format );
        }
        layout.setFormats( formats );
        layout.beginLayout();
        auto line = layout.createLine();
        line.setLineWidth( 1e6 );
        layout.endLayout();
        painter->setPen( textColor );
        layout.draw( painter, QPointF( x, baseline - line.ascent() ) );
        painter->restore();
    }
};

} // namespace

struct DirSearchPanel::SearchJob {
    std::atomic_bool cancelled{ false };
    std::atomic_bool finished{ false };
    std::atomic_int filesDone{ 0 };
    std::atomic_int filesTotal{ -1 };
    QMutex mutex;
    std::vector<dirsearch::FileResult> pending;
    QStringList errors;
};

struct DirSearchPanel::ExportJob {
    std::atomic_bool cancelled{ false };
    std::atomic_bool finished{ false };
    std::atomic_int filesDone{ 0 };
    int filesTotal = 0;
    bool ok = false;
    QString path;
    QString error;
};

struct DirSearchPanel::ReplaceJob {
    std::atomic_bool cancelled{ false };
    std::atomic_bool finished{ false };
    std::atomic_int filesDone{ 0 };
    int filesTotal = 0;
    qint64 replaced = 0;
    QStringList changedFiles;
    QStringList errors;
};

DirSearchPanel::DirSearchPanel( QWidget* parent )
    : QWidget( parent )
    , titleLabel_( new QLabel )
    , findEdit_( new QLineEdit )
    , caseButton_( new QToolButton )
    , regexButton_( new QToolButton )
    , replaceEdit_( new QLineEdit )
    , replaceButton_( new QPushButton( tr( "Replace" ) ) )
    , replaceAllButton_( new QPushButton( tr( "Replace All" ) ) )
    , includeEdit_( new QLineEdit )
    , statusLabel_( new QLabel )
    , stopButton_( new QToolButton )
    , exportButton_( new QToolButton )
    , results_( new QTreeWidget )
    , pollTimer_( new QTimer( this ) )
    , typingTimer_( new QTimer( this ) )
{
    auto* closeButton = new QToolButton;
    closeButton->setText( QStringLiteral( "✕" ) );
    closeButton->setToolTip( tr( "Close the search" ) );
    closeButton->setAutoRaise( true );
    connect( closeButton, &QToolButton::clicked, this, &DirSearchPanel::closeRequested );

    titleLabel_->setTextFormat( Qt::PlainText );
    auto* title = new QHBoxLayout;
    title->setContentsMargins( 0, 0, 0, 0 );
    title->addWidget( titleLabel_, 1 );
    title->addWidget( closeButton );

    findEdit_->setPlaceholderText( tr( "Search" ) );
    findEdit_->setClearButtonEnabled( true );
    caseButton_->setText( QStringLiteral( "Aa" ) );
    caseButton_->setToolTip( tr( "Match case" ) );
    regexButton_->setText( QStringLiteral( ".*" ) );
    regexButton_->setToolTip( tr( "Regular expression; \\1 in the replacement" ) );
    for ( auto* button : { caseButton_, regexButton_ } ) {
        button->setCheckable( true );
        button->setAutoRaise( true );
        button->setFocusPolicy( Qt::NoFocus );
    }
    auto* findRow = new QHBoxLayout;
    findRow->setContentsMargins( 0, 0, 0, 0 );
    findRow->setSpacing( 2 );
    findRow->addWidget( findEdit_, 1 );
    findRow->addWidget( caseButton_ );
    findRow->addWidget( regexButton_ );

    replaceEdit_->setPlaceholderText( tr( "Replace" ) );
    replaceEdit_->setClearButtonEnabled( true );
    replaceButton_->setToolTip( tr( "Replace the matches of the selected results (a file: all "
                                    "of its matches)" ) );
    replaceAllButton_->setToolTip( tr( "Replace every match in the files found" ) );
    auto* replaceRow = new QHBoxLayout;
    replaceRow->setContentsMargins( 0, 0, 0, 0 );
    replaceRow->setSpacing( 2 );
    replaceRow->addWidget( replaceEdit_, 1 );
    replaceRow->addWidget( replaceButton_ );
    replaceRow->addWidget( replaceAllButton_ );

    includeEdit_->setPlaceholderText( tr( "Files to include (e.g. *.log)" ) );
    includeEdit_->setClearButtonEnabled( true );
    includeEdit_->setToolTip( tr( "Patterns of file names separated by spaces or ';', as the "
                                  "file name filter" ) );

    stopButton_->setText( tr( "Stop" ) );
    stopButton_->setAutoRaise( true );
    stopButton_->hide();
    connect( stopButton_, &QToolButton::clicked, this, &DirSearchPanel::stop );
    exportButton_->setText( tr( "Open as Log" ) );
    exportButton_->setToolTip( tr( "Open every line found in a tab, as \"file:line:text\", "
                                   "to filter and read the results like a log" ) );
    exportButton_->setAutoRaise( true );
    connect( exportButton_, &QToolButton::clicked, this, &DirSearchPanel::exportResults );
    statusLabel_->setWordWrap( true );
    statusLabel_->setTextFormat( Qt::PlainText );
    auto* statusRow = new QHBoxLayout;
    statusRow->setContentsMargins( 0, 0, 0, 0 );
    statusRow->addWidget( statusLabel_, 1 );
    statusRow->addWidget( stopButton_ );
    statusRow->addWidget( exportButton_, 0, Qt::AlignTop );

    results_->setHeaderHidden( true );
    results_->setUniformRowHeights( true );
    results_->setRootIsDecorated( true );
    results_->setSelectionMode( QAbstractItemView::ExtendedSelection );
    results_->setItemDelegate( new ResultDelegate( results_ ) );
    results_->setContextMenuPolicy( Qt::CustomContextMenu );
    results_->setTextElideMode( Qt::ElideNone );
    results_->header()->setStretchLastSection( true );
    results_->installEventFilter( this );
    connect( results_, &QTreeWidget::customContextMenuRequested, this,
             &DirSearchPanel::showContextMenu );
    connect( results_, &QTreeWidget::itemActivated, this, [ this ]( QTreeWidgetItem* item ) {
        if ( isLineItem( item ) ) {
            openItem( item, false );
        }
    } );

    auto* layout = new QVBoxLayout( this );
    layout->setContentsMargins( 0, 0, 0, 0 );
    layout->setSpacing( 4 );
    layout->addLayout( title );
    layout->addLayout( findRow );
    layout->addLayout( replaceRow );
    layout->addWidget( includeEdit_ );
    layout->addLayout( statusRow );
    layout->addWidget( results_, 1 );

    // Searched as typed, once the typing stops
    typingTimer_->setSingleShot( true );
    typingTimer_->setInterval( 400 );
    connect( typingTimer_, &QTimer::timeout, this, &DirSearchPanel::startSearch );
    for ( auto* edit : { findEdit_, includeEdit_ } ) {
        connect( edit, &QLineEdit::textChanged, typingTimer_, qOverload<>( &QTimer::start ) );
        connect( edit, &QLineEdit::returnPressed, this, &DirSearchPanel::startSearch );
    }
    for ( auto* button : { caseButton_, regexButton_ } ) {
        connect( button, &QToolButton::toggled, this, &DirSearchPanel::startSearch );
    }
    connect( replaceEdit_, &QLineEdit::returnPressed, this, &DirSearchPanel::replaceSelected );
    connect( replaceButton_, &QPushButton::clicked, this, &DirSearchPanel::replaceSelected );
    connect( replaceAllButton_, &QPushButton::clicked, this, &DirSearchPanel::replaceAll );
    findEdit_->installEventFilter( this );

    pollTimer_->setInterval( 100 );
    connect( pollTimer_, &QTimer::timeout, this, &DirSearchPanel::poll );

    updateStatus();
}

DirSearchPanel::~DirSearchPanel()
{
    // A search stops at the next block, a replacement is let to finish its files
    if ( search_ ) {
        search_->cancelled = true;
    }
    searchFuture_.waitForFinished();
    replaceFuture_.waitForFinished();
    if ( export_ ) {
        export_->cancelled = true;
    }
    exportFuture_.waitForFinished();
}

void DirSearchPanel::setFolder( const QString& folder, const QString& root )
{
    if ( folder != folder_ ) {
        stop();
        clearResults();
    }
    folder_ = folder;
    root_ = root;
    const auto relative = QDir( root ).relativeFilePath( folder );
    const auto shown = relative == QLatin1String( "." ) || relative.isEmpty()
                           ? QDir( folder ).dirName()
                           : QDir::toNativeSeparators( relative );
    titleLabel_->setText( tr( "Search in %1" ).arg( shown ) );
    titleLabel_->setToolTip( QDir::toNativeSeparators( folder ) );
}

void DirSearchPanel::setIncludeText( const QString& text )
{
    const QSignalBlocker blocker( includeEdit_ );
    includeEdit_->setText( text );
}

void DirSearchPanel::focusFind()
{
    findEdit_->setFocus( Qt::ShortcutFocusReason );
    findEdit_->selectAll();
}

dirsearch::Query DirSearchPanel::query() const
{
    dirsearch::Query query;
    query.text = findEdit_->text();
    query.caseSensitive = caseButton_->isChecked();
    query.regex = regexButton_->isChecked();
    return query;
}

void DirSearchPanel::setQuery( const dirsearch::Query& query )
{
    const QSignalBlocker blockCase( caseButton_ );
    const QSignalBlocker blockRegex( regexButton_ );
    const QSignalBlocker blockFind( findEdit_ );
    caseButton_->setChecked( query.caseSensitive );
    regexButton_->setChecked( query.regex );
    findEdit_->setText( query.text );
}

void DirSearchPanel::setReplacement( const QString& text )
{
    replaceEdit_->setText( text );
}

void DirSearchPanel::setModifiedFilesProvider( std::function<QStringList()> provider )
{
    modifiedFilesProvider_ = std::move( provider );
}

void DirSearchPanel::setConfirmHandler( std::function<bool( const QString& )> confirm )
{
    confirm_ = std::move( confirm );
}

void DirSearchPanel::setWarningHandler( std::function<void( const QString& )> warn )
{
    warn_ = std::move( warn );
}

bool DirSearchPanel::isBusy() const
{
    // Until the results of a finished job came to the tree
    return pollTimer_->isActive() || replace_ != nullptr || export_ != nullptr;
}

int DirSearchPanel::fileCount() const
{
    return results_->topLevelItemCount();
}

QString DirSearchPanel::statusText() const
{
    return statusLabel_->text();
}

void DirSearchPanel::stop()
{
    typingTimer_->stop();
    if ( search_ && !search_->finished ) {
        search_->cancelled = true;
        poll();
    }
}

void DirSearchPanel::clearResults()
{
    results_->clear();
    matchCount_ = 0;
    updateStatus();
}

void DirSearchPanel::startSearch()
{
    typingTimer_->stop();
    if ( replace_ && !replace_->finished ) {
        return;
    }
    if ( search_ ) {
        search_->cancelled = true;
    }
    search_.reset();
    clearResults();

    const auto current = query();
    if ( current.text.isEmpty() || folder_.isEmpty() ) {
        updateStatus();
        return;
    }
    const auto pattern = current.pattern();
    if ( !pattern.isValid() ) {
        statusLabel_->setText( tr( "Invalid regular expression: %1" ).arg( pattern.errorString() ) );
        return;
    }
    resultsQuery_ = current;

    auto job = std::make_shared<SearchJob>();
    search_ = job;
    const auto names = FileTreePanel::namePattern( includeEdit_->text() );
    searchFuture_ = QtConcurrent::run( [ job, pattern, names, folder = folder_,
                                         hidden = showHidden_ ] {
        const auto files = dirsearch::listFiles( folder, names, hidden, &job->cancelled );
        job->filesTotal = static_cast<int>( files.size() );
        qint64 kept = 0;
        for ( const auto& file : files ) {
            if ( job->cancelled ) {
                break;
            }
            dirsearch::FileResult result;
            QString error;
            const auto ok = dirsearch::searchFile( file, pattern,
                                                   std::max<qint64>( 0, dirsearch::MaxResultLines - kept ),
                                                   &result, &error, &job->cancelled );
            if ( ok && result.matchCount > 0 ) {
                kept += static_cast<qint64>( result.lines.size() );
                QMutexLocker locker( &job->mutex );
                job->pending.push_back( std::move( result ) );
            }
            else if ( !error.isEmpty() ) {
                QMutexLocker locker( &job->mutex );
                job->errors.append( QDir::toNativeSeparators( file ) + QStringLiteral( ": " ) + error );
            }
            ++job->filesDone;
        }
        job->finished = true;
    } );
    replaceMessage_.clear();
    setBusyLook( true );
    pollTimer_->start();
    updateStatus();
}

void DirSearchPanel::poll()
{
    // Read before the results: once finished, every result is in the queue
    const auto searchDone = !search_ || search_->finished;
    if ( search_ ) {
        std::vector<dirsearch::FileResult> pending;
        {
            QMutexLocker locker( &search_->mutex );
            pending.swap( search_->pending );
        }
        for ( const auto& result : pending ) {
            addResult( result );
        }
    }
    if ( export_ ) {
        if ( export_->finished ) {
            exportFinished();
        }
        else {
            updateStatus();
        }
        return;
    }
    if ( replace_ ) {
        if ( replace_->finished ) {
            replaceFinished();
        }
        else {
            updateStatus();
        }
        return;
    }
    if ( searchDone ) {
        pollTimer_->stop();
        setBusyLook( false );
    }
    updateStatus();
}

void DirSearchPanel::addResult( const dirsearch::FileResult& result )
{
    auto* file = new QTreeWidgetItem;
    const QFileInfo info( result.path );
    file->setText( 0, info.fileName() );
    const auto folder = QDir( root_ ).relativeFilePath( info.absolutePath() );
    file->setData( 0, Qt::ToolTipRole,
                   folder == QLatin1String( "." ) ? QString{} : QDir::toNativeSeparators( folder ) );
    file->setData( 0, PathRole, result.path );
    file->setData( 0, CountRole, result.matchCount );
    file->setData( 0, SizeRole, result.size );
    file->setData( 0, ModifiedRole, result.modified );
    for ( const auto& line : result.lines ) {
        auto* item = new QTreeWidgetItem( file );
        item->setText( 0, line.preview );
        item->setData( 0, LineRole, line.line );
        QList<QPoint> ranges;
        for ( const auto& [ start, length ] : line.ranges ) {
            ranges.append( QPoint( start, length ) );
        }
        item->setData( 0, RangesRole, QVariant::fromValue( ranges ) );
    }
    results_->addTopLevelItem( file );
    file->setExpanded( true );
    matchCount_ += result.matchCount;
}

void DirSearchPanel::updateStatus()
{
    QString text;
    const auto files = results_->topLevelItemCount();
    if ( search_ && !search_->finished ) {
        const auto total = search_->filesTotal.load();
        text = total < 0 ? tr( "Listing the files…" )
                         : tr( "Searching… %1 / %2 files" ).arg( search_->filesDone.load() ).arg( total );
        if ( matchCount_ > 0 ) {
            text += QStringLiteral( "\n" ) + matchesInFiles( matchCount_, files );
        }
    }
    else if ( export_ ) {
        text = tr( "Writing the results… %1 / %2 files" )
                   .arg( export_->filesDone.load() )
                   .arg( export_->filesTotal );
    }
    else if ( replace_ && !replace_->finished ) {
        text = tr( "Replacing… %1 / %2 files" ).arg( replace_->filesDone.load() ).arg( replace_->filesTotal );
    }
    else if ( search_ ) {
        text = matchCount_ == 0 ? tr( "No results" ) : matchesInFiles( matchCount_, files );
        if ( search_->cancelled ) {
            text += QStringLiteral( " " ) + tr( "(stopped)" );
        }
        qint64 shown = 0;
        for ( int i = 0; i < files; ++i ) {
            shown += results_->topLevelItem( i )->childCount();
        }
        if ( shown >= dirsearch::MaxResultLines ) {
            text += QStringLiteral( "\n" )
                    + tr( "The first %1 lines are shown, Replace All replaces every match." )
                          .arg( dirsearch::MaxResultLines );
        }
        QMutexLocker locker( &search_->mutex );
        if ( !search_->errors.isEmpty() ) {
            text += QStringLiteral( "\n" )
                    + countText( search_->errors.size(), "%1 file could not be read",
                                 "%1 files could not be read" );
            statusLabel_->setToolTip( search_->errors.join( '\n' ) );
        }
        else {
            statusLabel_->setToolTip( {} );
        }
    }
    if ( !replaceMessage_.isEmpty() ) {
        text = replaceMessage_ + ( text.isEmpty() ? QString{} : QStringLiteral( "\n" ) + text );
    }
    statusLabel_->setText( text );
    statusLabel_->setVisible( !text.isEmpty() );
    const auto idle = !isBusy();
    replaceButton_->setEnabled( idle && files > 0 );
    replaceAllButton_->setEnabled( idle && files > 0 );
    exportButton_->setEnabled( idle && files > 0 && !exportFolder_.isEmpty() );
}

void DirSearchPanel::setBusyLook( bool busy )
{
    stopButton_->setVisible( busy && search_ && !search_->finished );
}

void DirSearchPanel::openItem( QTreeWidgetItem* item, bool editMode )
{
    if ( !item ) {
        return;
    }
    const auto* file = item->parent() ? item->parent() : item;
    const auto line = item->parent() ? item->data( 0, LineRole ).toLongLong()
                                     : ( file->childCount() > 0
                                             ? file->child( 0 )->data( 0, LineRole ).toLongLong()
                                             : 0 );
    Q_EMIT openRequested( file->data( 0, PathRole ).toString(), line, editMode, resultsQuery_ );
}

void DirSearchPanel::showContextMenu( QPoint pos )
{
    auto* item = results_->itemAt( pos );
    if ( !item ) {
        return;
    }
    if ( !item->isSelected() ) {
        results_->setCurrentItem( item );
    }
    QMenu menu;
    connect( menu.addAction( tr( "Open" ) ), &QAction::triggered, this,
             [ this, item ] { openItem( item, false ); } );
    connect( menu.addAction( tr( "Open in Edit Mode" ) ), &QAction::triggered, this,
             [ this, item ] { openItem( item, true ); } );
    menu.addSeparator();
    auto* replace = menu.addAction( tr( "Replace" ) );
    replace->setEnabled( !isBusy() );
    connect( replace, &QAction::triggered, this, &DirSearchPanel::replaceSelected );
    connect( menu.addAction( tr( "Dismiss" ) ), &QAction::triggered, this, [ this ] {
        for ( auto* selected : results_->selectedItems() ) {
            if ( isLineItem( selected ) && selected->parent()->isSelected() ) {
                continue;
            }
            auto* file = selected->parent() ? selected->parent() : selected;
            if ( selected->parent() ) {
                const auto ranges = selected->data( 0, RangesRole ).value<QList<QPoint>>();
                const auto count = file->data( 0, CountRole ).toLongLong() - ranges.size();
                file->setData( 0, CountRole, count );
                matchCount_ -= ranges.size();
                delete selected;
                if ( file->childCount() == 0 ) {
                    delete file;
                }
            }
            else {
                matchCount_ -= file->data( 0, CountRole ).toLongLong();
                delete file;
            }
        }
        updateStatus();
    } );
    auto* exportAction = menu.addAction( tr( "Open Results as Log" ) );
    exportAction->setEnabled( !isBusy() && !exportFolder_.isEmpty() );
    connect( exportAction, &QAction::triggered, this, &DirSearchPanel::exportResults );
    menu.addSeparator();
    connect( menu.addAction( tr( "Copy" ) ), &QAction::triggered, this, [ this ] {
        QStringList lines;
        for ( auto* selected : results_->selectedItems() ) {
            lines.append( isLineItem( selected )
                              ? selected->text( 0 )
                              : QDir::toNativeSeparators( selected->data( 0, PathRole ).toString() ) );
        }
        QApplication::clipboard()->setText( lines.join( '\n' ) );
    } );
    connect( menu.addAction( tr( "Copy Path" ) ), &QAction::triggered, this, [ item ] {
        const auto* file = item->parent() ? item->parent() : item;
        QApplication::clipboard()->setText(
            QDir::toNativeSeparators( file->data( 0, PathRole ).toString() ) );
    } );
    menu.exec( results_->viewport()->mapToGlobal( pos ) );
}

void DirSearchPanel::replaceSelected()
{
    if ( isBusy() || results_->topLevelItemCount() == 0 ) {
        return;
    }
    auto selected = results_->selectedItems();
    if ( selected.isEmpty() && results_->currentItem() ) {
        selected.append( results_->currentItem() );
    }
    // A file selected: all its matches; else the lines selected in it
    std::vector<std::pair<QString, std::optional<QSet<qint64>>>> targets;
    for ( int i = 0; i < results_->topLevelItemCount(); ++i ) {
        auto* file = results_->topLevelItem( i );
        if ( file->isSelected() || selected.contains( file ) ) {
            targets.emplace_back( file->data( 0, PathRole ).toString(), std::nullopt );
            continue;
        }
        QSet<qint64> lines;
        for ( int j = 0; j < file->childCount(); ++j ) {
            auto* line = file->child( j );
            if ( line->isSelected() || selected.contains( line ) ) {
                lines.insert( line->data( 0, LineRole ).toLongLong() );
            }
        }
        if ( lines.isEmpty() ) {
            continue;
        }
        // The lines are those of the search: the file must not have changed since
        const QFileInfo info( file->data( 0, PathRole ).toString() );
        if ( info.size() != file->data( 0, SizeRole ).toLongLong()
             || info.lastModified() != file->data( 0, ModifiedRole ).toDateTime() ) {
            warn( tr( "%1 changed since the search, search again before replacing in it." )
                      .arg( QDir::toNativeSeparators( info.filePath() ) ) );
            continue;
        }
        targets.emplace_back( info.filePath(), lines );
    }
    replaceIn( targets );
}

void DirSearchPanel::replaceAll()
{
    if ( isBusy() || results_->topLevelItemCount() == 0 ) {
        return;
    }
    const auto files = results_->topLevelItemCount();
    const auto question = tr( "Replace %1 with \"%2\"? The files are written, this can not be "
                              "undone." )
                              .arg( matchesInFiles( matchCount_, files ), replaceEdit_->text() );
    const auto confirmed
        = confirm_ ? confirm_( question )
                   : QMessageBox::question( this, tr( "Replace All" ), question,
                                            QMessageBox::Yes | QMessageBox::Cancel,
                                            QMessageBox::Cancel )
                         == QMessageBox::Yes;
    if ( !confirmed ) {
        return;
    }
    std::vector<std::pair<QString, std::optional<QSet<qint64>>>> targets;
    for ( int i = 0; i < files; ++i ) {
        targets.emplace_back( results_->topLevelItem( i )->data( 0, PathRole ).toString(),
                              std::nullopt );
    }
    replaceIn( targets );
}

void DirSearchPanel::replaceIn(
    const std::vector<std::pair<QString, std::optional<QSet<qint64>>>>& targets )
{
    if ( targets.empty() ) {
        return;
    }
    const auto pattern = resultsQuery_.pattern();
    if ( !pattern.isValid() ) {
        return;
    }

    // Not under the changes of an editor
    QSet<QString> modified;
    if ( modifiedFilesProvider_ ) {
        for ( const auto& file : modifiedFilesProvider_() ) {
            modified.insert( QFileInfo( file ).absoluteFilePath() );
        }
    }
    std::vector<std::pair<QString, std::optional<QSet<qint64>>>> files;
    QStringList skipped;
    for ( const auto& target : targets ) {
        if ( modified.contains( QFileInfo( target.first ).absoluteFilePath() ) ) {
            skipped.append( QDir::toNativeSeparators( target.first ) );
        }
        else {
            files.push_back( target );
        }
    }
    if ( !skipped.isEmpty() ) {
        warn( tr( "These files have changes not saved in the edit mode, nothing was replaced "
                  "in them:" )
              + QStringLiteral( "\n" ) + skipped.join( '\n' ) );
    }
    if ( files.empty() ) {
        return;
    }

    auto job = std::make_shared<ReplaceJob>();
    job->filesTotal = static_cast<int>( files.size() );
    replace_ = job;
    replaceFuture_ = QtConcurrent::run(
        [ job, files, pattern, replacement = replaceEdit_->text(), groups = resultsQuery_.regex ] {
            for ( const auto& [ path, lines ] : files ) {
                QString error;
                const auto count = dirsearch::replaceInFile( path, pattern, replacement, groups,
                                                             lines ? &*lines : nullptr, &error,
                                                             &job->cancelled );
                if ( count < 0 ) {
                    job->errors.append( QDir::toNativeSeparators( path ) + QStringLiteral( ": " )
                                        + error );
                }
                else if ( count > 0 ) {
                    job->replaced += count;
                    job->changedFiles.append( path );
                }
                ++job->filesDone;
            }
            job->finished = true;
        } );
    pollTimer_->start();
    updateStatus();
}

void DirSearchPanel::replaceFinished()
{
    auto job = std::exchange( replace_, nullptr );
    replaceMessage_ = tr( "Replaced %1." ).arg( matchesInFiles( job->replaced, job->changedFiles.size() ) );
    if ( !job->changedFiles.isEmpty() ) {
        Q_EMIT filesChanged( job->changedFiles );
    }
    if ( !job->errors.isEmpty() ) {
        warn( tr( "Some files were not changed:" ) + QStringLiteral( "\n" )
              + job->errors.join( '\n' ) );
    }
    // The results of the files as they are now
    const auto message = replaceMessage_;
    startSearch();
    replaceMessage_ = message;
    updateStatus();
}

QStringList DirSearchPanel::resultFiles() const
{
    QStringList files;
    for ( int i = 0; i < results_->topLevelItemCount(); ++i ) {
        files.append( results_->topLevelItem( i )->data( 0, PathRole ).toString() );
    }
    return files;
}

void DirSearchPanel::exportResults()
{
    if ( isBusy() || results_->topLevelItemCount() == 0 || exportFolder_.isEmpty() ) {
        return;
    }
    const auto pattern = resultsQuery_.pattern();
    if ( !pattern.isValid() ) {
        return;
    }
    // Named after the search, unique
    auto name = resultsQuery_.text.left( 40 );
    name.replace( QRegularExpression( QStringLiteral( "[^\\w.-]+" ) ), QStringLiteral( "_" ) );
    QTemporaryFile file( QDir( exportFolder_ ).filePath( QStringLiteral( "search_%1_XXXXXX.log" )
                                                             .arg( name ) ) );
    file.setAutoRemove( false );
    if ( !file.open() ) {
        warn( tr( "The results can not be written: %1" ).arg( file.errorString() ) );
        return;
    }
    const auto path = file.fileName();
    file.close();

    auto job = std::make_shared<ExportJob>();
    job->path = path;
    const auto files = resultFiles();
    job->filesTotal = static_cast<int>( files.size() );
    export_ = job;
    exportFuture_ = QtConcurrent::run( [ job, files, pattern, root = root_ ] {
        QFile output( job->path );
        if ( !output.open( QIODevice::WriteOnly | QIODevice::Truncate ) ) {
            job->error = output.errorString();
        }
        else {
            job->ok = dirsearch::exportMatches( files, root, pattern, &output, &job->error,
                                                &job->cancelled, &job->filesDone );
            output.close();
        }
        job->finished = true;
    } );
    pollTimer_->start();
    updateStatus();
}

void DirSearchPanel::exportFinished()
{
    auto job = std::exchange( export_, nullptr );
    pollTimer_->stop();
    if ( job->ok ) {
        Q_EMIT exported( job->path );
    }
    else {
        QFile::remove( job->path );
        if ( !job->error.isEmpty() ) {
            warn( tr( "The results can not be written: %1" ).arg( job->error ) );
        }
    }
    updateStatus();
}

void DirSearchPanel::warn( const QString& message )
{
    if ( warn_ ) {
        warn_( message );
        return;
    }
    auto* box = new QMessageBox( QMessageBox::Warning, tr( "Search in Folder" ), message,
                                 QMessageBox::Ok, this );
    box->setAttribute( Qt::WA_DeleteOnClose );
    box->open();
}

bool DirSearchPanel::eventFilter( QObject* watched, QEvent* event )
{
    if ( event->type() == QEvent::KeyPress ) {
        const auto* key = static_cast<QKeyEvent*>( event );
        if ( watched == findEdit_ && key->key() == Qt::Key_Down && results_->topLevelItemCount() ) {
            results_->setFocus();
            if ( !results_->currentItem() ) {
                results_->setCurrentItem( results_->topLevelItem( 0 ) );
            }
            return true;
        }
        if ( key->key() == Qt::Key_Escape && ( watched == findEdit_ || watched == results_ ) ) {
            if ( isBusy() ) {
                stop();
            }
            else {
                Q_EMIT closeRequested();
            }
            return true;
        }
    }
    return QWidget::eventFilter( watched, event );
}
