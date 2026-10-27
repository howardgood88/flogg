/*
 * Copyright (C) 2011, 2012 Nicolas Bonnefon and other contributors
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

// This file implements the Overview class.
// It provides support for drawing the match overview sidebar but
// the actual drawing is done in AbstractLogView which uses this class.

#include "linetypes.h"
#include "log.h"

#include "logfiltereddata.h"

#include "overview.h"

Overview::Overview()
    : matchLines_()
    , markLines_()
{
    logFilteredData_ = nullptr;
    height_ = 0;
    dirty_ = true;
    visible_ = false;
}

void Overview::setFilteredData( const LogFilteredData* logFilteredData )
{
    LOG_INFO << "OverviewWidget::setFilteredData " << (void*)logFilteredData;

    logFilteredData_ = logFilteredData;
    dirty_ = true;
}

void Overview::updateData( LinesCount totalNbLine )
{
    LOG_INFO << "OverviewWidget::updateData " << totalNbLine;

    linesInFile_ = totalNbLine;
    dirty_ = true;
}

void Overview::updateView( unsigned height )
{
    // We don't touch the cache if the height hasn't changed
    if ( ( height != height_ ) || ( dirty_ == true ) ) {
        height_ = height;

        recalculatesLines();
    }
}

const klogg::vector<Overview::WeightedLine>* Overview::getMatchLines() const
{
    return &matchLines_;
}

const klogg::vector<Overview::WeightedLine>* Overview::getMarkLines() const
{
    return &markLines_;
}

std::pair<int, int> Overview::getViewLines() const
{
    int top = 0;
    int bottom = static_cast<int>( height_ ) - 1;

    if ( linesInFile_.get() > 0 ) {
        top = static_cast<int>( ( topLine_.get() ) * height_ / ( linesInFile_.get() ) );

        bottom = top + static_cast<int>( nbLines_.get() * height_ / ( linesInFile_.get() ) );
    }

    return std::make_pair( top, bottom );
}

LineNumber Overview::fileLineFromY( int position ) const
{
    const auto line = static_cast<LineNumber::UnderlyingType>(
        static_cast<LineNumber::UnderlyingType>( position ) * linesInFile_.get() / static_cast<LineNumber::UnderlyingType>( height_ ) );

    return LineNumber{ line };
}

int Overview::yFromFileLine( LineNumber fileLine ) const
{
    int position = 0;

    if ( linesInFile_.get() > 0 )
        position = static_cast<int>( fileLine.get() * height_ / linesInFile_.get() );

    return position;
}

// Update the internal cache
void Overview::recalculatesLines()
{
    LOG_INFO << "OverviewWidget::recalculatesLines";

    if ( logFilteredData_ != nullptr ) {
        matchLines_.clear();
        markLines_.clear();

        const auto nbLines = linesInFile_.get();
        if ( nbLines > 0 && height_ > 0 ) {
            // The lines drawn at y are the ones with line * height / nbLines == y, counted
            // per row instead of going over every match
            std::vector<uint64_t> bounds( height_ + 1 );
            for ( uint64_t y = 0; y <= height_; ++y ) {
                bounds[ y ] = ( y * nbLines + height_ - 1 ) / height_;
            }

            std::vector<uint64_t> matchCounts;
            std::vector<uint64_t> markCounts;
            logFilteredData_->countLinesInRanges( bounds, matchCounts, markCounts );

            // The first line of a row has the lowest weight, each other one adds a step
            const auto addRow = []( klogg::vector<WeightedLine>& lines, int y, uint64_t count ) {
                if ( count == 0 ) {
                    return;
                }
                lines.emplace_back( y );
                for ( uint64_t i = 1; i < count && i < WeightedLine::WEIGHT_STEPS; ++i ) {
                    lines.back().load();
                }
            };
            for ( unsigned y = 0; y < height_; ++y ) {
                addRow( matchLines_, static_cast<int>( y ), matchCounts[ y ] );
                addRow( markLines_, static_cast<int>( y ), markCounts[ y ] );
            }
        }
    }
    else
        LOG_INFO << "Overview::recalculatesLines: logFilteredData_ == NULL";

    dirty_ = false;
}
