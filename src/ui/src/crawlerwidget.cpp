/*
 * Copyright (C) 2009, 2010, 2011, 2012, 2013, 2014, 2015 Nicolas Bonnefon and other contributors
 *
 * This file is part of glogg.
 *
 * glogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * glogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with glogg.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Copyright (C) 2016 -- 2019 Anton Filimonov and other contributors
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

// This file implements the CrawlerWidget class.
// It is responsible for creating and managing the two views and all
// the UI elements.  It implements the connection between the UI elements.
// It also interacts with the sets of data (full and filtered).

#include "abstractlogview.h"
#include "active_screen.h"
#include "linetypes.h"
#include "log.h"

#include <algorithm>
#include <cassert>
#include <chrono>

#include <QAction>
#include <QApplication>
#include <QCompleter>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLineEdit>
#include <QListView>
#include <QPointer>
#include <QShortcut>
#include <QStandardItemModel>
#include <QStringListModel>
#include <QTimer>
#include <qglobal.h>
#include <qobject.h>
#include <string>

#include "regularexpression.h"

#include "crawlerwidget.h"

#include "configuration.h"
#include "dispatch_to.h"
#include "fontutils.h"
#include "infoline.h"
#include "quickfindpattern.h"
#include "savedsearches.h"
#include "shortcuts.h"

static constexpr char AnsiColorSequenceRegex[] = "\\x1B\\[([0-9]{1,4}((;|:)[0-9]{1,3})*)?[mK]";

// Palette for error signaling (yellow background)
const QPalette CrawlerWidget::ErrorPalette( Qt::darkYellow );

// Implementation of the view context for the CrawlerWidget
class CrawlerWidgetContext : public ViewContextInterface {
public:
    // Construct from the stored string representation
    explicit CrawlerWidgetContext( const QString& string );
    // Construct from the value passsed
    CrawlerWidgetContext( QList<int> sizes, bool ignoreCase, bool autoRefresh, bool followFile,
                          bool useRegexp, bool inverseRegexp, bool useBooleanCombination,
                          QList<LineNumber> markedLines, QVariantMap filters )
        : sizes_( sizes )
        , ignoreCase_( ignoreCase )
        , autoRefresh_( autoRefresh )
        , followFile_( followFile )
        , useRegexp_( useRegexp )
        , inverseRegexp_( inverseRegexp )
        , useBooleanCombination_( useBooleanCombination )
        , filters_( std::move( filters ) )
    {
        std::transform( markedLines.cbegin(), markedLines.cend(), std::back_inserter( marks_ ),
                        []( const auto& m ) { return m.get(); } );
    }

    // Implementation of the ViewContextInterface function
    QString toString() const override;

    // Access the Qt sizes array for the QSplitter
    QList<int> sizes() const
    {
        return sizes_;
    }

    bool ignoreCase() const
    {
        return ignoreCase_;
    }
    bool autoRefresh() const
    {
        return autoRefresh_;
    }
    bool followFile() const
    {
        return followFile_;
    }
    bool useRegexp() const
    {
        return useRegexp_;
    }
    bool inverseRegexp() const
    {
        return inverseRegexp_;
    }
    bool useBooleanCombination() const
    {
        return useBooleanCombination_;
    }

    QList<LineNumber::UnderlyingType> marks() const
    {
        return marks_;
    }

    // Filter file and filters of the tab, empty for tabs that never showed their filters
    QVariantMap filters() const
    {
        return filters_;
    }

private:
    void loadFromString( const QString& string );
    void loadFromJson( const QString& json );

private:
    QList<int> sizes_;

    bool ignoreCase_;
    bool autoRefresh_;
    bool followFile_;
    bool useRegexp_;
    bool inverseRegexp_;
    bool useBooleanCombination_;

    QList<LineNumber::UnderlyingType> marks_;

    QVariantMap filters_;
};

// Constructor only does trivial construction. The real work is done once
// the data is attached.
CrawlerWidget::CrawlerWidget( QWidget* parent )
    : QSplitter( parent )
    , iconLoader_{ this }
{
}

CrawlerWidget::~CrawlerWidget()
{
    // The members (log data, filtered data...) are destroyed before the child widgets.
    // Deleting the children still sends signals, e.g. removing the filtered view tabs
    // emits currentChanged, and the handlers would use the destroyed members.
    for ( auto* child : findChildren<QObject*>() ) {
        child->disconnect( this );
    }
}

// The top line is first one on the main display
LineNumber CrawlerWidget::getTopLine() const
{
    return logMainView_->getTopLine();
}

QString CrawlerWidget::getSelectedText() const
{
    if ( filteredView_->hasFocus() )
        return filteredView_->getSelectedText();
    else
        return logMainView_->getSelectedText();
}

bool CrawlerWidget::isPartialSelection() const
{
    if ( filteredView_->hasFocus() )
        return filteredView_->isPartialSelection();
    else
        return logMainView_->isPartialSelection();
}

void CrawlerWidget::selectAll()
{
    activeView()->selectAll();
}

std::optional<int> CrawlerWidget::encodingMib() const
{
    return encodingMib_;
}

bool CrawlerWidget::isFollowEnabled() const
{
    return logMainView_->isFollowEnabled();
}

bool CrawlerWidget::isTextWrapEnabled() const
{
    return logMainView_->isTextWrapEnabled();
}

QString CrawlerWidget::encodingText() const
{
    return encodingText_;
}

// Return a pointer to the view in which we should do the QuickFind
SearchableWidgetInterface* CrawlerWidget::doGetActiveSearchable() const
{
    return activeView();
}

// Return all the searchable widgets (views)
std::vector<QObject*> CrawlerWidget::doGetAllSearchables() const
{
    std::vector<QObject*> searchables = { logMainView_, filteredView_ };

    return searchables;
}

// Update the state of the parent
void CrawlerWidget::doSendAllStateSignals()
{
    Q_EMIT newSelection( currentLineNumber_, 0_lcount, 0_lcol, 0_length );
    if ( !loadingInProgress_ )
        Q_EMIT loadingFinished( LoadingStatus::Successful );
}

void CrawlerWidget::changeEvent( QEvent* event )
{
    if ( event->type() == QEvent::StyleChange ) {
        dispatchToMainThread( [ this ] {
            loadIcons();
            searchInfoLineDefaultPalette_ = this->palette();
        } );
    }

    QWidget::changeEvent( event );
}

//
// Public Q_SLOTS:
//

void CrawlerWidget::stopLoading()
{
    logFilteredData_->interruptSearch();
    logData_->interruptLoading();
}

void CrawlerWidget::reload()
{
    searchState_.resetState();
    constexpr auto DropCache = true;
    logFilteredData_->clearSearch( DropCache );
    logFilteredData_->clearMarks();
    filteredView_->updateData();
    printSearchInfoMessage();

    logData_->reload();

    // A reload is considered as a first load,
    // this is to prevent the "new data" icon to be triggered.
    firstLoadDone_ = false;
}

void CrawlerWidget::setEncoding( std::optional<int> mib )
{
    encodingMib_ = std::move( mib );
    updateEncoding();

    update();
}

void CrawlerWidget::focusSearchEdit()
{
    searchLineEdit_->setFocus( Qt::ShortcutFocusReason );
}

void CrawlerWidget::goToLine()
{
    bool isLineSelected = true;
    auto newLine = QInputDialog::getText( this, "Jump to line", "Line number" )
                       .toULongLong( &isLineSelected );

    if ( isLineSelected ) {
        if ( newLine == 0 ) {
            newLine = 1;
        }

        const auto selectedLine
            = LineNumber( static_cast<LineNumber::UnderlyingType>( newLine - 1 ) );
        filteredView_->trySelectLine( logFilteredData_->getLineIndexNumber( selectedLine ) );
        logMainView_->trySelectLine( selectedLine );
    }
}

//
// Protected functions
//
void CrawlerWidget::doSetData( std::shared_ptr<LogData> logData,
                               std::shared_ptr<LogFilteredData> filteredData )
{
    logData_ = std::move( logData );
    logFilteredData_ = std::move( filteredData );
}

void CrawlerWidget::doSetQuickFindPattern( std::shared_ptr<QuickFindPattern> qfp )
{
    quickFindPattern_ = std::move( qfp );
}

void CrawlerWidget::doSetSavedSearches( SavedSearches* saved_searches )
{
    savedSearches_ = saved_searches;

    // We do setup now, assuming doSetData has been called before
    // us, that's not great really...
    setup();
}

void CrawlerWidget::doSetViewContext( const QString& view_context )
{
    LOG_DEBUG << "CrawlerWidget::doSetViewContext: " << view_context.toLocal8Bit().data();

    const auto context = CrawlerWidgetContext{ view_context };

    setSizes( context.sizes() );
    matchCaseButton_->setChecked( !context.ignoreCase() );
    useRegexpButton_->setChecked( context.useRegexp() );
    inverseButton_->setChecked( context.inverseRegexp() );
    booleanButton_->setChecked( context.useBooleanCombination() );

    searchRefreshButton_->setChecked( context.autoRefresh() );
    // Manually call the handler as it is not called when changing the state programmatically
    searchRefreshChangedHandler( context.autoRefresh() );

    const auto& config = Configuration::get();
    logMainView_->followSet( context.followFile() && config.anyFileWatchEnabled() );

    const auto filters = context.filters();
    if ( !filters.isEmpty() ) {
        filterTabState_ = FilterPanel::restoredTabState(
            filters.value( "file" ).toString(),
            filters::fromJson( QJsonObject::fromVariantMap( filters.value( "root" ).toMap() ) ) );
    }

    const auto savedMarks = context.marks();
    std::transform( savedMarks.cbegin(), savedMarks.cend(), std::back_inserter( savedMarkedLines_ ),
                    []( const auto& l ) { return LineNumber( l ); } );
}

std::shared_ptr<const ViewContextInterface> CrawlerWidget::doGetViewContext() const
{
    QVariantMap filters;
    if ( filterTabState_.initialized ) {
        filters[ "file" ] = filterTabState_.file;
        filters[ "root" ] = filters::toJson( filterTabState_.root ).toVariantMap();
    }

    auto context = std::make_shared<const CrawlerWidgetContext>(
        sizes(), ( !matchCaseButton_->isChecked() ), searchRefreshButton_->isChecked(),
        logMainView_->isFollowEnabled(), useRegexpButton_->isChecked(), inverseButton_->isChecked(),
        booleanButton_->isChecked(), logFilteredData_->getMarks(), filters );

    return static_cast<std::shared_ptr<const ViewContextInterface>>( context );
}

//
// Q_SLOTS:
//

void CrawlerWidget::startNewSearch()
{
    // An empty search goes back to the filters of the filter panel
    if ( searchLineEdit_->currentText().isEmpty() ) {
        setFilterMode( true );
        runFilterSearch();
        return;
    }

    setFilterMode( false );

    if ( keepSearchResultsButton_->isChecked() ) {
        keepSearchResultsButton_->setChecked( false );

        logFilteredData_->interruptSearch();
        logFilteredData_ = logData_->getNewFilteredData();

        filteredView_ = new FilteredView( logFilteredData_.get(), quickFindPattern_.get() );
        filteredViewsData_[ filteredView_ ] = logFilteredData_;

        connectAllFilteredViewSlots( filteredView_ );

        auto index = tabbedFilteredView_->addTab( filteredView_, "" );
        tabbedFilteredView_->setCurrentIndex( index );

        connect( logFilteredData_.get(), &LogFilteredData::searchProgressed, this,
                 &CrawlerWidget::updateFilteredView, Qt::QueuedConnection );

        logMainView_->useNewFiltering( logFilteredData_.get() );

        applyConfiguration();
    }

    tabbedFilteredView_->setTabText( tabbedFilteredView_->currentIndex(),
                                     "Find \"" + searchLineEdit_->currentText() + "\"" );

    // Record the search line in the recent list
    // (reload the list first in case another glogg changed it)
    const auto& searches = SavedSearches::getSynced();
    savedSearches_->addRecent( searchLineEdit_->currentText() );
    searches.save();

    // Update the SearchLine (history)
    updateSearchCombo();
    // Call the private function to do the search
    replaceCurrentSearch( searchLineEdit_->currentText() );
}

void CrawlerWidget::stopSearch()
{
    logFilteredData_->interruptSearch();
    searchState_.stopSearch();
    printSearchInfoMessage();
}

void CrawlerWidget::clearSearchHistory()
{
    // Clear line
    searchLineEdit_->clear();

    // Sync and clear saved searches
    auto& searches = SavedSearches::getSynced();
    savedSearches_->clear();
    searches.save();

    searchLineCompleter_->setModel( new QStringListModel( {}, searchLineCompleter_ ) );
}

void CrawlerWidget::editSearchHistory()
{
    // Sync and clear saved searches
    auto& searches = SavedSearches::getSynced();

    auto history = savedSearches_->recentSearches().join( QChar::LineFeed );
    bool ok;
    QString newHistory = QInputDialog::getMultiLineText( this, tr( "flogg" ),
                                                         tr( "Search history:" ), history, &ok );

    if ( ok ) {
        savedSearches_->clear();
#if QT_VERSION >= QT_VERSION_CHECK( 5, 15, 0 )
        auto items = newHistory.split( QChar::LineFeed, Qt::SkipEmptyParts );
#else
        auto items = newHistory.split( QChar::LineFeed, QString::SkipEmptyParts );
#endif
        std::for_each( items.rbegin(), items.rend(), [ this ]( const auto& item ) {
            savedSearches_->addRecent( item );
            LOG_INFO << item;
        } );
    }
    searches.save();

    updateSearchCombo();
}

void CrawlerWidget::saveSearchAsFilter()
{
    Q_EMIT addToFilters( searchLineEdit_->currentText() );
}

void CrawlerWidget::showSearchContextMenu()
{
    if ( searchLineContextMenu_ )
        searchLineContextMenu_->exec( QCursor::pos( activeScreen( this ) ) );
}

// When receiving the 'newDataAvailable' signal from LogFilteredData
void CrawlerWidget::updateFilteredView( LinesCount nbMatches, int progress,
                                        LineNumber initialPosition )
{
    LOG_DEBUG << "updateFilteredView received.";

    searchInfoLine_->show();

    if ( progress == 100 ) {
        // Searching done
        printSearchInfoMessage( nbMatches );
        Q_EMIT filterHitsChanged( filterHits() );
        searchInfoLine_->hideGauge();
        // De-activate the stop button
        stopButton_->setEnabled( false );
        stopButton_->hide();
        searchButton_->show();
        clearButton_->show();
    }
    else {
        // Search in progress
        // We ignore 0% and 100% to avoid a flash when the search is very short
        if ( progress > 0 ) {
            // Some languages translate the plural the same as the singular, so use the full string

            searchInfoLine_->setText(
                tr( "Search in progress (%1 %)..." ).arg( QString::number( progress ) )
                + ( nbMatches.get() > 1 ? tr( " %1 matches found so far." )
                                              .arg( QString::number( nbMatches.get() ) )
                                        : tr( " %1 match found so far." )
                                              .arg( QString::number( nbMatches.get() ) ) ) );

            searchInfoLine_->displayGauge( progress );
        }
    }

    // If more (or less, e.g. come back to 0) matches have been found
    if ( nbMatches != nbMatches_ ) {
        nbMatches_ = nbMatches;

        // Recompute the content of the filtered window.
        filteredView_->updateData();

        // Update the match overview
        overview_.updateData( logData_->getNbLine() );

        // New data found icon
        if ( initialPosition > 0_lnum ) {
            changeDataStatus( DataStatus::NEW_FILTERED_DATA );
        }

        // Also update the top window for the coloured bullets.
        update();
    }

    // Try to restore the filtered window selection close to where it was
    // only for full searches to avoid disconnecting follow mode!
    if ( ( progress == 100 ) && ( initialPosition == searchStartLine_ )
         && ( !isFollowEnabled() ) ) {
        const auto currenLineIndex = logFilteredData_->getLineIndexNumber( currentLineNumber_ );
        LOG_DEBUG << "updateFilteredView: restoring selection: "
                  << " absolute line number (0based) " << currentLineNumber_ << " index "
                  << currenLineIndex;
        filteredView_->selectAndDisplayLine( currenLineIndex );
        filteredView_->setSearchLimits( searchStartLine_, searchEndLine_ );
    }
}

void CrawlerWidget::jumpToMatchingLine( LineNumber filteredLineNb, LinesCount nLines,
                                        LineColumn startCol, LineLength nSymbols )
{
    const auto mainViewLine = logFilteredData_->getMatchingLineNumber( filteredLineNb );
    logMainView_->selectPortionAndDisplayLine( mainViewLine, nLines, startCol,
                                               nSymbols ); // FIXME: should be done with a signal.
}

void CrawlerWidget::updateLineNumberHandler( LineNumber line, LinesCount nLines,
                                             LineColumn startCol, LineLength nSymbols )
{
    currentLineNumber_ = line;
    Q_EMIT newSelection( line, nLines, startCol, nSymbols );
}

void CrawlerWidget::markLinesFromMain( const klogg::vector<LineNumber>& lines )
{
    klogg::vector<LineNumber> alreadyMarkedLines;
    alreadyMarkedLines.reserve( lines.size() );

    bool markAdded = false;
    for ( const auto& line : lines ) {
        if ( line >= logData_->getNbLine() ) {
            continue;
        }

        if ( !logFilteredData_->lineTypeByLine( line ).testFlag(
                 AbstractLogData::LineTypeFlags::Mark ) ) {
            logFilteredData_->addMark( line );
            markAdded = true;
        }
        else {
            alreadyMarkedLines.push_back( line );
        }
    }

    if ( !markAdded ) {
        for ( const auto& line : alreadyMarkedLines ) {
            logFilteredData_->toggleMark( line );
        }
    }

    // Recompute the content of both window.
    filteredView_->updateData();
    logMainView_->updateData();

    // Update the match overview
    overview_.updateData( logData_->getNbLine() );

    // Also update the top window for the coloured bullets.
    update();
}

void CrawlerWidget::markLinesFromFiltered( const klogg::vector<LineNumber>& lines )
{
    klogg::vector<LineNumber> linesInMain( lines.size() );
    std::transform( lines.cbegin(), lines.cend(), linesInMain.begin(),
                    [ this ]( const auto& filteredLine ) {
                        if ( filteredLine < logData_->getNbLine() ) {
                            return logFilteredData_->getMatchingLineNumber( filteredLine );
                        }
                        else {
                            return maxValue<LineNumber>();
                        }
                    } );

    markLinesFromMain( linesInMain );
}

void CrawlerWidget::applyConfiguration()
{
    const auto& config = Configuration::get();
    QFont font = config.mainFont();

    LOG_DEBUG << "CrawlerWidget::applyConfiguration";

    registerShortcuts();

    // Whatever font we use, we should NOT use kerning
    font.setKerning( false );
    font.setFixedPitch( true );

    // Necessary on systems doing subpixel positionning (e.g. Ubuntu 12.04)
    if ( config.forceFontAntialiasing() ) {
        font.setStyleStrategy( QFont::PreferAntialias );
    }

    font.setBold( config.useBoldFont() );

    if ( config.hideAnsiColorSequences() ) {
        logData_->setPrefilter( AnsiColorSequenceRegex );
    }
    else {
        logData_->setPrefilter( {} );
    }

    logMainView_->setLineNumbersVisible( config.mainLineNumbersVisible() );

    const auto isFollowModeAllowed = config.anyFileWatchEnabled();
    logMainView_->allowFollowMode( isFollowModeAllowed );
    overview_.setVisible( config.isOverviewVisible() );
    logMainView_->refreshOverview();
    logMainView_->updateFont( font );

    for ( auto i = 0; i < tabbedFilteredView_->count(); ++i ) {
        auto fv = qobject_cast<FilteredView*>( tabbedFilteredView_->widget( i ) );
        fv->setLineNumbersVisible( config.filteredLineNumbersVisible() );
        fv->allowFollowMode( isFollowModeAllowed );
        fv->updateFont( font );
    }

    // Update the SearchLine (history)
    updateSearchCombo();

    FileWatcher::getFileWatcher().updateConfiguration();

    if ( isFollowEnabled() ) {
        changeDataStatus( DataStatus::OLD_DATA );
    }
}

void CrawlerWidget::enteringQuickFind()
{
    LOG_DEBUG << "CrawlerWidget::enteringQuickFind";

    // Remember who had the focus (only if it is one of our views)
    QWidget* focus_widget = QApplication::focusWidget();

    if ( ( focus_widget == logMainView_ ) || ( focus_widget == filteredView_ ) )
        qfSavedFocus_ = focus_widget;
    else
        qfSavedFocus_ = nullptr;
}

void CrawlerWidget::exitingQuickFind()
{
    // Restore the focus once the QFBar has been hidden
    if ( qfSavedFocus_ )
        qfSavedFocus_->setFocus();
}

void CrawlerWidget::loadingFinishedHandler( LoadingStatus status )
{
    LOG_INFO << "file loading finished, status " << static_cast<int>( status );

    // We need to refresh the main window because the view lines on the
    // overview have probably changed.
    overview_.updateData( logData_->getNbLine() );

    // FIXME, handle topLine
    // logMainView_->updateData( logData_, topLine );
    logMainView_->updateData();

    // Shall we Forbid starting a search when loading in progress?
    // searchButton_->setEnabled( false );

    // searchButton_->setEnabled( true );

    // See if we need to auto-refresh the search
    if ( searchState_.isAutorefreshAllowed() ) {
        searchEndLine_ = LineNumber( logData_->getNbLine().get() );
        if ( searchState_.isFileTruncated() )
            // We need to restart the search
            rerunCurrentSearch();
        else
            logFilteredData_->updateSearch( searchStartLine_, searchEndLine_ );
    }

    // Set the encoding for the views
    updateEncoding();

    clearSearchLimits();

    // Also change the data available icon
    if ( firstLoadDone_ ) {
        changeDataStatus( DataStatus::NEW_DATA );
    }
    else {
        firstLoadDone_ = true;
        for ( const auto& m : savedMarkedLines_ ) {
            logFilteredData_->addMark( m );
        }
        logMainView_->setFocus();

        if ( filterMode_ ) {
            // Not right away: the search processes events, which can delete this widget
            // (its tab may have been closed while the file was loading)
            QTimer::singleShot( 0, this, &CrawlerWidget::runFilterSearch );
        }
    }

    loadingInProgress_ = false;
    Q_EMIT loadingFinished( status );
}

void CrawlerWidget::fileChangedHandler( MonitoredFileStatus status )
{
    // Handle the case where the file has been truncated
    if ( status == MonitoredFileStatus::Truncated ) {
        // Clear all marks (TODO offer the option to keep them)
        logFilteredData_->clearMarks();
        if ( !searchInfoLine_->text().isEmpty() ) {
            // Invalidate the search
            constexpr auto DropCache = true;
            logFilteredData_->clearSearch( DropCache );
            filteredView_->updateData();
            searchState_.truncateFile();
            printSearchInfoMessage();
            nbMatches_ = 0_lcount;
        }
    }
}

// Returns a pointer to the window in which the search should be done
AbstractLogView* CrawlerWidget::activeView() const
{
    QWidget* activeView;

    // Search in the window that has focus, or the window where 'Find' was
    // called from, or the main window.
    if ( filteredView_->hasFocus() || logMainView_->hasFocus() )
        activeView = QApplication::focusWidget();
    else
        activeView = qfSavedFocus_;

    if ( activeView ) {
        auto* view = qobject_cast<AbstractLogView*>( activeView );
        return view;
    }
    else {
        LOG_WARNING << "No active view, defaulting to logMainView";
        return logMainView_;
    }
}

void CrawlerWidget::searchForward()
{
    LOG_DEBUG << "CrawlerWidget::searchForward";

    activeView()->searchForward();
}

void CrawlerWidget::searchBackward()
{
    LOG_DEBUG << "CrawlerWidget::searchBackward";

    activeView()->searchBackward();
}

void CrawlerWidget::resetStateOnSearchPatternChanges()
{
    // We suspend auto-refresh

    searchState_.changeExpression();
    printSearchInfoMessage( logFilteredData_->getNbMatches() );
}

void CrawlerWidget::searchRefreshChangedHandler( bool isRefreshing )
{
    searchState_.setAutorefresh( isRefreshing );
    printSearchInfoMessage( logFilteredData_->getNbMatches() );
}

void CrawlerWidget::matchCaseChangedHandler( bool shouldMatchCase )
{
    searchLineCompleter_->setCaseSensitivity( shouldMatchCase ? Qt::CaseSensitive
                                                              : Qt::CaseInsensitive );

    resetStateOnSearchPatternChanges();
}

void CrawlerWidget::booleanCombiningChangedHandler( bool )
{
    resetStateOnSearchPatternChanges();
}

void CrawlerWidget::useRegexpChangeHandler( bool )
{
    resetStateOnSearchPatternChanges();
}

void CrawlerWidget::searchTextChangeHandler( QString )
{
    resetStateOnSearchPatternChanges();
}

void CrawlerWidget::changeFilteredViewVisibility( int index )
{
    QStandardItem* item = visibilityModel_->item( index );
    auto visibility = item->data().value<FilteredView::Visibility>();

    filteredView_->setVisibility( visibility );

    if ( logFilteredData_->getNbLine() > 0_lcount ) {
        const auto lineIndex = logFilteredData_->getLineIndexNumber( currentLineNumber_ );
        filteredView_->selectAndDisplayLine( lineIndex );
    }
}

QString CrawlerWidget::escapeSearchPattern( const QString& pattern, bool isRegex ) const
{
    auto escapedPattern = ( !isRegex && useRegexpButton_->isChecked() )
                              ? QRegularExpression::escape( pattern )
                              : pattern;

    if ( booleanButton_->isChecked() ) {
        escapedPattern.replace( '"', "\"" ).prepend( '"' ).append( '"' );
    }

    return escapedPattern;
}

QString& CrawlerWidget::combinePatterns( QString& currentPattern, const QString& newPattern ) const
{
    if ( !currentPattern.isEmpty() ) {
        if ( booleanButton_->isChecked() ) {
            currentPattern.append( " or " );
        }
        else if ( useRegexpButton_->isChecked() ) {
            currentPattern.append( '|' );
        }
    }

    currentPattern.append( newPattern );

    return currentPattern;
}

void CrawlerWidget::addToSearch( const QString& searchString )
{
    const auto newPattern = escapeSearchPattern( searchString );
    QString currentPattern = searchLineEdit_->currentText();
    setSearchPattern( combinePatterns( currentPattern, newPattern ) );
}

void CrawlerWidget::excludeFromSearch( const QString& searchString )
{
    QString currentPattern = searchLineEdit_->currentText();

    const auto wasInBooleanCombinationMode = booleanButton_->isChecked();
    if ( !wasInBooleanCombinationMode ) {
        currentPattern.replace( '"', "\"" ).prepend( '"' ).append( '"' );
    }

    booleanButton_->setChecked( true );

    const auto newPattern = escapeSearchPattern( searchString );

    if ( !currentPattern.isEmpty() ) {
        currentPattern.append( " and " );
    }

    currentPattern.append( "not(" ).append( newPattern ).append( ')' );
    setSearchPattern( currentPattern );
}

void CrawlerWidget::replaceSearch( const QString& searchString )
{
    setSearchPattern( escapeSearchPattern( searchString ) );
}

void CrawlerWidget::setSearchPattern( const QString& searchPattern )
{
    searchLineEdit_->setEditText( searchPattern );
    // Set the focus to lineEdit so that the user can press 'Return' immediately
    searchLineEdit_->lineEdit()->setFocus();

    if ( Configuration::get().autoRunSearchOnPatternChange() ) {
        dispatchToMainThread( [ this ] { startNewSearch(); } );
    }
}

void CrawlerWidget::mouseHoveredOverMatch( LineNumber line )
{
    const auto line_in_mainview = logFilteredData_->getMatchingLineNumber( line );

    overviewWidget_->highlightLine( line_in_mainview );
}

void CrawlerWidget::activityDetected()
{
    changeDataStatus( DataStatus::OLD_DATA );
}

void CrawlerWidget::setSearchLimits( LineNumber startLine, LineNumber endLine )
{
    searchStartLine_ = startLine;
    searchEndLine_ = endLine;

    logMainView_->setSearchLimits( startLine, endLine );
    filteredView_->setSearchLimits( startLine, endLine );
}

void CrawlerWidget::clearSearchLimits()
{
    setSearchLimits( 0_lnum, LineNumber( logData_->getNbLine().get() ) );
}

//
// Private functions
//

// Build the widget and connect all the signals, this must be done once
// the data are attached.
void CrawlerWidget::setup()
{
    LOG_INFO << "Setup crawler widget";
    setOrientation( Qt::Vertical );

    assert( logData_ );
    assert( logFilteredData_ );

    // The views
    auto bottomWindow = new QWidget;
    bottomWindow->setContentsMargins( 2, 0, 2, 0 );

    overviewWidget_ = new OverviewWidget();
    logMainView_
        = new LogMainView( logData_.get(), quickFindPattern_.get(), &overview_, overviewWidget_ );
    logMainView_->setContentsMargins( 2, 0, 2, 0 );

    filteredView_ = new FilteredView( logFilteredData_.get(), quickFindPattern_.get() );
    filteredViewsData_[ filteredView_ ] = logFilteredData_;
    filteredView_->setContentsMargins( 2, 0, 2, 0 );

    overviewWidget_->setOverview( &overview_ );
    overviewWidget_->setParent( logMainView_ );

    // Connect the search to the top view
    logMainView_->useNewFiltering( logFilteredData_.get() );
    logMainView_->setLineToolTipProvider( [ this ]( LineNumber line ) { return filterToolTip( line ); } );

    // Construct the visibility button
    using VisibilityFlags = LogFilteredData::VisibilityFlags;
    visibilityModel_ = new QStandardItemModel( this );

    QStandardItem* marksAndMatchesItem = new QStandardItem( tr( "Marks and matches" ) );
    marksAndMatchesItem->setData(
        QVariant::fromValue( VisibilityFlags::Marks | VisibilityFlags::Matches ) );
    visibilityModel_->appendRow( marksAndMatchesItem );

    QStandardItem* marksItem = new QStandardItem( tr( "Marks" ) );
    marksItem->setData( QVariant::fromValue<FilteredView::Visibility>( VisibilityFlags::Marks ) );
    visibilityModel_->appendRow( marksItem );

    QStandardItem* matchesItem = new QStandardItem( tr( "Matches" ) );
    matchesItem->setData(
        QVariant::fromValue<FilteredView::Visibility>( VisibilityFlags::Matches ) );
    visibilityModel_->appendRow( matchesItem );

    auto* visibilityView = new QListView( this );
    visibilityView->setMovement( QListView::Static );
    // visibilityView->setMinimumWidth( 170 ); // Only needed with custom style-sheet

    visibilityBox_ = new QComboBox();
    visibilityBox_->setModel( visibilityModel_ );
    visibilityBox_->setView( visibilityView );

    // Select "Marks and matches" by default (same default as the filtered view)
    visibilityBox_->setCurrentIndex( 0 );
    visibilityBox_->setContentsMargins( 2, 2, 2, 2 );

    // TODO: Maybe there is some way to set the popup width to be
    // sized-to-content (as it is when the stylesheet is not overriden) in the
    // stylesheet as opposed to setting a hard min-width on the view above.
    /*visibilityBox_->setStyleSheet( " \
        QComboBox:on {\
            padding: 1px 2px 1px 6px;\
            width: 19px;\
        } \
        QComboBox:!on {\
            padding: 1px 2px 1px 7px;\
            width: 19px;\
            height: 16px;\
            border: 1px solid gray;\
        } \
        QComboBox::drop-down::down-arrow {\
            width: 0px;\
            border-width: 0px;\
        } \
" );*/

    // Construct the Search Info line
    searchInfoLine_ = new InfoLine();
    searchInfoLine_->setFrameStyle( QFrame::StyledPanel );
    searchInfoLine_->setFrameShadow( QFrame::Sunken );
    searchInfoLine_->setLineWidth( 1 );
    searchInfoLine_->setSizePolicy( QSizePolicy::Minimum, QSizePolicy::Minimum );
    auto searchInfoLineSizePolicy = searchInfoLine_->sizePolicy();
    searchInfoLineSizePolicy.setRetainSizeWhenHidden( false );
    searchInfoLine_->setSizePolicy( searchInfoLineSizePolicy );
    searchInfoLineDefaultPalette_ = this->palette();
    searchInfoLine_->setContentsMargins( 2, 2, 2, 2 );

    matchCaseButton_ = new QToolButton();
    matchCaseButton_->setToolTip( tr( "Match case" ) );
    matchCaseButton_->setCheckable( true );
    matchCaseButton_->setFocusPolicy( Qt::NoFocus );
    matchCaseButton_->setContentsMargins( 2, 2, 2, 2 );

    useRegexpButton_ = new QToolButton();
    useRegexpButton_->setToolTip( tr( "Use regex" ) );
    useRegexpButton_->setCheckable( true );
    useRegexpButton_->setFocusPolicy( Qt::NoFocus );
    useRegexpButton_->setContentsMargins( 2, 2, 2, 2 );

    inverseButton_ = new QToolButton();
    inverseButton_->setToolTip( tr( "Inverse match" ) );
    inverseButton_->setCheckable( true );
    inverseButton_->setFocusPolicy( Qt::NoFocus );
    inverseButton_->setContentsMargins( 2, 2, 2, 2 );

    booleanButton_ = new QToolButton();
    booleanButton_->setToolTip( tr( "Enable regular expression logical combining" ) );
    booleanButton_->setCheckable( true );
    booleanButton_->setFocusPolicy( Qt::NoFocus );
    booleanButton_->setContentsMargins( 2, 2, 2, 2 );

    filterModeButton_ = new QToolButton();
    filterModeButton_->setText( tr( "Filters" ) );
    filterModeButton_->setToolTip(
        tr( "Show lines selected by the filter panel (an empty search also switches back to "
            "the filters)" ) );
    filterModeButton_->setCheckable( true );
    filterModeButton_->setChecked( filterMode_ );
    filterModeButton_->setFocusPolicy( Qt::NoFocus );
    filterModeButton_->setContentsMargins( 2, 2, 2, 2 );

    searchRefreshButton_ = new QToolButton();
    searchRefreshButton_->setToolTip( tr( "Auto-refresh" ) );
    searchRefreshButton_->setCheckable( true );
    searchRefreshButton_->setFocusPolicy( Qt::NoFocus );
    searchRefreshButton_->setContentsMargins( 2, 2, 2, 2 );

    // Construct the Search line
    searchLineCompleter_ = new QCompleter( savedSearches_->recentSearches(), this );
    searchLineEdit_ = new QComboBox;
    searchLineEdit_->setEditable( true );
    searchLineEdit_->setCompleter( searchLineCompleter_ );
    searchLineEdit_->addItems( savedSearches_->recentSearches() );
    searchLineEdit_->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Minimum );
    searchLineEdit_->setSizeAdjustPolicy( QComboBox::AdjustToMinimumContentsLengthWithIcon );
    searchLineEdit_->lineEdit()->setMaxLength( std::numeric_limits<int>::max() / 1024 );
    searchLineEdit_->setContentsMargins( 2, 2, 2, 2 );

    QAction* clearSearchHistoryAction = new QAction( tr( "Clear search history" ), this );
    QAction* editSearchHistoryAction = new QAction( tr( "Edit search history" ), this );
    QAction* saveAsFilterAction = new QAction( tr( "Add as filter..." ), this );

    searchLineContextMenu_ = searchLineEdit_->lineEdit()->createStandardContextMenu();
    searchLineContextMenu_->addSeparator();
    searchLineContextMenu_->addAction( saveAsFilterAction );
    searchLineContextMenu_->addSeparator();
    searchLineContextMenu_->addAction( editSearchHistoryAction );
    searchLineContextMenu_->addAction( clearSearchHistoryAction );
    searchLineEdit_->setContextMenuPolicy( Qt::CustomContextMenu );

    setFocusProxy( searchLineEdit_ );

    clearButton_ = new QToolButton();
    clearButton_->setText( tr( "Clear search text" ) );
    clearButton_->setAutoRaise( true );
    clearButton_->setContentsMargins( 2, 2, 2, 2 );

    searchButton_ = new QToolButton();
    searchButton_->setText( tr( "Search" ) );
    searchButton_->setAutoRaise( true );
    searchButton_->setContentsMargins( 2, 2, 2, 2 );

    keepSearchResultsButton_ = new QToolButton();
    keepSearchResultsButton_->setText( tr( "Keep Results" ) );
    keepSearchResultsButton_->setToolTip(
        tr( "Keep these results and show subsequent results in a new window" ) );
    keepSearchResultsButton_->setCheckable( true );
    keepSearchResultsButton_->setContentsMargins( 2, 2, 2, 2 );

    stopButton_ = new QToolButton();
    stopButton_->setAutoRaise( true );
    stopButton_->setEnabled( false );
    stopButton_->setVisible( false );
    stopButton_->setContentsMargins( 2, 2, 2, 2 );

    auto* searchLineLayout = new QHBoxLayout;
    searchLineLayout->setContentsMargins( 2, 2, 2, 2 );

    searchLineLayout->addWidget( filterModeButton_ );
    searchLineLayout->addWidget( visibilityBox_ );
    searchLineLayout->addWidget( matchCaseButton_ );
    searchLineLayout->addWidget( useRegexpButton_ );
    searchLineLayout->addWidget( inverseButton_ );
    searchLineLayout->addWidget( booleanButton_ );
    searchLineLayout->addWidget( searchRefreshButton_ );
    searchLineLayout->addWidget( searchLineEdit_ );
    searchLineLayout->addWidget( clearButton_ );
    searchLineLayout->addWidget( searchButton_ );
    searchLineLayout->addWidget( keepSearchResultsButton_ );
    searchLineLayout->addWidget( stopButton_ );
    searchLineLayout->addWidget( searchInfoLine_ );

    // Construct the bottom window
    tabbedFilteredView_ = new QTabWidget;
    tabbedFilteredView_->setTabsClosable( true );
    tabbedFilteredView_->addTab( filteredView_, "" );
    tabbedFilteredView_->setDocumentMode( true );
    tabbedFilteredView_->setTabBarAutoHide( true );

    auto* bottomMainLayout = new QVBoxLayout;
    bottomMainLayout->addLayout( searchLineLayout );
    bottomMainLayout->addWidget( tabbedFilteredView_ );
    bottomMainLayout->setContentsMargins( 2, 2, 2, 2 );
    bottomWindow->setLayout( bottomMainLayout );

    addWidget( logMainView_ );
    addWidget( bottomWindow );

    // Default search checkboxes
    auto& config = Configuration::get();
    searchRefreshButton_->setChecked( config.isSearchAutoRefreshDefault() );
    matchCaseButton_->setChecked( !config.isSearchIgnoreCaseDefault() );
    useRegexpButton_->setChecked( config.mainRegexpType() == SearchRegexpType::ExtendedRegexp );
    booleanButton_->setChecked( config.isSearchLogicalCombiningDefault() );

    // Manually call the handler as it is not called when changing the state programmatically
    searchRefreshChangedHandler( searchRefreshButton_->isChecked() );
    useRegexpChangeHandler( useRegexpButton_->isChecked() );
    matchCaseChangedHandler( matchCaseButton_->isChecked() );
    booleanCombiningChangedHandler( booleanButton_->isChecked() );

    // Default splitter position (usually overridden by the config file)
    setSizes( config.splitterSizes() );

    registerShortcuts();
    loadIcons();

    // Connect the signals
    connect( searchLineEdit_->lineEdit(), &QLineEdit::returnPressed, searchButton_,
             &QToolButton::click );
    connect( searchLineEdit_->lineEdit(), &QLineEdit::textEdited, this,
             &CrawlerWidget::searchTextChangeHandler );

    connect( searchLineEdit_, &QWidget::customContextMenuRequested, this,
             &CrawlerWidget::showSearchContextMenu );
    connect( saveAsFilterAction, &QAction::triggered, this, &CrawlerWidget::saveSearchAsFilter );
    connect( clearSearchHistoryAction, &QAction::triggered, this,
             &CrawlerWidget::clearSearchHistory );
    connect( editSearchHistoryAction, &QAction::triggered, this,
             &CrawlerWidget::editSearchHistory );
    connect( searchButton_, &QToolButton::clicked, this, &CrawlerWidget::startNewSearch );
    connect( filterModeButton_, &QToolButton::clicked, this, [ this ]( bool checked ) {
        setFilterMode( checked );
        rerunCurrentSearch();
    } );
    connect( stopButton_, &QToolButton::clicked, this, &CrawlerWidget::stopSearch );
    connect( clearButton_, &QToolButton::clicked, searchLineEdit_, &QComboBox::clearEditText );

    connect( visibilityBox_, QOverload<int>::of( &QComboBox::currentIndexChanged ), this,
             &CrawlerWidget::changeFilteredViewVisibility );

    connect( logMainView_, &LogMainView::newSelection,
             [ this ]( auto ) { logMainView_->update(); } );

    connect( logMainView_, &LogMainView::newSelection, this,
             &CrawlerWidget::updateLineNumberHandler );

    connect( logMainView_, &LogMainView::markLines, this, &CrawlerWidget::markLinesFromMain );


    connect( logMainView_, QOverload<const QString&>::of( &LogMainView::addToSearch ), this,
             &CrawlerWidget::addToSearch );

    connect( logMainView_, QOverload<const QString&>::of( &LogMainView::excludeFromSearch ), this,
             &CrawlerWidget::excludeFromSearch );
    connect( logMainView_, &LogMainView::addToFilters, this, &CrawlerWidget::addToFilters );

    connect( logMainView_, QOverload<const QString&>::of( &LogMainView::replaceSearch ), this,
             &CrawlerWidget::replaceSearch );

    // Follow option (up and down)
    connect( this, &CrawlerWidget::followSet, logMainView_, &LogMainView::followSet );
    connect( logMainView_, &LogMainView::followModeChanged, this,
             &CrawlerWidget::followModeChanged );

    connect( this, &CrawlerWidget::textWrapSet, logMainView_, &LogMainView::textWrapSet );

    // Detect activity in the views
    connect( logMainView_, &LogMainView::activity, this, &CrawlerWidget::activityDetected );

    connect( logMainView_, &LogMainView::changeSearchLimits, this,
             &CrawlerWidget::setSearchLimits );

    connect( logMainView_, &LogMainView::clearSearchLimits, this,
             &CrawlerWidget::clearSearchLimits );

    connect( tabbedFilteredView_, &QTabWidget::currentChanged, this,
             &CrawlerWidget::changeFilteredView );

    connect( tabbedFilteredView_, &QTabWidget::tabCloseRequested, this,
             &CrawlerWidget::closeFilteredView );

    connect( logMainView_, &LogMainView::saveDefaultSplitterSizes, this,
             &CrawlerWidget::saveSplitterSizes );

    connect( logMainView_, &LogMainView::changeFontSize, this, &CrawlerWidget::changeFontSize );

    connect( logFilteredData_.get(), &LogFilteredData::searchProgressed, this,
             &CrawlerWidget::updateFilteredView, Qt::QueuedConnection );

    // Sent load file update to MainWindow (for status update)
    connect( logData_.get(), &LogData::loadingProgressed, this, &CrawlerWidget::loadingProgressed );
    connect( logData_.get(), &LogData::loadingFinished, this,
             &CrawlerWidget::loadingFinishedHandler );
    connect( logData_.get(), &LogData::fileChanged, this, &CrawlerWidget::fileChangedHandler );

    // Search auto-refresh
    connect( searchRefreshButton_, &QPushButton::toggled, this,
             &CrawlerWidget::searchRefreshChangedHandler );

    connect( matchCaseButton_, &QPushButton::toggled, this,
             &CrawlerWidget::matchCaseChangedHandler );

    connect( useRegexpButton_, &QPushButton::toggled, this,
             &CrawlerWidget::useRegexpChangeHandler );

    connect( booleanButton_, &QPushButton::toggled, this,
             &CrawlerWidget::booleanCombiningChangedHandler );

    // Advise the parent the checkboxes have been changed
    // (for maintaining default config)
    connect( searchRefreshButton_, &QPushButton::toggled, this,
             &CrawlerWidget::searchRefreshChanged );
    connect( matchCaseButton_, &QPushButton::toggled, this, &CrawlerWidget::matchCaseChanged );

    // Switch between views
    connect( logMainView_, &AbstractLogView::clearColorLabels, this,
             &CrawlerWidget::clearColorLabels );

    connect( logMainView_, &AbstractLogView::addColorLabel, this,
             &CrawlerWidget::addColorLabelToSelection );

    connect( logMainView_, &AbstractLogView::sendSelectionToScratchpad, this,
             [ this ]() { Q_EMIT sendToScratchpad( logMainView_->getSelectedText() ); } );

    connect( logMainView_, &AbstractLogView::replaceScratchpadWithSelection, this,
             [ this ]() { Q_EMIT replaceDataInScratchpad( logMainView_->getSelectedText() ); } );

    connectAllFilteredViewSlots( filteredView_ );

    const auto defaultEncodingMib = config.defaultEncodingMib();
    if ( defaultEncodingMib >= 0 ) {
        encodingMib_ = defaultEncodingMib;
    }
}

