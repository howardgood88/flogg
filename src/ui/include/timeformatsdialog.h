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

#ifndef FLOGG_TIMEFORMATSDIALOG_H
#define FLOGG_TIMEFORMATSDIALOG_H

#include <QDialog>
#include <QStringList>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

// The time formats flogg reads: the built-in ones with examples, and the formats of the
// user, written like "dd.MM.yyyy HH:mm:ss.f", tried on a line as they are typed. The
// formats of the user are kept in the settings; Cancel gives back the previous ones.
class TimeFormatsDialog : public QDialog {
    Q_OBJECT
  public:
    // `sample` fills the line to try, e.g. the first selected line
    explicit TimeFormatsDialog( const QString& sample, QWidget* parent = nullptr );

    QStringList formats() const;
    void setFormats( const QStringList& formats );
    // What the line to try shows
    QString result() const;
    void setSample( const QString& line );

    void accept() override;
    void reject() override;

  Q_SIGNALS:
    void formatsChanged();

  private:
    void apply();
    void updateButtons();

    QStringList previous_;
    QListWidget* list_ = nullptr;
    QPushButton* remove_ = nullptr;
    QLineEdit* sample_ = nullptr;
    QLabel* result_ = nullptr;
    QLabel* problem_ = nullptr;
};

#endif
