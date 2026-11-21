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
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

#include "crawlerwidget.h"
#include "logdata.h"
#include "persistentinfo.h"
#include "timestamp.h"

namespace {

constexpr auto StartKey = "analysis/durationStart";
constexpr auto EndKey = "analysis/durationEnd";
constexpr auto CaseKey = "analysis/durationCase";
constexpr auto RulesKey = "analysis/rulesFile";
constexpr auto GeometryKey = "analysis/geometry";

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

enum Role { FirstLineRole = Qt::UserRole + 1, LastLineRole, SideRole };

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
    std::vector<QString> texts;
};

struct CompareRows {
    std::vector<templates::Compared> rows;
    templates::Table first;
    templates::Table second;
    bool hasSecond = false;
    bool cancelled = false;
};

struct DiffRow {
    templates::DiffLine::Op op = templates::DiffLine::Op::Same;
    std::optional<LineNumber> first;
    std::optional<LineNumber> second;
    QString text;
    // Unchanged lines left out here
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
    endResetModel();
    setFilter( filter_ );
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

int CompareModel::rowCount( const QModelIndex& parent ) const
{
    return parent.isValid() ? 0 : static_cast<int>( shown_.size() );
}

int CompareModel::columnCount( const QModelIndex& parent ) const
{
    return parent.isValid() ? 0 : ( hasSecond_ ? NbColumns : FirstLine + 1 );
}

QVariant CompareModel::data( const QModelIndex& index, int role ) const
{
    using St = templates::Compared::Status;
    if ( !index.isValid() ) {
        return {};
    }
    const auto& row = compared( index.row() );
    const auto column = index.column();

    if ( role == Qt::DisplayRole ) {
        switch ( column ) {
        case Status:
            switch ( row.status ) {
            case St::OnlyFirst:
                return QApplication::translate( "AnalysisDialog", "only here" );
            case St::OnlySecond:
                return QApplication::translate( "AnalysisDialog", "only there" );
            case St::More:
                return QApplication::translate( "AnalysisDialog", "more there" );
            case St::Fewer:
                return QApplication::translate( "AnalysisDialog", "fewer there" );
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
        return hasSecond_ ? tr( "Here" ) : tr( "Lines" );
    case SecondCount:
        return tr( "There" );
    case FirstLine:
        return hasSecond_ ? tr( "First line here" ) : tr( "First line" );
    case SecondLine:
        return tr( "First line there" );
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

    pages_ = new QTabWidget( this );
    pages_->addTab( createDurationsPage(), tr( "Durations" ) );
    pages_->addTab( createRulesPage(), tr( "Rules" ) );
    pages_->addTab( createComparePage(), tr( "Compare" ) );

    progress_ = new QProgressBar( this );
    progress_->setRange( 0, 100 );
    progress_->setVisible( false );
    stop_ = new QPushButton( tr( "Stop" ), this );
    stop_->setEnabled( false );
    connect( stop_, &QPushButton::clicked, this, &AnalysisDialog::stop );
    message_ = new QLabel( this );
    message_->setWordWrap( true );

    auto* bottom = new QHBoxLayout();
    bottom->addWidget( message_, 1 );
    bottom->addWidget( progress_ );
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

CrawlerWidget* AnalysisDialog::currentCrawler( bool tellWhenNone )
{
    auto* crawler = context_.current ? context_.current() : nullptr;
    if ( crawler && crawler->isLoading() ) {
        if ( tellWhenNone ) {
            setMessage( tr( "The file is still loading" ), true );
        }
        return nullptr;
    }
    if ( !crawler && tellWhenNone ) {
        setMessage( tr( "Open a file first" ), true );
    }
    return crawler;
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

    auto* hint = new QLabel(
        tr( "Regular expressions. The groups in parentheses pair the lines: an end goes with "
            "the start that has the same groups; without groups, with the oldest start." ),
        page );
    hint->setWordWrap( true );

    auto* form = new QFormLayout();
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
    durationTable_->setToolTip( tr( "Double click to select the lines in the log" ) );
    connect( durationTable_, &QTreeWidget::itemActivated, this,
             [ this ]( QTreeWidgetItem* item ) {
                 showLinesOf( durationCrawler_,
                       LineNumber( item->data( 0, FirstLineRole ).toULongLong() ),
                       LineNumber( item->data( 0, LastLineRole ).toULongLong() ) );
             } );

    auto* layout = new QVBoxLayout( page );
    layout->addLayout( form );
    layout->addWidget( hint );
    layout->addWidget( durationSummary_ );
    layout->addWidget( durationTable_, 1 );
    return page;
}

void AnalysisDialog::runDurations( const QString& startPattern, const QString& endPattern )
{
    if ( !startsOn( Durations ) ) {
        return;
    }
    auto* crawler = currentCrawler();
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
    const analysis::DurationSpec spec{ startPattern, endPattern, durationCase_->isChecked() };
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
    auto* run = new QPushButton( tr( "Check" ), page );
    runButtons_.push_back( run );
    connect( run, &QPushButton::clicked, this, [ this ] { runRules( rulesPath_->text() ); } );

    auto* top = new QHBoxLayout();
    top->addWidget( new QLabel( tr( "Rules:" ), page ) );
    top->addWidget( rulesPath_, 1 );
    top->addWidget( browse );
    top->addWidget( example );
    top->addWidget( run );

    auto* hint = new QLabel(
        tr( "A rule tells a known problem: a line (match), lines in an order (sequence), a line "
            "not followed by another in time (absence), a line repeated in time (count)." ),
        page );
    hint->setWordWrap( true );

    rulesTree_ = new QTreeWidget( page );
    rulesTree_->setUniformRowHeights( true );
    rulesTree_->setHeaderLabels( { tr( "Rule / line" ), tr( "Finding" ), tr( "Text" ) } );
    rulesTree_->header()->resizeSection( 1, 260 );
    rulesTree_->setToolTip( tr( "Double click a line to select it in the log" ) );
    connect( rulesTree_, &QTreeWidget::itemActivated, this, [ this ]( QTreeWidgetItem* item ) {
        if ( !item->data( 0, FirstLineRole ).isValid() ) {
            const auto link = item->data( 1, Qt::UserRole ).toString();
            if ( !link.isEmpty() ) {
                QDesktopServices::openUrl( QUrl( link ) );
            }
            return;
        }
        showLinesOf( rulesCrawler_, LineNumber( item->data( 0, FirstLineRole ).toULongLong() ),
              LineNumber( item->data( 0, LastLineRole ).toULongLong() ) );
    } );

    auto* layout = new QVBoxLayout( page );
    layout->addLayout( top );
    layout->addWidget( hint );
    layout->addWidget( rulesTree_, 1 );
    return page;
}

void AnalysisDialog::runRules( const QString& rulesFile )
{
    if ( !startsOn( Rules ) ) {
        return;
    }
    auto* crawler = currentCrawler();
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
    start<RuleRows>(
        [ data, rules ]( AtomicFlag& cancel, const analysis::Progress& progress ) {
            RuleRows rows;
            rows.result = analysis::runRules( *data, rules, cancel, progress );
            std::vector<size_t> listed( rules.size(), 0 );
            for ( const auto& finding : rows.result.findings ) {
                rows.texts.push_back( listed[ finding.rule ]++ < MaxListedFindings
                                          ? lineText( *data, finding.line )
                                          : QString{} );
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

            std::vector<QTreeWidgetItem*> ruleItems;
            size_t found = 0;
            for ( size_t r = 0; r < rules.size(); ++r ) {
                const auto& rule = rules[ r ];
                const auto total = result.totalFindings[ r ];
                found += total > 0 ? 1 : 0;
                auto* item = new QTreeWidgetItem(
                    { tr( "%1 (%L2)" ).arg( rule.name ).arg( total ), rule.description } );
                QStringList tip{ rule.description };
                if ( !rule.link.isEmpty() ) {
                    tip.append( tr( "Double click to open %1" ).arg( rule.link ) );
                    item->setData( 1, Qt::UserRole, rule.link );
                }
                item->setToolTip( 0, tip.join( "\n" ) );
                item->setToolTip( 1, tip.join( "\n" ) );
                const auto color = total == 0             ? palette().color( QPalette::PlaceholderText )
                                   : rule.severity == "error" ? QColor( 0xd0, 0x30, 0x30 )
                                   : rule.severity == "info"  ? QColor( 0x30, 0x70, 0xd0 )
                                                              : QColor( 0xc0, 0x80, 0x00 );
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
                QStringList numbers;
                for ( const auto& line : finding.lines ) {
                    if ( numbers.size() == 5 ) {
                        numbers.append( QChar( 0x2026 ) );
                        break;
                    }
                    numbers.append( QString::number( line.get() + 1 ) );
                }
                auto* child = new QTreeWidgetItem( ruleItems[ finding.rule ],
                                                   { tr( "Line %1" ).arg( numbers.join( ", " ) ),
                                                     finding.detail, rows.texts[ f ] } );
                setLines( child, finding.lines.front(), finding.lines.back() );
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
    firstScope_ = new QComboBox( page );
    firstScope_->addItems( scopes );
    otherTab_ = new QComboBox( page );
    otherTab_->setSizeAdjustPolicy( QComboBox::AdjustToMinimumContentsLengthWithIcon );
    otherTab_->setMinimumContentsLength( 20 );
    secondScope_ = new QComboBox( page );
    secondScope_->addItems( scopes );
    merge_ = new QCheckBox( tr( "Merge templates that differ by one word" ), page );
    merge_->setToolTip( tr( "Lines of 4 words or more that differ by one word (a name, a state) "
                            "count as one template" ) );

    auto* compareButton = new QPushButton( tr( "Compare templates" ), page );
    compareButton->setToolTip( tr( "Counts the lines of each kind in both: what is new, gone, "
                                   "more or less frequent" ) );
    auto* diffButton = new QPushButton( tr( "Compare line by line" ), page );
    diffButton->setToolTip( tr( "Up to %L1 lines on each side, times and numbers left out" )
                                .arg( templates::MaxDiffLines ) );
    runButtons_.push_back( compareButton );
    runButtons_.push_back( diffButton );
    connect( compareButton, &QPushButton::clicked, this, [ this ] { runCompare( false ); } );
    connect( diffButton, &QPushButton::clicked, this, [ this ] { runCompare( true ); } );

    statusFilter_ = new QComboBox( page );
    statusFilter_->addItems( { tr( "All templates" ), tr( "Changed" ), tr( "Only here" ),
                               tr( "Only there" ), tr( "More there" ), tr( "Fewer there" ) } );

    auto* form = new QFormLayout();
    form->addRow( tr( "This tab:" ), firstScope_ );
    auto* other = new QHBoxLayout();
    other->addWidget( otherTab_, 1 );
    other->addWidget( secondScope_ );
    form->addRow( tr( "With:" ), other );
    auto* buttons = new QHBoxLayout();
    buttons->addWidget( merge_ );
    buttons->addStretch();
    buttons->addWidget( statusFilter_ );
    buttons->addWidget( compareButton );
    buttons->addWidget( diffButton );
    form->addRow( buttons );

    auto* hint = new QLabel( tr( "Pasted text can be compared too: paste it as a tab "
                                 "(File > Paste from clipboard) and choose that tab." ),
                             page );
    hint->setWordWrap( true );

    compareModel_ = new CompareModel( this );
    compareView_ = new QTreeView( page );
    compareView_->setRootIsDecorated( false );
    compareView_->setUniformRowHeights( true );
    compareView_->setModel( compareModel_ );
    compareView_->setToolTip( tr( "Double click to show the first line of the template, in the "
                                  "tab of the column clicked" ) );
    connect( statusFilter_, QOverload<int>::of( &QComboBox::currentIndexChanged ), this,
             [ this ]( int index ) {
                 compareModel_->setFilter( static_cast<CompareModel::Filter>( index ) );
             } );
    connect( compareView_, &QTreeView::activated, this, [ this ]( const QModelIndex& index ) {
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

    diffTree_ = new QTreeWidget( page );
    diffTree_->setRootIsDecorated( false );
    diffTree_->setUniformRowHeights( true );
    diffTree_->setHeaderLabels( { {}, tr( "Here" ), tr( "There" ), tr( "Line" ) } );
    diffTree_->setToolTip( tr( "Double click to show the line in its tab" ) );
    connect( diffTree_, &QTreeWidget::itemActivated, this, [ this ]( QTreeWidgetItem* item ) {
        const auto first = item->data( 1, Qt::UserRole );
        const auto second = item->data( 2, Qt::UserRole );
        if ( item->data( 0, SideRole ).toInt() == 2 && second.isValid() ) {
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

    auto* layout = new QVBoxLayout( page );
    layout->addLayout( form );
    layout->addWidget( hint );
    layout->addWidget( compareResults_, 1 );
    return page;
}

void AnalysisDialog::refreshTabs()
{
    if ( !otherTab_ ) {
        return;
    }
    const auto previous = otherTab_->currentData().value<QObject*>();
    otherTab_->clear();
    otherTab_->addItem( tr( "(nothing: the templates of this tab)" ),
                        QVariant::fromValue<QObject*>( nullptr ) );
    auto* current = context_.current ? context_.current() : nullptr;
    int selected = 0;
    if ( context_.tabs ) {
        for ( const auto& tab : context_.tabs() ) {
            if ( !tab.crawler || tab.crawler == current ) {
                continue;
            }
            otherTab_->addItem( tab.name, QVariant::fromValue<QObject*>( tab.crawler.data() ) );
            if ( tab.crawler == previous || ( !previous && selected == 0 ) ) {
                selected = otherTab_->count() - 1;
            }
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
    auto* first = currentCrawler();
    if ( !first ) {
        return;
    }
    refreshTabs();
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
    const auto firstData = first->logData();
    const auto secondData = second ? second->logData() : std::shared_ptr<LogData>{};
    const auto* a = firstData.get();
    const auto* b = secondData.get();

    if ( !lineByLine ) {
        const auto merge = merge_->isChecked();
        compareResults_->setCurrentWidget( compareView_ );
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
                compareView_->header()->resizeSection( CompareModel::Status, 90 );
                compareView_->header()->resizeSection( CompareModel::Template, 460 );
                auto text = rows.hasSecond
                                ? tr( "%L1 templates: %L2 lines here, %L3 lines there" )
                                      .arg( rows.rows.size() )
                                      .arg( rows.first.lines )
                                      .arg( rows.second.lines )
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
            uint64_t gap = 0;
            for ( size_t i = 0; i < diff.lines.size(); ++i ) {
                const auto& line = diff.lines[ i ];
                rows.added += line.op == templates::DiffLine::Op::Added ? 1 : 0;
                rows.removed += line.op == templates::DiffLine::Op::Removed ? 1 : 0;
                if ( !shown[ i ] ) {
                    ++gap;
                    continue;
                }
                if ( gap > 0 ) {
                    rows.rows.push_back( { templates::DiffLine::Op::Same, {}, {}, {}, gap } );
                    gap = 0;
                }
                const auto text = line.op == templates::DiffLine::Op::Added
                                      ? lineText( *b, *line.second )
                                      : lineText( *a, *line.first );
                rows.rows.push_back( { line.op, line.first, line.second, text, 0 } );
            }
            if ( gap > 0 ) {
                rows.rows.push_back( { templates::DiffLine::Op::Same, {}, {}, {}, gap } );
            }
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
            const QColor removed( 0xd0, 0x30, 0x30 );
            const QColor added( 0x20, 0x90, 0x40 );
            for ( const auto& row : rows.rows ) {
                auto* item = new QTreeWidgetItem();
                if ( row.gap > 0 ) {
                    item->setText( 3, tr( "%L1 lines alike" ).arg( row.gap ) );
                    item->setForeground( 3, palette().color( QPalette::PlaceholderText ) );
                    diffTree_->addTopLevelItem( item );
                    continue;
                }
                const auto op = row.op;
                item->setText( 0, op == templates::DiffLine::Op::Added     ? "+"
                                  : op == templates::DiffLine::Op::Removed ? "-"
                                                                           : " " );
                if ( row.first ) {
                    item->setText( 1, QString::number( row.first->get() + 1 ) );
                    item->setData( 1, Qt::UserRole,
                                   QVariant::fromValue<qulonglong>( row.first->get() ) );
                }
                if ( row.second ) {
                    item->setText( 2, QString::number( row.second->get() + 1 ) );
                    item->setData( 2, Qt::UserRole,
                                   QVariant::fromValue<qulonglong>( row.second->get() ) );
                }
                item->setData( 0, SideRole, op == templates::DiffLine::Op::Added ? 2 : 1 );
                item->setText( 3, row.text );
                if ( op != templates::DiffLine::Op::Same ) {
                    const auto color = op == templates::DiffLine::Op::Added ? added : removed;
                    for ( int c = 0; c < 4; ++c ) {
                        item->setForeground( c, color );
                    }
                }
                for ( int c = 1; c < 3; ++c ) {
                    item->setTextAlignment( c, Qt::AlignRight | Qt::AlignVCenter );
                }
                diffTree_->addTopLevelItem( item );
            }
            for ( int c = 0; c < 3; ++c ) {
                diffTree_->resizeColumnToContents( c );
            }
            setMessage( rows.added + rows.removed == 0
                            ? tr( "The lines are alike (times and numbers left out)" )
                            : tr( "Lines only here: %L1, only there: %L2" )
                                  .arg( rows.removed )
                                  .arg( rows.added ) );
        },
        { firstData, secondData } );
}