void CrawlerWidget::changeFilteredView( int tabIndex )
{
    logFilteredData_->interruptSearch();
    if ( tabIndex >= 0 ) {
        auto* tabFilteredView
            = qobject_cast<FilteredView*>( tabbedFilteredView_->widget( tabIndex ) );

        filteredView_ = tabFilteredView;
        logFilteredData_ = filteredViewsData_.at( tabFilteredView );

        Q_EMIT filteredViewChanged();

        logMainView_->useNewFiltering( logFilteredData_.get() );
        changeFilteredViewVisibility( visibilityBox_->currentIndex() );
    }
}

void CrawlerWidget::closeFilteredView( int tabIndex )
{
    auto* tabFilteredView = tabbedFilteredView_->widget( tabIndex );
    connect( tabFilteredView, &QObject::destroyed, this, &CrawlerWidget::filteredViewDestroyed );
    tabFilteredView->deleteLater();
}

void CrawlerWidget::filteredViewDestroyed( QObject* view )
{
    filteredViewsData_.erase( qobject_cast<FilteredView*>( view ) );
}

void CrawlerWidget::saveSplitterSizes() const
{
    LOG_INFO << "Saving default splitter size";
    auto& splitterConfig = Configuration::get();
    splitterConfig.setSplitterSizes( sizes() );
    splitterConfig.save();
}

