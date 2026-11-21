/*
 * Copyright (C) 2014, 2015 Nicolas Bonnefon and other contributors
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

#include "tabbedcrawlerwidget.h"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDir>
#include <QFileInfo>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <qobjectdefs.h>
#include <qpoint.h>

#include "crawlerwidget.h"

#include "clipboard.h"
#include "configuration.h"
#include "dispatch_to.h"
#include "iconloader.h"
#include "log.h"
#include "openfilehelper.h"
#include "shortcuts.h"
#include "styles.h"
#include "tabnamemapping.h"

namespace {
constexpr QLatin1String PathKey = QLatin1String( "path", 4 );
constexpr QLatin1String StatusKey = QLatin1String( "status", 6 );

// Width of the picture of the tab contents under the mouse while dragging
constexpr int DragPreviewWidth = 320;
constexpr int DragPreviewMaxHeight = 240;
} // namespace

TabbedCrawlerWidget::TabbedCrawlerWidget()
    : QTabWidget()
    , newdata_icon_( ":/images/newdata_icon.png" )
    , newfiltered_icon_( ":/images/newfiltered_icon.png" )
{
    updateTabBarStyle();

    // Shown as soon as a file is open, even with a single tab
    setTabBar( &myTabBar_ );
    myTabBar_.hide();

    myTabBar_.setContextMenuPolicy( Qt::CustomContextMenu );
    connect( &myTabBar_, &CrawlerTabBar::showTabContextMenu, this,
             &TabbedCrawlerWidget::showContextMenu );
    connect( &myTabBar_, &CrawlerTabBar::tabDraggedOut, this,
             &TabbedCrawlerWidget::startTabDrag );
    connect( &myTabBar_, &CrawlerTabBar::tabDragMoved, this, &TabbedCrawlerWidget::moveTabDrag );
    connect( &myTabBar_, &CrawlerTabBar::tabDragReleased, this,
             &TabbedCrawlerWidget::finishTabDrag );

    // Queued: tabs stop using the shared filters while their window is destroyed
    connect( &SharedFilters::instance(), &SharedFilters::usersChanged, this,
             &TabbedCrawlerWidget::updateSharedMarks, Qt::QueuedConnection );

    // Dropped when the widget is destroyed first
    dispatchToObject( [ this ] { loadIcons(); }, this );
}

TabbedCrawlerWidget::~TabbedCrawlerWidget()
{
    cancelTabDrag();
}

void TabbedCrawlerWidget::updateTabBarStyle()
{
    // The styles draw the labels of the tabs a few pixels low: below 28 pixels the bottom of
    // the text is cut on macOS, their natural height (32) is taller than needed
    QString tabStyle = "QTabBar::tab { height: 28px; }";
    QString tabCloseButtonStyle = " QTabBar::close-button {\
              height: 12px; width: 12px;\
              subcontrol-origin: padding;\
              subcontrol-position: right;\
              %1}";

    QString backgroundImage;
    QString backgroundHoverImage;

    const auto& config = Configuration::get();
    if ( config.style() == StyleManager::DarkStyleKey ) {
        backgroundImage = ":/images/icons8-close-window-16_inverse.png";
        backgroundHoverImage = ":/images/icons8-close-window-hover-16_inverse.png";
    }

#if defined( Q_OS_MAC )
    // work around Qt MacOSX bug missing tab close icons
    // see: https://bugreports.qt.io/browse/QTBUG-61092
    // still broken in document mode in Qt.5.12.2 !!!!
    if ( config.style() != StyleManager::DarkStyleKey ) {
        backgroundImage
            = ":/qt-project.org/styles/commonstyle/images/standardbutton-closetab-16.png";
        backgroundHoverImage
            = ":/qt-project.org/styles/commonstyle/images/standardbutton-closetab-hover-16.png";
    }
#elif defined( Q_OS_WIN )
    if ( config.style() == StyleManager::FusionKey ) {
        backgroundImage = ":/images/icons8-close-window-16.png";
        backgroundHoverImage = ":/images/icons8-close-window-hover-16.png";
    }
#endif

    if ( !backgroundImage.isEmpty() ) {
        const QString backgroundImageTemplate = " image: url(%1);";
        QString tabCloseButtonHoverStyle = " QTabBar::close-button:hover { %1 }";
        backgroundImage = backgroundImageTemplate.arg( backgroundImage );
        backgroundHoverImage = backgroundImageTemplate.arg( backgroundHoverImage );
        tabCloseButtonHoverStyle = tabCloseButtonHoverStyle.arg( backgroundHoverImage );
        tabCloseButtonStyle = tabCloseButtonStyle.arg( backgroundImage );
        tabCloseButtonStyle.append( tabCloseButtonHoverStyle );
    }
    else {
        tabCloseButtonStyle = tabCloseButtonStyle.arg( "" );
    }

    // Applying the same sheet again would polish the tab bar for nothing
    const auto styleSheet = tabStyle.append( tabCloseButtonStyle );
    if ( myTabBar_.styleSheet() != styleSheet ) {
        myTabBar_.setStyleSheet( styleSheet );
    }
}

void TabbedCrawlerWidget::loadIcons()
{
    IconLoader iconLoader{ this };
    olddata_icon_ = iconLoader.load( "olddata_icon" );
    for ( int tab = 0; tab < count(); ++tab ) {
        updateIcon( tab );
    }

    // Drawn again in the colours of the style
    shared_icon_ = sharedFiltersIcon( palette().color( QPalette::Active, QPalette::Highlight ) );
    for ( int tab = 0; tab < count(); ++tab ) {
        for ( const auto side : { QTabBar::LeftSide, QTabBar::RightSide } ) {
            if ( auto* mark = qobject_cast<QLabel*>( myTabBar_.tabButton( tab, side ) ) ) {
                myTabBar_.setTabButton( tab, side, nullptr );
                mark->deleteLater();
            }
        }
    }
    updateSharedMarks();
}

void TabbedCrawlerWidget::updateSharedMarks()
{
    if ( shared_icon_.isNull() ) {
        shared_icon_
            = sharedFiltersIcon( palette().color( QPalette::Active, QPalette::Highlight ) );
    }

    // On the side without the close button
    const auto closeSide = static_cast<QTabBar::ButtonPosition>(
        myTabBar_.style()->styleHint( QStyle::SH_TabBar_CloseButtonPosition, nullptr, &myTabBar_ ) );
    const auto markSide = closeSide == QTabBar::LeftSide ? QTabBar::RightSide : QTabBar::LeftSide;

    const auto& sharedFilters = SharedFilters::instance();
    for ( int tab = 0; tab < count(); ++tab ) {
        const auto isShared = sharedFilters.isUser( widget( tab ) );
        auto* mark = qobject_cast<QLabel*>( myTabBar_.tabButton( tab, markSide ) );
        if ( isShared && !mark ) {
            mark = new QLabel;
            mark->setPixmap( shared_icon_.pixmap( QSize( 16, 16 ), devicePixelRatioF() ) );
            mark->setToolTip( tr( "Uses the shared filters" ) );
            // Clicks select the tab
            mark->setAttribute( Qt::WA_TransparentForMouseEvents );
            myTabBar_.setTabButton( tab, markSide, mark );
        }
        else if ( !isShared && mark ) {
            myTabBar_.setTabButton( tab, markSide, nullptr );
            mark->deleteLater();
        }
    }
}

void TabbedCrawlerWidget::followParentPalette()
{
    // An empty palette takes the one of the parent and passes it to the tabs
    setPalette( QPalette() );
}

void TabbedCrawlerWidget::changeEvent( QEvent* event )
{
    if ( event->type() == QEvent::StyleChange ) {
        dispatchToObject(
            [ this ] {
                updateTabBarStyle();
                loadIcons();
            },
            this );
    }
    QWidget::changeEvent( event );
}

void TabbedCrawlerWidget::addTabBarItem( int index, const QString& fileName )
{
    const auto tabLabel = QFileInfo( fileName ).fileName();
    const auto tabName = TabNameMapping::getSynced().tabName( fileName );

    myTabBar_.setTabIcon( index, olddata_icon_ );
    updateSharedMarks();
    myTabBar_.setTabText( index, tabName.isEmpty() ? tabLabel : tabName );
    myTabBar_.setTabToolTip( index, QDir::toNativeSeparators( fileName ) );

    QVariantMap tabData;
    tabData[ PathKey ] = fileName;
    tabData[ StatusKey ] = static_cast<int>( DataStatus::OLD_DATA );

    myTabBar_.setTabData( index, tabData );

    setCurrentIndex( index );
    myTabBar_.show();
}

void TabbedCrawlerWidget::setTabModified( int index, bool modified )
{
    if ( index < 0 ) {
        return;
    }
    static const QString mark = QStringLiteral( " *" );
    auto text = myTabBar_.tabText( index );
    if ( text.endsWith( mark ) ) {
        text.chop( mark.size() );
    }
    myTabBar_.setTabText( index, modified ? text + mark : text );
}

void TabbedCrawlerWidget::removeCrawler( int index )
{
    // The status of the tab follows the file while it is in this widget only
    if ( auto* crawler = widget( index ) ) {
        disconnect( crawler, nullptr, this, nullptr );
    }

    QTabWidget::removeTab( index );

    if ( count() == 0 )
        myTabBar_.hide();
}

void TabbedCrawlerWidget::mouseReleaseEvent( QMouseEvent* event )
{
    LOG_DEBUG << "TabbedCrawlerWidget::mouseReleaseEvent";

    if ( event->button() == Qt::MiddleButton ) {
        int tab = this->myTabBar_.tabAt( event->pos() );
        if ( -1 != tab ) {
            Q_EMIT tabCloseRequested( tab );
            event->accept();
        }
    }

    event->ignore();
}

QString TabbedCrawlerWidget::tabPathAt( int index ) const
{
    return myTabBar_.tabData( index ).toMap()[ PathKey ].toString();
}

DataStatus TabbedCrawlerWidget::tabDataStatus( int index ) const
{
    return static_cast<DataStatus>( myTabBar_.tabData( index ).toMap()[ StatusKey ].toInt() );
}

int TabbedCrawlerWidget::dropIndexAt( QPoint pos ) const
{
    if ( !myTabBar_.isVisible() ) {
        return -1;
    }

    const auto barPos = myTabBar_.mapFrom( this, pos );
    const auto barHeight = myTabBar_.height();
    if ( barPos.y() < -barHeight || barPos.y() > 2 * barHeight ) {
        return -1;
    }

    const auto tab = myTabBar_.tabAt( QPoint( barPos.x(), barHeight / 2 ) );
    if ( tab < 0 ) {
        return count() > 0 && barPos.x() < myTabBar_.tabRect( 0 ).left() ? 0 : count();
    }
    return barPos.x() > myTabBar_.tabRect( tab ).center().x() ? tab + 1 : tab;
}

QPixmap TabbedCrawlerWidget::tabDragPixmap( int tab )
{
    const auto tabImage = myTabBar_.grab( myTabBar_.tabRect( tab ) );
    const auto tabSize = tabImage.deviceIndependentSize().toSize();

    // The views as they are drawn now, made smaller
    const auto contents = widget( tab )->grab();
    auto contentsSize = contents.deviceIndependentSize().toSize();
    if ( !contentsSize.isEmpty() ) {
        contentsSize.scale( DragPreviewWidth, DragPreviewMaxHeight, Qt::KeepAspectRatio );
    }

    const auto dpr = devicePixelRatioF();
    const QSize size( std::max( tabSize.width(), contentsSize.width() ),
                      tabSize.height() + contentsSize.height() );
    QPixmap pixmap( size * dpr );
    pixmap.setDevicePixelRatio( dpr );
    pixmap.fill( Qt::transparent );

    QPainter painter( &pixmap );
    painter.drawPixmap( QPoint( 0, 0 ), tabImage );
    if ( !contentsSize.isEmpty() ) {
        const QRect contentsRect( QPoint( 0, tabSize.height() ), contentsSize );
        painter.setRenderHint( QPainter::SmoothPixmapTransform );
        painter.setOpacity( 0.85 );
        painter.drawPixmap( contentsRect, contents );
        painter.setOpacity( 1 );
        painter.setPen( palette().color( QPalette::Mid ) );
        painter.drawRect( contentsRect.adjusted( 0, 0, -1, -1 ) );
    }
    return pixmap;
}

void TabbedCrawlerWidget::startTabDrag( int tab, QPoint hotSpot, QPoint globalPos )
{
    auto* crawler = qobject_cast<CrawlerWidget*>( widget( tab ) );

    // The window shows a file once it is loaded: a file still loading stays in its window
    if ( !crawler || crawler->isLoading() ) {
        return;
    }

    draggedCrawler_ = crawler;
    dragHotSpot_ = hotSpot;

    // A window of its own above the others, that the mouse goes through and that does
    // not take the focus
    dragPreview_ = std::make_unique<QLabel>(
        nullptr, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowTransparentForInput
                     | Qt::WindowDoesNotAcceptFocus );
    dragPreview_->setAttribute( Qt::WA_TranslucentBackground );
    dragPreview_->setAttribute( Qt::WA_ShowWithoutActivating );
    dragPreview_->setPixmap( tabDragPixmap( tab ) );
    dragPreview_->adjustSize();
    moveTabDrag( globalPos );
    dragPreview_->show();

    qApp->installEventFilter( this );
}

void TabbedCrawlerWidget::moveTabDrag( QPoint globalPos )
{
    if ( dragPreview_ ) {
        dragPreview_->move( globalPos - dragHotSpot_ );
    }
}

void TabbedCrawlerWidget::finishTabDrag( QPoint globalPos )
{
    const auto crawler = draggedCrawler_;
    cancelTabDrag();
    if ( crawler ) {
        Q_EMIT tabDropped( crawler, globalPos, dragHotSpot_ );
    }
}

void TabbedCrawlerWidget::cancelTabDrag()
{
    qApp->removeEventFilter( this );
    draggedCrawler_ = nullptr;
    // Its window is gone before looking for the window under the mouse
    dragPreview_.reset();
}

bool TabbedCrawlerWidget::eventFilter( QObject* watched, QEvent* event )
{
    // Before a shortcut of the window takes the key
    const auto type = event->type();
    if ( draggedCrawler_ && ( type == QEvent::ShortcutOverride || type == QEvent::KeyPress )
         && static_cast<QKeyEvent*>( event )->key() == Qt::Key_Escape ) {
        cancelTabDrag();
        event->accept();
        return true;
    }
    return QTabWidget::eventFilter( watched, event );
}

void CrawlerTabBar::mousePressEvent( QMouseEvent* mouseEvent )
{
    if ( mouseEvent->button() == Qt::LeftButton ) {
        draggedOut_ = false;
        pressPos_ = mouseEvent->pos();
        const auto tab = tabAt( pressPos_ );
        pressedTab_ = tab >= 0;
        if ( pressedTab_ ) {
            pressOffset_ = pressPos_ - tabRect( tab ).topLeft();
        }
    }

    QTabBar::mousePressEvent( mouseEvent );
}

void CrawlerTabBar::mouseMoveEvent( QMouseEvent* mouseEvent )
{
    if ( pressedTab_ && !draggedOut_ && ( mouseEvent->buttons() & Qt::LeftButton ) ) {
        // A tab moved along the bar stays in it, it leaves the bar further away
        const auto margin = std::max( height(), QApplication::startDragDistance() * 4 );
        if ( !rect().adjusted( -margin, -margin, margin, margin ).contains( mouseEvent->pos() ) ) {
            draggedOut_ = true;

            // Ends the move of the tab along the bar, the pressed tab is the current one
            QMouseEvent release( QEvent::MouseButtonRelease, mouseEvent->position(),
                                 mouseEvent->globalPosition(), Qt::LeftButton, Qt::NoButton,
                                 mouseEvent->modifiers() );
            QTabBar::mouseReleaseEvent( &release );

            Q_EMIT tabDraggedOut( currentIndex(), pressOffset_,
                                  mouseEvent->globalPosition().toPoint() );
            return;
        }
    }

    if ( draggedOut_ ) {
        Q_EMIT tabDragMoved( mouseEvent->globalPosition().toPoint() );
        return;
    }

    QTabBar::mouseMoveEvent( mouseEvent );
}

void CrawlerTabBar::mouseReleaseEvent( QMouseEvent* mouseEvent )
{
    if ( mouseEvent->button() == Qt::LeftButton ) {
        pressedTab_ = false;
        if ( std::exchange( draggedOut_, false ) ) {
            Q_EMIT tabDragReleased( mouseEvent->globalPosition().toPoint() );
            return;
        }
    }

    if ( mouseEvent->button() == Qt::RightButton ) {
        int tab = tabAt( mouseEvent->pos() );
        if ( tab != -1 ) {
            Q_EMIT showTabContextMenu( tab, mapToGlobal( mouseEvent->pos() ) );
            mouseEvent->accept();
            return;
        }
    }

    // QTabBar finishes moving a dragged tab on release, without it the tab stays where
    // the mouse left it
    QTabBar::mouseReleaseEvent( mouseEvent );
}

void TabbedCrawlerWidget::showContextMenu( int tab, QPoint globalPoint )
{
    QMenu menu( this );
    auto closeThis = menu.addAction( tr( "Close this" ) );
    auto closeOthers = menu.addAction( tr( "Close others" ) );
    auto closeLeft = menu.addAction( tr( "Close to the left" ) );
    auto closeRight = menu.addAction( tr( "Close to the right" ) );
    auto closeAll = menu.addAction( tr( "Close all" ) );
    menu.addSeparator();
    auto copyFullPath = menu.addAction( tr( "Copy full path" ) );
    auto openContainingFolder = menu.addAction( tr( "Open containing folder" ) );
    menu.addSeparator();
    auto renameTab = menu.addAction( tr( "Rename tab" ) );
    auto resetTabName = menu.addAction( tr( "Reset tab name" ) );
    Q_EMIT tabContextMenuAboutToShow( &menu, tab );

    const auto& shortcuts = Configuration::get().shortcuts();
    ShortcutAction::showInMenu( closeThis, ShortcutAction::MainWindowCloseFile, shortcuts );
    ShortcutAction::showInMenu( closeAll, ShortcutAction::MainWindowCloseAll, shortcuts );
    ShortcutAction::showInMenu( copyFullPath, ShortcutAction::MainWindowCopyPathToClipboard,
                                shortcuts );
    ShortcutAction::showInMenu( openContainingFolder,
                                ShortcutAction::MainWindowOpenContainingFolder, shortcuts );

    connect( closeThis, &QAction::triggered, [ tab, this ] { Q_EMIT tabCloseRequested( tab ); } );

    // A tab stays open when the user cancels saving its filters, the loops stop there
    const auto requestClose = [ this ]( int index ) {
        const auto tabsBefore = count();
        Q_EMIT tabCloseRequested( index );
        return count() < tabsBefore;
    };

    connect( closeOthers, &QAction::triggered, [ tabWidget = widget( tab ), this, requestClose ] {
        while ( count() != 1 ) {
            const auto other = indexOf( tabWidget ) == 0 ? 1 : 0;
            if ( !requestClose( other ) ) {
                break;
            }
        }
    } );

    connect( closeLeft, &QAction::triggered, [ tabWidget = widget( tab ), this, requestClose ] {
        while ( indexOf( tabWidget ) > 0 ) {
            if ( !requestClose( 0 ) ) {
                break;
            }
        }
    } );

    connect( closeRight, &QAction::triggered, [ tabWidget = widget( tab ), this, requestClose ] {
        while ( count() > indexOf( tabWidget ) + 1 ) {
            if ( !requestClose( indexOf( tabWidget ) + 1 ) ) {
                break;
            }
        }
    } );

    connect( closeAll, &QAction::triggered, [ this, requestClose ] {
        while ( count() ) {
            if ( !requestClose( 0 ) ) {
                break;
            }
        }
    } );

    if ( tab == 0 ) {
        closeLeft->setDisabled( true );
    }
    else if ( tab == count() - 1 ) {
        closeRight->setDisabled( true );
    }

    connect( copyFullPath, &QAction::triggered, this,
             [ this, tab ] { sendTextToClipboard( tabToolTip( tab ) ); } );

    connect( openContainingFolder, &QAction::triggered, this,
             [ this, tab ] { showPathInFileExplorer( tabToolTip( tab ) ); } );

    connect( renameTab, &QAction::triggered, this, [ this, tab ] {
        bool isNameEntered = false;
        auto newName = QInputDialog::getText( this, "Rename tab", "Tab name", QLineEdit::Normal,
                                              myTabBar_.tabText( tab ), &isNameEntered );
        if ( isNameEntered ) {
            const auto tabPath = tabPathAt( tab );
            TabNameMapping::getSynced().setTabName( tabPath, newName ).save();

            if ( newName.isEmpty() ) {
                myTabBar_.setTabText( tab, QFileInfo( tabPath ).fileName() );
            }
            else {
                myTabBar_.setTabText( tab, std::move( newName ) );
            }
        }
    } );

    connect( resetTabName, &QAction::triggered, this, [ this, tab ] {
        const auto tabPath = tabPathAt( tab );
        TabNameMapping::getSynced().setTabName( tabPath, "" ).save();
        myTabBar_.setTabText( tab, QFileInfo( tabPath ).fileName() );
    } );

    menu.exec( globalPoint );
}

void TabbedCrawlerWidget::keyPressEvent( QKeyEvent* event )
{
    const auto mod = event->modifiers();
    const auto key = event->key();

    LOG_DEBUG << "TabbedCrawlerWidget::keyPressEvent";

    // Ctrl + tab
    if ( ( mod == Qt::ControlModifier && key == Qt::Key_Tab )
         || ( mod == Qt::ControlModifier && key == Qt::Key_PageDown )
         || ( mod == ( Qt::ControlModifier | Qt::AltModifier | Qt::KeypadModifier )
              && key == Qt::Key_Right ) ) {
        setCurrentIndex( ( currentIndex() + 1 ) % count() );
    }
    // Ctrl + shift + tab
    else if ( ( mod == ( Qt::ControlModifier | Qt::ShiftModifier ) && key == Qt::Key_Tab )
              || ( mod == Qt::ControlModifier && key == Qt::Key_PageUp )
              || ( mod == ( Qt::ControlModifier | Qt::AltModifier | Qt::KeypadModifier )
                   && key == Qt::Key_Left ) ) {
        setCurrentIndex( ( currentIndex() - 1 >= 0 ) ? currentIndex() - 1 : count() - 1 );
    }
    // Ctrl + 1..5 are used by numbered bookmarks
    // Ctrl + 9
    else if ( mod == Qt::ControlModifier && key == Qt::Key_9 ) {
        setCurrentIndex( count() - 1 );
    }
    else if ( mod == Qt::ControlModifier && ( key == Qt::Key_Q || key == Qt::Key_W ) ) {
        Q_EMIT tabCloseRequested( currentIndex() );
    }
    else {
        QTabWidget::keyPressEvent( event );
    }
}

void TabbedCrawlerWidget::updateIcon( int index )
{
    auto tabData = myTabBar_.tabData( index ).toMap();

    const QIcon* icon;
    switch ( static_cast<DataStatus>( tabData[ StatusKey ].toInt() ) ) {
    case DataStatus::OLD_DATA:
        icon = &olddata_icon_;
        break;
    case DataStatus::NEW_DATA:
        icon = &newdata_icon_;
        break;
    case DataStatus::NEW_FILTERED_DATA:
        icon = &newfiltered_icon_;
        break;
    default:
        return;
    }

    myTabBar_.setTabIcon( index, *icon );
}

void TabbedCrawlerWidget::setTabDataStatus( int index, DataStatus status )
{
    LOG_DEBUG << "TabbedCrawlerWidget::setTabDataStatus " << index;

    auto tabData = myTabBar_.tabData( index ).toMap();
    tabData[ StatusKey ] = static_cast<int>( status );
    myTabBar_.setTabData( index, tabData );

    updateIcon( index );
}

void TabbedCrawlerWidget::paintEvent( QPaintEvent* event )
{
    if ( count() > 0 ) {
        QTabWidget::paintEvent( event );
        return;
    }
    QPainter painter( this );
    painter.fillRect( rect(), palette().brush( QPalette::Base ) );
    painter.setPen( palette().color( QPalette::Mid ) );
    painter.drawRect( rect().adjusted( 0, 0, -1, -1 ) );
    auto hint = palette().color( QPalette::Text );
    hint.setAlphaF( 0.45f );
    painter.setPen( hint );
    painter.drawText( rect().adjusted( 16, 16, -16, -16 ), Qt::AlignCenter | Qt::TextWordWrap,
                      tr( "Open a file or drop it here" ) );
}

void TabbedCrawlerWidget::tabInserted( int index )
{
    QTabWidget::tabInserted( index );
    update();
}

void TabbedCrawlerWidget::tabRemoved( int index )
{
    QTabWidget::tabRemoved( index );
    update();
}
