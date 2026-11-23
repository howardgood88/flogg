/*
 * Copyright (C) 2009, 2010, 2012, 2017 Nicolas Bonnefon and other contributors
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

// This file implements the FilteredView concrete class.
// Most of the actual drawing and event management is done in AbstractLogView
// Only behaviour specific to the filtered (bottom) view is implemented here.

#include <cassert>

#include "filteredview.h"
#include "shortcuts.h"

FilteredView::FilteredView( LogFilteredData* newLogData,
                            const QuickFindPattern* const quickFindPattern, QWidget* parent )
    : AbstractLogView( newLogData, quickFindPattern, parent )
{
    // We keep a copy of the filtered data for fast lookup of the line type
    logFilteredData_ = newLogData;
}

void FilteredView::setVisibility( Visibility visi )
{
    assert( logFilteredData_ );

    logFilteredData_->setVisibility( visi );

    updateData();
}

FilteredView::Visibility FilteredView::visibility() const
{
    assert( logFilteredData_ );

    return logFilteredData_->visibility();
}

// For the filtered view, a line is always matching!
uint32_t FilteredView::bookmarkSlots( LineNumber lineNumber ) const
{
    return logFilteredData_->getBookmarkSlots(
        logFilteredData_->getMatchingLineNumber( lineNumber ) );
}

bool FilteredView::hasBookmarks() const
{
    return logFilteredData_->hasBookmarks();
}

AbstractLogData::LineType FilteredView::lineType( LineNumber lineNumber ) const
{
    // line in filteredview corresponds to index
    return logFilteredData_->lineTypeByIndex( lineNumber );
}

LineNumber FilteredView::displayLineNumber( LineNumber lineNumber ) const
{
    // Display a 1-based index
    return logFilteredData_->getMatchingLineNumber( lineNumber ) + 1_lcount;
}

LineNumber FilteredView::lineIndex( LineNumber lineNumber ) const
{
    return logFilteredData_->getLineIndexNumber( lineNumber );
}

LineNumber FilteredView::maxDisplayLineNumber() const
{
    return LineNumber( logFilteredData_->getNbTotalLines().get() );
}

void FilteredView::doRegisterShortcuts()
{
    LOG_INFO << "Registering shortcuts for filtered view";
    AbstractLogView::doRegisterShortcuts();
    // As in the main view: next goes down the lines, previous up, to a mark or a note.
    // Found from the marks and the notes, so the jump does not depend on the lines between.
    const auto jump = [ this ]( bool backward ) {
        if ( logFilteredData_->getNbLine() == 0_lcount ) {
            return;
        }
        const auto last = LineNumber( logFilteredData_->getNbLine().get() - 1 );
        const auto from = logFilteredData_->getMatchingLineNumber( qMin( getViewPosition(), last ) );
        const auto line = logFilteredData_->findMarkOrNote( from, backward, true );
        if ( line.has_value() ) {
            selectAndDisplayLine( logFilteredData_->getLineIndexNumber( *line ) );
        }
    };
    registerShortcut( ShortcutAction::LogViewNextMark, [ jump ] { jump( false ); } );
    registerShortcut( ShortcutAction::LogViewPrevMark, [ jump ] { jump( true ); } );
}
