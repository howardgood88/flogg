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

#include "analysisdialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTextBrowser>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

#include "crawlerwidget.h"
#include "logdata.h"
#include "persistentinfo.h"
#include "ruleseditor.h"
#include "timestamp.h"

namespace {

constexpr auto StartKey = "analysis/durationStart";
constexpr auto EndKey = "analysis/durationEnd";
constexpr auto CaseKey = "analysis/durationCase";
constexpr auto RulesKey = "analysis/rulesFile";
constexpr auto GeometryKey = "analysis/geometry";
constexpr auto RulesHiddenColumnsKey = "analysis/rulesHiddenColumns";
// Set on a list of tabs once the user chose one
constexpr auto ChosenProperty = "floggTabChosen";

void rememberChoice( QComboBox* list )
{
    QObject::connect( list, QOverload<int>::of( &QComboBox::activated ), list,
                      [ list ]( int ) { list->setProperty( ChosenProperty, true ); } );
}

enum RuleColumn {
    RuleNameColumn,
    RuleKindColumn,
    RuleSeverityColumn,
    RuleFindingColumn,
    RuleTextColumn,
    RuleNoteColumn,
};

// Findings listed per rule, the others counted
constexpr size_t MaxListedFindings = 1000;
// Unchanged lines shown around the changes of a line by line comparison
constexpr size_t DiffContext = 3;

QSettings& settings()
{
    return PersistentInfo::getSettings( app_settings{} );
}

QString lineText( const LogData& logData, LineNumber line )
{
    // Long lines are cut for the lists
    auto text = logData.getLineString( line );
    if ( text.size() > 300 ) {
        text = text.left( 300 ) + QChar( 0x2026 );
    }
    return text;
}

QString duration( int64_t us, int digits )
{
    return timestamps::formatDuration( us, digits, false );
}

enum Role { FirstLineRole = Qt::UserRole + 1, LastLineRole, SideRole, RuleRole };

void setLines( QTreeWidgetItem* item, LineNumber first, LineNumber last, int side = 0 )
{
    item->setData( 0, FirstLineRole, QVariant::fromValue<qulonglong>( first.get() ) );
    item->setData( 0, LastLineRole, QVariant::fromValue<qulonglong>( last.get() ) );
    item->setData( 0, SideRole, side );
}

struct DurationRows {
    analysis::DurationResult result;
    std::vector<QString> startTexts;
    std::vector<QString> openTexts;
};

struct RuleRows {
    analysis::RuleResult result;
    // The text of every line of each finding listed
    std::vector<std::vector<QString>> texts;
};

struct CompareRows {
    std::vector<templates::Compared> rows;
    templates::Table first;
    templates::Table second;
    bool hasSecond = false;
    bool cancelled = false;
};

// A row of the two columns: a line of each side (alike), a line on one side only (the other
// side empty), or lines changed side by side; or a fold of lines alike
struct DiffRow {
    std::optional<LineNumber> first;
    std::optional<LineNumber> second;
    QString firstText;
    QString secondText;
    bool changed = false;
    // Lines alike left out here
    uint64_t gap = 0;
};

struct DiffRows {
    QString error;
    bool cancelled = false;
    std::vector<DiffRow> rows;
    uint64_t added = 0;
    uint64_t removed = 0;
};

} // namespace

//
// CompareModel
//

void CompareModel::setResults( std::vector<templates::Compared> rows, bool hasSecond )
{
    beginResetModel();
    all_ = std::move( rows );
    hasSecond_ = hasSecond;
    shown_.clear();
    texts_.clear();
    endResetModel();
    setFilter( filter_ );
}

void CompareModel::setNames( const QString& first, const QString& second )
{
    firstName_ = first;
    secondName_ = second;
    headerDataChanged( Qt::Horizontal, 0, NbColumns - 1 );
}

void CompareModel::setFilter( Filter filter )
{
    using St = templates::Compared::Status;
    beginResetModel();
    filter_ = filter;
    shown_.clear();
    for ( size_t i = 0; i < all_.size(); ++i ) {
        const auto status = all_[ i ].status;
        const auto keep
            = filter == All || ( filter == Changed && status != St::Same )
              || ( filter == OnlyFirst && status == St::OnlyFirst )
              || ( filter == OnlySecond && status == St::OnlySecond )
              || ( filter == More && status == St::More )
              || ( filter == Fewer && status == St::Fewer );
        if ( keep ) {
            shown_.push_back( i );
        }
    }
    endResetModel();
}

int CompareModel::childCount( int row ) const
{
    const auto& compared = this->compared( row );
    const auto side = []( const std::vector<LineNumber>& lines, uint64_t count ) {
        return static_cast<int>( lines.size() ) + ( count > lines.size() ? 1 : 0 );
    };
    return side( compared.firstLines, compared.firstCount )
           + ( hasSecond_ ? side( compared.secondLines, compared.secondCount ) : 0 );
}

CompareModel::Child CompareModel::childAt( int row, int child ) const
{
    const auto& compared = this->compared( row );
    auto k = static_cast<size_t>( child );
    for ( int side = 0; side < 2; ++side ) {
        const auto& lines = side == 0 ? compared.firstLines : compared.secondLines;
        const auto count = side == 0 ? compared.firstCount : compared.secondCount;
        if ( k < lines.size() ) {
            return { side, lines[ k ], 0 };
        }
        k -= lines.size();
        if ( count > lines.size() ) {
            if ( k == 0 ) {
                return { side, {}, count - lines.size() };
            }
            --k;
        }
    }
    return {};
}

std::optional<std::pair<int, LineNumber>> CompareModel::lineAt( const QModelIndex& index ) const
{
    if ( !index.isValid() || !index.parent().isValid() ) {
        return {};
    }
    const auto child = childAt( index.parent().row(), index.row() );
    if ( !child.line ) {
        return {};
    }
    return std::make_pair( child.side, *child.line );
}

QModelIndex CompareModel::index( int row, int column, const QModelIndex& parent ) const
{
    if ( row < 0 || column < 0 || column >= columnCount( parent ) ) {
        return {};
    }
    if ( !parent.isValid() ) {
        return row < static_cast<int>( shown_.size() ) ? createIndex( row, column, quintptr{ 0 } )
                                                       : QModelIndex{};
    }
    if ( parent.parent().isValid() || row >= childCount( parent.row() ) ) {
        return {};
    }
    // The rows of the lines keep the row of their template, from 1
    return createIndex( row, column, static_cast<quintptr>( parent.row() ) + 1 );
}

QModelIndex CompareModel::parent( const QModelIndex& child ) const
{
    if ( !child.isValid() || child.internalId() == 0 ) {
        return {};
    }
    return createIndex( static_cast<int>( child.internalId() - 1 ), 0, quintptr{ 0 } );
}

int CompareModel::rowCount( const QModelIndex& parent ) const
{
    if ( !parent.isValid() ) {
        return static_cast<int>( shown_.size() );
    }
    if ( parent.parent().isValid() || parent.column() != 0 ) {
        return 0;
    }
    return childCount( parent.row() );
}

int CompareModel::columnCount( const QModelIndex& ) const
{
    return hasSecond_ ? NbColumns : FirstLine + 1;
}

