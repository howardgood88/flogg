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

#include <array>
#include <vector>

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

// Scroll bars with a rounded handle over the groove of the base style, in a colour of each
// style: the handle of the base styles is often close to the groove (the button colour)
class RoundedScrollBarStyle : public QProxyStyle {
  public:
    using QProxyStyle::QProxyStyle;

    void drawComplexControl( ComplexControl control, const QStyleOptionComplex* option,
                             QPainter* painter, const QWidget* widget ) const override
    {
        const auto* scrollBar = qstyleoption_cast<const QStyleOptionSlider*>( option );
        if ( control != CC_ScrollBar || !scrollBar
             || !( scrollBar->subControls & SC_ScrollBarSlider ) ) {
            QProxyStyle::drawComplexControl( control, option, painter, widget );
            return;
        }

        const auto handle = subControlRect( control, option, SC_ScrollBarSlider, widget );

        // The groove under the handle too: styles drawing it in pages (Windows, Windows
        // Vista) paint nothing there, black on Windows. The window colour first in case a
        // style does not ask its proxy for the page rectangle.
        painter->fillRect( handle, option->palette.window() );
        auto withoutHandle = *scrollBar;
        withoutHandle.subControls &= ~SC_ScrollBarSlider;
        withoutHandle.subControls |= SC_ScrollBarSubPage;
        subPageWithHandle_ = true;
        QProxyStyle::drawComplexControl( control, &withoutHandle, painter, widget );
        subPageWithHandle_ = false;

        if ( handle.isEmpty() || !option->state.testFlag( State_Enabled ) ) {
            return;
        }
        const auto active = scrollBar->activeSubControls & SC_ScrollBarSlider;
        const auto state = active && option->state.testFlag( State_Sunken ) ? HandleState::Pressed
                           : active && option->state.testFlag( State_MouseOver )
                               ? HandleState::Hovered
                               : HandleState::Normal;

        // A small margin to the groove, along the bar the full length
        const auto margin = scrollBar->orientation == Qt::Horizontal
                                ? QMarginsF( 0.5, 2.5, 0.5, 2.5 )
                                : QMarginsF( 2.5, 0.5, 2.5, 0.5 );
        const auto rect = QRectF( handle ) - margin;
        const auto radius = std::min( rect.width(), rect.height() ) / 2;
        painter->save();
        painter->setRenderHint( QPainter::Antialiasing );
        painter->setPen( Qt::NoPen );
        painter->setBrush( handleColor( option->palette, state ) );
        painter->drawRoundedRect( rect, radius, radius );
        painter->restore();
    }

    QRect subControlRect( ComplexControl control, const QStyleOptionComplex* option,
                          SubControl subControl, const QWidget* widget ) const override
    {
        auto rect = QProxyStyle::subControlRect( control, option, subControl, widget );
        if ( subPageWithHandle_ && control == CC_ScrollBar && subControl == SC_ScrollBarSubPage ) {
            rect |= QProxyStyle::subControlRect( control, option, SC_ScrollBarSlider, widget );
        }
        return rect;
    }

  private:
    // Only while the groove is drawn, on the GUI thread
    mutable bool subPageWithHandle_ = false;

  protected:
    enum class HandleState { Normal, Hovered, Pressed };

    // By default between the window and its text colour, darker or lighter on use
    virtual QColor handleColor( const QPalette& palette, HandleState state ) const
    {
        return mix( palette.color( QPalette::Window ), palette.color( QPalette::WindowText ),
                    state == HandleState::Pressed   ? 60
                    : state == HandleState::Hovered ? 45
                                                    : 30 );
    }

    // From the first colour towards the second one, weight in percent
    static QColor mix( const QColor& from, const QColor& to, int weight )
    {
        const auto channel = [ weight ]( int a, int b ) { return a + ( b - a ) * weight / 100; };
        return QColor( channel( from.red(), to.red() ), channel( from.green(), to.green() ),
                       channel( from.blue(), to.blue() ) );
    }
};

