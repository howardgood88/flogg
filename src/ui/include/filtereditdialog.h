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

#ifndef KLOGG_FILTEREDITDIALOG_H
#define KLOGG_FILTEREDITDIALOG_H

#include <QDialog>

#include "filtertree.h"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QToolButton;

class FilterEditDialog : public QDialog {
    Q_OBJECT

  public:
    FilterEditDialog( const FilterNode& filter, QWidget* parent = nullptr );

    // The edited filter, id and check state are preserved
    FilterNode filter() const;

  private:
    void chooseColor( QColor& color, QToolButton* button );
    void updateColorButton( QToolButton* button, const QColor& color );
    void updatePreview();

  private:
    FilterNode filter_;

    QLineEdit* patternEdit_;
    QLineEdit* descriptionEdit_;
    QCheckBox* regexCheck_;
    QCheckBox* caseCheck_;
    QCheckBox* excludeCheck_;
    QComboBox* colorModeCombo_;
    QToolButton* foreButton_;
    QToolButton* backButton_;
    QLabel* preview_;
    QLabel* errorLabel_;
    QDialogButtonBox* buttons_;
};

#endif
