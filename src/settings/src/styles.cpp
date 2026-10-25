/*
 * Copyright (C) 2021 Anton Filimonov and other contributors
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

#include <algorithm>

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QProxyStyle>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QStyleOption>
#include <QToolTip>
#include <qcolor.h>

#include "configuration.h"
#include "log.h"
#include "styles.h"

namespace {

// Fusion drawn like the light Windows style: blue filled check boxes, flat white headers
// and light blue checked tool buttons. Colours taken from the Windows build.
class LightStyle : public QProxyStyle {
  public:
    LightStyle()
        : QProxyStyle( QStyleFactory::create( StyleManager::FusionKey ) )
    {
    }

    QPalette standardPalette() const override
    {
        QPalette palette;
        palette.setColor( QPalette::Window, QColor( 0xf0, 0xf0, 0xf0 ) );
        palette.setColor( QPalette::WindowText, Qt::black );
        palette.setColor( QPalette::Base, Qt::white );
        palette.setColor( QPalette::AlternateBase, QColor( 0xf5, 0xf5, 0xf5 ) );
        palette.setColor( QPalette::ToolTipBase, Qt::white );
        palette.setColor( QPalette::ToolTipText, Qt::black );
        palette.setColor( QPalette::PlaceholderText, QColor( 0x6d, 0x6d, 0x6d ) );
        palette.setColor( QPalette::Text, Qt::black );
        // Windows button face, also the background of the tool bars and their widgets
        palette.setColor( QPalette::Button, QColor( 0xf0, 0xf0, 0xf0 ) );
        palette.setColor( QPalette::ButtonText, Qt::black );
        palette.setColor( QPalette::BrightText, Qt::white );
        palette.setColor( QPalette::Light, Qt::white );
        palette.setColor( QPalette::Midlight, QColor( 0xe3, 0xe3, 0xe3 ) );
        palette.setColor( QPalette::Mid, QColor( 0xa0, 0xa0, 0xa0 ) );
        palette.setColor( QPalette::Dark, QColor( 0xa0, 0xa0, 0xa0 ) );
        palette.setColor( QPalette::Shadow, QColor( 0x69, 0x69, 0x69 ) );
        palette.setColor( QPalette::Highlight, QColor( 0x00, 0x78, 0xd7 ) );
        palette.setColor( QPalette::HighlightedText, Qt::white );
        palette.setColor( QPalette::Link, QColor( 0x00, 0x66, 0xcc ) );
        palette.setColor( QPalette::Accent, QColor( 0x00, 0x5f, 0xb8 ) );

        const QColor disabledText( 0x6d, 0x6d, 0x6d );
        palette.setColor( QPalette::Disabled, QPalette::WindowText, disabledText );
        palette.setColor( QPalette::Disabled, QPalette::Text, disabledText );
        palette.setColor( QPalette::Disabled, QPalette::ButtonText, disabledText );
        return palette;
    }

    void drawPrimitive( PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                        const QWidget* widget ) const override
    {
        switch ( element ) {
        case PE_IndicatorCheckBox:
        case PE_IndicatorItemViewItemCheck:
            drawCheckBox( option, painter );
            return;
        case PE_PanelButtonCommand: {
            // Windows 11 push buttons: near white with a thin grey border
            const auto enabled = option->state.testFlag( State_Enabled );
            const auto pressed = option->state & ( State_Sunken | State_On );
            const auto hovered = option->state.testFlag( State_MouseOver );
            QColor fill( 0xfd, 0xfd, 0xfd );
            QColor border( 0xd0, 0xd0, 0xd0 );
            if ( enabled && pressed ) {
                fill = QColor( 0xcc, 0xe4, 0xf7 );
                border = QColor( 0x00, 0x54, 0x99 );
            }
            else if ( enabled && hovered ) {
                fill = QColor( 0xe0, 0xee, 0xf9 );
                border = QColor( 0x00, 0x78, 0xd4 );
            }
            else if ( !enabled ) {
                fill = QColor( 0xf5, 0xf5, 0xf5 );
                border = QColor( 0xe0, 0xe0, 0xe0 );
            }
            painter->save();
            painter->setRenderHint( QPainter::Antialiasing );
            painter->setPen( border );
            painter->setBrush( fill );
            painter->drawRoundedRect( QRectF( option->rect ).adjusted( 0.5, 0.5, -0.5, -0.5 ), 3,
                                      3 );
            painter->restore();
            return;
        }
        case PE_PanelButtonTool:
            if ( option->state & ( State_On | State_Sunken | State_MouseOver )
                 && option->state & State_Enabled ) {
                const auto pressed = option->state & ( State_On | State_Sunken );
                painter->save();
                painter->setPen( pressed ? QColor( 0x99, 0xc9, 0xef ) : QColor( 0xcc, 0xe4, 0xf7 ) );
                painter->setBrush( pressed ? QColor( 0xcb, 0xe3, 0xf7 ) : QColor( 0xe5, 0xf1, 0xfb ) );
                painter->drawRect( option->rect.adjusted( 0, 0, -1, -1 ) );
                painter->restore();
                return;
            }
            break;
        default:
            break;
        }
        QProxyStyle::drawPrimitive( element, option, painter, widget );
    }

    void drawControl( ControlElement element, const QStyleOption* option, QPainter* painter,
                      const QWidget* widget ) const override
    {
        if ( element == CE_HeaderSection || element == CE_HeaderEmptyArea ) {
            const QColor separator( 0xe5, 0xe5, 0xe5 );
            painter->save();
            const auto hovered = element == CE_HeaderSection && option->state & State_MouseOver;
            painter->fillRect( option->rect, hovered ? QColor( 0xd9, 0xeb, 0xf9 ) : Qt::white );
            painter->setPen( separator );
            painter->drawLine( option->rect.bottomLeft(), option->rect.bottomRight() );
            if ( element == CE_HeaderSection ) {
                painter->drawLine( option->rect.topRight(), option->rect.bottomRight() );
            }
            painter->restore();
            return;
        }
        // Flat instead of the Fusion gradients, toolbars use the window colour like Windows
        if ( element == CE_ToolBar ) {
            painter->fillRect( option->rect, option->palette.window() );
            return;
        }
        if ( element == CE_DockWidgetTitle ) {
            painter->fillRect( option->rect, QColor( 0xda, 0xda, 0xda ) );
            if ( const auto* dock = qstyleoption_cast<const QStyleOptionDockWidget*>( option ) ) {
                const auto textRect
                    = subElementRect( SE_DockWidgetTitleBarText, option, widget ).adjusted( 4, 0, 0, 0 );
                drawItemText( painter, textRect, Qt::AlignLeft | Qt::AlignVCenter, option->palette,
                              option->state.testFlag( State_Enabled ),
                              painter->fontMetrics().elidedText( dock->title, Qt::ElideRight,
                                                                 textRect.width() ),
                              QPalette::WindowText );
            }
            return;
        }
        QProxyStyle::drawControl( element, option, painter, widget );
    }

  private:
    static void drawCheckBox( const QStyleOption* option, QPainter* painter )
    {
        const auto enabled = option->state.testFlag( State_Enabled );
        const auto checked = option->state.testFlag( State_On );
        const auto partial = option->state.testFlag( State_NoChange );

        const auto size = std::min( { option->rect.width(), option->rect.height(), 14 } );
        QRectF box( 0, 0, size - 1, size - 1 );
        box.moveCenter( QRectF( option->rect ).center() );

        painter->save();
        painter->setRenderHint( QPainter::Antialiasing );
        if ( checked || partial ) {
            const QColor fill = enabled ? QColor( 0x00, 0x5f, 0xb8 ) : QColor( 0xc5, 0xc5, 0xc5 );
            painter->setPen( fill );
            painter->setBrush( fill );
            painter->drawRoundedRect( box, 3, 3 );

            QPen markPen( Qt::white, std::max( 1.5, size / 8.0 ) );
            markPen.setCapStyle( Qt::RoundCap );
            markPen.setJoinStyle( Qt::RoundJoin );
            painter->setPen( markPen );
            painter->setBrush( Qt::NoBrush );
            const auto x = box.left();
            const auto y = box.top();
            const auto w = box.width();
            if ( partial ) {
                painter->drawLine( QPointF( x + w * 0.28, y + w * 0.5 ),
                                   QPointF( x + w * 0.72, y + w * 0.5 ) );
            }
            else {
                QPainterPath mark;
                mark.moveTo( x + w * 0.25, y + w * 0.52 );
                mark.lineTo( x + w * 0.43, y + w * 0.70 );
                mark.lineTo( x + w * 0.76, y + w * 0.33 );
                painter->drawPath( mark );
            }
        }
        else {
            painter->setPen( enabled ? QColor( 0x86, 0x86, 0x86 ) : QColor( 0xc5, 0xc5, 0xc5 ) );
            painter->setBrush( QColor( 0xf3, 0xf3, 0xf3 ) );
            painter->drawRoundedRect( box.adjusted( 0.5, 0.5, -0.5, -0.5 ), 3, 3 );
        }
        painter->restore();
    }
};

} // namespace

QStringList StyleManager::availableStyles()
{
    QStringList styles;
#ifdef Q_OS_WIN
    styles << VistaKey;
    styles << WindowsKey;
    styles << FusionKey;
#else
    styles << QStyleFactory::keys();
#endif

    auto removedStyles = std::remove_if( styles.begin(), styles.end(), []( const QString& style ) {
        return style.startsWith( Gtk2Key, Qt::CaseInsensitive )
               || style.startsWith( Bb10Key, Qt::CaseInsensitive );
    } );

    styles.erase( removedStyles, styles.end() );

    styles << DarkStyleKey;
    styles << LightStyleKey;

#ifndef Q_OS_MACOS
    styles << DarkWindowsStyleKey;
#endif

    std::sort( styles.begin(), styles.end(), []( const auto& lhs, const auto& rhs ) {
        return lhs.compare( rhs, Qt::CaseInsensitive ) < 0;
    } );

    return styles;
}

QString StyleManager::defaultPlatformStyle()
{
#if defined( Q_OS_WIN )
    return VistaKey;
#elif defined( Q_OS_MACOS )
    return MacintoshKey;
#else
    return FusionKey;
#endif
}

void StyleManager::applyStyle( const QString& style )
{
    LOG_INFO << "Setting style to " << style;

    if ( style == DarkStyleKey || style == DarkWindowsStyleKey ) {
        const auto palette = Configuration::get().darkPalette();

        QPalette darkPalette;
        darkPalette.setColor( QPalette::Window, QColor( palette.at( "Window" ) ) );
        darkPalette.setColor( QPalette::WindowText, QColor( palette.at( "WindowText" ) ) );
        darkPalette.setColor( QPalette::Base, QColor( palette.at( "Base" ) ) );
        darkPalette.setColor( QPalette::AlternateBase, QColor( palette.at( "AlternateBase" ) ) );
        // Tool tips follow the window colours (the saved blue ToolTipBase of klogg is ignored)
        darkPalette.setColor( QPalette::ToolTipBase, QColor( palette.at( "Window" ) ).lighter( 130 ) );
        darkPalette.setColor( QPalette::ToolTipText, QColor( palette.at( "WindowText" ) ) );
        darkPalette.setColor( QPalette::Text, QColor( palette.at( "Text" ) ) );
        darkPalette.setColor( QPalette::Button, QColor( palette.at( "Button" ) ) );
        darkPalette.setColor( QPalette::ButtonText, QColor( palette.at( "ButtonText" ) ) );
        darkPalette.setColor( QPalette::Link, QColor( palette.at( "Link" ) ) );
        darkPalette.setColor( QPalette::Highlight, QColor( palette.at( "Highlight" ) ) );
        darkPalette.setColor( QPalette::HighlightedText,
                              QColor( palette.at( "HighlightedText" ) ) );

        darkPalette.setColor( QPalette::Active, QPalette::Button,
                              QColor( palette.at( "ActiveButton" ) ) );
        darkPalette.setColor( QPalette::Disabled, QPalette::ButtonText,
                              QColor( palette.at( "DisabledButtonText" ) ) );
        darkPalette.setColor( QPalette::Disabled, QPalette::WindowText,
                              QColor( palette.at( "DisabledWindowText" ) ) );
        darkPalette.setColor( QPalette::Disabled, QPalette::Text,
                              QColor( palette.at( "DisabledText" ) ) );
        darkPalette.setColor( QPalette::Disabled, QPalette::Light,
                              QColor( palette.at( "DisabledLight" ) ) );

        if ( style == DarkWindowsStyleKey ) {
            qApp->setStyle( QStyleFactory::create( WindowsKey ) );
        }
        else {
            qApp->setStyle( QStyleFactory::create( FusionKey ) );
        }

        qApp->setPalette( darkPalette );
        QToolTip::setPalette( darkPalette );
#if QT_VERSION >= QT_VERSION_CHECK( 6, 8, 0 )
        // Native parts (title bar, dialogs) follow the palette
        qApp->styleHints()->setColorScheme( Qt::ColorScheme::Dark );
#endif
    }
    else if ( style == LightStyleKey ) {
#if QT_VERSION >= QT_VERSION_CHECK( 6, 8, 0 )
        qApp->styleHints()->setColorScheme( Qt::ColorScheme::Light );
#endif
        qApp->setStyle( new LightStyle );
        const auto lightPalette = qApp->style()->standardPalette();
        qApp->setPalette( lightPalette );
        QToolTip::setPalette( lightPalette );
    }
    else {
        qApp->setStyle( style );
        qApp->setStyleSheet( "" );
    }
}