// Fusion drawn like the light Windows style: blue filled check boxes, flat white headers
// and light blue checked tool buttons. Colours taken from the Windows build.
class LightStyle : public RoundedScrollBarStyle {
  public:
    LightStyle()
        : RoundedScrollBarStyle( QStyleFactory::create( StyleManager::FusionKey ) )
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
                // Rounded like the buttons that are not pressed
                painter->setRenderHint( QPainter::Antialiasing );
                painter->setPen( pressed ? QColor( 0x99, 0xc9, 0xef ) : QColor( 0xcc, 0xe4, 0xf7 ) );
                painter->setBrush( pressed ? QColor( 0xcb, 0xe3, 0xf7 ) : QColor( 0xe5, 0xf1, 0xfb ) );
                painter->drawRoundedRect( QRectF( option->rect ).adjusted( 0.5, 0.5, -0.5, -0.5 ),
                                          3, 3 );
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

  protected:
    // Windows 11 light greys
    QColor handleColor( const QPalette&, HandleState state ) const override
    {
        return state == HandleState::Pressed   ? QColor( 0x86, 0x86, 0x86 )
               : state == HandleState::Hovered ? QColor( 0xa3, 0xa3, 0xa3 )
                                               : QColor( 0xc2, 0xc2, 0xc2 );
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

// Fusion (or Windows) with the pressed and hovered buttons of the Light style in dark colours:
// checked tool buttons are clearly on
class DarkStyle : public RoundedScrollBarStyle {
  public:
    DarkStyle( QStyle* base, bool windows )
        : RoundedScrollBarStyle( base )
        , windows_( windows )
    {
    }

    void drawPrimitive( PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                        const QWidget* widget ) const override
    {
        const auto enabled = option->state.testFlag( State_Enabled );
        const auto pressed = option->state & ( State_On | State_Sunken );
        const auto hovered = option->state.testFlag( State_MouseOver );
        if ( ( element == PE_PanelButtonCommand || element == PE_PanelButtonTool ) && enabled
             && ( pressed || hovered ) ) {
            // Rounded like the buttons that are not pressed
            painter->save();
            painter->setRenderHint( QPainter::Antialiasing );
            painter->setPen( pressed ? QColor( 0x4a, 0x90, 0xc8 ) : QColor( 0x5a, 0x64, 0x70 ) );
            painter->setBrush( pressed ? QColor( 0x2b, 0x4a, 0x63 ) : QColor( 0x3e, 0x44, 0x4b ) );
            painter->drawRoundedRect( QRectF( option->rect ).adjusted( 0.5, 0.5, -0.5, -0.5 ), 3,
                                      3 );
            painter->restore();
            return;
        }
        RoundedScrollBarStyle::drawPrimitive( element, option, painter, widget );
    }

  protected:
    QColor handleColor( const QPalette& palette, HandleState state ) const override
    {
        if ( windows_ ) {
            // Windows 11 dark: neutral greys, brighter than the Fusion ones
            return state == HandleState::Pressed   ? QColor( 0xb5, 0xb5, 0xb5 )
                   : state == HandleState::Hovered ? QColor( 0x9a, 0x9a, 0x9a )
                                                   : QColor( 0x7a, 0x7a, 0x7a );
        }
        // Between the window and its text colour, follows the dark palette of the options
        return mix( palette.color( QPalette::Window ), palette.color( QPalette::WindowText ),
                    state == HandleState::Pressed   ? 70
                    : state == HandleState::Hovered ? 55
                                                    : 40 );
    }

  private:
    bool windows_;
};

// The platform styles with the rounded scroll bar handle, in a colour of their palette (the
// system one, light or dark)
class PlatformStyle : public RoundedScrollBarStyle {
  public:
    using RoundedScrollBarStyle::RoundedScrollBarStyle;
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

namespace {

// Classes the platform theme gives their own palette (macOS: the text of line edits and
// item views is not white in dark mode), it would win over the one of the Dark and Light styles
constexpr std::array<const char*, 17> ThemeClasses{
    "QToolButton", "QAbstractButton", "QCheckBox", "QRadioButton", "QHeaderView",
    "QAbstractItemView", "QMessageBoxLabel", "QTabBar", "QLabel", "QGroupBox", "QMenu",
    "QMenuBar", "QTextEdit", "QTextControl", "QLineEdit", "QComboBox", "QAbstractSpinBox" };

// The palettes of the platform, taken before a style changes them
const std::vector<QPalette>& themePalettes()
{
    static const auto palettes = [] {
        std::vector<QPalette> saved;
        for ( const auto* className : ThemeClasses ) {
            saved.push_back( QApplication::palette( className ) );
        }
        return saved;
    }();
    return palettes;
}

// The selection colours of the options, in the active and the inactive windows
QPalette withSelection( QPalette palette )
{
    for ( const auto group : { QPalette::Active, QPalette::Inactive } ) {
        palette.setColor( group, QPalette::Highlight, StyleManager::selectionBackColor() );
        palette.setColor( group, QPalette::HighlightedText, StyleManager::selectionTextColor() );
    }
    return palette;
}

void setThemePalettes( const QPalette& palette )
{
    for ( const auto* className : ThemeClasses ) {
        QApplication::setPalette( palette, className );
    }
}

void restoreThemePalettes()
{
    const auto& palettes = themePalettes();
    for ( size_t i = 0; i < ThemeClasses.size(); ++i ) {
        QApplication::setPalette( withSelection( palettes[ i ] ), ThemeClasses[ i ] );
    }
}

// The log views and the tree of the filter panel (the only QTreeWidget of flogg): their text
// colour may differ from the other widgets. Set last, an exact class name wins.
void setViewsPalette( const QPalette& palette )
{
    for ( const auto* className : { "LogMainView", "FilteredView", "QTreeWidget" } ) {
        QApplication::setPalette( palette, className );
    }
}

} // namespace

QColor StyleManager::selectionBackColor()
{
    return Configuration::get().selectionBackColor();
}

QColor StyleManager::selectionTextColor()
{
    return Configuration::get().selectionTextColor();
}

void StyleManager::applyStyle( const QString& style )
{
    LOG_INFO << "Setting style to " << style;
    // Before the application palette changes: classes without their own palette use it
    themePalettes();

    if ( style == DarkStyleKey || style == DarkWindowsStyleKey ) {
#if QT_VERSION >= QT_VERSION_CHECK( 6, 8, 0 )
        // Native parts (title bar, dialogs) follow the palette. First: the change of the
        // platform theme resets the palettes of the widget classes.
        qApp->styleHints()->setColorScheme( Qt::ColorScheme::Dark );
#endif
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

        qApp->setStyle( new DarkStyle(
            QStyleFactory::create( style == DarkWindowsStyleKey ? WindowsKey : FusionKey ),
            style == DarkWindowsStyleKey ) );
        darkPalette = withSelection( darkPalette );

        qApp->setPalette( darkPalette );
        QToolTip::setPalette( darkPalette );
        setThemePalettes( darkPalette );
        auto viewsPalette = darkPalette;
        viewsPalette.setColor( QPalette::Text, QColor( palette.at( "LogText" ) ) );
        setViewsPalette( viewsPalette );
    }
    else if ( style == LightStyleKey ) {
#if QT_VERSION >= QT_VERSION_CHECK( 6, 8, 0 )
        qApp->styleHints()->setColorScheme( Qt::ColorScheme::Light );
#endif
        qApp->setStyle( new LightStyle );
        const auto lightPalette = withSelection( qApp->style()->standardPalette() );
        qApp->setPalette( lightPalette );
        QToolTip::setPalette( lightPalette );
        setThemePalettes( lightPalette );
        setViewsPalette( lightPalette );
    }
    else {
        // Also undoes a Dark or Light style chosen while flogg runs
#if QT_VERSION >= QT_VERSION_CHECK( 6, 8, 0 )
        qApp->styleHints()->unsetColorScheme();
#endif
        // The native scroll bars of macOS stay
        auto* platformStyle = QStyleFactory::create( style );
        if ( platformStyle && style.compare( MacintoshKey, Qt::CaseInsensitive ) != 0 ) {
            qApp->setStyle( new PlatformStyle( platformStyle ) );
        }
        else {
            delete platformStyle;
            qApp->setStyle( style );
        }
        qApp->setStyleSheet( "" );
        // An empty palette gives back the one of the platform
        qApp->setPalette( QPalette() );
        qApp->setPalette( withSelection( QApplication::palette() ) );
        QToolTip::setPalette( QApplication::palette( "QTipLabel" ) );
        restoreThemePalettes();
        setViewsPalette( QApplication::palette() );
    }
}