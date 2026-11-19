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

#include "paneldock.h"

#include <QDockWidget>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QToolButton>

namespace {

constexpr int IconSize = 16;

// Both pixel ratios, drawn on the 16 px grid
template <typename Draw>
QIcon drawIcon( Draw draw )
{
    QIcon icon;
    for ( int ratio : { 1, 2 } ) {
        QPixmap pixmap( IconSize * ratio, IconSize * ratio );
        pixmap.setDevicePixelRatio( ratio );
        pixmap.fill( Qt::transparent );
        QPainter painter( &pixmap );
        painter.setRenderHint( QPainter::Antialiasing, ratio > 1 );
        draw( painter );
        painter.end();
        icon.addPixmap( pixmap );
    }
    return icon;
}

QPen linePen( const QColor& color )
{
    QPen pen( color, 1.0 );
    pen.setCapStyle( Qt::FlatCap );
    pen.setJoinStyle( Qt::MiterJoin );
    return pen;
}

} // namespace

namespace panelicons {

QIcon fileTree( const QColor& lines )
{
    return drawIcon( [ &lines ]( QPainter& painter ) {
        painter.setPen( linePen( lines ) );
        QPainterPath folder;
        folder.moveTo( 1.5, 3.5 );
        folder.lineTo( 6.0, 3.5 );
        folder.lineTo( 7.5, 5.0 );
        folder.lineTo( 14.5, 5.0 );
        folder.lineTo( 14.5, 13.5 );
        folder.lineTo( 1.5, 13.5 );
        folder.closeSubpath();
        painter.drawPath( folder );
        painter.drawLine( QPointF( 1.5, 6.5 ), QPointF( 14.5, 6.5 ) );
    } );
}

QIcon filters( const QColor& lines )
{
    // Rules of the filter panel: a colour, then the pattern
    return drawIcon( [ &lines ]( QPainter& painter ) {
        painter.fillRect( QRectF( 1, 2, 4, 3 ), QColor( 0xd9, 0x43, 0x3b ) );
        painter.fillRect( QRectF( 1, 7, 4, 3 ), QColor( 0x2e, 0x9b, 0x4f ) );
        painter.fillRect( QRectF( 1, 12, 4, 3 ), QColor( 0x2f, 0x6f, 0xdb ) );
        painter.setPen( linePen( lines ) );
        painter.drawLine( QPointF( 7, 3.5 ), QPointF( 15, 3.5 ) );
        painter.drawLine( QPointF( 7, 8.5 ), QPointF( 13, 8.5 ) );
        painter.drawLine( QPointF( 7, 13.5 ), QPointF( 14, 13.5 ) );
    } );
}

QIcon render( const QColor& lines )
{
    return drawIcon( [ &lines ]( QPainter& painter ) {
        painter.setRenderHint( QPainter::Antialiasing, true );
        painter.setPen( linePen( lines ) );
        QPainterPath eye;
        eye.moveTo( 1.0, 8.0 );
        eye.quadTo( 8.0, 0.5, 15.0, 8.0 );
        eye.quadTo( 8.0, 15.5, 1.0, 8.0 );
        painter.drawPath( eye );
        painter.setBrush( lines );
        painter.drawEllipse( QPointF( 8.0, 8.0 ), 2.5, 2.5 );
    } );
}

QIcon edit( const QColor& lines )
{
    return drawIcon( [ &lines ]( QPainter& painter ) {
        painter.setRenderHint( QPainter::Antialiasing, true );
        painter.setPen( linePen( lines ) );
        // A pencil going down to the left, its point on the line
        QPainterPath pencil;
        pencil.moveTo( 11.0, 2.0 );
        pencil.lineTo( 14.0, 5.0 );
        pencil.lineTo( 5.5, 13.5 );
        pencil.lineTo( 2.0, 14.0 );
        pencil.lineTo( 2.5, 10.5 );
        pencil.closeSubpath();
        painter.drawPath( pencil );
        painter.drawLine( QPointF( 9.5, 3.5 ), QPointF( 12.5, 6.5 ) );
    } );
}

} // namespace panelicons

DockTitleBar::DockTitleBar( QDockWidget* dock )
    : QWidget( dock )
    , dock_( dock )
    , iconLabel_( new QLabel )
    , titleLabel_( new QLabel( dock->windowTitle() ) )
    , floatButton_( new QToolButton )
    , closeButton_( new QToolButton )
{
    auto* layout = new QHBoxLayout( this );
    layout->setContentsMargins( 6, 3, 3, 3 );
    layout->setSpacing( 5 );

    iconLabel_->setFixedSize( IconSize, IconSize );
    for ( auto* button : { floatButton_, closeButton_ } ) {
        button->setAutoRaise( true );
        button->setIconSize( QSize( 12, 12 ) );
        button->setFocusPolicy( Qt::NoFocus );
    }
    floatButton_->setToolTip( tr( "Float" ) );
    closeButton_->setToolTip( tr( "Close" ) );

    layout->addWidget( iconLabel_ );
    layout->addWidget( titleLabel_, 1 );
    layout->addWidget( floatButton_ );
    layout->addWidget( closeButton_ );

    connect( dock, &QDockWidget::windowTitleChanged, titleLabel_, &QLabel::setText );
    connect( dock, &QDockWidget::featuresChanged, this, &DockTitleBar::updateButtons );
    connect( floatButton_, &QToolButton::clicked, dock,
             [ dock ] { dock->setFloating( !dock->isFloating() ); } );
    connect( closeButton_, &QToolButton::clicked, dock, &QDockWidget::close );

    updateButtons();
}

void DockTitleBar::setIcon( const QIcon& icon )
{
    iconLabel_->setPixmap( icon.pixmap( IconSize, IconSize ) );
}

void DockTitleBar::changeEvent( QEvent* event )
{
    if ( event->type() == QEvent::StyleChange ) {
        updateButtons();
    }
    QWidget::changeEvent( event );
}

void DockTitleBar::updateButtons()
{
    const auto features = dock_->features();
    floatButton_->setVisible( features.testFlag( QDockWidget::DockWidgetFloatable ) );
    closeButton_->setVisible( features.testFlag( QDockWidget::DockWidgetClosable ) );
    floatButton_->setIcon( style()->standardIcon( QStyle::SP_TitleBarNormalButton, nullptr, this ) );
    closeButton_->setIcon( style()->standardIcon( QStyle::SP_TitleBarCloseButton, nullptr, this ) );
}