QVariant CompareModel::data( const QModelIndex& index, int role ) const
{
    using St = templates::Compared::Status;
    if ( !index.isValid() ) {
        return {};
    }
    const auto column = index.column();

    if ( index.parent().isValid() ) {
        // A line of the template, or the count of the others
        const auto templateRow = index.parent().row();
        const auto child = childAt( templateRow, index.row() );
        const auto name = child.side == 0 ? firstName_ : secondName_;
        if ( role == Qt::DisplayRole ) {
            switch ( column ) {
            case Status:
                return name;
            case Template: {
                if ( !child.line ) {
                    return QApplication::translate( "AnalysisDialog", "%L1 more lines in %2" )
                        .arg( child.more )
                        .arg( name );
                }
                const auto key = static_cast<quint64>( shown_[ static_cast<size_t>( templateRow ) ] )
                                     * 256
                                 + static_cast<quint64>( index.row() );
                auto text = texts_.find( key );
                if ( text == texts_.end() ) {
                    text = texts_.insert( key, reader_ ? reader_( child.side, *child.line )
                                                       : QString{} );
                }
                return *text;
            }
            case FirstLine:
                return child.side == 0 && child.line ? QVariant( child.line->get() + 1 )
                                                     : QVariant();
            case SecondLine:
                return child.side == 1 && child.line ? QVariant( child.line->get() + 1 )
                                                     : QVariant();
            default:
                return {};
            }
        }
        if ( role == Qt::ForegroundRole && ( column == Status || !child.line ) ) {
            return QApplication::palette().color( QPalette::PlaceholderText );
        }
        if ( role == Qt::TextAlignmentRole && ( column == FirstLine || column == SecondLine ) ) {
            return static_cast<int>( Qt::AlignRight | Qt::AlignVCenter );
        }
        if ( role == Qt::ToolTipRole && column == Template && child.line ) {
            return data( index, Qt::DisplayRole );
        }
        return {};
    }

    const auto& row = compared( index.row() );
    if ( role == Qt::DisplayRole ) {
        switch ( column ) {
        case Status:
            switch ( row.status ) {
            case St::OnlyFirst:
                return QApplication::translate( "AnalysisDialog", "only in %1" ).arg( firstName_ );
            case St::OnlySecond:
                return QApplication::translate( "AnalysisDialog", "only in %1" ).arg( secondName_ );
            case St::More:
                return QApplication::translate( "AnalysisDialog", "more in %1" ).arg( secondName_ );
            case St::Fewer:
                return QApplication::translate( "AnalysisDialog", "fewer in %1" ).arg( secondName_ );
            case St::Same:
                return {};
            }
            return {};
        case Template:
            return row.text;
        case FirstCount:
            return QString( "%L1" ).arg( row.firstCount );
        case SecondCount:
            return QString( "%L1" ).arg( row.secondCount );
        case FirstLine:
            return row.firstLine ? QVariant( row.firstLine->get() + 1 ) : QVariant();
        case SecondLine:
            return row.secondLine ? QVariant( row.secondLine->get() + 1 ) : QVariant();
        default:
            return {};
        }
    }
    if ( role == Qt::ForegroundRole && column == Status ) {
        switch ( row.status ) {
        case St::OnlySecond:
        case St::More:
            return QColor( 0xd0, 0x30, 0x30 );
        case St::OnlyFirst:
        case St::Fewer:
            return QColor( 0x30, 0x70, 0xd0 );
        default:
            return {};
        }
    }
    if ( role == Qt::TextAlignmentRole && column != Template && column != Status ) {
        return static_cast<int>( Qt::AlignRight | Qt::AlignVCenter );
    }
    if ( role == Qt::ToolTipRole && column == Template ) {
        return row.text;
    }
    return {};
}

QVariant CompareModel::headerData( int section, Qt::Orientation orientation, int role ) const
{
    if ( orientation != Qt::Horizontal || role != Qt::DisplayRole ) {
        return {};
    }
    const auto tr = []( const char* text ) {
        return QApplication::translate( "AnalysisDialog", text );
    };
    switch ( section ) {
    case Status:
        return tr( "Change" );
    case Template:
        return tr( "Template" );
    case FirstCount:
        return hasSecond_ ? firstName_ : tr( "Lines" );
    case SecondCount:
        return secondName_;
    case FirstLine:
        return hasSecond_ ? tr( "First line in %1" ).arg( firstName_ ) : tr( "First line" );
    case SecondLine:
        return tr( "First line in %1" ).arg( secondName_ );
    default:
        return {};
    }
}

//
// AnalysisDialog
//

AnalysisDialog::AnalysisDialog( Context context, QWidget* parent )
    : QDialog( parent )
    , context_( std::move( context ) )
{
    setWindowTitle( tr( "Analyze log" ) );
    setSizeGripEnabled( true );
    // The lists open under them (not around their current item, as on macOS)
    setStyleSheet( "QComboBox { combobox-popup: 0; }" );

    pages_ = new QTabWidget( this );
    pages_->addTab( createDurationsPage(), tr( "Durations" ) );
    pages_->addTab( createRulesPage(), tr( "Rules" ) );
    pages_->addTab( createComparePage(), tr( "Compare" ) );

    progress_ = new QProgressBar( this );
    progress_->setRange( 0, 100 );
    progress_->setVisible( false );
    stop_ = new QPushButton( tr( "Stop" ), this );
    stop_->setEnabled( false );
    stop_->setToolTip( tr( "Ends the analysis running; its results so far are dropped" ) );
    // How to use the page shown, with examples
    helpButton_ = new QToolButton( this );
    helpButton_->setText( "?" );
    helpButton_->setToolTip( tr( "How to use this page, with examples" ) );
    auto helpFont = helpButton_->font();
    helpFont.setBold( true );
    helpButton_->setFont( helpFont );
    helpButton_->setStyleSheet( "QToolButton { border: 1px solid palette(mid); border-radius: 10px; "
                                 "min-width: 18px; max-width: 18px; min-height: 18px; "
                                 "max-height: 18px; padding: 0px; }" );
    connect( helpButton_, &QToolButton::clicked, this, &AnalysisDialog::showHelp );
    connect( stop_, &QPushButton::clicked, this, &AnalysisDialog::stop );
    message_ = new QLabel( this );
    message_->setWordWrap( true );

    auto* bottom = new QHBoxLayout();
    bottom->addWidget( message_, 1 );
    bottom->addWidget( progress_ );
    bottom->addWidget( helpButton_ );
    bottom->addWidget( stop_ );

    auto* layout = new QVBoxLayout( this );
    layout->addWidget( pages_, 1 );
    layout->addLayout( bottom );

    connect( pages_, &QTabWidget::currentChanged, this, &AnalysisDialog::showMessageOf );

    resize( 900, 600 );
    restoreGeometry( settings().value( GeometryKey ).toByteArray() );
}

AnalysisDialog::~AnalysisDialog()
{
    if ( watcher_ ) {
        cancel_->set();
        watcher_->waitForFinished();
    }
}

void AnalysisDialog::showPage( Page page )
{
    pages_->setCurrentIndex( static_cast<int>( page ) );
}

QString AnalysisDialog::message() const
{
    return messages_[ static_cast<size_t>( messagePage_ ) ].first;
}

void AnalysisDialog::showEvent( QShowEvent* event )
{
    refreshTabs();
    QDialog::showEvent( event );
}

void AnalysisDialog::changeEvent( QEvent* event )
{
    // Back in the window: the tabs may have changed meanwhile (not while a list is open)
    if ( event->type() == QEvent::ActivationChange && isActiveWindow()
         && !QApplication::activePopupWidget() && !isRunning() ) {
        refreshTabs();
    }
    QDialog::changeEvent( event );
}

void AnalysisDialog::closeEvent( QCloseEvent* event )
{
    settings().setValue( GeometryKey, saveGeometry() );
    QDialog::closeEvent( event );
}

void AnalysisDialog::setMessage( const QString& text, bool error )
{
    messages_[ static_cast<size_t>( messagePage_ ) ] = { text, error };
    if ( pages_->currentIndex() == messagePage_ ) {
        showMessageOf( messagePage_ );
    }
}

void AnalysisDialog::showMessageOf( int page )
{
    const auto& [ text, error ] = messages_[ static_cast<size_t>( page ) ];
    message_->setText( text );
    message_->setStyleSheet( error ? "color: #d03030" : QString{} );
}

bool AnalysisDialog::startsOn( Page page )
{
    if ( isRunning() ) {
        return false;
    }
    messagePage_ = static_cast<int>( page );
    return true;
}

void AnalysisDialog::setRunning( bool running )
{
    progress_->setVisible( running );
    progress_->setValue( 0 );
    stop_->setEnabled( running );
    for ( auto* button : runButtons_ ) {
        button->setEnabled( !running );
    }
}

void AnalysisDialog::stop()
{
    if ( cancel_ ) {
        cancel_->set();
    }
}

template <typename Result>
void AnalysisDialog::start( std::function<Result( AtomicFlag&, const analysis::Progress& )> work,
                            std::function<void( const Result& )> done,
                            std::vector<std::shared_ptr<LogData>> keep )
{
    if ( watcher_ ) {
        return;
    }
    cancel_ = std::make_shared<AtomicFlag>();
    keep_ = std::move( keep );
    setRunning( true );
    setMessage( tr( "Reading the log..." ) );

    // Progress comes from the threads of the search
    QPointer<QProgressBar> bar = progress_;
    const analysis::Progress progress = [ bar ]( int percent ) {
        QMetaObject::invokeMethod(
            qApp,
            [ bar, percent ] {
                if ( bar && percent > bar->value() ) {
                    bar->setValue( percent );
                }
            },
            Qt::QueuedConnection );
    };

    auto* watcher = new QFutureWatcher<Result>( this );
    watcher_ = watcher;
    connect( watcher, &QFutureWatcher<Result>::finished, this, [ this, watcher, done ] {
        watcher_ = nullptr;
        watcher->deleteLater();
        setRunning( false );
        setMessage( {} );
        done( watcher->result() );
        // Released here, on the thread that owns the log data
        keep_.clear();
    } );
    watcher->setFuture( QtConcurrent::run(
        [ work, cancel = cancel_, progress ] { return work( *cancel, progress ); } ) );
}

