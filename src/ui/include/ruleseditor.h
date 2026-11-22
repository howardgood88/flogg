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

#ifndef FLOGG_RULESEDITOR_H
#define FLOGG_RULESEDITOR_H

#include <vector>

#include <QDialog>

#include "loganalysis.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

// The rules of a rule file edited in a form, no JSON to write: a list of the rules on the
// left, the rule selected on the right (kind, patterns one per line, time, count...), each
// rule checked as it is typed. Saved to the file, then used by the Rules page.
class RulesEditor : public QDialog {
    Q_OBJECT
  public:
    // An empty file name starts a new rule file, written where Save asks
    RulesEditor( const QString& fileName, QWidget* parent = nullptr );

    // A new rule matching the text (escaped), selected for its name to be typed
    void addRuleFor( const QString& text );

    const std::vector<analysis::Rule>& rules() const
    {
        return rules_;
    }
    QString fileName() const
    {
        return fileName_;
    }
    // Writes the rules, false (and a message) when one can not be used
    bool save( const QString& fileName );

    // The note as HTML: the text escaped, the web addresses as links
    static QString noteHtml( const QString& note );

  Q_SIGNALS:
    // Saved, to be checked on the current tab when `check` is set
    void saved( const QString& fileName, bool check );

  private:
    void addRule( analysis::Rule rule );
    void showRule( int row );
    void storeRule();
    void updateList();
    void updateForm();
    void move( int delta );
    bool saveAs();

    QString fileName_;
    std::vector<analysis::Rule> rules_;
    int current_ = -1;
    bool loading_ = false;

    QListWidget* list_ = nullptr;
    QPushButton* remove_ = nullptr;
    QPushButton* duplicate_ = nullptr;
    QPushButton* up_ = nullptr;
    QPushButton* down_ = nullptr;
    QWidget* form_ = nullptr;
    QLineEdit* name_ = nullptr;
    QLineEdit* description_ = nullptr;
    QComboBox* type_ = nullptr;
    QLabel* typeHint_ = nullptr;
    QComboBox* severity_ = nullptr;
    QPlainTextEdit* patterns_ = nullptr;
    QLabel* patternsHint_ = nullptr;
    QCheckBox* caseSensitive_ = nullptr;
    QCheckBox* within_ = nullptr;
    QDoubleSpinBox* seconds_ = nullptr;
    QSpinBox* count_ = nullptr;
    QLabel* countLabel_ = nullptr;
    QPlainTextEdit* note_ = nullptr;
    QLabel* notePreview_ = nullptr;
    QLabel* problem_ = nullptr;
    QLabel* file_ = nullptr;
};

#endif
