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

#ifndef KLOGG_TOOLTIPPLACEMENT_H
#define KLOGG_TOOLTIPPLACEMENT_H

#include <QPoint>
#include <QRect>

// Qt puts a tool tip beside a large cursor (macOS with a Retina screen), and with no room
// on the right it moves the tip left by its width only: the tip ends under the cursor,
// which hides a part of its text. Every tool tip of the application across the cursor is
// moved to the left of it.
namespace tooltips {

// Once for the application
void installPlacement();

// Top left of the tip: the same, or left of the cursor when the tip is across it and
// fits there
QPoint placed( const QRect& tip, const QPoint& cursor, const QRect& screen );

} // namespace tooltips

#endif