CrawlerWidget* AnalysisDialog::chosenCrawler( QComboBox* tabs, const QString& side )
{
    auto* crawler = qobject_cast<CrawlerWidget*>( tabs->currentData().value<QObject*>() );
    if ( !crawler ) {
        setMessage( tr( "Open a file first" ), true );
        return nullptr;
    }
    if ( crawler->isLoading() ) {
        setMessage( side.isEmpty() ? tr( "The file is still loading" )
                                   : tr( "%1 is still loading" ).arg( side ),
                    true );
        return nullptr;
    }
    return crawler;
}

void AnalysisDialog::setHelp( Page page, const QString& title, const QString& html )
{
    helps_[ static_cast<size_t>( page ) ] = { title, html };
}

void AnalysisDialog::showHelp()
{
    const auto& [ title, html ] = helps_[ static_cast<size_t>( pages_->currentIndex() ) ];
    // One window, showing the help of the page it was opened for
    if ( !help_ ) {
        help_ = new QDialog( this );
        help_->setAttribute( Qt::WA_DeleteOnClose );
        helpText_ = new QTextBrowser( help_ );
        helpText_->setOpenExternalLinks( true );
        // The patterns and the lines in the font of the log views
        helpText_->document()->setDefaultStyleSheet(
            QString( "code, pre { font-family: '%1'; }" )
                .arg( QFontDatabase::systemFont( QFontDatabase::FixedFont ).family() ) );
        auto* layout = new QVBoxLayout( help_ );
        layout->addWidget( helpText_ );
        help_->resize( 620, 560 );
    }
    help_->setWindowTitle( tr( "%1 - how to use" ).arg( title ) );
    helpText_->setHtml(
        html
        + tr( "<p><b>Stop</b> (at the bottom right) ends the analysis running: the results found so "
              "far are dropped and the message tells <i>Stopped</i>. One analysis runs at a time; "
              "the buttons that start one wait until it is done or stopped. Closing the window "
              "stops it too.</p>" ) );
    help_->show();
    help_->raise();
    help_->activateWindow();
}

void AnalysisDialog::showLinesOf( const QPointer<CrawlerWidget>& crawler, LineNumber first,
                                  LineNumber last )
{
    if ( !crawler ) {
        setMessage( tr( "The tab of these results was closed" ), true );
        return;
    }
    if ( context_.activate ) {
        context_.activate( crawler );
    }
    crawler->showLines( first, last );
}

//
// Durations
//

QWidget* AnalysisDialog::createDurationsPage()
{
    auto* page = new QWidget( this );
    durationTab_ = new QComboBox( page );
    durationTab_->setSizeAdjustPolicy( QComboBox::AdjustToMinimumContentsLengthWithIcon );
    durationTab_->setMinimumContentsLength( 20 );
    rememberChoice( durationTab_ );
    startEdit_ = new QLineEdit( settings().value( StartKey ).toString(), page );
    startEdit_->setPlaceholderText( tr( "begin request id=(\\d+)" ) );
    endEdit_ = new QLineEdit( settings().value( EndKey ).toString(), page );
    endEdit_->setPlaceholderText( tr( "end request id=(\\d+)" ) );
    durationCase_ = new QCheckBox( tr( "Match case" ), page );
    durationCase_->setChecked( settings().value( CaseKey, false ).toBool() );
    auto* run = new QPushButton( tr( "Measure" ), page );
    run->setDefault( true );
    runButtons_.push_back( run );
    connect( run, &QPushButton::clicked, this,
             [ this ] { runDurations( startEdit_->text(), endEdit_->text() ); } );
    connect( startEdit_, &QLineEdit::returnPressed, run, &QPushButton::click );
    connect( endEdit_, &QLineEdit::returnPressed, run, &QPushButton::click );

    auto* tabRow = new QHBoxLayout();
    tabRow->addWidget( durationTab_, 1 );
    setHelp( Durations, tr( "Durations" ),
        tr( "<p>The time between a start line and an end line, for every pair in the log of "
            "the tab: how long the requests, the connections or the scans take, and which "
            "ones never end.</p>"
            "<p><b>Start</b> and <b>End</b> are regular expressions. The groups in "
            "parentheses pair the lines: an end goes with the latest start that has the same "
            "groups. Without groups an end goes with the latest start; a start never ended is "
            "listed as <i>no end</i> and does not shift the pairs after it.</p>"
            "<p><b>Example</b>: with the lines</p>"
            "<pre>10:00:01.000 request begin id=7\n"
            "10:00:01.100 request begin id=8\n"
            "10:00:01.250 request end id=8\n"
            "10:00:02.000 request end id=7</pre>"
            "<p>Start <code>request begin id=(\\d+)</code> and End "
            "<code>request end id=(\\d+)</code> give two pairs: id 8 in 150 ms and id 7 in "
            "1 s. Without the groups (<code>request begin</code>, <code>request end</code>) the "
            "first end would close the start of id 8, the latest one, too.</p>"
            "<p>The results: the number of pairs, the mean, the median, the 90 and 99 "
            "percentiles, the 1000 slowest pairs and the starts without an end. Double click "
            "the start or the end line to show it, another column to select both lines: the "
            "tool bar then shows the time between them.</p>"
            "<p>The time stamps compared are the ones chosen for the tab in the right click "
            "of the time between lines (the first of the line by default).</p>" ) );

    auto* form = new QFormLayout();
    form->addRow( tr( "Tab:" ), tabRow );
    form->addRow( tr( "Start:" ), startEdit_ );
    form->addRow( tr( "End:" ), endEdit_ );
    auto* options = new QHBoxLayout();
    options->addWidget( durationCase_ );
    options->addStretch();
    options->addWidget( run );
    form->addRow( options );

    durationSummary_ = new QLabel( page );
    durationSummary_->setWordWrap( true );
    durationSummary_->setTextInteractionFlags( Qt::TextSelectableByMouse );

    durationTable_ = new QTreeWidget( page );
    durationTable_->setRootIsDecorated( false );
    durationTable_->setUniformRowHeights( true );
    durationTable_->setHeaderLabels(
        { tr( "Duration" ), tr( "Start line" ), tr( "End line" ), tr( "Key" ), tr( "Start" ) } );
    durationTable_->setToolTip( tr( "Double click the start or the end line to show it, another "
                                    "column to select both lines" ) );
    connect( durationTable_, &QTreeWidget::itemActivated, this,
             [ this ]( QTreeWidgetItem* item, int column ) {
                 const LineNumber first( item->data( 0, FirstLineRole ).toULongLong() );
                 const LineNumber last( item->data( 0, LastLineRole ).toULongLong() );
                 if ( column == 1 ) {
                     showLinesOf( durationCrawler_, first, first );
                 }
                 else if ( column == 2 ) {
                     showLinesOf( durationCrawler_, last, last );
                 }
                 else {
                     showLinesOf( durationCrawler_, first, last );
                 }
             } );

    auto* layout = new QVBoxLayout( page );
    layout->addLayout( form );
    layout->addWidget( durationSummary_ );
    layout->addWidget( durationTable_, 1 );
    return page;
}

