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

#ifndef KLOGG_PANELDOCK_H
#define KLOGG_PANELDOCK_H

#include <QColor>
#include <QIcon>
#include <QWidget>

class QDockWidget;
class QLabel;
class QToolButton;

// Icons of the side panels, drawn for the lines colour of the current theme
namespace panelicons {

QIcon fileTree( const QColor& lines );
QIcon filters( const QColor& lines );
// Render mode of the tab: an eye, the file is previewed
QIcon render( const QColor& lines );
// Edit mode of the tab: a pencil
QIcon edit( const QColor& lines );

} // namespace panelicons

// Title bar of a side panel: its icon and title, then the float and close buttons.
// Dragging and double clicking it are left to the dock widget.
class DockTitleBar : public QWidget {
    Q_OBJECT

  public:
    explicit DockTitleBar( QDockWidget* dock );

    void setIcon( const QIcon& icon );

  protected:
    void changeEvent( QEvent* event ) override;

  private:
    void updateButtons();

    QDockWidget* dock_;
    QLabel* iconLabel_;
    QLabel* titleLabel_;
    QToolButton* floatButton_;
    QToolButton* closeButton_;
};

#endif
