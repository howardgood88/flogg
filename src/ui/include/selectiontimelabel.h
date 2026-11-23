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

#ifndef FLOGG_SELECTIONTIMELABEL_H
#define FLOGG_SELECTIONTIMELABEL_H

#include <QColor>
#include <QWidget>

// The time between the selected lines in the tool bar: a rounded badge (orange with a
// warning, grey when the time can not be worked out), shown at once without animation.
// The right click is left to the owner (copy, choice of the time stamps).
class SelectionTimeLabel : public QWidget {
    Q_OBJECT
  public:
    explicit SelectionTimeLabel( QWidget* parent = nullptr );

    // From the time reference line: in its orange, else like the checked tool buttons
    void setTime( const QString& text, const QString& details, bool fromReference = false );
    bool isFromReference() const
    {
        return fromReference_;
    }
    QString text() const
    {
        return text_;
    }
    QString details() const
    {
        return details_;
    }
    // "+1.234 s" out of "Δ +1.234 s ⚠", the message when there is no time
    QString durationText() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  Q_SIGNALS:
    void menuRequested( const QPoint& globalPos );

  protected:
    void paintEvent( QPaintEvent* event ) override;
    void contextMenuEvent( QContextMenuEvent* event ) override;
    void changeEvent( QEvent* event ) override;

  private:
    enum class Kind { Time, Warning, NoTime };
    Kind kind() const;
    // Fill and border of a checked tool button in the current style, so the badge at rest
    // looks like the checked buttons next to it. Worked out again when the style changes.
    void updateCheckedColors();

    QColor checkedFill_;
    QColor checkedBorder_;
    bool checkedColorsValid_ = false;

    QString text_;
    bool fromReference_ = false;
    QString details_;
};

#endif