void AnalysisDialog::runDurations( const QString& startPattern, const QString& endPattern )
{
    if ( !startsOn( Durations ) ) {
        return;
    }
    refreshTabs();
    auto* crawler = chosenCrawler( durationTab_ );
    if ( !crawler ) {
        return;
    }
    startEdit_->setText( startPattern );
    endEdit_->setText( endPattern );
    settings().setValue( StartKey, startPattern );
    settings().setValue( EndKey, endPattern );
    settings().setValue( CaseKey, durationCase_->isChecked() );

    durationCrawler_ = crawler;
    durationTable_->clear();
    durationSummary_->clear();

    const auto logData = crawler->logData();
    const auto* data = logData.get();
    // The time stamps chosen for the time between lines of the tab
    const analysis::DurationSpec spec{ startPattern, endPattern, durationCase_->isChecked(),
                                       crawler->timeFormat() };
    start<DurationRows>(
        [ data, spec ]( AtomicFlag& cancel, const analysis::Progress& progress ) {
            DurationRows rows;
            rows.result = analysis::measureDurations( *data, spec, cancel, progress );
            for ( const auto& pair : rows.result.slowest ) {
                rows.startTexts.push_back( lineText( *data, pair.start ) );
            }
            for ( const auto& line : rows.result.openStartLines ) {
                rows.openTexts.push_back( lineText( *data, line ) );
            }
            return rows;
        },
        [ this ]( const DurationRows& rows ) {
            const auto& result = rows.result;
            if ( !result.error.isEmpty() ) {
                setMessage( result.error, true );
                return;
            }
            if ( result.cancelled ) {
                setMessage( tr( "Stopped" ) );
                return;
            }

            const auto digits = result.fractionDigits;
            QStringList summary;
            if ( result.pairs > 0 ) {
                summary.append(
                    tr( "<b>Pairs: %L1</b> mean %2, median %3, 90%: %4, 99%: %5, max %6, min %7" )
                        .arg( result.pairs )
                        .arg( duration( static_cast<int64_t>( result.meanUs ), digits ),
                              duration( result.p50Us, digits ), duration( result.p90Us, digits ),
                              duration( result.p99Us, digits ), duration( result.maxUs, digits ),
                              duration( result.minUs, digits ) ) );
                if ( result.sampled ) {
                    summary.append( tr( "(percentiles from a sample of %L1 pairs)" )
                                        .arg( analysis::MaxDurationSamples ) );
                }
            }
            else {
                summary.append( tr( "<b>No pairs found</b>" ) );
            }
            QStringList issues;
            if ( result.openStarts > 0 ) {
                issues.append( tr( "starts without an end: %L1 (listed below)" )
                                   .arg( result.openStarts ) );
            }
            if ( result.unmatchedEnds > 0 ) {
                issues.append( tr( "ends without a start: %L1" ).arg( result.unmatchedEnds ) );
            }
            if ( result.withoutTime > 0 ) {
                issues.append( tr( "pairs without comparable time stamps: %L1" )
                                   .arg( result.withoutTime ) );
            }
            if ( result.negative > 0 ) {
                issues.append( tr( "pairs ending before they start (the clock went back): %L1" )
                                   .arg( result.negative ) );
            }
            if ( !issues.isEmpty() ) {
                auto text = issues.join( ", " );
                text[ 0 ] = text[ 0 ].toUpper();
                summary.append( "<br>" + text );
            }
            durationSummary_->setText( summary.join( " " ) );

            for ( size_t i = 0; i < result.slowest.size(); ++i ) {
                const auto& pair = result.slowest[ i ];
                auto* item = new QTreeWidgetItem(
                    { duration( pair.us, digits ), QString::number( pair.start.get() + 1 ),
                      QString::number( pair.end.get() + 1 ),
                      QString( pair.key ).replace( QChar( 0x1f ), ", " ), rows.startTexts[ i ] } );
                item->setTextAlignment( 0, Qt::AlignRight | Qt::AlignVCenter );
                setLines( item, pair.start, pair.end );
                durationTable_->addTopLevelItem( item );
            }
            for ( size_t i = 0; i < result.openStartLines.size(); ++i ) {
                const auto line = result.openStartLines[ i ];
                auto* item = new QTreeWidgetItem( { tr( "no end" ),
                                                    QString::number( line.get() + 1 ), {}, {},
                                                    rows.openTexts[ i ] } );
                item->setForeground( 0, QColor( 0xd0, 0x30, 0x30 ) );
                item->setTextAlignment( 0, Qt::AlignRight | Qt::AlignVCenter );
                setLines( item, line, line );
                durationTable_->addTopLevelItem( item );
            }
            for ( int c = 0; c < 4; ++c ) {
                durationTable_->resizeColumnToContents( c );
            }
        },
        { logData } );
}

//
// Rules
//

QWidget* AnalysisDialog::createRulesPage()
{
    auto* page = new QWidget( this );
    rulesPath_ = new QLineEdit( settings().value( RulesKey ).toString(), page );
    rulesPath_->setPlaceholderText( tr( "Rule file (.json)" ) );
    auto* browse = new QPushButton( tr( "Open..." ), page );
    connect( browse, &QPushButton::clicked, this, [ this ] {
        const auto fileName = QFileDialog::getOpenFileName(
            this, tr( "Rule file" ), rulesPath_->text(),
            tr( "flogg rules (*.json);;All files (*)" ) );
        if ( !fileName.isEmpty() ) {
            rulesPath_->setText( fileName );
        }
    } );
    auto* example = new QPushButton( tr( "New from example..." ), page );
    example->setToolTip( tr( "Writes an example rule file with one rule of each kind" ) );
    connect( example, &QPushButton::clicked, this, [ this ] {
        const auto fileName = QFileDialog::getSaveFileName(
            this, tr( "New rule file" ), rulesPath_->text().isEmpty() ? "rules.json" : rulesPath_->text(),
            tr( "flogg rules (*.json);;All files (*)" ) );
        if ( fileName.isEmpty() ) {
            return;
        }
        QSaveFile file( fileName );
        if ( !file.open( QIODevice::WriteOnly ) || file.write( analysis::exampleRules() ) < 0
             || !file.commit() ) {
            messagePage_ = Rules;
            setMessage( tr( "Can not write %1" ).arg( fileName ), true );
            return;
        }
        rulesPath_->setText( fileName );
        QDesktopServices::openUrl( QUrl::fromLocalFile( fileName ) );
    } );
    example->setText( tr( "Example..." ) );
    auto* edit = new QPushButton( tr( "Edit..." ), page );
    edit->setToolTip( tr( "Add and change the rules of the file in a form" ) );
    connect( edit, &QPushButton::clicked, this, [ this ] { editRules(); } );
    auto* newFile = new QPushButton( tr( "New..." ), page );
    newFile->setToolTip( tr( "Starts a new rule file in the form" ) );
    connect( newFile, &QPushButton::clicked, this, [ this ] {
        rulesPath_->clear();
        editRules();
    } );
    auto* run = new QPushButton( tr( "Check" ), page );
    runButtons_.push_back( run );
    connect( run, &QPushButton::clicked, this, [ this ] { runRules( rulesPath_->text() ); } );

    rulesTab_ = new QComboBox( page );
    rulesTab_->setSizeAdjustPolicy( QComboBox::AdjustToMinimumContentsLengthWithIcon );
    rulesTab_->setMinimumContentsLength( 20 );
    rememberChoice( rulesTab_ );
    auto* tabRow = new QHBoxLayout();
    tabRow->addWidget( new QLabel( tr( "Tab:" ), page ) );
    tabRow->addWidget( rulesTab_, 1 );
    setHelp( Rules, tr( "Rules" ),
        tr( "<p>Known problems, written once in a rule file (JSON, shared like the filter "
            "files) and looked for in the log of the tab with <b>Check</b>. A rule is one of "
            "four kinds:</p>"
            "<ul>"
            "<li><b>match</b>: a line, e.g. <code>FATAL EXCEPTION|Segmentation fault</code>.</li>"
            "<li><b>sequence</b>: lines in this order, within a time when one is given, e.g. "
            "<code>login user=(\\w+)</code> then <code>logout user=(\\w+)</code> within 2 s: "
            "a user logged out right away.</li>"
            "<li><b>absence</b>: a line not followed by another within the time, e.g. "
            "<code>connect start id=(\\d+)</code> without <code>connected id=(\\d+)</code> "
            "within 5 s: a connection never established.</li>"
            "<li><b>count</b>: a line at least <i>count</i> times within the time, e.g. "
            "<code>retry request=(\\w+)</code> 3 times within 60 s: a retry storm.</li>"
            "</ul>"
            "<p>The groups in parentheses tie the lines of one thing: the same connection id, "
            "the same user. Without groups any lines go together.</p>"
            "<p>A rule of the file:</p>"
            "<pre>{ \"name\": \"Retry storm\", \"type\": \"count\",\n"
            "  \"severity\": \"warning\",\n"
            "  \"patterns\": [ \"retry request=(\\\\w+)\" ],\n"
            "  \"count\": 3, \"withinSeconds\": 60,\n"
            "  \"note\": \"See https://wiki/retries\" }</pre>"
            "<p><b>Edit...</b> changes the rules of the file in a form, <b>New...</b> starts a "
            "new file, <b>Example...</b> writes one rule of each kind. In the log views, "
            "<b>Add as rule...</b> of the right click menu starts a rule from the selected "
            "text.</p>"
            "<p>In the results, double click a line to show it in the log; select a rule to "
            "read its note, its web addresses as links. The right click on the column titles "
            "chooses the columns.</p>" ) );

    auto* top = new QHBoxLayout();
    top->addWidget( new QLabel( tr( "Rules:" ), page ) );
    top->addWidget( rulesPath_, 1 );
    top->addWidget( browse );
    top->addWidget( edit );
    top->addWidget( newFile );
    top->addWidget( example );
    top->addWidget( run );

    rulesTree_ = new QTreeWidget( page );
    rulesTree_->setUniformRowHeights( true );
    rulesTree_->setHeaderLabels( { tr( "Rule / line" ), tr( "Kind" ), tr( "Severity" ),
                                   tr( "Finding" ), tr( "Text" ), tr( "Note" ) } );
    rulesTree_->header()->resizeSection( RuleFindingColumn, 220 );
    rulesTree_->setToolTip( tr( "Double click a line to select it in the log, a rule to open its "
                                "note below; right click the column titles to choose the columns" ) );
    // The columns shown, chosen with the right click on their titles
    rulesTree_->header()->setContextMenuPolicy( Qt::CustomContextMenu );
    connect( rulesTree_->header(), &QWidget::customContextMenuRequested, this,
             [ this ]( const QPoint& pos ) {
                 QMenu menu;
                 for ( int c = 1; c < rulesTree_->columnCount(); ++c ) {
                     auto* action = menu.addAction( rulesTree_->headerItem()->text( c ) );
                     action->setCheckable( true );
                     action->setChecked( !rulesTree_->isColumnHidden( c ) );
                     connect( action, &QAction::toggled, this, [ this, c ]( bool shown ) {
                         rulesTree_->setColumnHidden( c, !shown );
                         QStringList hidden;
                         for ( int h = 1; h < rulesTree_->columnCount(); ++h ) {
                             if ( rulesTree_->isColumnHidden( h ) ) {
                                 hidden.append( QString::number( h ) );
                             }
                         }
                         settings().setValue( RulesHiddenColumnsKey, hidden );
                     } );
                 }
                 menu.exec( rulesTree_->header()->mapToGlobal( pos ) );
             } );
    const auto hiddenColumns
        = settings().value( RulesHiddenColumnsKey, QStringList{ QString::number( RuleNoteColumn ) } )
              .toStringList();
    for ( const auto& column : hiddenColumns ) {
        rulesTree_->setColumnHidden( column.toInt(), true );
    }
    connect( rulesTree_, &QTreeWidget::itemActivated, this, [ this ]( QTreeWidgetItem* item ) {
        if ( !item->data( 0, FirstLineRole ).isValid() ) {
            return;
        }
        showLinesOf( rulesCrawler_, LineNumber( item->data( 0, FirstLineRole ).toULongLong() ),
              LineNumber( item->data( 0, LastLineRole ).toULongLong() ) );
    } );

    // The rule of the row selected: its description and its note, the web addresses in the
    // note as links
    ruleDetails_ = new QLabel( page );
    ruleDetails_->setWordWrap( true );
    ruleDetails_->setOpenExternalLinks( true );
    ruleDetails_->setTextInteractionFlags( Qt::TextBrowserInteraction );
    ruleDetails_->setFrameShape( QFrame::StyledPanel );
    ruleDetails_->setContentsMargins( 6, 4, 6, 4 );
    ruleDetails_->hide();
    connect( rulesTree_, &QTreeWidget::currentItemChanged, this,
             [ this ]( QTreeWidgetItem* item ) {
                 auto* top = item;
                 while ( top && top->parent() ) {
                     top = top->parent();
                 }
                 const auto index = top ? top->data( 0, RuleRole ) : QVariant{};
                 if ( !index.isValid() || index.toInt() >= static_cast<int>( shownRules_.size() ) ) {
                     ruleDetails_->hide();
                     return;
                 }
                 const auto& rule = shownRules_[ static_cast<size_t>( index.toInt() ) ];
                 QString html = QString( "<b>%1</b>" ).arg( rule.name.toHtmlEscaped() );
                 if ( !rule.description.isEmpty() ) {
                     html += " - " + rule.description.toHtmlEscaped();
                 }
                 if ( !rule.note.isEmpty() ) {
                     html += "<br>" + RulesEditor::noteHtml( rule.note );
                 }
                 ruleDetails_->setText( html );
                 ruleDetails_->show();
             } );

    auto* layout = new QVBoxLayout( page );
    layout->addLayout( tabRow );
    layout->addLayout( top );
    layout->addWidget( rulesTree_, 1 );
    layout->addWidget( ruleDetails_ );
    return page;
}