void CrawlerWidget::changeFontSize( bool increase )
{
    auto& fontConfig = Configuration::get();

    auto fontInfo = QFontInfo( fontConfig.mainFont() );
    const auto availableSizes = FontUtils::availableFontSizes( fontInfo.family() );

    auto currentSize
        = std::find( availableSizes.cbegin(), availableSizes.cend(), fontInfo.pointSize() );
    if ( increase && currentSize != std::prev( availableSizes.cend() ) ) {
        currentSize = std::next( currentSize );
    }
    else if ( !increase && currentSize != availableSizes.begin() ) {
        currentSize = std::prev( currentSize );
    }

    if ( currentSize != availableSizes.cend() ) {
        QFont newFont{ fontInfo.family(), *currentSize };

        fontConfig.setMainFont( newFont );
        logMainView_->updateFont( newFont );
        filteredView_->updateFont( newFont );
    }
}

void CrawlerWidget::connectAllFilteredViewSlots( FilteredView* view )
{
    view->setFilterHighlighterSet( filters_ ? filters_->highlighters : nullptr );
    view->setLineToolTipProvider( [ this ]( LineNumber line ) { return filterToolTip( line ); } );
    connect( view, &FilteredView::addToFilters, this, &CrawlerWidget::addToFilters );

    connect( view, &FilteredView::newSelection, view, [ view ]( auto ) { view->update(); } );

    connect( view, &FilteredView::newSelection, this, &CrawlerWidget::jumpToMatchingLine );

    connect( view, &FilteredView::markLines, this, &CrawlerWidget::markLinesFromFiltered );


    connect( view, QOverload<const QString&>::of( &FilteredView::addToSearch ), this,
             &CrawlerWidget::addToSearch );

    connect( view, QOverload<const QString&>::of( &FilteredView::excludeFromSearch ), this,
             &CrawlerWidget::excludeFromSearch );

    connect( view, QOverload<const QString&>::of( &FilteredView::replaceSearch ), this,
             &CrawlerWidget::replaceSearch );

    connect( view, &FilteredView::mouseHoveredOverLine, this,
             &CrawlerWidget::mouseHoveredOverMatch );

    connect( view, &FilteredView::mouseLeftHoveringZone, overviewWidget_,
             &OverviewWidget::removeHighlight );

    connect( this, &CrawlerWidget::followSet, view, &FilteredView::followSet );

    connect( view, &FilteredView::followModeChanged, this, &CrawlerWidget::followModeChanged );

    connect( this, &CrawlerWidget::textWrapSet, view, &FilteredView::textWrapSet );

    connect( view, &FilteredView::activity, this, &CrawlerWidget::activityDetected );

    connect( view, &FilteredView::changeSearchLimits, this, &CrawlerWidget::setSearchLimits );

    connect( view, &FilteredView::saveDefaultSplitterSizes, this,
             &CrawlerWidget::saveSplitterSizes );

    connect( view, &FilteredView::changeFontSize, this, &CrawlerWidget::changeFontSize );

    connect( view, &FilteredView::clearSearchLimits, this, &CrawlerWidget::clearSearchLimits );

    connect( view, &AbstractLogView::addColorLabel, this,
             &CrawlerWidget::addColorLabelToSelection );

    connect( view, &AbstractLogView::sendSelectionToScratchpad, this,
             [ view, this ]() { Q_EMIT sendToScratchpad( view->getSelectedText() ); } );

    connect( view, &AbstractLogView::replaceScratchpadWithSelection, this,
             [ view, this ]() { Q_EMIT replaceDataInScratchpad( view->getSelectedText() ); } );

    connect( view, &FilteredView::exitView, logMainView_,
             QOverload<>::of( &LogMainView::setFocus ) );

    connect( view, &AbstractLogView::clearColorLabels, this, &CrawlerWidget::clearColorLabels );

    connect( logMainView_, &LogMainView::exitView, view,
             QOverload<>::of( &FilteredView::setFocus ) );
}

