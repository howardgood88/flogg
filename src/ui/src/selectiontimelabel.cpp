/*
 * Copyright (C) 2026 flogg contributors
 *
 * This file is part of flogg.
 *
 * flogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * flogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with flogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "selectiontimelabel.h"

#include <QContextMenuEvent>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QStyleOptionToolButton>

namespace {
constexpr int HorizontalPadding = 8;
constexpr int VerticalPadding = 2;
} // namespace

void SelectionTimeLabel::updateCheckedColors()
{
    // Let the style draw a checked tool button once and read its colours back: the middle
    // is the fill, the middle of the top edge the border. Styles that draw no panel (or a
    // gradient we cannot read) leave the pixels transparent, the badge then falls back on
    // a tint of the highlight colour.
    constexpr int Size = 24;
    QImage image( Size, Size, QImage::Format_ARGB32_Premultiplied );
    image.fill( Qt::transparent );
    {
        QPainter painter( &image );
        QStyleOptionToolButton option;
        option.initFrom( this );
        option.rect = QRect( 0, 0, Size, Size );
        option.state = QStyle::State_Enabled | QStyle::State_On | QStyle::State_AutoRaise;
        style()->drawPrimitive( QStyle::PE_PanelButtonTool, &option, &painter, this );
    }
    const auto fill = image.pixelColor( Size / 2, Size / 2 );
    const auto border = image.pixelColor( Size / 2, 0 );
    checkedFill_ = fill.alpha() == 255 ? fill : QColor();
    checkedBorder_ = border.alpha() == 255 ? border : QColor();
    checkedColorsValid_ = true;
}

SelectionTimeLabel::SelectionTimeLabel( QWidget* parent )
    : QWidget( parent )
{
    setSizePolicy( QSizePolicy::Fixed, QSizePolicy::Preferred );
}

void SelectionTimeLabel::setTime( const QString& text, const QString& details, bool fromReference )
{
    text_ = text;
    details_ = details;
    fromReference_ = fromReference;

    auto tip = details;
    if ( !text.isEmpty() ) {
        if ( !tip.isEmpty() ) {
            tip += "\n\n";
        }
        tip += tr( "Right click to copy it or to choose the time stamps compared" );
    }
    setToolTip( tip );
    updateGeometry();

    update();
}

QString SelectionTimeLabel::durationText() const
{
    auto text = text_;
    for ( const auto* prefix : { "Δ ref:", "Δ:" } ) {
        if ( text.startsWith( QString::fromUtf8( prefix ) ) ) {
            return text.mid( QString::fromUtf8( prefix ).size() ).trimmed();
        }
    }
    for ( const auto* prefix : { "Δ ref", "Δ" } ) {
        if ( text.startsWith( QString::fromUtf8( prefix ) ) ) {
            text = text.mid( QString::fromUtf8( prefix ).size() );
            break;
        }
    }
    if ( text.endsWith( "⚠" ) ) {
        text.chop( 1 );
    }
    return text.trimmed();
}

SelectionTimeLabel::Kind SelectionTimeLabel::kind() const
{
    if ( text_.startsWith( QString::fromUtf8( "Δ:" ) )
         || text_.startsWith( QString::fromUtf8( "Δ ref:" ) ) ) {
        return Kind::NoTime;
    }
    return text_.endsWith( "⚠" ) ? Kind::Warning : Kind::Time;
}

QSize SelectionTimeLabel::sizeHint() const
{
    auto bold = font();
    bold.setBold( true );
    const QFontMetrics metrics( bold );
    return { metrics.horizontalAdvance( text_ ) + 2 * HorizontalPadding + 2,
             metrics.height() + 2 * VerticalPadding };
}

QSize SelectionTimeLabel::minimumSizeHint() const
{
    return sizeHint();
}

void SelectionTimeLabel::paintEvent( QPaintEvent* )
{
    if ( text_.isEmpty() ) {
        return;
    }
    QPainter painter( this );
    painter.setRenderHint( QPainter::Antialiasing );

    const auto& colors = palette();
    QColor accent = colors.color( QPalette::Highlight );
    if ( kind() == Kind::Warning ) {
        accent = QColor( 0xe0, 0x8a, 0x1e );
    }
    else if ( kind() == Kind::NoTime ) {
        accent = colors.color( QPalette::PlaceholderText );
    }

    if ( !checkedColorsValid_ ) {
        updateCheckedColors();
    }
    // A time at rest looks like the checked tool buttons next to it, the other kinds (and
    // styles without such buttons) a light tint of their accent.
    // From the time reference line: in the orange of its frame in the views
    const auto reference = fromReference_ && kind() != Kind::NoTime;
    if ( reference ) {
        accent = QColor( 0xff, 0x98, 0x00 );
    }
    const auto likeChecked = kind() == Kind::Time && !reference && checkedFill_.isValid();
    auto rest = accent;
    rest.setAlphaF( reference ? 0.3f : 0.18f );
    if ( likeChecked ) {
        rest = checkedFill_;
    }
    const auto& background = rest;
    const auto foreground = colors.color( QPalette::WindowText );

    const auto box = QRectF( rect() ).adjusted( 0.5, 1.5, -0.5, -1.5 );
    QPainterPath path;
    path.addRoundedRect( box, box.height() / 2, box.height() / 2 );
    painter.fillPath( path, background );
    auto border = accent;
    border.setAlphaF( reference ? 1.0f : 0.55f );
    if ( likeChecked && checkedBorder_.isValid() ) {
        border = checkedBorder_;
    }
    painter.setPen( QPen( border, 1 ) );
    painter.drawPath( path );

    auto bold = font();
    bold.setBold( true );
    painter.setFont( bold );
    painter.setPen( foreground );
    painter.drawText( rect(), Qt::AlignCenter, text_ );
}

void SelectionTimeLabel::contextMenuEvent( QContextMenuEvent* event )
{
    if ( !text_.isEmpty() ) {
        Q_EMIT menuRequested( event->globalPos() );
    }
    event->accept();
}

void SelectionTimeLabel::changeEvent( QEvent* event )
{
    if ( event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange ) {
        checkedColorsValid_ = false;
    }
    if ( event->type() == QEvent::PaletteChange || event->type() == QEvent::FontChange
         || event->type() == QEvent::StyleChange ) {
        updateGeometry();
        update();
    }
    QWidget::changeEvent( event );
}
