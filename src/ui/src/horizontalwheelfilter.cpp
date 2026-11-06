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

#include "horizontalwheelfilter.h"

#include <QAbstractScrollArea>
#include <QCoreApplication>
#include <QScrollBar>
#include <QWheelEvent>

namespace {

QAbstractScrollArea* horizontalArea( QWidget* widget )
{
    for ( ; widget; widget = widget->parentWidget() ) {
        auto* area = qobject_cast<QAbstractScrollArea*>( widget );
        if ( area && area->horizontalScrollBarPolicy() != Qt::ScrollBarAlwaysOff ) {
            const auto* bar = area->horizontalScrollBar();
            if ( bar->minimum() < bar->maximum() ) {
                return area;
            }
        }
        if ( widget->isWindow() ) {
            break;
        }
    }
    return nullptr;
}

// macOS already turns the wheel of a mouse sideways while Shift is held
QPoint sideways( const QPoint& delta )
{
    return delta.y() != 0 ? QPoint( delta.y(), 0 ) : delta;
}

} // namespace

bool HorizontalWheelFilter::eventFilter( QObject* watched, QEvent* event )
{
    // This runs for every event of the application, all other events stop here
    if ( event->type() != QEvent::Wheel ) {
        return false;
    }

    const auto* wheel = static_cast<QWheelEvent*>( event );
    if ( wheel->modifiers() != Qt::ShiftModifier || !watched->isWidgetType() ) {
        return false;
    }

    auto* area = horizontalArea( static_cast<QWidget*>( watched ) );
    if ( !area ) {
        return false;
    }

    // Without Shift, which makes a scroll bar jump by pages
    auto* viewport = area->viewport();
    QWheelEvent horizontal( viewport->mapFromGlobal( wheel->globalPosition() ),
                            wheel->globalPosition(), sideways( wheel->pixelDelta() ),
                            sideways( wheel->angleDelta() ), wheel->buttons(), Qt::NoModifier,
                            wheel->phase(), wheel->inverted(), wheel->source(),
                            wheel->pointingDevice() );
    QCoreApplication::sendEvent( viewport, &horizontal );
    return true;
}