void CrawlerWidget::registerShortcuts()
{
    LOG_INFO << "registering shortcuts for crawler widget";

    for ( auto& shortcut : shortcuts_ ) {
        shortcut.second->deleteLater();
    }

    shortcuts_.clear();

    const auto& config = Configuration::get();
    const auto& configuredShortcuts = config.shortcuts();

    ShortcutAction::registerShortcut(
        configuredShortcuts, shortcuts_, this, Qt::WidgetWithChildrenShortcut,
        ShortcutAction::CrawlerChangeVisibilityForward, [ this ]() {
            visibilityBox_->setCurrentIndex( ( visibilityBox_->currentIndex() + 1 )
                                             % visibilityBox_->count() );
        } );

    ShortcutAction::registerShortcut(
        configuredShortcuts, shortcuts_, this, Qt::WidgetWithChildrenShortcut,
        ShortcutAction::CrawlerEnableCaseMatching, [ this ]() { matchCaseButton_->toggle(); } );

    ShortcutAction::registerShortcut(
        configuredShortcuts, shortcuts_, this, Qt::WidgetWithChildrenShortcut,
        ShortcutAction::CrawlerEnableRegex, [ this ]() { useRegexpButton_->toggle(); } );

    ShortcutAction::registerShortcut(
        configuredShortcuts, shortcuts_, this, Qt::WidgetWithChildrenShortcut,
        ShortcutAction::CrawlerEnableInverseMatching, [ this ]() { inverseButton_->toggle(); } );

    ShortcutAction::registerShortcut(
        configuredShortcuts, shortcuts_, this, Qt::WidgetWithChildrenShortcut,
        ShortcutAction::CrawlerEnableRegexCombining, [ this ]() { booleanButton_->toggle(); } );

    ShortcutAction::registerShortcut(
        configuredShortcuts, shortcuts_, this, Qt::WidgetWithChildrenShortcut,
        ShortcutAction::CrawlerEnableAutoRefresh, [ this ]() { searchRefreshButton_->toggle(); } );

    ShortcutAction::registerShortcut(
        configuredShortcuts, shortcuts_, this, Qt::WidgetWithChildrenShortcut,
        ShortcutAction::CrawlerKeepResults, [ this ]() { keepSearchResultsButton_->toggle(); } );

    ShortcutAction::registerShortcut( configuredShortcuts, shortcuts_, this,
                                      Qt::WidgetWithChildrenShortcut,
                                      ShortcutAction::CrawlerChangeVisibilityBackward, [ this ]() {
                                          int nextIndex = visibilityBox_->currentIndex() - 1;
                                          if ( nextIndex < 0 ) {
                                              nextIndex = visibilityBox_->count() - 1;
                                          }
                                          visibilityBox_->setCurrentIndex( nextIndex );
                                      } );

    ShortcutAction::registerShortcut(
        configuredShortcuts, shortcuts_, this, Qt::WidgetWithChildrenShortcut,
        ShortcutAction::CrawlerChangeVisibilityToMarksAndMatches, [ this ]() {
            if ( visibilityBox_->count() > 0 ) {
                visibilityBox_->setCurrentIndex( 0 );
            }
        } );

    ShortcutAction::registerShortcut( configuredShortcuts, shortcuts_, this,
                                      Qt::WidgetWithChildrenShortcut,
                                      ShortcutAction::CrawlerChangeVisibilityToMarks, [ this ]() {
                                          if ( visibilityBox_->count() > 1 ) {
                                              visibilityBox_->setCurrentIndex( 1 );
                                          }
                                      } );

    ShortcutAction::registerShortcut( configuredShortcuts, shortcuts_, this,
                                      Qt::WidgetWithChildrenShortcut,
                                      ShortcutAction::CrawlerChangeVisibilityToMatches, [ this ]() {
                                          if ( visibilityBox_->count() > 2 ) {
                                              visibilityBox_->setCurrentIndex( 2 );
                                          }
                                      } );

    ShortcutAction::registerShortcut(
        configuredShortcuts, shortcuts_, this, Qt::WidgetWithChildrenShortcut,
        ShortcutAction::CrawlerIncreseTopViewSize, [ this ]() { changeTopViewSize( 1 ); } );

    ShortcutAction::registerShortcut(
        configuredShortcuts, shortcuts_, this, Qt::WidgetWithChildrenShortcut,
        ShortcutAction::CrawlerDecreaseTopViewSize, [ this ]() { changeTopViewSize( -1 ); } );

    const auto exitSearchKeySequence = QKeySequence( QKeySequence::Cancel );
    ShortcutAction::registerShortcut( exitSearchKeySequence.toString(), shortcuts_, this,
                                      Qt::WidgetWithChildrenShortcut, [ this ]() {
                                          const auto activeView = this->activeView();
                                          if ( activeView ) {
                                              activeView->setFocus();
                                          }
                                      } );

    std::array<std::string, 9> colorLables = {
        ShortcutAction::LogViewAddColorLabel1, ShortcutAction::LogViewAddColorLabel2,
        ShortcutAction::LogViewAddColorLabel3, ShortcutAction::LogViewAddColorLabel4,
        ShortcutAction::LogViewAddColorLabel5, ShortcutAction::LogViewAddColorLabel6,
        ShortcutAction::LogViewAddColorLabel7, ShortcutAction::LogViewAddColorLabel8,
        ShortcutAction::LogViewAddColorLabel9,
    };

    for ( auto label = 0u; label < colorLables.size(); ++label ) {
        ShortcutAction::registerShortcut(
            configuredShortcuts, shortcuts_, this, Qt::WidgetWithChildrenShortcut,
            colorLables[ label ], [ this, label ]() { addColorLabelToSelection( label ); } );
    }

    ShortcutAction::registerShortcut(
        configuredShortcuts, shortcuts_, this, Qt::WidgetWithChildrenShortcut,
        ShortcutAction::LogViewAddNextColorLabel, [ this ]() { addNextColorLabelToSelection(); } );

    ShortcutAction::registerShortcut(
        configuredShortcuts, shortcuts_, this, Qt::WidgetWithChildrenShortcut,
        ShortcutAction::LogViewClearColorLabels, [ this ]() { clearColorLabels(); } );

    const std::array<std::string, LogFilteredData::NbBookmarks> setBookmarkActions = {
        ShortcutAction::LogViewSetBookmark1, ShortcutAction::LogViewSetBookmark2,
        ShortcutAction::LogViewSetBookmark3, ShortcutAction::LogViewSetBookmark4,
        ShortcutAction::LogViewSetBookmark5, ShortcutAction::LogViewSetBookmark6,
        ShortcutAction::LogViewSetBookmark7, ShortcutAction::LogViewSetBookmark8,
    };
    const std::array<std::string, LogFilteredData::NbBookmarks> jumpToBookmarkActions = {
        ShortcutAction::LogViewJumpToBookmark1, ShortcutAction::LogViewJumpToBookmark2,
        ShortcutAction::LogViewJumpToBookmark3, ShortcutAction::LogViewJumpToBookmark4,
        ShortcutAction::LogViewJumpToBookmark5, ShortcutAction::LogViewJumpToBookmark6,
        ShortcutAction::LogViewJumpToBookmark7, ShortcutAction::LogViewJumpToBookmark8,
    };
    const std::array<std::string, LogFilteredData::NbBookmarks> jumpToPreviousBookmarkActions = {
        ShortcutAction::LogViewJumpToPreviousBookmark1,
        ShortcutAction::LogViewJumpToPreviousBookmark2,
        ShortcutAction::LogViewJumpToPreviousBookmark3,
        ShortcutAction::LogViewJumpToPreviousBookmark4,
        ShortcutAction::LogViewJumpToPreviousBookmark5,
        ShortcutAction::LogViewJumpToPreviousBookmark6,
        ShortcutAction::LogViewJumpToPreviousBookmark7,
        ShortcutAction::LogViewJumpToPreviousBookmark8,
    };

    for ( auto slot = 0u; slot < LogFilteredData::NbBookmarks; ++slot ) {
        ShortcutAction::registerShortcut( configuredShortcuts, shortcuts_, this,
                                          Qt::WidgetWithChildrenShortcut,
                                          setBookmarkActions[ slot ],
                                          [ this, slot ]() { toggleBookmark( slot ); } );
        ShortcutAction::registerShortcut( configuredShortcuts, shortcuts_, this,
                                          Qt::WidgetWithChildrenShortcut,
                                          jumpToBookmarkActions[ slot ],
                                          [ this, slot ]() { jumpToBookmark( slot, false ); } );
        ShortcutAction::registerShortcut( configuredShortcuts, shortcuts_, this,
                                          Qt::WidgetWithChildrenShortcut,
                                          jumpToPreviousBookmarkActions[ slot ],
                                          [ this, slot ]() { jumpToBookmark( slot, true ); } );
    }

    logMainView_->registerShortcuts();
    filteredView_->registerShortcuts();
}

