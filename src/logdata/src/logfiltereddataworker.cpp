/*
 * Copyright (C) 2009, 2010 Nicolas Bonnefon and other contributors
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

#include <chrono>
#include <array>
#include <map>
#include <optional>
#include <cmath>
#include <exception>
#include <qsemaphore.h>
#include <QScopeGuard>
#include <utility>

#include <robin_hood.h>
#include <tbb/flow_graph.h>
#include <vector>

#include "configuration.h"
#include "dispatch_to.h"
#include "issuereporter.h"
#include "linetypes.h"
#include "log.h"
#include "progress.h"
#include "runnable_lambda.h"

#include "logdata.h"
#include "regularexpression.h"

#include "logfiltereddataworker.h"
#include "synchronization.h"

namespace {
struct PartialSearchResults {
    PartialSearchResults() = default;

    PartialSearchResults( const PartialSearchResults& ) = delete;
    PartialSearchResults( PartialSearchResults&& ) = default;
    PartialSearchResults& operator=( const PartialSearchResults& ) = delete;
    PartialSearchResults& operator=( PartialSearchResults&& ) = default;

    SearchResultArray matchingLines;
    LineLength maxLength;

    LineNumber chunkStart;
    LinesCount processedLines;
};

struct SearchBlockData {
    SearchBlockData() = default;
    SearchBlockData( LineNumber start, LogData::RawLines blockLines )
        : chunkStart( start )
        , lines( std::move( blockLines ) )
    {
    }

    SearchBlockData( const SearchBlockData& ) = delete;
    SearchBlockData( SearchBlockData&& ) = default;

    SearchBlockData& operator=( const SearchBlockData& ) = delete;
    SearchBlockData& operator=( SearchBlockData&& ) = default;

    LineNumber chunkStart;
    LogData::RawLines lines;

    PartialSearchResults searchResults;
};

PartialSearchResults filterLines( const PatternMatcher& matcher, const LogData::RawLines& rawLines,
                                  LineNumber chunkStart )
{
    LOG_DEBUG << "Filter lines at " << chunkStart;
    PartialSearchResults results;
    results.chunkStart = chunkStart;
    results.processedLines = LinesCount{ rawLines.endOfLines.size() };

    const auto& lines = rawLines.buildUtf8View();

    for ( auto offset = 0u; offset < lines.size(); ++offset ) {
        const auto& line = lines[ offset ];

        const auto hasMatch = matcher.hasMatch( line );

        if ( hasMatch ) {
            results.maxLength = qMax( results.maxLength, getUntabifiedLength( line ) );
            const auto lineNumber = chunkStart + LinesCount{ offset };
            results.matchingLines.add( lineNumber.get() );

            // LOG_INFO << "Match at " << lineNumber << ": " << line;
        }
    }
    return results;
}

struct PartialMultiSearchResults {
    klogg::vector<SearchResultArray> matchingLines;
    klogg::vector<LineLength> maxLengths;

    LineNumber chunkStart;
    LinesCount processedLines;
};

struct MultiSearchBlockData {
    MultiSearchBlockData( LineNumber start, LogData::RawLines blockLines )
        : chunkStart( start )
        , lines( std::move( blockLines ) )
    {
    }

    LineNumber chunkStart;
    LogData::RawLines lines;

    PartialMultiSearchResults searchResults;
};

// A matcher for a subset of the searched patterns
struct PatternGroupMatcher {
    const MultiPatternMatcher* matcher;
    const klogg::vector<size_t>* patternIndexes;
};

PartialMultiSearchResults filterLinesMulti( const klogg::vector<PatternGroupMatcher>& matchers,
                                            const LogData::RawLines& rawLines,
                                            LineNumber chunkStart, size_t nbPatterns )
{
    PartialMultiSearchResults results;
    results.chunkStart = chunkStart;
    results.processedLines = LinesCount{ rawLines.endOfLines.size() };
    results.matchingLines.resize( nbPatterns );
    results.maxLengths.resize( nbPatterns );

    const auto& lines = rawLines.buildUtf8View();

    for ( auto offset = 0u; offset < lines.size(); ++offset ) {
        const auto& line = lines[ offset ];
        const auto lineNumber = ( chunkStart + LinesCount{ offset } ).get();
        std::optional<LineLength> lineLength;

        for ( const auto& group : matchers ) {
            const auto matchedPatterns = group.matcher->matchAll( line );
            const auto& indexes = *group.patternIndexes;
            for ( size_t i = 0; i < matchedPatterns.size() && i < indexes.size(); ++i ) {
                if ( !matchedPatterns[ i ] ) {
                    continue;
                }

                if ( !lineLength ) {
                    lineLength = getUntabifiedLength( line );
                }
                const auto pattern = indexes[ i ];
                results.maxLengths[ pattern ] = qMax( results.maxLengths[ pattern ], *lineLength );
                results.matchingLines[ pattern ].add( lineNumber );
            }
        }
    }

    return results;
}

// Number of lines read at once: at least the configured number of lines, and enough lines
// for blocks of a few megabytes, every block has a fixed cost (seek, scheduling)
LinesCount searchBlockLines( const LogData& logData )
{
    constexpr qint64 TargetBlockSize = 4 * 1024 * 1024;
    constexpr qint64 MaxBlockLines = 1000000;

    const auto configuredLines
        = static_cast<qint64>( Configuration::get().searchReadBufferSizeLines() );
    const auto nbLines = static_cast<qint64>( logData.getNbLine().get() );
    const auto averageLineSize = nbLines > 0 ? qMax( qint64{ 1 }, logData.getFileSize() / nbLines )
                                             : qint64{ 1 };

    const auto blockLines
        = qBound( configuredLines, TargetBlockSize / averageLineSize, MaxBlockLines );
    return LinesCount( static_cast<LinesCount::UnderlyingType>( qMax( configuredLines,
                                                                     blockLines ) ) );
}

// Blocks being read or matched at the same time
int blocksInFlight( uint32_t matchingThreadsCount )
{
    return static_cast<int>( matchingThreadsCount * 2 );
}

} // namespace

void MultiSearchData::reset( uint64_t generation, size_t nbPatterns )
{
    ScopedLock lock( dataMutex_ );
    generation_ = generation;
    newMatches_ = klogg::vector<SearchResultArray>( nbPatterns );
    maxLengths_ = klogg::vector<LineLength>( nbPatterns );
    processedLines_ = 0_lcount;
}

MultiSearchResults MultiSearchData::takeCurrentResults() const
{
    ScopedLock lock( dataMutex_ );

    MultiSearchResults results;
    results.generation = generation_;
    results.newMatches = klogg::vector<SearchResultArray>( newMatches_.size() );
    std::swap( results.newMatches, newMatches_ );
    results.maxLengths = maxLengths_;
    results.processedLines = processedLines_;
    return results;
}

void MultiSearchData::addAll( const klogg::vector<SearchResultArray>& matches,
                              const klogg::vector<LineLength>& maxLengths,
                              LinesCount processedLines )
{
    ScopedLock lock( dataMutex_ );

    for ( size_t i = 0; i < matches.size() && i < newMatches_.size(); ++i ) {
        newMatches_[ i ] |= matches[ i ];
        maxLengths_[ i ] = qMax( maxLengths_[ i ], maxLengths[ i ] );
    }
    processedLines_ = qMax( processedLines_, processedLines );
}

SearchResults SearchData::takeCurrentResults() const
{
    UniqueLock lock( dataMutex_ );
    return SearchResults{ std::exchange( newMatches_, {} ), maxLength_, nbLinesProcessed_ };
}

void SearchData::addAll( LineLength length, const SearchResultArray& matches,
                         LinesCount matchedLines, LinesCount processedLines )
{
    UniqueLock lock( dataMutex_ );

    maxLength_ = qMax( maxLength_, length );
    nbLinesProcessed_ = qMax( nbLinesProcessed_, processedLines );
    nbMatches_ += matchedLines;

    newMatches_ |= matches;
}

LinesCount SearchData::getNbMatches() const
{
    SharedLock lock( dataMutex_ );
    return nbMatches_;
}

LineNumber SearchData::getLastProcessedLine() const
{
    SharedLock lock( dataMutex_ );
    return LineNumber{ nbLinesProcessed_.get() };
}

void SearchData::deleteMatch( LineNumber line )
{
    UniqueLock lock( dataMutex_ );
    matches_.remove( line.get() );
}

void SearchData::clear()
{
    UniqueLock locker( dataMutex_ );

    maxLength_ = LineLength( 0 );
    nbLinesProcessed_ = LinesCount( 0 );
    nbMatches_ = LinesCount( 0 );
    matches_ = {};
    newMatches_ = {};
}

LogFilteredDataWorker::LogFilteredDataWorker( const LogData& sourceLogData )
    : sourceLogData_( sourceLogData )
{
    operationsPool_.setMaxThreadCount( 1 );
}

LogFilteredDataWorker::~LogFilteredDataWorker() noexcept
{
    try {
        interruptRequested_.set();
        ScopedLock locker( operationsMutex_ );
        operationsPool_.waitForDone();
        LOG_INFO << "LogFilteredDataWorker shutdown";
    } catch ( const std::exception& e ) {
        LOG_ERROR << "Failed to destroy LogFilteredDataWorker: " << e.what();
    }
}

void LogFilteredDataWorker::connectSignalsAndRun( SearchOperation* operationRequested )
{
    connect( operationRequested, &SearchOperation::searchProgressed, this,
             &LogFilteredDataWorker::searchProgressed );
    connect( operationRequested, &SearchOperation::searchFinished, this,
             &LogFilteredDataWorker::searchFinished, Qt::QueuedConnection );

    operationRequested->run( searchData_ );
    operationRequested->disconnect( this );
}

void LogFilteredDataWorker::search( const RegularExpressionPattern& regExp, LineNumber startLine,
                                    LineNumber endLine )
{
    ScopedLock locker( operationsMutex_ ); // to protect operationRequested_
    operationsPool_.waitForDone();
    interruptRequested_.clear();

    LOG_INFO << "Search requested";
    QSemaphore operationStarted;
    operationsPool_.start( createRunnable( [ this, &operationStarted, regExp, startLine, endLine ] {
        operationStarted.release();
        ScopedLock operationLock( operationsMutex_ );
        auto operationRequested = std::make_unique<FullSearchOperation>(
            sourceLogData_, interruptRequested_, regExp, startLine, endLine );
        connectSignalsAndRun( operationRequested.get() );
    } ) );
    operationStarted.acquire();
}

void LogFilteredDataWorker::updateSearch( const RegularExpressionPattern& regExp,
                                          LineNumber startLine, LineNumber endLine,
                                          LineNumber position )
{
    ScopedLock locker( operationsMutex_ ); // to protect operationRequested_
    operationsPool_.waitForDone();
    interruptRequested_.clear();

    LOG_INFO << "Search update requested from " << position.get();

    QSemaphore operationStarted;
    operationsPool_.start(
        createRunnable( [ this, &operationStarted, regExp, startLine, endLine, position ] {
            operationStarted.release();
            ScopedLock operationLock( operationsMutex_ );
            auto operationRequested = std::make_unique<UpdateSearchOperation>(
                sourceLogData_, interruptRequested_, regExp, startLine, endLine, position );
            connectSignalsAndRun( operationRequested.get() );
        } ) );

    operationStarted.acquire();
}

uint64_t LogFilteredDataWorker::searchMultiple( const klogg::vector<RegularExpressionPattern>& patterns,
                                                LineNumber startLine, LineNumber endLine )
{
    ScopedLock locker( operationsMutex_ );
    operationsPool_.waitForDone();
    interruptRequested_.clear();

    const auto generation = ++multiSearchGeneration_;
    multiSearchData_.reset( generation, patterns.size() );

    LOG_INFO << "Multi pattern search requested, patterns " << patterns.size();
    QSemaphore operationStarted;
    operationsPool_.start(
        createRunnable( [ this, &operationStarted, patterns, startLine, endLine, generation ] {
            operationStarted.release();
            ScopedLock operationLock( operationsMutex_ );
            MultiSearchOperation operation( sourceLogData_, interruptRequested_, patterns,
                                            startLine, endLine, generation );

            connect( &operation, &MultiSearchOperation::searchProgressed, this,
                     &LogFilteredDataWorker::searchProgressed );
            connect( &operation, &MultiSearchOperation::searchFinished, this,
                     &LogFilteredDataWorker::searchFinished, Qt::QueuedConnection );

            operation.run( multiSearchData_ );
            operation.disconnect( this );
        } ) );
    operationStarted.acquire();

    return generation;
}

MultiSearchResults LogFilteredDataWorker::getMultiSearchResults() const
{
    return multiSearchData_.takeCurrentResults();
}

void LogFilteredDataWorker::interrupt()
{
    LOG_INFO << "Search interruption requested";
    interruptRequested_.set();
}

// This will do an atomic copy of the object
SearchResults LogFilteredDataWorker::getSearchResults() const
{
    return searchData_.takeCurrentResults();
}

//
// Operations implementation
//

SearchOperation::SearchOperation( const LogData& sourceLogData, AtomicFlag& interruptRequested,
                                  const RegularExpressionPattern& regExp, LineNumber startLine,
                                  LineNumber endLine )

    : interruptRequested_( interruptRequested )
    , regexp_( regExp )
    , sourceLogData_( sourceLogData )
    , startLine_( startLine )
    , endLine_( endLine )

{
}

void SearchOperation::doSearch( SearchData& searchData, LineNumber initialLine )
{
    const auto nbSourceLines = sourceLogData_.getNbLine();

    LOG_INFO << "Searching from line " << initialLine << " to " << nbSourceLines;

    using namespace std::chrono;
    high_resolution_clock::time_point t1 = high_resolution_clock::now();

    const auto& config = Configuration::get();
    const auto matchingThreadsCount = static_cast<uint32_t>( [ &config ]() {
        if ( !config.useParallelSearch() ) {
            return 1;
        }
        const auto configuredThreadPoolSize = config.searchThreadPoolSize();
        return qMax( 1, configuredThreadPoolSize == 0 ? tbb::info::default_concurrency()
                                                      : configuredThreadPoolSize );
    }() );

    LOG_INFO << "Using " << matchingThreadsCount << " matching threads";

    tbb::flow::graph searchGraph;

    if ( initialLine < startLine_ ) {
        initialLine = startLine_;
    }

    const auto endLine = qMin( LineNumber( nbSourceLines.get() ), endLine_ );
    const auto nbLinesInChunk = searchBlockLines( sourceLogData_ );

    std::chrono::microseconds fileReadingDuration{ 0 };

    using BlockDataType = SearchBlockData*;

    // The reader waits on this semaphore instead of polling a limiter node with short sleeps,
    // these can last much longer than asked (timer coalescing) and slowed down every block
    QSemaphore blocksSlots( blocksInFlight( matchingThreadsCount ) );

    auto lineBlocksQueue = tbb::flow::buffer_node<BlockDataType>( searchGraph );

    using RegexMatcherNode
        = tbb::flow::function_node<BlockDataType, BlockDataType, tbb::flow::rejecting>;

    using PatternMatcherPtr = std::unique_ptr<PatternMatcher>;
    using MatcherContext = std::tuple<PatternMatcherPtr, microseconds, RegexMatcherNode>;

    klogg::vector<MatcherContext> regexMatchers;
    regexMatchers.reserve( matchingThreadsCount );
    RegularExpression regularExpression{ regexp_ };
    for ( auto index = 0u; index < matchingThreadsCount; ++index ) {
        regexMatchers.emplace_back(
            regularExpression.createMatcher(), microseconds{ 0 },
            RegexMatcherNode(
                searchGraph, 1, [ &regexMatchers, index, this ]( const BlockDataType& blockData ) {
                    if ( interruptRequested_ ) {
                        LOG_INFO << "Matcher " << index << " interrupted";
                        auto results = std::make_shared<PartialSearchResults>();
                        blockData->searchResults.chunkStart = blockData->chunkStart;
                        blockData->searchResults.processedLines
                            = LinesCount{ blockData->lines.endOfLines.size() };
                        return blockData;
                    }

                    const auto& matcher = std::get<PatternMatcherPtr>( regexMatchers.at( index ) );
                    const auto matchStartTime = high_resolution_clock::now();

                    blockData->searchResults
                        = filterLines( *matcher, blockData->lines, blockData->chunkStart );

                    const auto matchEndTime = high_resolution_clock::now();

                    microseconds& matchDuration
                        = std::get<microseconds>( regexMatchers.at( index ) );
                    matchDuration += duration_cast<microseconds>( matchEndTime - matchStartTime );
                    LOG_DEBUG << "Searcher " << index << " block " << blockData->chunkStart
                              << " sending matches "
                              << blockData->searchResults.matchingLines.cardinality();
                    return blockData;
                } ) );
    }

    auto resultsQueue = tbb::flow::buffer_node<BlockDataType>( searchGraph );

    const auto totalLines = endLine - initialLine;
    LinesCount totalProcessedLines = 0_lcount;
    LineLength maxLength = 0_length;
    LinesCount nbMatches = searchData.getNbMatches();
    auto reportedMatches = nbMatches;
    int reportedPercentage = 0;

    std::chrono::microseconds matchCombiningDuration{ 0 };

    auto matchProcessor
        = tbb::flow::function_node<BlockDataType, tbb::flow::continue_msg, tbb::flow::rejecting>(
            searchGraph, 1, [ & ]( const BlockDataType& blockData ) {
                const auto releaseSlot = qScopeGuard( [ &blocksSlots ] { blocksSlots.release(); } );
                if ( interruptRequested_ ) {
                    LOG_INFO << "Match processor interrupted";
                    delete blockData;
                    return tbb::flow::continue_msg{};
                }

                const auto& matchResults = blockData->searchResults;

                const auto matchProcessorStartTime = high_resolution_clock::now();

                if ( matchResults.processedLines.get() ) {

                    maxLength = qMax( maxLength, matchResults.maxLength );
                    const LinesCount matchesCount
                        = LinesCount( matchResults.matchingLines.cardinality() );
                    nbMatches += matchesCount;

                    const auto processedLines = LinesCount{ matchResults.chunkStart.get()
                                                            + matchResults.processedLines.get() };

                    totalProcessedLines += matchResults.processedLines;

                    // After each block, copy the data to shared data
                    // and update the client
                    searchData.addAll( maxLength, matchResults.matchingLines, matchesCount,
                                       processedLines );

                    LOG_DEBUG << "done Searching chunk starting at " << matchResults.chunkStart
                              << ", " << matchResults.processedLines << " lines read.";
                }

                delete blockData;

                const auto matchProcessorEndTime = high_resolution_clock::now();
                matchCombiningDuration += duration_cast<microseconds>( matchProcessorEndTime
                                                                       - matchProcessorStartTime );
                const int percentage
                    = calculateProgress( totalProcessedLines.get(), totalLines.get() );

                if ( percentage > reportedPercentage || nbMatches > reportedMatches ) {

                    Q_EMIT searchProgressed( nbMatches, std::min( 99, percentage ), initialLine );

                    reportedPercentage = percentage;
                    reportedMatches = nbMatches;
                }

                return tbb::flow::continue_msg{};
            } );

    for ( auto& regexMatcher : regexMatchers ) {
        tbb::flow::make_edge( lineBlocksQueue, std::get<RegexMatcherNode>( regexMatcher ) );
        tbb::flow::make_edge( std::get<RegexMatcherNode>( regexMatcher ), resultsQueue );
    }

    tbb::flow::make_edge( resultsQueue, matchProcessor );

    auto chunkStart = initialLine;
    while ( chunkStart < endLine && !interruptRequested_ ) {
        const auto lineSourceStartTime = high_resolution_clock::now();
        LOG_DEBUG << "Reading chunk starting at " << chunkStart;

        const auto linesInChunk
            = LinesCount( qMin( nbLinesInChunk.get(), ( endLine - chunkStart ).get() ) );
        auto lines = sourceLogData_.getLinesRaw( chunkStart, linesInChunk );

        /*LOG_DEBUG << "Sending chunk starting at " << chunkStart << ", " <<
            lines.second.size()
                << " lines read.";*/
        BlockDataType blockData = new SearchBlockData{ chunkStart, std::move( lines ) };

        const auto lineSourceEndTime = high_resolution_clock::now();
        const auto chunkReadTime
            = duration_cast<microseconds>( lineSourceEndTime - lineSourceStartTime );

        /*LOG_DEBUG << "Sent chunk starting at " << chunkStart << ", " <<
        blockData->lines.second.size()
                << " lines read in " << static_cast<float>( chunkReadTime.count() )
        / 1000.f
                << " ms";*/

        chunkStart = chunkStart + nbLinesInChunk;
        fileReadingDuration += chunkReadTime;

        bool hasSlot = false;
        while ( !( hasSlot = blocksSlots.tryAcquire( 1, 50 ) ) && !interruptRequested_ ) {
        }

        if ( !hasSlot ) {
            delete blockData;
            break;
        }

        lineBlocksQueue.try_put( blockData );
    }

    searchGraph.wait_for_all();

    high_resolution_clock::time_point t2 = high_resolution_clock::now();
    const auto durationUs = duration_cast<microseconds>( t2 - t1 );
    const auto durationMs = duration_cast<milliseconds>( t2 - t1 );

    LOG_INFO << "Searching done, overall duration " << durationUs;
    LOG_INFO << "Line reading took " << fileReadingDuration;
    LOG_INFO << "Results combining took " << matchCombiningDuration;

    for ( const auto& regexMatcher : regexMatchers ) {
        LOG_INFO << "Matching took " << std::get<microseconds>( regexMatcher );
    }

    const auto totalFileSize = sourceLogData_.getFileSize();

    LOG_INFO << "Searching perf "
             << static_cast<uint64_t>(
                    std::floor( 1000.f * static_cast<float>( ( endLine - initialLine ).get() )
                                / static_cast<float>( durationMs.count() ) ) )
             << " lines/s";
    LOG_INFO << "Searching io perf "
             << ( 1000.f * static_cast<float>( totalFileSize )
                  / static_cast<float>( durationMs.count() ) )
                    / ( 1024 * 1024 )
             << " MiB/s";

    Q_EMIT searchProgressed( nbMatches, 100, initialLine );
    Q_EMIT searchFinished();
}

