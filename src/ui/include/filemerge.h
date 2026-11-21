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

#ifndef FLOGG_FILEMERGE_H
#define FLOGG_FILEMERGE_H

#include <functional>

#include <QDialog>
#include <QFutureWatcher>
#include <QString>
#include <QStringList>

#include "atomicflag.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;

namespace filemerge {

struct Options {
    // A line "==> name <==" before each file, like `tail` with several files
    bool fileNameLines = false;
};

// Writes the files one after the other into `output`, as bytes, in 1 MB blocks: a line end
// is added after a file that does not end with one (CRLF when the file uses them), the
// UTF-8 byte order mark of the files after the first is left out. The output is written
// to a temporary file renamed at the end, so a failure leaves nothing half written.
// Returns an error message, empty on success.
QString merge( const QStringList& inputs, const QString& output, const Options& options,
               AtomicFlag& cancel, const std::function<void( int percent )>& progress = {} );

// "merged.log" in the folder of the first file, or merged-2.log... when it exists
QString defaultOutput( const QStringList& inputs );

} // namespace filemerge

// The files to merge in the order of the file panel, to be ordered again (drag, buttons),
// and where to write them; the result is opened in a tab.
class MergeFilesDialog : public QDialog {
    Q_OBJECT
  public:
    explicit MergeFilesDialog( const QStringList& files, QWidget* parent = nullptr );
    ~MergeFilesDialog() override;

    QStringList files() const;
    QString output() const;
    void setOutput( const QString& output );
    // Starts the merge, as the button does
    void merge();
    bool isRunning() const
    {
        return watcher_ != nullptr;
    }

  Q_SIGNALS:
    void merged( const QString& output );

  private:
    void addFiles();
    void moveSelected( int delta );
    void updateButtons();

    QListWidget* list_ = nullptr;
    QPushButton* up_ = nullptr;
    QPushButton* down_ = nullptr;
    QPushButton* remove_ = nullptr;
    QLineEdit* output_ = nullptr;
    QCheckBox* fileNames_ = nullptr;
    QLabel* message_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton* mergeButton_ = nullptr;
    std::shared_ptr<AtomicFlag> cancel_;
    QFutureWatcher<QString>* watcher_ = nullptr;
};

#endif