void CrawlerWidget::loadIcons()
{
    searchRefreshButton_->setIcon( iconLoader_.load( "icons8-search-refresh" ) );
    useRegexpButton_->setIcon( iconLoader_.load( "regex" ) );
    inverseButton_->setIcon( iconLoader_.load( "icons8-not-equal" ) );
    booleanButton_->setIcon( iconLoader_.load( "icons8-venn-diagram" ) );
    clearButton_->setIcon( iconLoader_.load( "icons8-delete" ) );
    searchButton_->setIcon( iconLoader_.load( "icons8-search" ) );
    keepSearchResultsButton_->setIcon( iconLoader_.load( "icons8-lock" ) );
    matchCaseButton_->setIcon( iconLoader_.load( "icons8-font-size" ) );
    stopButton_->setIcon( iconLoader_.load( "icons8-close-window" ) );
}

// Create a new search using the text passed, replace the currently
// used one and destroy the old one.
void CrawlerWidget::replaceCurrentSearch( const QString& searchText )
{
    replaceCurrentSearch( RegularExpressionPattern(
        searchText, matchCaseButton_->isChecked(), inverseButton_->isChecked(),
        booleanButton_->isChecked(), !useRegexpButton_->isChecked() ) );
}

void CrawlerWidget::replaceCurrentSearch( const RegularExpressionPattern& regexpPattern )
{
    const auto& searchText = regexpPattern.pattern;
    LOG_INFO << "replacing current search with " << searchText;
    // Interrupt the search if it's ongoing
    logFilteredData_->interruptSearch();

    // We have to wait for the last search update (100%)
    // before clearing/restarting to avoid having remaining results.

    // FIXME: this is a bit of a hack, we call processEvents
    // for Qt to empty its event queue, including (hopefully)
    // the search update event sent by logFilteredData_. It saves
    // us the overhead of having proper sync.
    const QPointer<CrawlerWidget> self( this );
    QApplication::processEvents( QEventLoop::ExcludeUserInputEvents );
    if ( !self ) {
        // Deleted while processing the events (its tab was closed)
        return;
    }

    nbMatches_ = 0_lcount;

    // Switch to "Marks and matches" view when in "Marks" view
    using VisibilityFlags = LogFilteredData::VisibilityFlags;
    if ( !filteredView_->visibility().testFlag( VisibilityFlags::Matches ) ) {
        visibilityBox_->setCurrentIndex( 0 );
    }

    // Clear and recompute the content of the filtered window.
    logFilteredData_->clearSearch();
    filteredView_->updateData();

    // Update the match overview
    overview_.updateData( logData_->getNbLine() );

    if ( !searchText.isEmpty() ) {

        // Filters are validated one by one when edited and searched one by one, compiling
        // the combined expression on every toggle would only slow the panel down
        const auto isFilterSearch = filterMode_ && filters_ && !filters_->filters.empty();
        std::optional<RegularExpression> hsExpression;
        if ( !isFilterSearch ) {
            hsExpression.emplace( regexpPattern );
        }
        const auto isValidExpression = isFilterSearch || hsExpression->isValid();

        if ( isValidExpression ) {
            // Activate the stop button
            stopButton_->setEnabled( true );
            stopButton_->show();
            clearButton_->hide();
            searchButton_->hide();
            // Start a new asynchronous search
            if ( isFilterSearch ) {
                // The combined expression is only used for validation and the views,
                // the filters are searched (and cached) one by one
                std::vector<LogFilteredData::FilterSpec> filterSpecs;
                for ( const auto& filter : filters_->filters ) {
                    filterSpecs.push_back( { filters::toPattern( filter ), filter.exclude } );
                }
                logFilteredData_->runFilterSearch( filterSpecs, searchStartLine_,
                                                   searchEndLine_ );
            }
            else {
                logFilteredData_->runSearch( regexpPattern, searchStartLine_, searchEndLine_ );
            }
            // Accept auto-refresh of the search
            searchState_.startSearch();
            searchInfoLine_->hide();
            logMainView_->setSearchPattern( regexpPattern );
            filteredView_->setSearchPattern( regexpPattern );
        }
        else {
            // The regexp is wrong
            logFilteredData_->clearSearch();
            filteredView_->updateData();
            searchState_.resetState();

            // Inform the user
            QString errorString = hsExpression->errorString();
            QString errorMessage = tr( "Error in expression" );
            // const int offset = regexp.patternErrorOffset();
            // if ( offset != -1 ) {
            //     errorMessage += " at position ";
            //     errorMessage += QString::number( offset );
            // }
            errorMessage += ": ";
            errorMessage += errorString;
            searchInfoLine_->setPalette( ErrorPalette );
            searchInfoLine_->setText( errorMessage );
            searchInfoLine_->show();

            logMainView_->setSearchPattern( {} );
            filteredView_->setSearchPattern( {} );
        }
    }
    else {
        searchState_.resetState();
        printSearchInfoMessage();
    }
}

