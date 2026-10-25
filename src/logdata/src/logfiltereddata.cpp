/*
 * Copyright (C) 2009, 2010, 2011, 2012, 2013, 2017 Nicolas Bonnefon and other contributors
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

// This file implements LogFilteredData
// It stores a pointer to the LogData that created it,
// so should always be destroyed before the LogData.

#include "log.h"

#include <KDSignalThrottler.h>
#include <QString>
#include <QTimer>

#include <algorithm>
#include <cassert>
#include <functional>
#include <numeric>
#include <tuple>
#include <vector>

#include "logdata.h"
#include "logfiltereddata.h"

#include "configuration.h"
#include "readablesize.h"
#include "synchronization.h"

// Usual constructor: just copy the data, the search is started by runSearch()
LogFilteredData::LogFilteredData( const LogData* logData )
    : AbstractLogData()
    , matching_lines_( SearchResultArray() )
    , currentRegExp_()
    , visibility_()
    , workerThread_( *logData )
{
    // Starts with an empty result list
    maxLength_ = 0_length;
    maxLengthMarks_ = 0_length;
    nbLinesProcessed_ = 0_lcount;

    sourceLogData_ = logData;

    visibility_ = VisibilityFlags::Marks | VisibilityFlags::Matches;

    // Forward the update signal
    connect( &workerThread_, &LogFilteredDataWorker::searchProgressed, this,
             &LogFilteredData::handleSearchProgressed );

    searchProgressThrottler_.setTimeout( 100 );
    connect( this, &LogFilteredData::searchProgressedThrottled, &searchProgressThrottler_,
             &KDToolBox::KDGenericSignalThrottler::throttle );

    connect( &searchProgressThrottler_, &KDToolBox::KDGenericSignalThrottler::triggered, this,
             &LogFilteredData::handleSearchProgressedThrottled );
}

void LogFilteredData::runSearch( const RegularExpressionPattern& regExp )
{
    runSearch( regExp, 0_lnum, LineNumber( getNbTotalLines().get() ) );
}

// Run the search and send newDataAvailable() signals.
void LogFilteredData::runSearch( const RegularExpressionPattern& regExp, LineNumber startLine,
                                 LineNumber endLine )
{
    LOG_DEBUG << "Entering runSearch";

    const auto& config = Configuration::get();

    clearSearch();
    currentRegExp_ = regExp;
    currentSearchKey_ = makeCacheKey( regExp, startLine, endLine );
    LOG_INFO << "Search cache key: " << regExp.pattern << "_" << startLine.get() << "_"
             << endLine.get();

    bool shouldRunSearch = true;
    if ( config.useSearchResultsCache() ) {
        const auto cachedResults = searchResultsCache_.find( currentSearchKey_ );
        if ( cachedResults != std::end( searchResultsCache_ ) ) {
            LOG_INFO << "Got result from cache";
            shouldRunSearch = false;
            matching_lines_ = cachedResults->second.matching_lines;
            maxLength_ = cachedResults->second.maxLength;

            marks_and_matches_ = matching_lines_ | marks_;

            Q_EMIT searchProgressed( LinesCount( matching_lines_.cardinality() ), 100, startLine );
        }
    }

    if ( shouldRunSearch ) {
        attachReader();
        workerThread_.search( currentRegExp_, startLine, endLine );
    }
}

void LogFilteredData::updateSearch( LineNumber startLine, LineNumber endLine )
{
    LOG_DEBUG << "Entering updateSearch";

    if ( filterMode_ ) {
        if ( startLine != filterStartLine_ ) {
            runFilterSearch( currentFilters_, startLine, endLine );
        }
        else {
            filterEndLine_ = endLine;
            startMissingFilterSearch();
        }
        return;
    }

    currentSearchKey_ = {};

    attachReader();
    workerThread_.updateSearch( currentRegExp_, startLine, endLine,
                                LineNumber( nbLinesProcessed_.get() ) );
}

void LogFilteredData::interruptSearch()
{
    LOG_DEBUG << "Entering interruptSearch";

    workerThread_.interrupt();
}

void LogFilteredData::clearSearch( bool dropCache )
{
    interruptSearch();

    currentRegExp_ = {};
    matching_lines_ = {};
    marks_and_matches_ = marks_;
    maxLength_ = 0_length;
    nbLinesProcessed_ = 0_lcount;

    filterMode_ = false;
    currentFilters_.clear();
    scanningFilterKeys_.clear();

    if ( dropCache ) {
        searchResultsCache_.clear();
        filterResults_.clear();
    }
}

LineNumber LogFilteredData::getMatchingLineNumber( LineNumber matchNum ) const
{
    return findLogDataLine( matchNum );
}

LineNumber LogFilteredData::getLineIndexNumber( LineNumber lineNumber ) const
{
    return findFilteredLine( lineNumber );
}

// Scan the list for the 'lineNumber' passed
bool LogFilteredData::isLineMatched( LineNumber lineNumber ) const
{
    return matching_lines_.contains( lineNumber.get() );
}

LinesCount LogFilteredData::getNbTotalLines() const
{
    return sourceLogData_->getNbLine();
}

LinesCount LogFilteredData::getNbMatches() const
{
    return LinesCount( matching_lines_.cardinality() );
}

LinesCount LogFilteredData::getNbMarks() const
{
    return LinesCount( marks_.cardinality() );
}

LogFilteredData::LineType LogFilteredData::lineTypeByIndex( LineNumber index ) const
{
    return lineTypeByLine( findLogDataLine( index ) );
}

LogFilteredData::LineType LogFilteredData::lineTypeByLine( LineNumber lineNumber ) const
{
    LineType line_type = LineTypeFlags::Plain;

    if ( isLineMarked( lineNumber ) )
        line_type |= LineTypeFlags::Mark;

    if ( isLineMatched( lineNumber ) )
        line_type |= LineTypeFlags::Match;

    return line_type;
}

void LogFilteredData::iterateOverLines( const std::function<void( LineNumber )>& callback ) const
{
    using CallbackFn = std::function<void( LineNumber )>;
    const auto& currentResults = currentResultArray();
    currentResults.iterate(
        []( uint64_t line, void* context ) -> bool {
            auto* callbackFn = static_cast<CallbackFn*>( context );
            callbackFn->operator()( LineNumber( line ) );
            return true;
        },
        static_cast<void*>( const_cast<CallbackFn*>( &callback ) ) );
}

// Delegation to our Marks object

void LogFilteredData::toggleMark( LineNumber line )
{
    if ( ( line >= 0_lnum ) && line < sourceLogData_->getNbLine() ) {
        if ( !marks_.addChecked( line.get() ) ) {
            marks_.remove( line.get() );
            removeBookmarksOnLine( line );
            updateMaxLengthMarks( {}, line );
        }
        else {
            updateMaxLengthMarks( line, {} );
        }
    }
    else {
        LOG_ERROR << "LogFilteredData::toggleMark trying to toggle a mark outside of the file.";
    }
}

void LogFilteredData::addMark( LineNumber line )
{
    if ( ( line >= 0_lnum ) && line < sourceLogData_->getNbLine() ) {
        marks_.add( line.get() );
        updateMaxLengthMarks( line, {} );
    }
    else {
        LOG_ERROR << "LogFilteredData::addMark trying to create a mark outside of the file.";
    }
}

bool LogFilteredData::isLineMarked( LineNumber line ) const
{
    return marks_.contains( line.get() );
}

OptionalLineNumber LogFilteredData::getMarkAfter( LineNumber line ) const
{
    OptionalLineNumber marked_line;
    const LineNumber::UnderlyingType rank = marks_.rank( line.get() );
    LineNumber::UnderlyingType nextMark;
    if ( marks_.select( rank, &nextMark ) ) {
        marked_line = LineNumber( nextMark );
    }

    return marked_line;
}

OptionalLineNumber LogFilteredData::getMarkBefore( LineNumber line ) const
{
    OptionalLineNumber marked_line;

    const LineNumber::UnderlyingType rank = marks_.rank( line.get() );

    if ( rank < 2 ) {
        return marked_line;
    }

    LineNumber::UnderlyingType nextMark;
    if ( marks_.select( rank - 2, &nextMark ) ) {
        marked_line = LineNumber( nextMark );
    }

    return marked_line;
}

void LogFilteredData::deleteMark( LineNumber line )
{
    marks_.remove( line.get() );
    removeBookmarksOnLine( line );
    updateMaxLengthMarks( {}, line );
}

void LogFilteredData::updateMaxLengthMarks( OptionalLineNumber added_line,
                                            OptionalLineNumber removed_line )
{
    marks_and_matches_ = matching_lines_ | marks_;

    if ( added_line.has_value() ) {
        maxLengthMarks_ = qMax( maxLengthMarks_, sourceLogData_->getLineLength( *added_line ) );
    }

    // Now update the max length if needed
    if ( removed_line.has_value()
         && sourceLogData_->getLineLength( *removed_line ) >= maxLengthMarks_ ) {
        LOG_DEBUG << "deleteMark recalculating longest mark";
        maxLengthMarks_ = 0_length;
        marks_.iterate(
            []( uint64_t line, void* context ) -> bool {
                auto* self = static_cast<LogFilteredData*>( context );
                self->maxLengthMarks_
                    = qMax( self->maxLengthMarks_,
                            self->sourceLogData_->getLineLength( LineNumber( line ) ) );
                return true;
            },
            static_cast<void*>( this ) );
    }
}

void LogFilteredData::clearMarks()
{
    marks_ = {};
    bookmarks_.clear();
    maxLengthMarks_ = 0_length;
}

QList<LineNumber> LogFilteredData::getMarks() const
{
    QList<LineNumber> markedLines;
    marks_.iterate(
        []( uint64_t line, void* context ) -> bool {
            static_cast<QList<LineNumber>*>( context )->append( LineNumber( line ) );
            return true;
        },
        static_cast<void*>( &markedLines ) );

    return markedLines;
}

void LogFilteredData::setVisibility( Visibility visi )
{
    visibility_ = visi;
}

LogFilteredData::Visibility LogFilteredData::visibility() const
{
    return visibility_;
}

void LogFilteredData::updateSearchResultsCache()
{
    const auto& config = Configuration::get();
    if ( !config.useSearchResultsCache() ) {
        return;
    }

    if ( currentSearchKey_ == SearchCacheKey{} ) {
        return;
    }

    const uint64_t maxCacheLines = config.searchResultsCacheLines();

    if ( matching_lines_.cardinality() > maxCacheLines ) {
        LOG_DEBUG << "LogFilteredData: too many matches to place in cache";
    }
    else {
        LOG_INFO << "LogFilteredData: caching results for key "
                 << std::get<0>( currentSearchKey_ ).pattern << "_"
                 << std::get<1>( currentSearchKey_ ) << "_" << std::get<2>( currentSearchKey_ );

        searchResultsCache_[ currentSearchKey_ ] = { matching_lines_, maxLength_ };
        auto cacheSize = std::accumulate( searchResultsCache_.cbegin(), searchResultsCache_.cend(),
                                          uint64_t{ 0 }, []( const auto& acc, const auto& next ) {
                                              return acc + next.second.matching_lines.cardinality();
                                          } );

        LOG_INFO << "LogFilteredData: cache size " << cacheSize;

        auto cachedResult = std::begin( searchResultsCache_ );
        while ( cachedResult != std::end( searchResultsCache_ ) && cacheSize > maxCacheLines ) {

            if ( cachedResult->first == currentSearchKey_ ) {
                ++cachedResult;
                continue;
            }

            cacheSize -= cachedResult->second.matching_lines.cardinality();
            cachedResult = searchResultsCache_.erase( cachedResult );
        }
    }
}

//
// Q_SLOTS:
//
void LogFilteredData::handleSearchProgressed( LinesCount nbMatches, int progress,
                                              LineNumber initialLine )
{
    assert( nbMatches >= 0_lcount );

    if ( filterMode_ ) {
        handleMultiSearchProgressed( progress, initialLine );
        return;
    }

    const auto searchResults = workerThread_.getSearchResults();

    matching_lines_ |= searchResults.newMatches;
    marks_and_matches_ |= searchResults.newMatches;

    maxLength_ = searchResults.maxLength;
    nbLinesProcessed_ = searchResults.processedLines;

    if ( progress == 100
         && nbLinesProcessed_.get() == getExpectedSearchEnd( currentSearchKey_ ).get() ) {
        updateSearchResultsCache();
    }

    {
        ScopedLock lock( searchProgressMutex_ );
        searchProgress_ = std::make_tuple( nbMatches, progress, initialLine );
    }

    Q_EMIT searchProgressedThrottled();

    if ( progress == 100 ) {
        detachReader();

        LOG_INFO << "Matches size " << readableSize( matching_lines_.getSizeInBytes( false ) )
                 << ", marks size " << readableSize( marks_.getSizeInBytes( false ) )
                 << ", union size " << readableSize( marks_and_matches_.getSizeInBytes( false ) );
    }
}

void LogFilteredData::handleSearchProgressedThrottled()
{
    LinesCount nbMatches;
    int progress;
    LineNumber initialLine;
    {
        ScopedLock lock( searchProgressMutex_ );
        std::tie( nbMatches, progress, initialLine ) = searchProgress_;
    }
    Q_EMIT searchProgressed( nbMatches, progress, initialLine );
}

LineNumber LogFilteredData::findLogDataLine( LineNumber index ) const
{
    const auto& currentResults = currentResultArray();

    LineNumber::UnderlyingType line = {};
    if ( currentResults.select( index.get(), &line ) ) {
        return LineNumber( line );
    }
    else {
        if ( !currentResults.isEmpty() ) {
            LOG_ERROR << "Index too big in LogFilteredData: " << index << " cache size "
                      << currentResults.cardinality();
        }
        return maxValue<LineNumber>();
    }
}

const SearchResultArray& LogFilteredData::currentResultArray() const
{
    if ( visibility_.testFlag( VisibilityFlags::Marks )
         && visibility_.testFlag( VisibilityFlags::Matches ) ) {
        return marks_and_matches_;
    }
    else if ( visibility_.testFlag( VisibilityFlags::Matches ) ) {
        return matching_lines_;
    }
    else {
        return marks_;
    }
}

LineNumber LogFilteredData::findFilteredLine( LineNumber lineNum ) const
{
    LineNumber::UnderlyingType index = currentResultArray().rank( lineNum.get() );

    if ( index > 0 ) {
        index--;
    }
    return LineNumber( index );
}

// Implementation of the virtual function.
QString LogFilteredData::doGetLineString( LineNumber index ) const
{
    const auto line = findLogDataLine( index );
    return sourceLogData_->getLineString( line );
}

// Implementation of the virtual function.
QString LogFilteredData::doGetExpandedLineString( LineNumber index ) const
{
    const auto line = findLogDataLine( index );
    return sourceLogData_->getExpandedLineString( line );
}

// Implementation of the virtual function.
klogg::vector<QString> LogFilteredData::doGetLines( LineNumber first_line, LinesCount number ) const
{
    return doGetLines( first_line, number,
                       [ this ]( const auto& line ) { return doGetLineString( line ); } );
}

// Implementation of the virtual function.
klogg::vector<QString> LogFilteredData::doGetExpandedLines( LineNumber first_line,
                                                          LinesCount number ) const
{
    return doGetLines( first_line, number,
                       [ this ]( const auto& line ) { return doGetExpandedLineString( line ); } );
}

klogg::vector<QString>
LogFilteredData::doGetLines( LineNumber first_line, LinesCount number,
                             const std::function<QString( LineNumber )>& lineGetter ) const
{
    klogg::vector<LineNumber::UnderlyingType> lineNumbers( number.get() );
    std::iota( lineNumbers.begin(), lineNumbers.end(), first_line.get() );

    klogg::vector<QString> lines( number.get() );
    std::transform(
        lineNumbers.cbegin(), lineNumbers.cend(), lines.begin(),
        [ &lineGetter ]( const auto& line ) { return lineGetter( LineNumber( line ) ); } );

    return lines;
}

LineNumber LogFilteredData::doGetLineNumber(LineNumber index) const
{
    return getMatchingLineNumber(index);
}

// Implementation of the virtual function.
LinesCount LogFilteredData::doGetNbLine() const
{
    const LinesCount::UnderlyingType nbLines = currentResultArray().cardinality();
    return LinesCount( nbLines );
}

// Implementation of the virtual function.
LineLength LogFilteredData::doGetMaxLength() const
{
    return qMax( maxLength_, maxLengthMarks_ );
}

// Implementation of the virtual function.
LineLength LogFilteredData::doGetLineLength( LineNumber lineNum ) const
{
    LineNumber line = findLogDataLine( lineNum );
    return sourceLogData_->getLineLength( line );
}

void LogFilteredData::doSetDisplayEncoding( const char* encoding )
{
    LOG_DEBUG << "AbstractLogData::setDisplayEncoding: " << encoding;
}

QTextCodec* LogFilteredData::doGetDisplayEncoding() const
{
    return sourceLogData_->getDisplayEncoding();
}

void LogFilteredData::doAttachReader() const
{
    sourceLogData_->attachReader();
}

void LogFilteredData::doDetachReader() const
{
    sourceLogData_->detachReader();
}

//
// Filter search
//

namespace {
// Memory allowed for the results of the filters that are not currently used
constexpr uint64_t UnusedFilterResultsBudget = 256 * 1024 * 1024;
} // namespace

QString LogFilteredData::makeFilterKey( const RegularExpressionPattern& pattern )
{
    return QString( "%1%2:%3" )
        .arg( pattern.isCaseSensitive ? 'C' : 'c' )
        .arg( pattern.isPlainText ? 'T' : 'R' )
        .arg( pattern.pattern );
}

void LogFilteredData::runFilterSearch( const std::vector<FilterSpec>& filters,
                                       LineNumber startLine, LineNumber endLine )
{
    LOG_DEBUG << "Entering runFilterSearch";

    clearSearch();

    if ( startLine != filterStartLine_ ) {
        // Cached results are only valid for the same search start
        filterResults_.clear();
    }

    filterMode_ = true;
    currentFilters_ = filters;
    filterStartLine_ = startLine;
    filterEndLine_ = endLine;

    ++filterUseCounter_;
    for ( const auto& filter : currentFilters_ ) {
        const auto result = filterResults_.find( makeFilterKey( filter.pattern ) );
        if ( result != filterResults_.end() ) {
            result->second.lastUse = filterUseCounter_;
        }
    }
    trimFilterResults();

    startMissingFilterSearch();
}

void LogFilteredData::startMissingFilterSearch()
{
    klogg::vector<QString> missingKeys;
    klogg::vector<RegularExpressionPattern> missingPatterns;
    auto initialLine = filterEndLine_;

    for ( const auto& filter : currentFilters_ ) {
        const auto key = makeFilterKey( filter.pattern );
        if ( std::find( missingKeys.begin(), missingKeys.end(), key ) != missingKeys.end() ) {
            continue;
        }

        auto [ result, inserted ] = filterResults_.try_emplace( key );
        auto& filterResult = result->second;
        if ( inserted ) {
            filterResult.processedEnd = filterStartLine_;
        }
        filterResult.lastUse = filterUseCounter_;

        if ( filterResult.processedEnd >= filterEndLine_ ) {
            continue;
        }

        // The last searched line might have been incomplete (not LF terminated yet)
        if ( filterResult.processedEnd > filterStartLine_ ) {
            filterResult.processedEnd = filterResult.processedEnd - 1_lcount;
            filterResult.matchingLines.remove( filterResult.processedEnd.get() );
        }

        initialLine = qMin( initialLine, filterResult.processedEnd );
        missingKeys.push_back( key );
        auto pattern = filter.pattern;
        pattern.isExclude = false;
        missingPatterns.push_back( pattern );
    }

    composeFilterResults();

    if ( missingPatterns.empty() ) {
        scanningFilterKeys_.clear();
        Q_EMIT searchProgressed( getNbMatches(), 100, filterStartLine_ );
        return;
    }

    LOG_INFO << "Searching " << missingPatterns.size() << " filter patterns from line "
             << initialLine;

    scanningFilterKeys_ = std::move( missingKeys );
    attachReader();
    scanningGeneration_
        = workerThread_.searchMultiple( missingPatterns, initialLine, filterEndLine_ );
}

void LogFilteredData::handleMultiSearchProgressed( int progress, LineNumber initialLine )
{
    const auto searchResults = workerThread_.getMultiSearchResults();

    if ( searchResults.generation == scanningGeneration_
         && searchResults.newMatches.size() == scanningFilterKeys_.size() ) {
        for ( size_t i = 0; i < scanningFilterKeys_.size(); ++i ) {
            auto& filterResult = filterResults_[ scanningFilterKeys_[ i ] ];
            filterResult.matchingLines |= searchResults.newMatches[ i ];
            filterResult.maxLength = qMax( filterResult.maxLength, searchResults.maxLengths[ i ] );
            filterResult.processedEnd = qMax(
                filterResult.processedEnd, LineNumber( searchResults.processedLines.get() ) );
        }
    }

    // Progress is reported for every block of lines, combining all the filters each time
    // would make the search quadratic
    constexpr qint64 ComposeIntervalMs = 250;
    if ( progress == 100 || !lastFilterCompose_.isValid()
         || lastFilterCompose_.elapsed() >= ComposeIntervalMs ) {
        composeFilterResults();
    }

    {
        ScopedLock lock( searchProgressMutex_ );
        searchProgress_ = std::make_tuple( getNbMatches(), progress, initialLine );
    }

    Q_EMIT searchProgressedThrottled();

    if ( progress == 100 ) {
        detachReader();
        scanningFilterKeys_.clear();
        LOG_INFO << "Filter matches size " << readableSize( matching_lines_.getSizeInBytes( false ) )
                 << ", cached filters " << filterResults_.size();
    }
}

void LogFilteredData::composeFilterResults()
{
    if ( currentFilters_.empty() ) {
        matching_lines_ = {};
        marks_and_matches_ = marks_;
        maxLength_ = 0_length;
        return;
    }

    auto processedEnd = filterEndLine_;
    SearchResultArray includes;
    SearchResultArray excludes;
    bool hasInclude = false;
    LineLength maxLength = 0_length;

    for ( const auto& filter : currentFilters_ ) {
        const auto result = filterResults_.find( makeFilterKey( filter.pattern ) );
        if ( result == filterResults_.end() ) {
            processedEnd = filterStartLine_;
            continue;
        }

        const auto& filterResult = result->second;
        processedEnd = qMin( processedEnd, filterResult.processedEnd );

        if ( filter.exclude ) {
            excludes |= filterResult.matchingLines;
        }
        else {
            hasInclude = true;
            includes |= filterResult.matchingLines;
            maxLength = qMax( maxLength, filterResult.maxLength );
        }
    }

    if ( !hasInclude ) {
        // Only exclusions: everything that is not excluded is shown
        if ( processedEnd > filterStartLine_ ) {
            includes.addRange( filterStartLine_.get(), processedEnd.get() );
        }
        maxLength = sourceLogData_->getMaxLength();
    }

    includes -= excludes;

    // Lines not searched by every filter yet could still be excluded
    if ( processedEnd < filterEndLine_ ) {
        includes.removeRange( processedEnd.get(), filterEndLine_.get() );
    }

    matching_lines_ = std::move( includes );
    marks_and_matches_ = matching_lines_ | marks_;
    maxLength_ = maxLength;
    nbLinesProcessed_ = LinesCount( processedEnd.get() );

    lastFilterCompose_.start();
}

void LogFilteredData::trimFilterResults()
{
    std::vector<std::pair<uint64_t, QString>> unused;
    uint64_t unusedSize = 0;
    for ( const auto& [ key, result ] : filterResults_ ) {
        if ( result.lastUse != filterUseCounter_ ) {
            unused.emplace_back( result.lastUse, key );
            unusedSize += result.matchingLines.getSizeInBytes( false );
        }
    }

    // Least recently used first
    std::sort( unused.begin(), unused.end() );
    for ( const auto& [ lastUse, key ] : unused ) {
        if ( unusedSize <= UnusedFilterResultsBudget ) {
            break;
        }
        const auto result = filterResults_.find( key );
        unusedSize -= result->second.matchingLines.getSizeInBytes( false );
        filterResults_.erase( result );
    }
}

std::optional<LinesCount>
LogFilteredData::getFilterMatchCount( const RegularExpressionPattern& pattern ) const
{
    const auto result = filterResults_.find( makeFilterKey( pattern ) );
    if ( result == filterResults_.end() || result->second.processedEnd < filterEndLine_ ) {
        return {};
    }

    return LinesCount( result->second.matchingLines.cardinality() );
}

std::optional<bool> LogFilteredData::isFilterMatch( const RegularExpressionPattern& pattern,
                                                    LineNumber line ) const
{
    const auto result = filterResults_.find( makeFilterKey( pattern ) );
    if ( result == filterResults_.end() || result->second.processedEnd < filterEndLine_
         || line < filterStartLine_ || line >= filterEndLine_ ) {
        return {};
    }

    return result->second.matchingLines.contains( line.get() );
}

std::optional<OptionalLineNumber>
LogFilteredData::findFilterMatch( const std::vector<RegularExpressionPattern>& patterns,
                                  LineNumber line, bool backward ) const
{
    SearchResultArray anyMatch;
    for ( const auto& pattern : patterns ) {
        const auto result = filterResults_.find( makeFilterKey( pattern ) );
        if ( result == filterResults_.end() || result->second.processedEnd < filterEndLine_ ) {
            return {};
        }
        anyMatch |= result->second.matchingLines;
    }

    const auto nbMatches = anyMatch.cardinality();
    if ( nbMatches == 0 ) {
        return OptionalLineNumber{};
    }

    LineNumber::UnderlyingType found = 0;
    if ( backward ) {
        // Number of matches strictly before the line, wrapping to the last match
        const auto before = line.get() > 0 ? anyMatch.rank( line.get() - 1 ) : 0;
        anyMatch.select( before > 0 ? before - 1 : nbMatches - 1, &found );
    }
    else {
        // rank is the number of matches up to and including the line,
        // wrapping to the first match
        const auto upTo = anyMatch.rank( line.get() );
        anyMatch.select( upTo < nbMatches ? upTo : 0, &found );
    }

    return OptionalLineNumber( LineNumber( found ) );
}

//
// Numbered bookmarks
//

void LogFilteredData::toggleBookmark( size_t slot, LineNumber line )
{
    if ( slot >= NbBookmarks || line >= sourceLogData_->getNbLine() ) {
        return;
    }

    const auto slotBit = 1u << slot;
    auto bookmarked = bookmarks_.find( line );
    if ( bookmarked == bookmarks_.end() ) {
        BookmarkedLine newLine;
        newLine.slots = slotBit;
        newLine.addedMark = !isLineMarked( line );
        if ( newLine.addedMark ) {
            addMark( line );
        }
        bookmarks_.emplace( line, newLine );
        return;
    }

    bookmarked->second.slots ^= slotBit;
    if ( bookmarked->second.slots == 0 ) {
        // Remove the mark that the bookmarks added
        if ( bookmarked->second.addedMark ) {
            marks_.remove( line.get() );
            updateMaxLengthMarks( {}, line );
        }
        bookmarks_.erase( bookmarked );
    }
}

OptionalLineNumber LogFilteredData::findBookmark( size_t slot, LineNumber from,
                                                  bool backward ) const
{
    if ( slot >= NbBookmarks ) {
        return {};
    }

    const auto hasSlot
        = [ slotBit = 1u << slot ]( const auto& bookmarked ) { return bookmarked.second.slots & slotBit; };

    if ( backward ) {
        const auto before = std::make_reverse_iterator( bookmarks_.lower_bound( from ) );
        auto found = std::find_if( before, bookmarks_.rend(), hasSlot );
        if ( found == bookmarks_.rend() ) {
            found = std::find_if( bookmarks_.rbegin(), bookmarks_.rend(), hasSlot );
        }
        return found != bookmarks_.rend() ? OptionalLineNumber{ found->first }
                                          : OptionalLineNumber{};
    }

    auto found = std::find_if( bookmarks_.upper_bound( from ), bookmarks_.end(), hasSlot );
    if ( found == bookmarks_.end() ) {
        found = std::find_if( bookmarks_.begin(), bookmarks_.end(), hasSlot );
    }
    return found != bookmarks_.end() ? OptionalLineNumber{ found->first } : OptionalLineNumber{};
}

uint32_t LogFilteredData::getBookmarkSlots( LineNumber line ) const
{
    const auto bookmarked = bookmarks_.find( line );
    return bookmarked != bookmarks_.end() ? bookmarked->second.slots : 0;
}

void LogFilteredData::removeBookmarksOnLine( LineNumber line )
{
    bookmarks_.erase( line );
}