// Called in the worker thread's context
void FullSearchOperation::run( SearchData& searchData )
{
    try {
        // Clear the shared data
        searchData.clear();
        doSearch( searchData, 0_lnum );
    } catch ( const std::exception& err ) {
        const auto errorString = QString( "FullSearchOperation failed: %1" ).arg( err.what() );
        LOG_ERROR << errorString;
        dispatchToMainThread( [ errorString ]() {
            IssueReporter::askUserAndReportIssue( IssueTemplate::Exception, errorString );
        } );
        searchData.clear();
    }
}

// Called in the worker thread's context
void UpdateSearchOperation::run( SearchData& searchData )
{
    try {
        auto initialLine = qMax( searchData.getLastProcessedLine(), initialPosition_ );

        if ( initialLine.get() >= 1 ) {
            // We need to re-search the last line because it might have
            // been updated (if it was not LF-terminated)
            --initialLine;
            // In case the last line matched, we don't want it to match twice.
            searchData.deleteMatch( initialLine );
        }

        doSearch( searchData, initialLine );
    } catch ( const std::exception& err ) {
        const auto errorString = QString( "UpdateSearchOpertaion failed: %1" ).arg( err.what() );
        LOG_ERROR << errorString;
        dispatchToMainThread( [ errorString ]() {
            IssueReporter::askUserAndReportIssue( IssueTemplate::Exception, errorString );
        } );
        searchData.clear();
    }
}