// Updates the content of the drop down list for the saved searches,
// called when the SavedSearch has been changed.
void CrawlerWidget::updateSearchCombo()
{
    const QString text = searchLineEdit_->lineEdit()->text();
    searchLineEdit_->clear();

    auto searchHistory = savedSearches_->recentSearches();

    searchLineEdit_->addItems( searchHistory );
    // In case we had something that wasn't added to the list (blank...):
    searchLineEdit_->lineEdit()->setText( text );

    searchLineCompleter_->setModel( new QStringListModel( searchHistory, searchLineCompleter_ ) );
}

// Print the search info message.
void CrawlerWidget::printSearchInfoMessage( LinesCount nbMatches )
{
    QString text;

    switch ( searchState_.getState() ) {
    case SearchState::NoSearch:
        // Blank text is fine
        break;
    case SearchState::Static:
    case SearchState::Autorefreshing:
        // Some languages translate the plural the same as the singular, so use the full string
        text = nbMatches.get() > 1 ? tr( "%1 matches found" ).arg( nbMatches.get() )
                                   : tr( "%1 match found" ).arg( nbMatches.get() );
        break;
    case SearchState::FileTruncated:
    case SearchState::TruncatedAutorefreshing:
        text = tr( "File truncated on disk" );
        break;
    }

    searchInfoLine_->setPalette( searchInfoLineDefaultPalette_ );
    searchInfoLine_->setText( text );
    searchInfoLine_->setVisible( !text.isEmpty() );
}

// Change the data status and, if needed, advise upstream.
void CrawlerWidget::changeDataStatus( DataStatus status )
{
    if ( ( status != dataStatus_ )
         && ( !( dataStatus_ == DataStatus::NEW_FILTERED_DATA
                 && status == DataStatus::NEW_DATA ) ) ) {
        dataStatus_ = status;
        Q_EMIT dataStatusChanged( dataStatus_ );
    }
}

