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

#ifndef FLOGG_FILECOMMANDS_H
#define FLOGG_FILECOMMANDS_H

#include <memory>
#include <optional>
#include <vector>

#include <QDialog>
#include <QPointer>
#include <QProcessEnvironment>
#include <QString>
#include <QStringDecoder>
#include <QStringList>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QTableWidget;
class QTextBrowser;
class QTimer;

// A command of the file panel menu, run by a shell on a file or a folder without a
// terminal; its output is shown in a window of flogg.
struct FileCommand {
    enum class Target { FilesAndFolders, Files, Folders };
    enum class Output { Always, OnFailure };

    QString name;
    QString command;
    Target target = Target::FilesAndFolders;
    Output output = Output::Always;

    bool appliesTo( bool isDir ) const
    {
        return target == Target::FilesAndFolders || ( isDir ? target == Target::Folders
                                                            : target == Target::Files );
    }
};

namespace filecommands {

// Kept in the settings of this computer only, never in the exported filters or rules:
// a shared file can not run anything here
std::vector<FileCommand> load();
void save( const std::vector<FileCommand>& commands );

// The shell set in the settings, empty for the default one
QString configuredShell();
void setConfiguredShell( const QString& shell );
// PowerShell on Windows, zsh on macOS, $SHELL (or /bin/sh) on Linux
QString defaultShell();
QString shell();

struct Invocation {
    QString program;
    QStringList arguments;
    // cmd.exe parses its command line itself, it is passed as it is (Windows only)
    QString nativeArguments;
};
// PowerShell and pwsh: -NoProfile -NonInteractive -ExecutionPolicy Bypass and the command
// encoded, with the output in UTF-8;
// cmd: /D /S /C; any other shell is a POSIX one: -l -c (a login shell, so that the PATH
// of the profile is there also when flogg was started from the Finder)
Invocation invocation( const QString& shell, const QString& command );

// FLOGG_PATH (native path), FLOGG_NAME, FLOGG_DIR (the folder it is in), FLOGG_ROOT
// (the root folder of the panel) added to the environment of flogg
QProcessEnvironment environment( const QString& path, const QString& root );
// The folder itself, the folder of a file
QString workingDirectory( const QString& path );

// How the commands run and the variables (HTML), in the syntax of the shell
QString variablesHelp( const QString& shell );

} // namespace filecommands

// The commands in a table, with the shell they run in
class FileCommandsDialog : public QDialog {
    Q_OBJECT

  public:
    explicit FileCommandsDialog( QWidget* parent = nullptr );

    std::vector<FileCommand> commands() const;
    QString shell() const;
    // Saves the commands and the shell
    void accept() override;

    // How the commands run and the variables, in a window next to the dialog
    void showHelp();

    // For the tests
    QTableWidget* table() const
    {
        return table_;
    }
    QDialog* helpWindow() const
    {
        return help_;
    }

  private:
    static QComboBox* makeTargetBox( QWidget* parent );
    static QComboBox* makeOutputBox( QWidget* parent );
    void addRow( const FileCommand& command );
    void moveRow( int delta );
    void updateButtons();
    void updateHelp();

    QTableWidget* table_ = nullptr;
    QLineEdit* shell_ = nullptr;
    QPointer<QDialog> help_;
    QTextBrowser* helpText_ = nullptr;
    QPushButton* remove_ = nullptr;
    QPushButton* up_ = nullptr;
    QPushButton* down_ = nullptr;
};

// Runs a command on the paths one after the other and shows what they print. Closing
// the window stops the command. The window deletes itself when closed.
class CommandOutputWindow : public QWidget {
    Q_OBJECT

  public:
    CommandOutputWindow( const FileCommand& command, const QStringList& paths,
                         const QString& root, const QString& shell, QWidget* parent );
    ~CommandOutputWindow() override;

    void start();
    void stop();
    bool isRunning() const;

    // For the tests
    QString outputText() const;
    int failures() const
    {
        return failures_;
    }

  Q_SIGNALS:
    void finished();

  protected:
    void closeEvent( QCloseEvent* event ) override;

  private:
    void runNext();
    void readOutput();
    void flushOutput();
    void appendText( const QString& text );
    void onProcessFinished( int exitCode, bool crashed );
    void onAllFinished();
    void updateTitle();

    FileCommand command_;
    QStringList paths_;
    QString root_;
    QString shell_;
    int next_ = 0;
    int failures_ = 0;
    bool stopped_ = false;
    bool done_ = false;

    QProcess* process_ = nullptr;
    std::optional<QStringDecoder> decoder_;
    QString pending_;
    qsizetype shownChars_ = 0;
    bool truncated_ = false;

    QPlainTextEdit* text_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QTimer* flushTimer_ = nullptr;
};

#endif