MultiSearchOperation::MultiSearchOperation( const LogData& sourceLogData,
                                            AtomicFlag& interruptRequested,
                                            const klogg::vector<RegularExpressionPattern>& patterns,
                                            LineNumber startLine, LineNumber endLine,
                                            uint64_t generation )
    : interruptRequested_( interruptRequested )
    , patterns_( patterns )
    , sourceLogData_( sourceLogData )
    , startLine_( startLine )
    , endLine_( endLine )
    , generation_( generation )
{
}

void MultiSearchOperation::run( MultiSearchData& result )
{
    try {
        doSearch( result );
    } catch ( const std::exception& err ) {
        const auto errorString = QString( "MultiSearchOperation failed: %1" ).arg( err.what() );
        LOG_ERROR << errorString;
        result.reset( generation_, patterns_.size() );
        Q_EMIT searchProgressed( 0_lcount, 100, startLine_ );
        Q_EMIT searchFinished();
    }
}

// Same pipeline as SearchOperation::doSearch, each matcher looks for all the patterns
void MultiSearchOperation::doSearch( MultiSearchData& searchData )
{
    using namespace std::chrono;

    const auto nbSourceLines = sourceLogData_.getNbLine();
    const auto endLine = qMin( LineNumber( nbSourceLines.get() ), endLine_ );
    const auto initialLine = startLine_;

    LOG_INFO << "Searching " << patterns_.size() << " patterns from line " << initialLine
             << " to " << endLine;

    const auto searchStartTime = high_resolution_clock::now();

    const auto& config = Configuration::get();
    const auto matchingThreadsCount = static_cast<uint32_t>( [ &config ]() {
        if ( !config.useParallelSearch() ) {
            return 1;
        }
        const auto configuredThreadPoolSize = config.searchThreadPoolSize();
        return qMax( 1, configuredThreadPoolSize == 0 ? tbb::info::default_concurrency()
                                                      : configuredThreadPoolSize );
    }() );

    const auto nbLinesInChunk = searchBlockLines( sourceLogData_ );

    using BlockDataType = MultiSearchBlockData*;
    using RegexMatcherNode
        = tbb::flow::function_node<BlockDataType, BlockDataType, tbb::flow::rejecting>;

    tbb::flow::graph searchGraph;

    // Bounds the number of blocks being read or matched. The reader blocks on it instead of
    // polling a limiter node: short sleeps can last much longer than asked (timer coalescing)
    // and the polling made the search wait on every block.
    QSemaphore blocksSlots( blocksInFlight( matchingThreadsCount ) );

    auto lineBlocksQueue = tbb::flow::buffer_node<BlockDataType>( searchGraph );
    auto resultsQueue = tbb::flow::buffer_node<BlockDataType>( searchGraph );

    const auto nbPatterns = patterns_.size();

    // Patterns hyperscan can't handle alone (back references, look-arounds...) need every
    // candidate line to be checked again with Qt regex. They are kept in their own database
    // so that the other patterns are not slowed down.
    std::array<klogg::vector<size_t>, 2> groupIndexes;
    auto& acceleratedIndexes = groupIndexes[ 0 ];
    auto& slowIndexes = groupIndexes[ 1 ];
    for ( size_t i = 0; i < nbPatterns; ++i ) {
        const MultiRegularExpression single{ { patterns_[ i ] } };
        ( single.isFullyAccelerated() ? acceleratedIndexes : slowIndexes ).push_back( i );
    }
    if ( !slowIndexes.empty() ) {
        LOG_WARNING << slowIndexes.size() << " patterns are not fully supported by hyperscan";
    }

    klogg::vector<std::unique_ptr<MultiRegularExpression>> expressions;
    klogg::vector<const klogg::vector<size_t>*> expressionIndexes;
    for ( const auto& indexes : groupIndexes ) {
        if ( indexes.empty() ) {
            continue;
        }
        klogg::vector<RegularExpressionPattern> groupPatterns;
        for ( const auto i : indexes ) {
            groupPatterns.push_back( patterns_[ i ] );
        }
        expressions.push_back( std::make_unique<MultiRegularExpression>( groupPatterns ) );
        expressionIndexes.push_back( &indexes );
    }

    klogg::vector<std::unique_ptr<MultiPatternMatcher>> matchers;
    klogg::vector<klogg::vector<PatternGroupMatcher>> threadMatchers( matchingThreadsCount );
    klogg::vector<RegexMatcherNode> matcherNodes;
    matcherNodes.reserve( matchingThreadsCount );
    for ( auto index = 0u; index < matchingThreadsCount; ++index ) {
        for ( size_t group = 0; group < expressions.size(); ++group ) {
            matchers.push_back( expressions[ group ]->createMatcher() );
            threadMatchers[ index ].push_back(
                { matchers.back().get(), expressionIndexes[ group ] } );
        }

        matcherNodes.emplace_back(
            searchGraph, 1,
            [ this, groups = &threadMatchers[ index ], nbPatterns ](
                const BlockDataType& blockData ) {
                if ( interruptRequested_ ) {
                    blockData->searchResults.chunkStart = blockData->chunkStart;
                    return blockData;
                }

                blockData->searchResults = filterLinesMulti( *groups, blockData->lines,
                                                             blockData->chunkStart, nbPatterns );
                return blockData;
            } );
    }

    const auto totalLines = endLine - initialLine;
    LinesCount totalProcessedLines = 0_lcount;
    LinesCount nbMatches = 0_lcount;
    auto reportedMatches = nbMatches;

    // Blocks are matched in parallel and can complete out of order, only the lines
    // before the first gap are reported as processed (results are cached per pattern).
    std::map<LineNumber::UnderlyingType, LineNumber::UnderlyingType> completedChunks;
    auto contiguousEnd = initialLine.get();
    int reportedPercentage = 0;

    auto matchProcessor
        = tbb::flow::function_node<BlockDataType, tbb::flow::continue_msg, tbb::flow::rejecting>(
            searchGraph, 1, [ & ]( const BlockDataType& blockData ) {
                std::unique_ptr<MultiSearchBlockData> block( blockData );
                const auto releaseSlot = qScopeGuard( [ &blocksSlots ] { blocksSlots.release(); } );
                if ( interruptRequested_ ) {
                    return tbb::flow::continue_msg{};
                }

                const auto& matchResults = block->searchResults;
                if ( matchResults.processedLines.get() ) {
                    for ( const auto& matches : matchResults.matchingLines ) {
                        nbMatches += LinesCount( matches.cardinality() );
                    }

                    totalProcessedLines += matchResults.processedLines;

                    completedChunks[ matchResults.chunkStart.get() ]
                        = matchResults.chunkStart.get() + matchResults.processedLines.get();
                    while ( !completedChunks.empty()
                            && completedChunks.begin()->first == contiguousEnd ) {
                        contiguousEnd = completedChunks.begin()->second;
                        completedChunks.erase( completedChunks.begin() );
                    }

                    searchData.addAll( matchResults.matchingLines, matchResults.maxLengths,
                                       LinesCount{ contiguousEnd } );
                }

                const int percentage
                    = calculateProgress( totalProcessedLines.get(), totalLines.get() );

                if ( percentage > reportedPercentage || nbMatches > reportedMatches ) {
                    Q_EMIT searchProgressed( nbMatches, std::min( 99, percentage ), initialLine );
                    reportedPercentage = percentage;
                    reportedMatches = nbMatches;
                }

                return tbb::flow::continue_msg{};
            } );

    for ( auto& matcherNode : matcherNodes ) {
        tbb::flow::make_edge( lineBlocksQueue, matcherNode );
        tbb::flow::make_edge( matcherNode, resultsQueue );
    }
    tbb::flow::make_edge( resultsQueue, matchProcessor );

    auto chunkStart = initialLine;
    while ( chunkStart < endLine && !interruptRequested_ ) {
        const auto linesInChunk
            = LinesCount( qMin( nbLinesInChunk.get(), ( endLine - chunkStart ).get() ) );
        auto* blockData = new MultiSearchBlockData{
            chunkStart, sourceLogData_.getLinesRaw( chunkStart, linesInChunk ) };

        chunkStart = chunkStart + nbLinesInChunk;

        bool hasSlot = false;
        while ( !( hasSlot = blocksSlots.tryAcquire( 1, 50 ) ) && !interruptRequested_ ) {
        }

        if ( !hasSlot ) {
            delete blockData;
            break;
        }

        lineBlocksQueue.try_put( blockData );
    }

    searchGraph.wait_for_all();

    const auto duration
        = duration_cast<milliseconds>( high_resolution_clock::now() - searchStartTime );
    LOG_INFO << "Multi pattern search done, overall duration " << duration.count() << " ms";

    Q_EMIT searchProgressed( nbMatches, 100, initialLine );
    Q_EMIT searchFinished();
}