// Determine the right encoding and set the views.
void CrawlerWidget::updateEncoding()
{
    const QTextCodec* textCodec = [ this ]() {
        QTextCodec* codec = nullptr;
        if ( !encodingMib_ ) {
            codec = logData_->getDetectedEncoding();
        }
        else {
            codec = QTextCodec::codecForMib( *encodingMib_ );
        }
        return codec ? codec : QTextCodec::codecForLocale();
    }();

    QString encodingPrefix = encodingMib_ ? tr( "Displayed as %1" ) : tr( "Detected as %1" );
    encodingText_ = encodingPrefix.arg( textCodec->name().constData() );

    logData_->interruptLoading();

    logData_->setDisplayEncoding( textCodec->name().constData() );
    logMainView_->forceRefresh();
    logFilteredData_->setDisplayEncoding( textCodec->name().constData() );
    filteredView_->forceRefresh();
}

void CrawlerWidget::setMainViewVisible( bool visible )
{
    if ( visible == !logMainView_->isHidden() ) {
        return;
    }

    if ( !visible ) {
        const auto viewSizes = sizes();
        const auto total = viewSizes.at( 0 ) + viewSizes.at( 1 );
        if ( total > 0 ) {
            hiddenMainViewRatio_ = static_cast<double>( viewSizes.at( 0 ) ) / total;
        }

        const auto mainViewHadFocus = logMainView_->hasFocus();
        logMainView_->hide();
        if ( mainViewHadFocus ) {
            filteredView_->setFocus();
        }
        return;
    }

    logMainView_->show();

    // The height may have changed while the main view was hidden: keep the ratio
    const auto viewSizes = sizes();
    const auto total = viewSizes.at( 0 ) + viewSizes.at( 1 );
    const auto mainViewSize = static_cast<int>( total * hiddenMainViewRatio_ + 0.5 );
    setSizes( { mainViewSize, total - mainViewSize } );
}

// Change the respective size of the two views
void CrawlerWidget::changeTopViewSize( int32_t delta )
{
    int min, max;
    getRange( 1, &min, &max );
    LOG_DEBUG << "CrawlerWidget::changeTopViewSize " << sizes().at( 0 ) << " " << min << " " << max;
    moveSplitter( closestLegalPosition( sizes().at( 0 ) + ( delta * 10 ), 1 ), 1 );
    LOG_DEBUG << "CrawlerWidget::changeTopViewSize " << sizes().at( 0 );
}

void CrawlerWidget::addColorLabelToSelection( size_t label )
{
    updateColorLabels( colorLabelsManager_.setColorLabel( label, getSelectedText() ) );
}

void CrawlerWidget::addNextColorLabelToSelection()
{
    updateColorLabels( colorLabelsManager_.setNextColorLabel( getSelectedText() ) );
}

void CrawlerWidget::clearColorLabels()
{
    updateColorLabels( colorLabelsManager_.clear() );
}

void CrawlerWidget::updateColorLabels(
    const ColorLabelsManager::QuickHighlightersCollection& labels )
{
    logMainView_->setQuickHighlighters( labels );
    filteredView_->setQuickHighlighters( labels );
}

//
// SearchState implementation
//
void CrawlerWidget::SearchState::resetState()
{
    state_ = NoSearch;
}

void CrawlerWidget::SearchState::setAutorefresh( bool refresh )
{
    autoRefreshRequested_ = refresh;

    if ( refresh ) {
        if ( state_ == Static )
            state_ = Autorefreshing;
        /*
        else if ( state_ == FileTruncated )
            state_ = TruncatedAutorefreshing;
        */
    }
    else {
        if ( state_ == Autorefreshing )
            state_ = Static;
        else if ( state_ == TruncatedAutorefreshing )
            state_ = FileTruncated;
    }
}

void CrawlerWidget::SearchState::truncateFile()
{
    if ( state_ == Autorefreshing || state_ == TruncatedAutorefreshing ) {
        state_ = TruncatedAutorefreshing;
    }
    else {
        state_ = FileTruncated;
    }
}

void CrawlerWidget::SearchState::changeExpression()
{
    if ( state_ == Autorefreshing )
        state_ = Static;
}

void CrawlerWidget::SearchState::stopSearch()
{
    if ( state_ == Autorefreshing )
        state_ = Static;
}

void CrawlerWidget::SearchState::startSearch()
{
    if ( autoRefreshRequested_ )
        state_ = Autorefreshing;
    else
        state_ = Static;
}

/*
 * CrawlerWidgetContext
 */
CrawlerWidgetContext::CrawlerWidgetContext( const QString& string )
{
    if ( string.startsWith( '{' ) ) {
        loadFromJson( string );
    }
    else {
        loadFromString( string );
    }
}

void CrawlerWidgetContext::loadFromString( const QString& string )
{
    QRegularExpression regex( "S(\\d+):(\\d+)" );
    QRegularExpressionMatch match = regex.match( string );
    if ( match.hasMatch() ) {
        sizes_ = { match.captured( 1 ).toInt(), match.captured( 2 ).toInt() };
        LOG_DEBUG << "sizes_: " << sizes_[ 0 ] << " " << sizes_[ 1 ];
    }
    else {
        LOG_WARNING << "Unrecognised view size: " << string.toLocal8Bit().data();

        // Default values;
        sizes_ = { 400, 100 };
    }

    QRegularExpression case_refresh_regex( "IC(\\d+):AR(\\d+)" );
    match = case_refresh_regex.match( string );
    if ( match.hasMatch() ) {
        ignoreCase_ = ( match.captured( 1 ).toInt() == 1 );
        autoRefresh_ = ( match.captured( 2 ).toInt() == 1 );

        LOG_DEBUG << "ignore_case_: " << ignoreCase_ << " auto_refresh_: " << autoRefresh_;
    }
    else {
        LOG_WARNING << "Unrecognised case/refresh: " << string.toLocal8Bit().data();
        ignoreCase_ = false;
        autoRefresh_ = false;
    }

    QRegularExpression follow_regex( "AR(\\d+):FF(\\d+)" );
    match = follow_regex.match( string );
    if ( match.hasMatch() ) {
        followFile_ = ( match.captured( 2 ).toInt() == 1 );

        LOG_DEBUG << "follow_file_: " << followFile_;
    }
    else {
        LOG_WARNING << "Unrecognised follow file " << string.toLocal8Bit().data();
        followFile_ = false;
    }

    useRegexp_ = Configuration::get().mainRegexpType() == SearchRegexpType::ExtendedRegexp;
}

void CrawlerWidgetContext::loadFromJson( const QString& json )
{
    // Filter patterns are not limited to Latin-1
    const auto properties = QJsonDocument::fromJson( json.toUtf8() ).toVariant().toMap();

    if ( properties.contains( "S" ) ) {
        const auto sizes = properties.value( "S" ).toList();
        for ( const auto& s : sizes ) {
            sizes_.append( s.toInt() );
        }
    }

    ignoreCase_ = properties.value( "IC" ).toBool();
    autoRefresh_ = properties.value( "AR" ).toBool();
    followFile_ = properties.value( "FF" ).toBool();
    if ( properties.contains( "RE" ) ) {
        useRegexp_ = properties.value( "RE" ).toBool();
    }
    else {
        useRegexp_ = Configuration::get().mainRegexpType() == SearchRegexpType::ExtendedRegexp;
    }

    if ( properties.contains( "IR" ) ) {
        inverseRegexp_ = properties.value( "IR" ).toBool();
    }
    else {
        inverseRegexp_ = false;
    }

    if ( properties.contains( "BC" ) ) {
        useBooleanCombination_ = properties.value( "BC" ).toBool();
    }
    else {
        useBooleanCombination_ = false;
    }

    if ( properties.contains( "M" ) ) {
        const auto marks = properties.value( "M" ).toList();
        for ( const auto& m : marks ) {
            marks_.append( m.toUInt() );
        }
    }

    filters_ = properties.value( "FL" ).toMap();
}

QString CrawlerWidgetContext::toString() const
{
    const auto toVariantList = []( const auto& list ) -> QVariantList {
        QVariantList variantList;
        for ( const auto& item : list ) {
            variantList.append( static_cast<qulonglong>( item ) );
        }
        return variantList;
    };

    QVariantMap properies;

    properies[ "S" ] = toVariantList( sizes_ );
    properies[ "IC" ] = ignoreCase_;
    properies[ "AR" ] = autoRefresh_;
    properies[ "FF" ] = followFile_;
    properies[ "RE" ] = useRegexp_;
    properies[ "IR" ] = inverseRegexp_;
    properies[ "BC" ] = useBooleanCombination_;
    properies[ "M" ] = toVariantList( marks_ );
    if ( !filters_.isEmpty() ) {
        properies[ "FL" ] = filters_;
    }

    return QJsonDocument::fromVariant( properies ).toJson( QJsonDocument::Compact );
}

void CrawlerWidget::setFilters( std::shared_ptr<const FilterSnapshot> filters )
{
    filters_ = std::move( filters );

    logMainView_->setFilterHighlighterSet( filters_->highlighters );
    for ( const auto& view : filteredViewsData_ ) {
        view.first->setFilterHighlighterSet( filters_->highlighters );
    }

    // The search is started when the file is loaded
    if ( filterMode_ && firstLoadDone_ ) {
        runFilterSearch();
    }
}

FilterHits CrawlerWidget::filterHits() const
{
    FilterHits hits;
    if ( !filterMode_ || !filters_ ) {
        return hits;
    }

    for ( const auto& filter : filters_->filters ) {
        const auto count = logFilteredData_->getFilterMatchCount( filters::toPattern( filter ) );
        if ( count ) {
            hits[ filter.id ] = static_cast<qint64>( count->get() );
        }
    }
    return hits;
}

uint64_t CrawlerWidget::filtersRevision() const
{
    return filters_ ? filters_->revision : 0;
}

void CrawlerWidget::setFilterMode( bool filterMode )
{
    filterMode_ = filterMode;
    filterModeButton_->setChecked( filterMode );
}

void CrawlerWidget::rerunCurrentSearch()
{
    if ( filterMode_ ) {
        runFilterSearch();
    }
    else {
        replaceCurrentSearch( searchLineEdit_->currentText() );
    }
}

void CrawlerWidget::runFilterSearch()
{
    tabbedFilteredView_->setTabText( tabbedFilteredView_->currentIndex(), tr( "Filters" ) );
    replaceCurrentSearch( filters_ ? filters_->searchPattern : RegularExpressionPattern{} );
}