void AnalysisDialog::editRules( const QString& textOfNewRule )
{
    auto* editor = new RulesEditor( rulesPath_->text(), this );
    editor->setAttribute( Qt::WA_DeleteOnClose );
    connect( editor, &RulesEditor::saved, this, [ this ]( const QString& fileName, bool check ) {
        rulesPath_->setText( fileName );
        settings().setValue( RulesKey, fileName );
        if ( check ) {
            showPage( Rules );
            runRules( fileName );
        }
    } );
    if ( !textOfNewRule.isEmpty() ) {
        editor->addRuleFor( textOfNewRule );
    }
    editor->show();
    editor->raise();
}

void AnalysisDialog::runRules( const QString& rulesFile )
{
    if ( !startsOn( Rules ) ) {
        return;
    }
    refreshTabs();
    auto* crawler = chosenCrawler( rulesTab_ );
    if ( !crawler ) {
        return;
    }
    rulesPath_->setText( rulesFile );
    settings().setValue( RulesKey, rulesFile );

    QFile file( rulesFile );
    if ( rulesFile.isEmpty() || !file.open( QIODevice::ReadOnly ) ) {
        setMessage( tr( "Choose a rule file, or make one from the example" ), true );
        return;
    }
    const auto set = analysis::parseRules( file.readAll() );
    if ( !set.error.isEmpty() ) {
        setMessage( set.error, true );
        return;
    }

    rulesCrawler_ = crawler;
    rulesTree_->clear();
    const auto logData = crawler->logData();
    const auto* data = logData.get();
    const auto rules = set.rules;
    const auto timeFormat = crawler->timeFormat();
    start<RuleRows>(
        [ data, rules, timeFormat ]( AtomicFlag& cancel, const analysis::Progress& progress ) {
            RuleRows rows;
            rows.result = analysis::runRules( *data, rules, cancel, progress, timeFormat );
            std::vector<size_t> listed( rules.size(), 0 );
            for ( const auto& finding : rows.result.findings ) {
                std::vector<QString> texts;
                if ( listed[ finding.rule ]++ < MaxListedFindings ) {
                    for ( const auto& line : finding.lines ) {
                        texts.push_back( lineText( *data, line ) );
                    }
                }
                rows.texts.push_back( std::move( texts ) );
            }
            return rows;
        },
        [ this, rules ]( const RuleRows& rows ) {
            const auto& result = rows.result;
            if ( !result.error.isEmpty() ) {
                setMessage( result.error, true );
                return;
            }
            if ( result.cancelled ) {
                setMessage( tr( "Stopped" ) );
                return;
            }

            shownRules_ = rules;
            std::vector<QTreeWidgetItem*> ruleItems;
            size_t found = 0;
            for ( size_t r = 0; r < rules.size(); ++r ) {
                const auto& rule = rules[ r ];
                const auto total = result.totalFindings[ r ];
                found += total > 0 ? 1 : 0;
                auto* item = new QTreeWidgetItem();
                item->setText( RuleNameColumn, tr( "%1 (%L2)" ).arg( rule.name ).arg( total ) );
                item->setText( RuleKindColumn, analysis::ruleTypeName( rule.type ) );
                item->setText( RuleSeverityColumn, rule.severity );
                item->setText( RuleFindingColumn, rule.description );
                item->setText( RuleNoteColumn, QString( rule.note ).replace( '\n', ' ' ) );
                item->setData( 0, RuleRole, static_cast<int>( r ) );
                QStringList tip{ rule.description };
                if ( !rule.note.isEmpty() ) {
                    tip.append( rule.note );
                    tip.append( tr( "Select the rule to click the links of its note" ) );
                }
                for ( int c = 0; c <= RuleNoteColumn; ++c ) {
                    item->setToolTip( c, tip.join( "\n" ) );
                }
                const auto color = total == 0             ? palette().color( QPalette::PlaceholderText )
                                   : rule.severity == "error" ? QColor( 0xd0, 0x30, 0x30 )
                                   : rule.severity == "info"  ? QColor( 0x30, 0x70, 0xd0 )
                                                              : QColor( 0xc0, 0x80, 0x00 );
                item->setForeground( RuleSeverityColumn, color );
                item->setForeground( 0, color );
                auto font = item->font( 0 );
                font.setBold( total > 0 );
                item->setFont( 0, font );
                ruleItems.push_back( item );
            }

            std::vector<size_t> listed( rules.size(), 0 );
            for ( size_t f = 0; f < result.findings.size(); ++f ) {
                const auto& finding = result.findings[ f ];
                if ( listed[ finding.rule ]++ >= MaxListedFindings ) {
                    continue;
                }
                const auto& texts = rows.texts[ f ];
                const auto& rule = rules[ finding.rule ];
                auto* child = new QTreeWidgetItem( ruleItems[ finding.rule ] );
                child->setText( RuleFindingColumn, finding.detail );
                setLines( child, finding.lines.front(), finding.lines.back() );
                if ( finding.lines.size() == 1 ) {
                    child->setText( RuleNameColumn,
                                    tr( "Line %1" ).arg( finding.lines.front().get() + 1 ) );
                    child->setText( RuleTextColumn, texts.empty() ? QString{} : texts.front() );
                    continue;
                }
                // Several lines (a sequence, an absence, a count): one row each under it
                child->setText( RuleNameColumn, tr( "Lines %1 - %2 (%3)" )
                                                    .arg( finding.lines.front().get() + 1 )
                                                    .arg( finding.lines.back().get() + 1 )
                                                    .arg( finding.lines.size() ) );
                child->setText( RuleTextColumn, texts.empty() ? QString{} : texts.front() );
                for ( size_t l = 0; l < finding.lines.size(); ++l ) {
                    const auto line = finding.lines[ l ];
                    auto* lineItem = new QTreeWidgetItem( child );
                    lineItem->setText( RuleNameColumn, tr( "Line %1" ).arg( line.get() + 1 ) );
                    if ( l < texts.size() ) {
                        lineItem->setText( RuleTextColumn, texts[ l ] );
                    }
                    if ( rule.type == analysis::Rule::Type::Sequence
                         && l < static_cast<size_t>( rule.patterns.size() ) ) {
                        lineItem->setText( RuleFindingColumn, rule.patterns[ static_cast<int>( l ) ] );
                    }
                    else if ( rule.type == analysis::Rule::Type::Count ) {
                        lineItem->setText( RuleFindingColumn, tr( "#%1" ).arg( l + 1 ) );
                    }
                    setLines( lineItem, line, line );
                }
            }
            for ( size_t r = 0; r < rules.size(); ++r ) {
                const auto total = result.totalFindings[ r ];
                if ( total > MaxListedFindings ) {
                    new QTreeWidgetItem( ruleItems[ r ],
                                         { tr( "%L1 more" ).arg( total - MaxListedFindings ) } );
                }
            }

            // The rules with findings first
            std::stable_sort( ruleItems.begin(), ruleItems.end(), []( auto* a, auto* b ) {
                return ( a->childCount() > 0 ) > ( b->childCount() > 0 );
            } );
            for ( auto* item : ruleItems ) {
                rulesTree_->addTopLevelItem( item );
                item->setExpanded( item->childCount() > 0 && item->childCount() <= 20 );
            }
            rulesTree_->resizeColumnToContents( 0 );
            setMessage( tr( "Rules that found something: %1 of %2" ).arg( found ).arg( rules.size() ) );
        },
        { logData } );
}

