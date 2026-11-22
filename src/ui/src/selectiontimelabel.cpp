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
#include <QPainter>
#include <QPainterPath>
#include <QVariantAnimation>

namespace {
constexpr int HorizontalPadding = 8;
constexpr int VerticalPadding = 2;
constexpr int FlashMs = 1600;

QColor mix( const QColor& a, const QColor& b, double t )
{
    return QColor::fromRgbF( static_cast<float>( a.redF() + ( b.redF() - a.redF() ) * t ),
                             static_cast<float>( a.greenF() + ( b.greenF() - a.greenF() ) * t ),
                             static_cast<float>( a.blueF() + ( b.blueF() - a.blueF() ) * t ),
                             static_cast<float>( a.alphaF() + ( b.alphaF() - a.alphaF() ) * t ) );
}
} // namespace

SelectionTimeLabel::SelectionTimeLabel( QWidget* parent )
    : QWidget( parent )
    , flash_( new QVariantAnimation( this ) )
{
    flash_->setStartValue( 1.0 );
    flash_->setEndValue( 0.0 );
    flash_->setDuration( FlashMs );
    flash_->setEasingCurve( QEasingCurve::InQuad );
    connect( flash_, &QVariantAnimation::valueChanged, this, [ this ]( const QVariant& value ) {
        flashLevel_ = value.toDouble();
        update();
    } );
    setSizePolicy( QSizePolicy::Fixed, QSizePolicy::Preferred );
}

void SelectionTimeLabel::setTime( const QString& text, const QString& details )
{
    const auto isNew = !text.isEmpty() && text != text_;
    text_ = text;
    details_ = details;

    auto tip = details;
    if ( !text.isEmpty() ) {
        if ( !tip.isEmpty() ) {
            tip += "\n\n";
        }
        tip += tr( "Right click to copy it or to choose the time stamps compared" );
    }
    setToolTip( tip );
    updateGeometry();

    if ( isNew ) {
        flash_->stop();
        flashLevel_ = 1.0;
        flash_->start();
    }
    update();
}

QString SelectionTimeLabel::durationText() const
{
    auto text = text_;
    if ( text.startsWith( "Δ:" ) ) {
        return text.mid( 2 ).trimmed();
    }
    if ( text.startsWith( "Δ" ) ) {
        text = text.mid( 1 );
    }
    if ( text.endsWith( "⚠" ) ) {
        text.chop( 1 );
    }
    return text.trimmed();
}

SelectionTimeLabel::Kind SelectionTimeLabel::kind() const
{
    if ( text_.startsWith( "Δ:" ) ) {
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

    // At rest a light tint of the accent, right after a change the accent itself
    auto rest = accent;
    rest.setAlphaF( 0.18f );
    const auto background = mix( rest, accent, flashLevel_ );
    const auto foreground = mix( colors.color( QPalette::WindowText ),
                                 colors.color( QPalette::HighlightedText ), flashLevel_ );

    const auto box = QRectF( rect() ).adjusted( 0.5, 1.5, -0.5, -1.5 );
    QPainterPath path;
    path.addRoundedRect( box, box.height() / 2, box.height() / 2 );
    painter.fillPath( path, background );
    auto border = accent;
    border.setAlphaF( 0.55f );
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
    if ( event->type() == QEvent::PaletteChange || event->type() == QEvent::FontChange ) {
        updateGeometry();
        update();
    }
    QWidget::changeEvent( event );
}