QString CrawlerWidget::searchTextAsRegexBoolean() const
{
    auto text = searchLineEdit_->currentText();
    if ( text.isEmpty() ) {
        return text;
    }

    const auto isRegex = useRegexpButton_->isChecked();
    const auto toRegexSubPattern = [ isRegex ]( QString pattern ) {
        if ( !isRegex ) {
            pattern = QRegularExpression::escape( pattern );
        }
        // See filters::toBooleanSubPattern for the reason of the group
        pattern = "(?:" + pattern + ")";
        pattern.replace( "\"", "\\\"" );
        return "\"" + pattern + "\"";
    };

    if ( !booleanButton_->isChecked() ) {
        return toRegexSubPattern( text );
    }

    if ( isRegex ) {
        return text;
    }

    // Plain text sub-patterns of a boolean expression have to be escaped
    // as the expression is going to be switched to regex mode
    QString result;
    qsizetype position = 0;
    while ( position < text.size() ) {
        const auto start = text.indexOf( '"', position );
        if ( start < 0 ) {
            result.append( text.mid( position ) );
            break;
        }

        auto end = start + 1;
        while ( end < text.size() && !( text[ end ] == '"' && text[ end - 1 ] != '\\' ) ) {
            ++end;
        }

        result.append( text.mid( position, start - position ) );
        auto subPattern = text.mid( start + 1, end - start - 1 );
        subPattern.replace( "\\\"", "\"" );
        result.append( toRegexSubPattern( subPattern ) );
        position = end + 1;
    }

    return result;
}

void CrawlerWidget::addFiltersToSearch( const std::vector<FilterNode>& filters,
                                        FilterSearchMode mode )
{
    std::vector<const FilterNode*> includes;
    std::vector<const FilterNode*> excludes;
    for ( const auto& filter : filters ) {
        const auto exclude = mode == FilterSearchMode::Exclude || filter.exclude;
        ( exclude ? excludes : includes ).push_back( &filter );
    }

    const auto currentText = searchLineEdit_->currentText();
    const auto replace = mode == FilterSearchMode::Replace || currentText.isEmpty();

    // A boolean expression is only built when the search can't be written as a plain
    // pattern: includes and excludes together, or a search that is already boolean or
    // inverted
    const auto plainSearch = !booleanButton_->isChecked() && !inverseButton_->isChecked();
    const auto onlyIncludes = excludes.empty() && ( replace || plainSearch );
    const auto onlyExcludes = includes.empty() && replace;

    if ( onlyIncludes || onlyExcludes ) {
        const auto& subFilters = onlyIncludes ? includes : excludes;

        // The match case button carries the case of the filters when they agree,
        // the others get an inline flag
        auto matchCase = matchCaseButton_->isChecked();
        if ( replace ) {
            const auto caseSensitive = subFilters.front()->caseSensitive;
            if ( std::all_of( subFilters.begin(), subFilters.end(),
                              [ caseSensitive ]( const FilterNode* filter ) {
                                  return filter->caseSensitive == caseSensitive;
                              } ) ) {
                matchCase = caseSensitive;
            }
        }

        const auto plainText = replace && subFilters.size() == 1
                               && !subFilters.front()->useRegex
                               && subFilters.front()->caseSensitive == matchCase;

        QString expression;
        if ( plainText ) {
            expression = subFilters.front()->pattern;
        }
        else {
            // Inline options inside an alternative would leak into the following ones
            const auto toAlternative = []( QString regex ) {
                return regex.contains( "(?" ) ? "(?:" + regex + ")" : regex;
            };

            QStringList alternatives;
            if ( !replace ) {
                alternatives.append(
                    toAlternative( useRegexpButton_->isChecked()
                                       ? currentText
                                       : QRegularExpression::escape( currentText ) ) );
            }
            for ( const auto* filter : subFilters ) {
                const auto regex = filter->useRegex ? filter->pattern
                                                    : QRegularExpression::escape( filter->pattern );
                if ( filter->caseSensitive == matchCase ) {
                    alternatives.append( toAlternative( regex ) );
                }
                else {
                    alternatives.append( QString( filter->caseSensitive ? "(?-i:%1)" : "(?i:%1)" )
                                             .arg( regex ) );
                }
            }
            expression = alternatives.join( '|' );
        }

        matchCaseButton_->setChecked( matchCase );
        useRegexpButton_->setChecked( !plainText );
        booleanButton_->setChecked( false );
        inverseButton_->setChecked( onlyExcludes );

        setSearchPattern( expression );
        return;
    }

    const auto joinSubPatterns = []( const std::vector<const FilterNode*>& subFilters ) {
        QStringList subPatterns;
        for ( const auto* filter : subFilters ) {
            subPatterns.append( filters::toBooleanSubPattern( *filter ) );
        }
        return subPatterns.join( " or " );
    };

    auto expression = replace ? QString{} : searchTextAsRegexBoolean();
    if ( !expression.isEmpty() && inverseButton_->isChecked() ) {
        expression = "not(" + expression + ")";
    }

    if ( !includes.empty() ) {
        expression = expression.isEmpty()
                         ? joinSubPatterns( includes )
                         : "(" + expression + ") or " + joinSubPatterns( includes );
    }

    if ( !excludes.empty() ) {
        const auto excluded = "not(" + joinSubPatterns( excludes ) + ")";
        expression
            = expression.isEmpty() ? excluded : "(" + expression + ") and " + excluded;
    }

    // Sub-patterns carry their own case sensitivity
    useRegexpButton_->setChecked( true );
    booleanButton_->setChecked( true );
    inverseButton_->setChecked( false );

    setSearchPattern( expression );
}

void CrawlerWidget::findFilterMatch( const std::vector<FilterNode>& filters, bool backward )
{
    std::vector<const FilterNode*> filterPointers;
    for ( const auto& filter : filters ) {
        filterPointers.push_back( &filter );
    }

    const QRegularExpression regex( filters::toAnyOfRegex( filterPointers ),
                                    QRegularExpression::UseUnicodePropertiesOption );
    if ( !regex.isValid() ) {
        LOG_WARNING << "Invalid filter regex: " << regex.errorString();
        return;
    }

    // Triggered from the filter panel the jump goes to the filtered view, only a focused
    // main view keeps it
    const auto inFilteredView = !logMainView_->hasFocus();

    std::vector<RegularExpressionPattern> patterns;
    for ( const auto& filter : filters ) {
        patterns.push_back( filters::toPattern( filter ) );
    }

    // Use the cached results of the filters when available, no need to read the file
    const auto fromLine
        = inFilteredView && logFilteredData_->getNbLine() > 0_lcount
              ? logFilteredData_->getMatchingLineNumber( filteredView_->getViewPosition() )
              : logMainView_->getViewPosition();
    const auto cachedMatch = logFilteredData_->findFilterMatch( patterns, fromLine, backward );
    if ( cachedMatch ) {
        if ( !cachedMatch->has_value() ) {
            searchInfoLine_->setText( tr( "No match of the filter" ) );
            searchInfoLine_->show();
            return;
        }

        const auto line = **cachedMatch;
        if ( !inFilteredView ) {
            logMainView_->selectAndDisplayLine( line );
            return;
        }

        const auto filteredIndex = logFilteredData_->getLineIndexNumber( line );
        if ( filteredIndex < logFilteredData_->getNbLine()
             && logFilteredData_->getMatchingLineNumber( filteredIndex ) == line ) {
            filteredView_->selectAndDisplayLine( filteredIndex );
        }
        else {
            searchInfoLine_->setText( tr( "No match of the filter in the filtered view" ) );
            searchInfoLine_->show();
        }
        return;
    }

    AbstractLogView* view = logMainView_;
    if ( inFilteredView ) {
        view = filteredView_;
        connect(
            filteredView_, &AbstractLogView::quickFindDone, this,
            [ this ]( bool hasMatch ) {
                if ( !hasMatch ) {
                    searchInfoLine_->setText( tr( "No match of the filter in the filtered view" ) );
                    searchInfoLine_->show();
                }
            },
            Qt::SingleShotConnection );
    }
    view->searchUsingMatcher( QuickFindMatcher( true, regex ), backward );
}

LineNumber CrawlerWidget::currentLogLine() const
{
    return filteredView_->hasFocus()
               ? logFilteredData_->getMatchingLineNumber( filteredView_->getViewPosition() )
               : logMainView_->getViewPosition();
}

QString CrawlerWidget::filterToolTip( LineNumber line ) const
{
    if ( !filters_ || filters_->filters.empty() ) {
        return {};
    }

    // Only run when a tool tip is requested: the cached results of the filters are used
    // when available, otherwise the single line is matched
    std::optional<QString> lineText;
    QStringList rows;
    for ( const auto& filter : filters_->filters ) {
        auto matched = filterMode_
                           ? logFilteredData_->isFilterMatch( filters::toPattern( filter ), line )
                           : std::optional<bool>{};
        if ( !matched ) {
            if ( !lineText ) {
                lineText = logData_->getLineString( line );
            }
            const QRegularExpression regex( filters::toRegex( filter ),
                                            QRegularExpression::UseUnicodePropertiesOption );
            matched = regex.isValid() && regex.match( *lineText ).hasMatch();
        }
        if ( !*matched ) {
            continue;
        }

        const auto foreColor = filters::displayForeColor( filter );
        QString style;
        if ( foreColor.isValid() ) {
            style += QString( "color:%1;" ).arg( foreColor.name() );
        }
        if ( filter.backColor.isValid() ) {
            style += QString( "background-color:%1;" ).arg( filter.backColor.name() );
        }

        QStringList options;
        if ( filter.exclude ) {
            options.append( tr( "exclude" ) );
        }
        if ( filter.useRegex ) {
            options.append( tr( "regex" ) );
        }
        if ( filter.caseSensitive ) {
            options.append( tr( "match case" ) );
        }

        const auto description = filter.name.section( '\n', 0, 0 );
        rows.append( QString( "<tr><td align=right>%1</td>"
                              "<td style=\"white-space:pre;%2\"> %3 </td>"
                              "<td style=\"white-space:pre\"><i>%4</i></td>"
                              "<td style=\"white-space:pre\">%5</td></tr>" )
                         .arg( filters::numberText( filter ) )
                         .arg( style, filter.pattern.toHtmlEscaped(),
                               options.join( ", " ).toHtmlEscaped(),
                               description.toHtmlEscaped() ) );
    }

    if ( rows.isEmpty() ) {
        return {};
    }

    return "<table cellspacing=2 cellpadding=1>" + rows.join( "" ) + "</table>";
}

void CrawlerWidget::toggleBookmark( size_t slot )
{
    logFilteredData_->toggleBookmark( slot, currentLogLine() );

    // Bookmarks are marks: refresh both views and the overview
    filteredView_->updateData();
    logMainView_->updateData();
    overview_.updateData( logData_->getNbLine() );
    update();
}

void CrawlerWidget::jumpToBookmark( size_t slot, bool backward )
{
    const auto line = logFilteredData_->findBookmark( slot, currentLogLine(), backward );
    if ( !line ) {
        searchInfoLine_->setText( tr( "Bookmark %1 is not set" ).arg( slot + 1 ) );
        searchInfoLine_->show();
        return;
    }

    if ( filteredView_->hasFocus() || logMainView_->isHidden() ) {
        const auto index = logFilteredData_->getLineIndexNumber( *line );
        if ( logFilteredData_->getMatchingLineNumber( index ) == *line ) {
            filteredView_->selectAndDisplayLine( index );
            return;
        }
    }

    logMainView_->selectAndDisplayLine( *line );
    logMainView_->setFocus();
}