//
// Compare
//

QWidget* AnalysisDialog::createComparePage()
{
    auto* page = new QWidget( this );
    const QStringList scopes
        = { tr( "Whole file" ), tr( "Lines of the filtered view" ), tr( "Selected lines" ) };
    firstTab_ = new QComboBox( page );
    firstTab_->setSizeAdjustPolicy( QComboBox::AdjustToMinimumContentsLengthWithIcon );
    firstTab_->setMinimumContentsLength( 20 );
    rememberChoice( firstTab_ );
    connect( firstTab_, QOverload<int>::of( &QComboBox::activated ), this,
             [ this ]( int ) { refreshTabs(); } );
    firstScope_ = new QComboBox( page );
    firstScope_->addItems( scopes );
    otherTab_ = new QComboBox( page );
    otherTab_->setSizeAdjustPolicy( QComboBox::AdjustToMinimumContentsLengthWithIcon );
    otherTab_->setMinimumContentsLength( 20 );
    secondScope_ = new QComboBox( page );
    secondScope_->addItems( scopes );
    merge_ = new QCheckBox( tr( "Group lines that differ by one word" ), page );
    merge_->setToolTip( tr( "Numbers, ids and quoted text are already left out of the templates. "
                            "With this, lines of 4 words or more that differ by one more word "
                            "(a user name, a host, a state) count as one template, e.g. \"login "
                            "user alice ok\" and \"login user bob ok\" become \"login user <*> "
                            "ok\". Off, they are counted apart." ) );

    compareMode_ = new QComboBox( page );
    compareMode_->addItem( tr( "Templates" ) );
    compareMode_->addItem( tr( "Line by line" ) );
    compareMode_->setItemData( 0, tr( "Counts the lines of each kind in both: what is new, gone, "
                                      "more or less frequent" ),
                               Qt::ToolTipRole );
    compareMode_->setItemData(
        1, tr( "Up to %L1 lines on each side, times and numbers left out, in two columns" )
               .arg( templates::MaxDiffLines ),
        Qt::ToolTipRole );
    auto* compareButton = new QPushButton( tr( "Compare" ), page );
    runButtons_.push_back( compareButton );
    connect( compareButton, &QPushButton::clicked, this,
             [ this ] { runCompare( compareMode_->currentIndex() == 1 ); } );
    // Grouping by one word is about templates only
    connect( compareMode_, QOverload<int>::of( &QComboBox::currentIndexChanged ), this,
             [ this ]( int mode ) { merge_->setEnabled( mode == 0 ); } );

    statusFilter_ = new QComboBox( page );
    statusFilter_->addItems( { tr( "All templates" ), tr( "Changed" ), tr( "Only in the first" ),
                               tr( "Only in the other" ), tr( "More in the other" ),
                               tr( "Fewer in the other" ) } );

    auto* form = new QFormLayout();
    auto* firstRow = new QHBoxLayout();
    firstRow->addWidget( firstTab_, 1 );
    firstRow->addWidget( firstScope_ );
    setHelp( Compare, tr( "Compare" ),
        tr( "<p>What differs between two logs, such as a good day and a bad day, or before and "
            "after a change. Choose the tab of each side and its lines: the whole file, the "
            "lines of the filtered view or the selected lines. Pasted text can be compared too: "
            "paste it as a tab (<i>File &gt; Paste from clipboard</i>).</p>"
            "<p><b>Templates</b> turns each line into its kind: the time stamp left out, the "
            "numbers, ids, quoted text and the values of <code>key=value</code> replaced. The "
            "lines</p>"
            "<pre>2026-11-22 09:00:58.261 WARN [http-3] retry request=r77 attempt=2\n"
            "2026-11-22 09:01:01.711 WARN [http-3] retry request=r77 attempt=3</pre>"
            "<p>are both <code>WARN [http-&lt;NUM&gt;] retry request=&lt;*&gt; "
            "attempt=&lt;*&gt;</code>. The kinds of both sides are counted: the ones only in "
            "one file and the ones much more or less frequent (by their share of the lines) "
            "come first; <b>Show:</b> keeps some of them. Open a template to list its first "
            "%L1 lines in each file. Double click a line to show it in its tab, a template to "
            "show its first line (in the tab of the column clicked).</p>"
            "<p><b>Group lines that differ by one word</b> also counts as one the templates of 4 "
            "words or more that differ by one word, e.g. <code>login user alice ok</code> and "
            "<code>login user bob ok</code>.</p>"
            "<p>With <b>(nothing)</b> as the other side, the templates of the first one, the "
            "rarest first: the unusual lines of a log.</p>"
            "<p><b>Line by line</b> puts both sides in two columns like a code review, up to "
            "%L2 lines a side, the times and numbers left out: the lines of one side only in "
            "red and green, the lines alike folded.</p>" )
            .arg( templates::MaxTemplateLines )
            .arg( templates::MaxDiffLines ) );
    form->addRow( tr( "Compare:" ), firstRow );
    auto* other = new QHBoxLayout();
    other->addWidget( otherTab_, 1 );
    other->addWidget( secondScope_ );
    form->addRow( tr( "With:" ), other );
    auto* buttons = new QHBoxLayout();
    buttons->addWidget( merge_ );
    buttons->addStretch();
    buttons->addWidget( compareMode_ );
    buttons->addWidget( compareButton );
    form->addRow( buttons );

    compareModel_ = new CompareModel( this );
    compareView_ = new QTreeView( page );
    compareView_->setUniformRowHeights( true );
    // A double click shows a line, the arrow opens a template
    compareView_->setExpandsOnDoubleClick( false );
    compareView_->setModel( compareModel_ );
    compareView_->setToolTip( tr( "Open a template to list its lines. Double click a line to show "
                                  "it, a template to show its first line in the tab of the "
                                  "column clicked" ) );
    compareModel_->setLineReader( [ this ]( int side, LineNumber line ) {
        const auto& crawler = side == 0 ? firstCrawler_ : secondCrawler_;
        const auto data = crawler ? crawler->logData() : std::shared_ptr<LogData>{};
        return data && line < data->getNbLine() ? lineText( *data, line ) : QString{};
    } );
    connect( statusFilter_, QOverload<int>::of( &QComboBox::currentIndexChanged ), this,
             [ this ]( int index ) {
                 compareModel_->setFilter( static_cast<CompareModel::Filter>( index ) );
             } );
    connect( compareView_, &QTreeView::activated, this, [ this ]( const QModelIndex& index ) {
        if ( index.parent().isValid() ) {
            if ( const auto line = compareModel_->lineAt( index ) ) {
                showLinesOf( line->first == 0 ? firstCrawler_ : secondCrawler_, line->second,
                             line->second );
            }
            return;
        }
        const auto& compared = compareModel_->compared( index.row() );
        const auto inSecond = index.column() == CompareModel::SecondLine
                              || index.column() == CompareModel::SecondCount
                              || !compared.firstLine;
        if ( inSecond && compared.secondLine ) {
            showLinesOf( secondCrawler_, *compared.secondLine, *compared.secondLine );
        }
        else if ( compared.firstLine ) {
            showLinesOf( firstCrawler_, *compared.firstLine, *compared.firstLine );
        }
    } );

    // Two columns, like a code review: the first tab on the left, the other on the right
    diffTree_ = new QTreeWidget( page );
    diffTree_->setRootIsDecorated( false );
    diffTree_->setUniformRowHeights( true );
    diffTree_->setHeaderLabels( { tr( "Line" ), {}, tr( "Line" ), {} } );
    diffTree_->setToolTip( tr( "Double click a side to show its line in its tab" ) );
    connect( diffTree_, &QTreeWidget::itemActivated, this,
             [ this ]( QTreeWidgetItem* item, int column ) {
                 const auto first = item->data( 0, Qt::UserRole );
                 const auto second = item->data( 2, Qt::UserRole );
                 const auto right = column >= 2;
                 if ( ( right && second.isValid() ) || ( !first.isValid() && second.isValid() ) ) {
                     const LineNumber line( second.toULongLong() );
                     showLinesOf( secondCrawler_, line, line );
                 }
                 else if ( first.isValid() ) {
                     const LineNumber line( first.toULongLong() );
                     showLinesOf( firstCrawler_, line, line );
                 }
             } );

    compareResults_ = new QStackedWidget( page );
    compareResults_->addWidget( compareView_ );
    compareResults_->addWidget( diffTree_ );

    // Above the results, on the right: which templates to show, for the templates only
    filterRow_ = new QWidget( page );
    auto* filterLayout = new QHBoxLayout( filterRow_ );
    filterLayout->setContentsMargins( 0, 0, 0, 0 );
    filterLayout->addStretch();
    filterLayout->addWidget( new QLabel( tr( "Show:" ), filterRow_ ) );
    filterLayout->addWidget( statusFilter_ );
    filterRow_->hide();

    auto* layout = new QVBoxLayout( page );
    layout->addLayout( form );
    layout->addWidget( filterRow_ );
    layout->addWidget( compareResults_, 1 );
    return page;
}

