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

#ifndef KLOGG_EDITVIEW_H
#define KLOGG_EDITVIEW_H

#include <QFuture>
#include <QWidget>

#include <atomic>
#include <optional>
#include <tuple>
#include <memory>

// Its header uses the keywords of Qt, flogg is built without them
#ifdef QT_NO_KEYWORDS
#define signals Q_SIGNALS
#define slots Q_SLOTS
#include "ScintillaEditBase.h"
#undef signals
#undef slots
#else
#include "ScintillaEditBase.h"
#endif
#include "editdocument.h"

class LogData;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTimer;
class QToolButton;

namespace Scintilla {
class ILoader;
}

// The editor: Scintilla, keeping the keys of the text (letters, edition, moves) from the
// shortcuts of the window, and asking for Ctrl+S
class EditorWidget : public ScintillaEditBase {
    Q_OBJECT

  public:
    using ScintillaEditBase::ScintillaEditBase;

  Q_SIGNALS:
    void saveRequested();

  protected:
    bool event( QEvent* event ) override;
    void keyPressEvent( QKeyEvent* event ) override;
};

// Edit mode of a tab: the whole file in a text editor (Scintilla, as in Notepad++), loaded
// in the background when the mode is entered and freed when it is left. The other modes
// keep reading the file on disk, which changes when the edits are saved.
class EditView : public QWidget {
    Q_OBJECT

  public:
    explicit EditView( std::shared_ptr<LogData> logData, QWidget* parent = nullptr );
    ~EditView() override;

    // Loads the file of the log data, the caret on the line (from 0)
    void start( qint64 line );

    bool isLoaded() const
    {
        return loaded_;
    }
    // Loading or saving
    bool isBusy() const;
    bool isModified() const;
    bool isReadOnly() const
    {
        return readOnly_;
    }

    // Writes the file, in the background; false when it can not start
    bool save();
    // Asks to save the changes, if any, before the view is left. False to stay.
    bool confirmClose();

    void showFindBar();
    void setFindText( const QString& text, bool caseSensitive = false, bool regex = false );
    void findNext();
    void findPrevious();
    void replaceNext( const QString& replacement );
    int replaceAll( const QString& replacement );
    QString findStatus() const;
    void goToLine( qint64 line );

    // For the tests
    EditorWidget* editor() const
    {
        return editor_;
    }
    QString message() const;
    QString text() const;
    const edit::FileFormat& fileFormat() const
    {
        return format_;
    }
    LogData* logData() const
    {
        return logData_.get();
    }
    // No dialog when the file is larger than the memory (tests)
    static void setAskBeforeLoading( bool ask );

  Q_SIGNALS:
    void modifiedChanged( bool modified );
    // The file was written: the other modes read it again
    void saved();
    void loaded();

  private:
    void setupEditor();
    void loadFinished( bool ok, const QString& error );
    void saveFinished( bool ok, const QString& error );
    void updateProgress();
    void showMessage( const QString& text, bool warning = false );
    void fileChangedOnDisk();
    void reloadFile();
    void saveAs();
    void find( bool backward );
    void updateHighlights();
    void setFindStatus( const QString& status, bool warning = false );
    int searchFlags() const;
    bool documentLarge() const;

    std::shared_ptr<LogData> logData_;
    QString fileName_;
    qint64 startLine_ = 0;
    edit::FileFormat format_;
    bool loaded_ = false;
    bool readOnly_ = false;
    bool saving_ = false;
    // The next change of the file on disk is the save
    bool ownChange_ = false;

    QLabel* messageLabel_;
    QPushButton* saveButton_;
    QPushButton* saveAsButton_;
    QPushButton* reloadButton_;
    QStackedWidget* pages_;
    QLabel* placeholder_;
    EditorWidget* editor_;

    QWidget* findBar_;
    QLineEdit* findEdit_;
    QLineEdit* replaceEdit_;
    QToolButton* caseButton_;
    QToolButton* regexButton_;
    QLabel* findStatus_;

    // Background loading and saving
    struct Job {
        std::atomic_bool cancelled{ false };
        std::atomic_int progress{ 0 };
        Scintilla::ILoader* loader = nullptr;
        bool ok = false;
        QString error;
        edit::FileFormat format;
    };
    std::shared_ptr<Job> job_;
    // What the matches were highlighted for
    bool updatingHighlights_ = false;
    std::optional<std::tuple<QByteArray, int, Scintilla::sptr_t, Scintilla::sptr_t,
                             Scintilla::sptr_t, Scintilla::sptr_t, Scintilla::sptr_t>>
        highlighted_;
    QFuture<void> future_;
    QTimer* progressTimer_;
};

#endif
