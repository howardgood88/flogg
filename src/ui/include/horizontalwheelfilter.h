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

#ifndef HORIZONTALWHEELFILTER_H
#define HORIZONTALWHEELFILTER_H

#include <QObject>

// Installed on the application: Shift + mouse wheel scrolls the widget under the mouse
// sideways, in every scroll area of flogg
class HorizontalWheelFilter : public QObject {
    Q_OBJECT

  public:
    using QObject::QObject;

    bool eventFilter( QObject* watched, QEvent* event ) override;
};

#endif