void AnalysisDialog::fillTabs( QComboBox* list, const std::vector<Tab>& tabs,
                               CrawlerWidget* current )
{
    // The tab chosen in the list stays while it is open; until one is chosen, the list
    // follows the current tab
    const auto previous = list->property( ChosenProperty ).toBool()
                              ? list->currentData().value<QObject*>()
                              : nullptr;
    QSignalBlocker blocker( list );
    list->clear();
    int selected = -1;
    int currentIndex = -1;
    for ( const auto& tab : tabs ) {
        if ( !tab.crawler ) {
            continue;
        }
        list->addItem( tab.name, QVariant::fromValue<QObject*>( tab.crawler.data() ) );
        if ( tab.crawler == previous ) {
            selected = list->count() - 1;
        }
        if ( tab.crawler == current ) {
            currentIndex = list->count() - 1;
        }
    }
    if ( selected < 0 ) {
        selected = currentIndex >= 0 ? currentIndex : ( list->count() > 0 ? 0 : -1 );
    }
    list->setCurrentIndex( selected );
}

void AnalysisDialog::refreshTabs()
{
    if ( !otherTab_ ) {
        return;
    }
    const auto tabs = context_.tabs ? context_.tabs() : std::vector<Tab>{};
    auto* current = context_.current ? context_.current() : nullptr;
    fillTabs( durationTab_, tabs, current );
    fillTabs( rulesTab_, tabs, current );
    // The first side: the tab chosen before while it is open, the current one otherwise
    fillTabs( firstTab_, tabs, current );
    auto* first = firstTab_->currentData().value<QObject*>();

    // The other side: any other tab
    const auto previous = otherTab_->currentData().value<QObject*>();
    QSignalBlocker blocker( otherTab_ );
    otherTab_->clear();
    otherTab_->addItem( tr( "(nothing: the templates of the first tab)" ),
                        QVariant::fromValue<QObject*>( nullptr ) );
    int selected = 0;
    for ( const auto& tab : tabs ) {
        if ( !tab.crawler || tab.crawler == first ) {
            continue;
        }
        otherTab_->addItem( tab.name, QVariant::fromValue<QObject*>( tab.crawler.data() ) );
        if ( tab.crawler == previous || ( !previous && selected == 0 ) ) {
            selected = otherTab_->count() - 1;
        }
    }
    otherTab_->setCurrentIndex( selected );
}

std::optional<templates::Scope> AnalysisDialog::scopeOf( CrawlerWidget* crawler, int kind,
                                                         QString* error ) const
{
    templates::Scope scope;
    if ( kind == 1 ) {
        scope.lines = crawler->filteredViewLines();
        if ( scope.lines->isEmpty() ) {
            *error = tr( "The filtered view is empty" );
            return {};
        }
    }
    else if ( kind == 2 ) {
        const auto bounds = crawler->selectedLineBounds();
        if ( !bounds ) {
            *error = tr( "No lines are selected" );
            return {};
        }
        scope.range = bounds;
    }
    return scope;
}

