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

#include "tooltipplacement.h"

#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QPointer>
#include <QScreen>
#include <QWidget>

namespace tooltips {

namespace {
// Between the tip and the hot spot of the cursor
constexpr int Gap = 4;

class Placement : public QObject {
  public:
    using QObject::QObject;

  protected:
    // Called for every event of the application: the type is checked first
    bool eventFilter( QObject* watched, QEvent* event ) override
    {
        const auto type = event->type();
        if ( ( type == QEvent::Move || type == QEvent::Show ) && !moving_
             && watched->isWidgetType() && watched->inherits( "QTipLabel" ) ) {
            auto* tip = static_cast<QWidget*>( watched );
            // Where it was moved to, give or take what the window system changed: not again
            // (a new place of Qt is further)
            if ( tip == lastTip_ && ( tip->pos() - requested_ ).manhattanLength() <= 8 ) {
                return QObject::eventFilter( watched, event );
            }
            const auto cursor = QCursor::pos();
            const auto* screen = QApplication::screenAt( cursor );
            if ( screen ) {
                const auto position
                    = placed( tip->geometry(), cursor, screen->availableGeometry() );
                // Once: the Move event of this move is not looked at (the window system
                // may place it elsewhere)
                if ( position != tip->pos() ) {
                    lastTip_ = tip;
                    requested_ = position;
                    moving_ = true;
                    tip->move( position );
                    moving_ = false;
                }
            }
        }
        return QObject::eventFilter( watched, event );
    }

  private:
    bool moving_ = false;
    QPointer<QWidget> lastTip_;
    QPoint requested_;
};
} // namespace

QPoint placed( const QRect& tip, const QPoint& cursor, const QRect& screen )
{
    // Across the hot spot horizontally, on the row of the cursor: beside it, but under it.
    // Below the cursor (small cursors) it does not hide the text.
    const auto across = tip.left() <= cursor.x() + Gap && tip.right() >= cursor.x();
    const auto onRow = tip.top() <= cursor.y() + Gap && tip.bottom() >= cursor.y();
    if ( !across || !onRow ) {
        return tip.topLeft();
    }
    const auto left = cursor.x() - Gap - tip.width();
    if ( left < screen.left() ) {
        return tip.topLeft();
    }
    return { left, tip.top() };
}

void installPlacement()
{
    static Placement* placement = nullptr;
    if ( !placement && qApp ) {
        placement = new Placement( qApp );
        qApp->installEventFilter( placement );
    }
}

} // namespace tooltips
