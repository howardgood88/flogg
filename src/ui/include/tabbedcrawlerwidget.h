/*
 * Copyright (C) 2014 Nicolas Bonnefon and other contributors
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

#ifndef TABBEDCRAWLERWIDGET_H
#define TABBEDCRAWLERWIDGET_H

#include <QTabBar>
#include <QTabWidget>
#include <qobjectdefs.h>
#include <qtabbar.h>
#include <qwidget.h>

#include "loadingstatus.h"

// This class represents glogg's main widget, a tabbed
// group of CrawlerWidgets.
// This is a very slightly customised QTabWidget, with
// a particular style.

#include <QPointer>

#include <memory>

class QLabel;
class QMenu;
class CrawlerWidget;

class CrawlerTabBar : public QTabBar {
  Q_OBJECT

  Q_SIGNALS:
    void showTabContextMenu(int tab, QPoint point);
    // The tab was dragged off the bar, hotSpot is where the mouse holds the tab
    // (in the coordinates of the tab). The mouse moves and is released after.
    void tabDraggedOut( int tab, QPoint hotSpot, QPoint globalPos );
    void tabDragMoved( QPoint globalPos );
    void tabDragReleased( QPoint globalPos );

  protected:
    void mousePressEvent( QMouseEvent* ) override;
    void mouseMoveEvent( QMouseEvent* ) override;
    void mouseReleaseEvent( QMouseEvent* ) override;

  private:
    QPoint pressPos_;
    // Where the mouse holds the pressed tab, in the coordinates of the tab
    QPoint pressOffset_;
    bool pressedTab_ = false;
    bool draggedOut_ = false;
};

class TabbedCrawlerWidget : public QTabWidget {
    Q_OBJECT
  public:
    TabbedCrawlerWidget();
    ~TabbedCrawlerWidget() override;

    template <typename T>
    int addCrawler( T* crawler, const QString& fileName )
    {
        return insertCrawler( -1, crawler, fileName );
    }

    // At the end for a negative index
    template <typename T>
    int insertCrawler( int index, T* crawler, const QString& fileName,
                       DataStatus status = DataStatus::OLD_DATA )
    {
        index = QTabWidget::insertTab( index, crawler, QString{} );

        connect( crawler, &T::dataStatusChanged, this, [ this, fileName ]( DataStatus status ) {
            const auto tabsCount = count();
            for ( int i = 0; i < tabsCount; ++i ) {
                if ( tabPathAt( i ) == fileName ) {
                    setTabDataStatus( i, status );
                    return;
                }
            }
        } );

        // A tab with changes of the edit mode not saved shows a star
        connect( crawler, &T::editModifiedChanged, this, [ this, crawler ]( bool modified ) {
            setTabModified( indexOf( crawler ), modified );
        } );

        addTabBarItem( index, fileName );
        if ( status != DataStatus::OLD_DATA ) {
            setTabDataStatus( index, status );
        }

        return index;
    }

    void removeCrawler( int index );

    // " *" after the name of the tab
    void setTabModified( int index, bool modified );

    DataStatus tabDataStatus( int index ) const;

    // Index where a tab dropped at the position (in the coordinates of this widget) goes:
    // before or after the tab under it, at the end right of the tabs. -1 away from the bar.
    int dropIndexAt( QPoint pos ) const;

    // The style sheet of the tab bar makes Qt skip this widget and the tabs when the palette
    // of the application changes, the window passes it on
    void followParentPalette();

  protected:
    void keyPressEvent( QKeyEvent* event ) override;
    void mouseReleaseEvent( QMouseEvent* event ) override;
    void changeEvent( QEvent* event ) override;
    // Without a tab: the area of the views, framed, so that its edges (where the panels are
    // resized) show
    void paintEvent( QPaintEvent* event ) override;
    void tabInserted( int index ) override;
    void tabRemoved( int index ) override;

  Q_SIGNALS:
    // Before the context menu of the tab is shown, to add actions to it
    void tabContextMenuAboutToShow( QMenu* menu, int tab );
    // A tab dragged off the bar was released at the global position
    void tabDropped( CrawlerWidget* crawler, QPoint globalPos, QPoint hotSpot );

  protected:
    // Esc while a tab is dragged
    bool eventFilter( QObject* watched, QEvent* event ) override;

  private:
    // The drag is followed here and not by a system drag: on macOS a drag dropped out of
    // every window first slides back to where it started, and ends with its window active
    void startTabDrag( int tab, QPoint hotSpot, QPoint globalPos );
    void moveTabDrag( QPoint globalPos );
    void finishTabDrag( QPoint globalPos );
    void cancelTabDrag();
    // The tab over a small picture of its contents
    QPixmap tabDragPixmap( int tab );

    void addTabBarItem( int index, const QString& fileName );
    QString tabPathAt( int index ) const;

    // Set the data status (icon) for the tab number 'index'
    void setTabDataStatus( int index, DataStatus status );

    // The close buttons of the tabs depend on the style
    void updateTabBarStyle();
    void loadIcons();
    // Marks the tabs using the shared filters
    void updateSharedMarks();
    void updateIcon( int index );

  private Q_SLOTS:
    void showContextMenu( int tab, QPoint globalPoint );

  private:
    QIcon olddata_icon_;
    QIcon shared_icon_;
    QIcon newdata_icon_;
    QIcon newfiltered_icon_;

    CrawlerTabBar myTabBar_;

    // The tab being dragged off the bar, and its picture under the mouse
    QPointer<CrawlerWidget> draggedCrawler_;
    QPoint dragHotSpot_;
    std::unique_ptr<QLabel> dragPreview_;
};

#endif