void AnalysisDialog::runCompare( bool lineByLine )
{
    if ( !startsOn( Compare ) ) {
        return;
    }
    refreshTabs();
    auto* first = qobject_cast<CrawlerWidget*>( firstTab_->currentData().value<QObject*>() );
    if ( !first ) {
        setMessage( tr( "Open a file first" ), true );
        return;
    }
    if ( first->isLoading() ) {
        setMessage( tr( "The file is still loading" ), true );
        return;
    }
    auto* second = qobject_cast<CrawlerWidget*>( otherTab_->currentData().value<QObject*>() );
    if ( lineByLine && !second ) {
        setMessage( tr( "Choose the tab to compare with" ), true );
        return;
    }
    if ( second && second->isLoading() ) {
        setMessage( tr( "The other file is still loading" ), true );
        return;
    }

    QString error;
    const auto firstScope = scopeOf( first, firstScope_->currentIndex(), &error );
    if ( !firstScope ) {
        setMessage( error, true );
        return;
    }
    std::optional<templates::Scope> secondScope;
    if ( second ) {
        secondScope = scopeOf( second, secondScope_->currentIndex(), &error );
        if ( !secondScope ) {
            setMessage( tr( "Other tab: %1" ).arg( error ), true );
            return;
        }
    }

    firstCrawler_ = first;
    secondCrawler_ = second;
    // The results name the files, the tabs may change meanwhile
    const auto firstName = firstTab_->currentText();
    const auto secondName = second ? otherTab_->currentText() : QString{};
    compareModel_->setNames( firstName, secondName );
    statusFilter_->setItemText( 2, tr( "Only in %1" ).arg( firstName ) );
    statusFilter_->setItemText( 3, tr( "Only in %1" ).arg( second ? secondName : tr( "the other" ) ) );
    statusFilter_->setItemText( 4, tr( "More in %1" ).arg( second ? secondName : tr( "the other" ) ) );
    statusFilter_->setItemText( 5, tr( "Fewer in %1" ).arg( second ? secondName : tr( "the other" ) ) );
    const auto firstData = first->logData();
    const auto secondData = second ? second->logData() : std::shared_ptr<LogData>{};
    const auto* a = firstData.get();
    const auto* b = secondData.get();

    if ( !lineByLine ) {
        const auto merge = merge_->isChecked();
        compareResults_->setCurrentWidget( compareView_ );
        filterRow_->show();
        compareModel_->setResults( {}, b != nullptr );
        start<CompareRows>(
            [ a, b, scopeA = *firstScope, scopeB = secondScope,
              merge ]( AtomicFlag& cancel, const analysis::Progress& progress ) {
                CompareRows rows;
                rows.hasSecond = b != nullptr;
                rows.first = templates::build( *a, scopeA, cancel, [ &progress, b ]( int p ) {
                    progress( b ? p / 2 : p );
                } );
                if ( b && !cancel ) {
                    rows.second = templates::build(
                        *b, *scopeB, cancel, [ &progress ]( int p ) { progress( 50 + p / 2 ); } );
                }
                rows.cancelled = static_cast<bool>( cancel );
                if ( !rows.cancelled ) {
                    rows.rows = templates::compare( rows.first, b ? &rows.second : nullptr, merge );
                }
                return rows;
            },
            [ this ]( const CompareRows& rows ) {
                if ( rows.cancelled ) {
                    setMessage( tr( "Stopped" ) );
                    return;
                }
                compareModel_->setResults( rows.rows, rows.hasSecond );
                compareView_->resizeColumnToContents( CompareModel::Status );
                compareView_->header()->resizeSection( CompareModel::Template, 460 );
                auto text = rows.hasSecond
                                ? tr( "%L1 templates: %L2 lines in %3, %L4 lines in %5" )
                                      .arg( rows.rows.size() )
                                      .arg( rows.first.lines )
                                      .arg( compareModel_->headerData( CompareModel::FirstCount,
                                                                       Qt::Horizontal,
                                                                       Qt::DisplayRole )
                                                .toString() )
                                      .arg( rows.second.lines )
                                      .arg( compareModel_->headerData( CompareModel::SecondCount,
                                                                       Qt::Horizontal,
                                                                       Qt::DisplayRole )
                                                .toString() )
                                : tr( "%L1 templates in %L2 lines, the rarest first" )
                                      .arg( rows.rows.size() )
                                      .arg( rows.first.lines );
                if ( rows.first.truncated || rows.second.truncated ) {
                    text += tr( " (only the %L1 most frequent kept)" )
                                .arg( templates::MaxTemplates );
                }
                setMessage( text );
            },
            { firstData, secondData } );
        return;
    }

    compareResults_->setCurrentWidget( diffTree_ );
    filterRow_->hide();
    diffTree_->setHeaderLabels( { tr( "Line" ), firstName, tr( "Line" ), secondName } );
    diffTree_->clear();
    start<DiffRows>(
        [ a, b, scopeA = *firstScope, scopeB = *secondScope ]( AtomicFlag& cancel,
                                                               const analysis::Progress& ) {
            DiffRows rows;
            const auto diff = templates::diff( *a, scopeA, *b, scopeB, cancel );
            rows.error = diff.error;
            rows.cancelled = diff.cancelled;
            if ( !diff.error.isEmpty() || diff.cancelled ) {
                return rows;
            }

            // The changes and a few lines around them, the other lines folded
            std::vector<bool> shown( diff.lines.size(), false );
            for ( size_t i = 0; i < diff.lines.size(); ++i ) {
                if ( diff.lines[ i ].op == templates::DiffLine::Op::Same ) {
                    continue;
                }
                const auto from = i > DiffContext ? i - DiffContext : 0;
                const auto to = qMin( diff.lines.size(), i + DiffContext + 1 );
                for ( auto j = from; j < to; ++j ) {
                    shown[ j ] = true;
                }
            }
            using Op = templates::DiffLine::Op;
            uint64_t gap = 0;
            const auto flushGap = [ &rows, &gap ]() {
                if ( gap > 0 ) {
                    DiffRow fold;
                    fold.gap = gap;
                    rows.rows.push_back( fold );
                    gap = 0;
                }
            };
            for ( size_t i = 0; i < diff.lines.size(); ) {
                const auto& line = diff.lines[ i ];
                if ( line.op == Op::Same ) {
                    if ( !shown[ i ] ) {
                        ++gap;
                    }
                    else {
                        flushGap();
                        rows.rows.push_back( { line.first, line.second, lineText( *a, *line.first ),
                                               lineText( *b, *line.second ), false, 0 } );
                    }
                    ++i;
                    continue;
                }
                // A block of changes: the lines of each side next to each other
                flushGap();
                std::vector<LineNumber> removed;
                std::vector<LineNumber> added;
                while ( i < diff.lines.size() && diff.lines[ i ].op != Op::Same ) {
                    if ( diff.lines[ i ].op == Op::Removed ) {
                        removed.push_back( *diff.lines[ i ].first );
                    }
                    else {
                        added.push_back( *diff.lines[ i ].second );
                    }
                    ++i;
                }
                rows.removed += removed.size();
                rows.added += added.size();
                for ( size_t k = 0; k < qMax( removed.size(), added.size() ); ++k ) {
                    DiffRow row;
                    row.changed = true;
                    if ( k < removed.size() ) {
                        row.first = removed[ k ];
                        row.firstText = lineText( *a, removed[ k ] );
                    }
                    if ( k < added.size() ) {
                        row.second = added[ k ];
                        row.secondText = lineText( *b, added[ k ] );
                    }
                    rows.rows.push_back( row );
                }
            }
            flushGap();
            return rows;
        },
        [ this ]( const DiffRows& rows ) {
            if ( !rows.error.isEmpty() ) {
                setMessage( rows.error, true );
                return;
            }
            if ( rows.cancelled ) {
                setMessage( tr( "Stopped" ) );
                return;
            }
            // Light tints, readable in the light and the dark styles
            const QColor removed( 0xe0, 0x40, 0x40, 0x48 );
            const QColor added( 0x30, 0xb0, 0x50, 0x48 );
            const QColor empty( 0x80, 0x80, 0x80, 0x24 );
            for ( const auto& row : rows.rows ) {
                auto* item = new QTreeWidgetItem();
                if ( row.gap > 0 ) {
                    const auto text = tr( "%L1 lines alike" ).arg( row.gap );
                    item->setText( 1, text );
                    item->setText( 3, text );
                    item->setForeground( 1, palette().color( QPalette::PlaceholderText ) );
                    item->setForeground( 3, palette().color( QPalette::PlaceholderText ) );
                    item->setTextAlignment( 1, Qt::AlignCenter );
                    item->setTextAlignment( 3, Qt::AlignCenter );
                    diffTree_->addTopLevelItem( item );
                    continue;
                }
                if ( row.first ) {
                    item->setText( 0, QString::number( row.first->get() + 1 ) );
                    item->setData( 0, Qt::UserRole, QVariant::fromValue<qulonglong>( row.first->get() ) );
                    item->setText( 1, row.firstText );
                }
                if ( row.second ) {
                    item->setText( 2, QString::number( row.second->get() + 1 ) );
                    item->setData( 2, Qt::UserRole, QVariant::fromValue<qulonglong>( row.second->get() ) );
                    item->setText( 3, row.secondText );
                }
                if ( row.changed ) {
                    const auto left = row.first ? removed : empty;
                    const auto right = row.second ? added : empty;
                    item->setBackground( 0, left );
                    item->setBackground( 1, left );
                    item->setBackground( 2, right );
                    item->setBackground( 3, right );
                }
                item->setTextAlignment( 0, Qt::AlignRight | Qt::AlignVCenter );
                item->setTextAlignment( 2, Qt::AlignRight | Qt::AlignVCenter );
                diffTree_->addTopLevelItem( item );
            }
            diffTree_->resizeColumnToContents( 0 );
            diffTree_->resizeColumnToContents( 2 );
            // The two sides share the width
            const auto textWidth = qMax( 200, ( diffTree_->viewport()->width()
                                                - diffTree_->columnWidth( 0 )
                                                - diffTree_->columnWidth( 2 ) ) / 2 );
            diffTree_->setColumnWidth( 1, textWidth );
            setMessage( rows.added + rows.removed == 0
                            ? tr( "The lines are alike (times and numbers left out)" )
                            : tr( "Lines only in %1: %L2, only in %3: %L4" )
                                  .arg( diffTree_->headerItem()->text( 1 ) )
                                  .arg( rows.removed )
                                  .arg( diffTree_->headerItem()->text( 3 ) )
                                  .arg( rows.added ) );
        },
        { firstData, secondData } );
}
