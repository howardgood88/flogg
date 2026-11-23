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

#include "filecommands.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QStyle>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTextCursor>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>

#ifdef Q_OS_UNIX
#include <csignal>
#include <unistd.h>
#endif

#include "log.h"
#include "persistentinfo.h"

namespace {
constexpr auto CommandsKey = "fileTree/commands";
constexpr auto ShellKey = "fileTree/commandShell";
// What a window keeps of the output; past it the output is read and dropped
constexpr qsizetype MaxOutputChars = 2'000'000;
// The output is added to the window at most this often, not for each block read
constexpr int FlushIntervalMs = 100;

enum Column { NameColumn, CommandColumn, TargetColumn, OutputColumn, ColumnCount };

QString targetKey( FileCommand::Target target )
{
    switch ( target ) {
    case FileCommand::Target::Files:
        return "files";
    case FileCommand::Target::Folders:
        return "folders";
    case FileCommand::Target::FilesAndFolders:
        break;
    }
    return "both";
}

FileCommand::Target targetOf( const QString& key )
{
    if ( key == "files" ) {
        return FileCommand::Target::Files;
    }
    if ( key == "folders" ) {
        return FileCommand::Target::Folders;
    }
    return FileCommand::Target::FilesAndFolders;
}

QString shellName( const QString& shell )
{
    // Also a Windows path in the tests on macOS and Linux
    auto path = shell;
    return QFileInfo( path.replace( '\\', '/' ) ).baseName().toLower();
}

bool isPowerShell( const QString& shell )
{
    const auto name = shellName( shell );
    return name == "powershell" || name == "pwsh";
}

bool isCmd( const QString& shell )
{
    return shellName( shell ) == "cmd";
}

QString variable( const QString& shell, const QString& name )
{
    if ( isPowerShell( shell ) ) {
        return "$env:" + name;
    }
    if ( isCmd( shell ) ) {
        return "%" + name + "%";
    }
    return "\"$" + name + "\"";
}

#ifdef Q_OS_UNIX
// The shell is the leader of its own process group, so that stopping it also stops what
// it started
void killGroup( QProcess* process )
{
    const auto pid = process->processId();
    if ( pid > 0 ) {
        ::kill( -static_cast<pid_t>( pid ), SIGKILL );
    }
}
#endif
} // namespace

namespace filecommands {

std::vector<FileCommand> load()
{
    auto& settings = PersistentInfo::getSettings( app_settings{} );
    std::vector<FileCommand> commands;
    const auto size = settings.beginReadArray( CommandsKey );
    commands.reserve( static_cast<size_t>( size ) );
    for ( int i = 0; i < size; ++i ) {
        settings.setArrayIndex( i );
        FileCommand command;
        command.name = settings.value( "name" ).toString();
        command.command = settings.value( "command" ).toString();
        command.target = targetOf( settings.value( "target" ).toString() );
        command.output = settings.value( "output" ).toString() == "onFailure"
                             ? FileCommand::Output::OnFailure
                             : FileCommand::Output::Always;
        if ( !command.command.trimmed().isEmpty() ) {
            commands.push_back( command );
        }
    }
    settings.endArray();
    return commands;
}

void save( const std::vector<FileCommand>& commands )
{
    auto& settings = PersistentInfo::getSettings( app_settings{} );
    settings.remove( CommandsKey );
    settings.beginWriteArray( CommandsKey, static_cast<int>( commands.size() ) );
    for ( int i = 0; i < static_cast<int>( commands.size() ); ++i ) {
        const auto& command = commands.at( static_cast<size_t>( i ) );
        settings.setArrayIndex( i );
        settings.setValue( "name", command.name );
        settings.setValue( "command", command.command );
        settings.setValue( "target", targetKey( command.target ) );
        settings.setValue( "output", command.output == FileCommand::Output::OnFailure
                                         ? "onFailure"
                                         : "always" );
    }
    settings.endArray();
    settings.sync();
}

QString configuredShell()
{
    return PersistentInfo::getSettings( app_settings{} ).value( ShellKey ).toString().trimmed();
}

void setConfiguredShell( const QString& shell )
{
    auto& settings = PersistentInfo::getSettings( app_settings{} );
    const auto trimmed = shell.trimmed();
    if ( trimmed.isEmpty() || trimmed == defaultShell() ) {
        settings.remove( ShellKey );
    }
    else {
        settings.setValue( ShellKey, trimmed );
    }
    settings.sync();
}

QString defaultShell()
{
#if defined( Q_OS_WIN )
    return "powershell.exe";
#elif defined( Q_OS_MACOS )
    return "/bin/zsh";
#else
    const auto shell = qEnvironmentVariable( "SHELL" );
    return shell.isEmpty() ? QString( "/bin/sh" ) : shell;
#endif
}

QString shell()
{
    const auto configured = configuredShell();
    return configured.isEmpty() ? defaultShell() : configured;
}

Invocation invocation( const QString& shell, const QString& command )
{
    Invocation result;
    result.program = shell;
    if ( isPowerShell( shell ) ) {
        // Encoded, so no quote of the command is changed on the way; UTF-8 output
        const auto script = "[Console]::OutputEncoding = [System.Text.Encoding]::UTF8\n" + command;
        const auto utf16 = QByteArray( reinterpret_cast<const char*>( script.utf16() ),
                                       script.size() * 2 );
        // Bypass for this process only, so that a command can run a .ps1 script also where
        // the policy of the computer is Restricted (the default on Windows 10 and 11); a
        // policy set by the administrators (Group Policy) still applies
        result.arguments = QStringList{ "-NoProfile", "-NonInteractive", "-ExecutionPolicy",
                                        "Bypass", "-EncodedCommand",
                                        QString::fromLatin1( utf16.toBase64() ) };
    }
    else if ( isCmd( shell ) ) {
        result.nativeArguments = "/D /S /C \"" + command + "\"";
    }
    else {
        result.arguments = QStringList{ "-l", "-c", command };
    }
    return result;
}

QProcessEnvironment environment( const QString& path, const QString& root )
{
    auto env = QProcessEnvironment::systemEnvironment();
    const QFileInfo info( path );
    env.insert( "FLOGG_PATH", QDir::toNativeSeparators( info.absoluteFilePath() ) );
    env.insert( "FLOGG_NAME", info.fileName() );
    env.insert( "FLOGG_DIR", QDir::toNativeSeparators( info.absolutePath() ) );
    env.insert( "FLOGG_ROOT", QDir::toNativeSeparators( root ) );
    return env;
}

QString workingDirectory( const QString& path )
{
    const QFileInfo info( path );
    return info.isDir() ? info.absoluteFilePath() : info.absolutePath();
}

QString variablesHelp( const QString& shell )
{
    QString example;
    if ( isPowerShell( shell ) ) {
        example = "(Select-String -Pattern ERROR -Path $env:FLOGG_PATH).Count";
    }
    else if ( isCmd( shell ) ) {
        example = "findstr /c:ERROR \"%FLOGG_PATH%\"";
    }
    else {
        example = "grep -c ERROR \"$FLOGG_PATH\"";
    }
#ifdef Q_OS_WIN
    const QString root = "C:\\logs";
    const QString sep = "\\";
#else
    const QString root = "/logs";
    const QString sep = "/";
#endif
    const auto app = root + sep + "app";
    const auto file = app + sep + "main.log";
    const auto code = []( const QString& text ) { return "<code>" + text.toHtmlEscaped() + "</code>"; };
    const auto row = [ & ]( const QString& name, const QString& meaning, const QString& forFile,
                            const QString& forFolder ) {
        return QString( "<tr><td>%1</td><td>%2</td><td>%3</td><td>%4</td></tr>" )
            .arg( name, meaning, forFile, forFolder );
    };

    return QObject::tr( "<p>The command runs without a terminal, once for each selected item, "
                        "in <b>%1</b>; what it prints is shown in a window (always, or only "
                        "when it fails). The item is passed in environment variables, so spaces "
                        "and quotes in the paths need no care.</p>" )
               .arg( shell.toHtmlEscaped() )
           + QObject::tr( "<p>With %1 as the root folder of the panel:</p>" ).arg( code( root ) )
           + "<table border='1' cellspacing='0' cellpadding='4'>"
           + QObject::tr( "<tr><th>Variable</th><th>Meaning</th><th>File %1</th>"
                          "<th>Folder %2</th></tr>" )
                 .arg( code( file ), code( app ) )
           + row( code( variable( shell, "FLOGG_PATH" ) ), QObject::tr( "The item itself" ),
                  code( file ), code( app ) )
           + row( code( variable( shell, "FLOGG_NAME" ) ), QObject::tr( "Its name" ),
                  code( "main.log" ), code( "app" ) )
           + row( code( variable( shell, "FLOGG_DIR" ) ), QObject::tr( "The folder it is in" ),
                  code( app ), code( root ) + QObject::tr( " (one level up)" ) )
           + row( code( variable( shell, "FLOGG_ROOT" ) ),
                  QObject::tr( "The root folder of the panel" ), code( root ), code( root ) )
           + row( QObject::tr( "Working folder" ), QObject::tr( "Where the command runs" ),
                  code( app ), code( app ) + QObject::tr( " (the folder itself)" ) )
           + "</table>"
           + QObject::tr( "<p>For a folder, use %1 or <code>.</code> for the folder itself. "
                          "Right clicking the empty space of the panel runs the command on the "
                          "root folder.</p>" )
                 .arg( code( variable( shell, "FLOGG_PATH" ) ) )
           + QObject::tr( "<p>Example: %1</p>" ).arg( code( example ) );
}

} // namespace filecommands

FileCommandsDialog::FileCommandsDialog( QWidget* parent )
    : QDialog( parent )
{
    setWindowTitle( tr( "File Panel Commands" ) );

    table_ = new QTableWidget( 0, ColumnCount, this );
    table_->setHorizontalHeaderLabels(
        { tr( "Name" ), tr( "Command" ), tr( "Applies to" ), tr( "Output" ) } );
    table_->verticalHeader()->hide();
    table_->setSelectionBehavior( QAbstractItemView::SelectRows );
    table_->setSelectionMode( QAbstractItemView::SingleSelection );
    table_->horizontalHeader()->setSectionResizeMode( NameColumn, QHeaderView::Interactive );
    table_->horizontalHeader()->setSectionResizeMode( CommandColumn, QHeaderView::Stretch );
    table_->horizontalHeader()->setSectionResizeMode( TargetColumn, QHeaderView::Fixed );
    table_->horizontalHeader()->setSectionResizeMode( OutputColumn, QHeaderView::Fixed );
    table_->setColumnWidth( NameColumn, 160 );
    // The combo boxes are cell widgets, which the header does not measure: the columns and
    // rows take their size, also before the first row
    {
        const std::unique_ptr<QComboBox> target( makeTargetBox( nullptr ) );
        const std::unique_ptr<QComboBox> output( makeOutputBox( nullptr ) );
        const auto header = table_->horizontalHeader();
        table_->setColumnWidth( TargetColumn,
                                std::max( target->sizeHint().width(),
                                          header->sectionSizeHint( TargetColumn ) ) );
        table_->setColumnWidth( OutputColumn,
                                std::max( output->sizeHint().width(),
                                          header->sectionSizeHint( OutputColumn ) ) );
        table_->verticalHeader()->setDefaultSectionSize(
            std::max( table_->verticalHeader()->defaultSectionSize(),
                      target->sizeHint().height() + 2 ) );
    }
    for ( const auto& command : filecommands::load() ) {
        addRow( command );
    }
    connect( table_, &QTableWidget::itemSelectionChanged, this,
             &FileCommandsDialog::updateButtons );

    auto* add = new QPushButton( tr( "Add" ), this );
    remove_ = new QPushButton( tr( "Remove" ), this );
    up_ = new QPushButton( tr( "Move Up" ), this );
    down_ = new QPushButton( tr( "Move Down" ), this );
    connect( add, &QPushButton::clicked, this, [ this ] {
        addRow( FileCommand{} );
        table_->setCurrentCell( table_->rowCount() - 1, NameColumn );
        table_->editItem( table_->item( table_->rowCount() - 1, NameColumn ) );
    } );
    connect( remove_, &QPushButton::clicked, this, [ this ] {
        const auto row = table_->currentRow();
        if ( row >= 0 ) {
            table_->removeRow( row );
        }
        updateButtons();
    } );
    connect( up_, &QPushButton::clicked, this, [ this ] { moveRow( -1 ); } );
    connect( down_, &QPushButton::clicked, this, [ this ] { moveRow( 1 ); } );

    auto* buttonsColumn = new QVBoxLayout;
    buttonsColumn->addWidget( add );
    buttonsColumn->addWidget( remove_ );
    buttonsColumn->addWidget( up_ );
    buttonsColumn->addWidget( down_ );
    buttonsColumn->addStretch();

    auto* tableRow = new QHBoxLayout;
    tableRow->addWidget( table_, 1 );
    tableRow->addLayout( buttonsColumn );

    shell_ = new QLineEdit( filecommands::configuredShell(), this );
    shell_->setPlaceholderText( filecommands::defaultShell() );
    connect( shell_, &QLineEdit::textChanged, this, &FileCommandsDialog::updateHelp );
    auto* browse = new QPushButton( tr( "Browse..." ), this );
    connect( browse, &QPushButton::clicked, this, [ this ] {
        const auto file = QFileDialog::getOpenFileName( this, tr( "Shell" ), shell() );
        if ( !file.isEmpty() ) {
            shell_->setText( QDir::toNativeSeparators( file ) );
        }
    } );
    auto* shellRow = new QHBoxLayout;
    shellRow->addWidget( new QLabel( tr( "Shell:" ), this ) );
    shellRow->addWidget( shell_, 1 );
    shellRow->addWidget( browse );

    // How the commands run and the variables, in a window of its own
    auto* helpButton = new QToolButton( this );
    helpButton->setText( "?" );
    helpButton->setToolTip( tr( "How the commands run, the variables, an example" ) );
    auto helpFont = helpButton->font();
    helpFont.setBold( true );
    helpButton->setFont( helpFont );
    helpButton->setStyleSheet( "QToolButton { border: 1px solid palette(mid); border-radius: 10px; "
                               "min-width: 18px; max-width: 18px; min-height: 18px; "
                               "max-height: 18px; padding: 0px; }" );
    connect( helpButton, &QToolButton::clicked, this, &FileCommandsDialog::showHelp );

    // OK and Cancel in the order of the platform, the help right next to Cancel on the
    // outer side: "? Cancel OK" on macOS and GNOME, "OK Cancel ?" on Windows and KDE
    auto* ok = new QPushButton( tr( "OK" ), this );
    ok->setDefault( true );
    auto* cancel = new QPushButton( tr( "Cancel" ), this );
    connect( ok, &QPushButton::clicked, this, &FileCommandsDialog::accept );
    connect( cancel, &QPushButton::clicked, this, &QDialog::reject );
    const auto buttonLayout = style()->styleHint( QStyle::SH_DialogButtonLayout, nullptr, this );
    const auto cancelFirst = buttonLayout == QDialogButtonBox::MacLayout
                             || buttonLayout == QDialogButtonBox::GnomeLayout;
    auto* bottom = new QHBoxLayout;
    bottom->addStretch();
    if ( cancelFirst ) {
        bottom->addWidget( helpButton );
        bottom->addWidget( cancel );
        bottom->addWidget( ok );
    }
    else {
        bottom->addWidget( ok );
        bottom->addWidget( cancel );
        bottom->addWidget( helpButton );
    }

    auto* layout = new QVBoxLayout( this );
    layout->addLayout( tableRow, 1 );
    layout->addLayout( shellRow );
    layout->addLayout( bottom );

    resize( 820, 420 );
    updateButtons();
}

QComboBox* FileCommandsDialog::makeTargetBox( QWidget* parent )
{
    auto* box = new QComboBox( parent );
    box->addItem( tr( "Files and folders" ),
                  static_cast<int>( FileCommand::Target::FilesAndFolders ) );
    box->addItem( tr( "Files" ), static_cast<int>( FileCommand::Target::Files ) );
    box->addItem( tr( "Folders" ), static_cast<int>( FileCommand::Target::Folders ) );
    return box;
}

QComboBox* FileCommandsDialog::makeOutputBox( QWidget* parent )
{
    auto* box = new QComboBox( parent );
    box->addItem( tr( "Always show" ), static_cast<int>( FileCommand::Output::Always ) );
    box->addItem( tr( "Show on failure" ), static_cast<int>( FileCommand::Output::OnFailure ) );
    return box;
}

void FileCommandsDialog::addRow( const FileCommand& command )
{
    const auto row = table_->rowCount();
    table_->insertRow( row );
    table_->setItem( row, NameColumn, new QTableWidgetItem( command.name ) );
    auto* commandItem = new QTableWidgetItem( command.command );
    commandItem->setFont( QFontDatabase::systemFont( QFontDatabase::FixedFont ) );
    table_->setItem( row, CommandColumn, commandItem );

    auto* target = makeTargetBox( table_ );
    target->setCurrentIndex( target->findData( static_cast<int>( command.target ) ) );
    table_->setCellWidget( row, TargetColumn, target );

    auto* output = makeOutputBox( table_ );
    output->setCurrentIndex( output->findData( static_cast<int>( command.output ) ) );
    table_->setCellWidget( row, OutputColumn, output );
}

std::vector<FileCommand> FileCommandsDialog::commands() const
{
    std::vector<FileCommand> result;
    for ( int row = 0; row < table_->rowCount(); ++row ) {
        FileCommand command;
        const auto* name = table_->item( row, NameColumn );
        const auto* text = table_->item( row, CommandColumn );
        command.name = name ? name->text().trimmed() : QString{};
        command.command = text ? text->text().trimmed() : QString{};
        if ( command.command.isEmpty() ) {
            continue;
        }
        if ( command.name.isEmpty() ) {
            command.name = command.command;
        }
        if ( const auto* target = qobject_cast<QComboBox*>( table_->cellWidget( row, TargetColumn ) ) ) {
            command.target = static_cast<FileCommand::Target>( target->currentData().toInt() );
        }
        if ( const auto* output = qobject_cast<QComboBox*>( table_->cellWidget( row, OutputColumn ) ) ) {
            command.output = static_cast<FileCommand::Output>( output->currentData().toInt() );
        }
        result.push_back( command );
    }
    return result;
}

QString FileCommandsDialog::shell() const
{
    const auto text = shell_->text().trimmed();
    return text.isEmpty() ? filecommands::defaultShell() : text;
}

void FileCommandsDialog::accept()
{
    filecommands::save( commands() );
    filecommands::setConfiguredShell( shell_->text() );
    QDialog::accept();
}

// The rows hold combo boxes, so the table is filled again in the new order
void FileCommandsDialog::moveRow( int delta )
{
    const auto row = table_->currentRow();
    const auto target = row + delta;
    if ( row < 0 || target < 0 || target >= table_->rowCount() ) {
        return;
    }
    // Empty rows are kept while moving
    std::vector<FileCommand> rows;
    for ( int i = 0; i < table_->rowCount(); ++i ) {
        FileCommand command;
        if ( const auto* name = table_->item( i, NameColumn ) ) {
            command.name = name->text();
        }
        if ( const auto* text = table_->item( i, CommandColumn ) ) {
            command.command = text->text();
        }
        if ( const auto* box = qobject_cast<QComboBox*>( table_->cellWidget( i, TargetColumn ) ) ) {
            command.target = static_cast<FileCommand::Target>( box->currentData().toInt() );
        }
        if ( const auto* box = qobject_cast<QComboBox*>( table_->cellWidget( i, OutputColumn ) ) ) {
            command.output = static_cast<FileCommand::Output>( box->currentData().toInt() );
        }
        rows.push_back( command );
    }
    std::swap( rows.at( static_cast<size_t>( row ) ), rows.at( static_cast<size_t>( target ) ) );
    table_->setRowCount( 0 );
    for ( const auto& command : rows ) {
        addRow( command );
    }
    table_->setCurrentCell( target, NameColumn );
    updateButtons();
}

void FileCommandsDialog::updateButtons()
{
    const auto row = table_->currentRow();
    const auto selected = row >= 0 && !table_->selectedItems().isEmpty();
    remove_->setEnabled( selected );
    up_->setEnabled( selected && row > 0 );
    down_->setEnabled( selected && row + 1 < table_->rowCount() );
}

void FileCommandsDialog::showHelp()
{
    if ( !help_ ) {
        help_ = new QDialog( this );
        help_->setAttribute( Qt::WA_DeleteOnClose );
        help_->setWindowTitle( tr( "File Panel Commands - how to use" ) );
        helpText_ = new QTextBrowser( help_ );
        helpText_->setOpenExternalLinks( true );
        helpText_->document()->setDefaultStyleSheet(
            QString( "code { font-family: '%1'; }" )
                .arg( QFontDatabase::systemFont( QFontDatabase::FixedFont ).family() ) );
        auto* layout = new QVBoxLayout( help_ );
        layout->addWidget( helpText_ );
        help_->resize( 760, 440 );
    }
    updateHelp();
    help_->show();
    help_->raise();
    help_->activateWindow();
}

// The syntax of the variables follows the shell typed
void FileCommandsDialog::updateHelp()
{
    if ( help_ ) {
        helpText_->setHtml( filecommands::variablesHelp( shell() ) );
    }
}

CommandOutputWindow::CommandOutputWindow( const FileCommand& command, const QStringList& paths,
                                          const QString& root, const QString& shell,
                                          QWidget* parent )
    : QWidget( parent, Qt::Window )
    , command_( command )
    , paths_( paths )
    , root_( root )
    , shell_( shell )
{
    setAttribute( Qt::WA_DeleteOnClose );

    text_ = new QPlainTextEdit( this );
    text_->setReadOnly( true );
    text_->setLineWrapMode( QPlainTextEdit::NoWrap );
    text_->setFont( QFontDatabase::systemFont( QFontDatabase::FixedFont ) );

    status_ = new QLabel( this );
    status_->setTextInteractionFlags( Qt::TextSelectableByMouse );
    stopButton_ = new QPushButton( tr( "Stop" ), this );
    connect( stopButton_, &QPushButton::clicked, this, &CommandOutputWindow::stop );
    auto* closeButton = new QPushButton( tr( "Close" ), this );
    connect( closeButton, &QPushButton::clicked, this, &QWidget::close );

    auto* bottom = new QHBoxLayout;
    bottom->addWidget( status_, 1 );
    bottom->addWidget( stopButton_ );
    bottom->addWidget( closeButton );

    auto* layout = new QVBoxLayout( this );
    layout->addWidget( text_, 1 );
    layout->addLayout( bottom );

    flushTimer_ = new QTimer( this );
    flushTimer_->setSingleShot( true );
    flushTimer_->setInterval( FlushIntervalMs );
    connect( flushTimer_, &QTimer::timeout, this, &CommandOutputWindow::flushOutput );

    process_ = new QProcess( this );
    process_->setProcessChannelMode( QProcess::MergedChannels );
#ifdef Q_OS_UNIX
    process_->setChildProcessModifier( [] { ::setpgid( 0, 0 ); } );
#endif
#ifdef Q_OS_WIN
    process_->setCreateProcessArgumentsModifier( []( QProcess::CreateProcessArguments* args ) {
        args->flags |= 0x08000000; // CREATE_NO_WINDOW
    } );
#endif
    connect( process_, &QProcess::readyReadStandardOutput, this,
             &CommandOutputWindow::readOutput );
    connect( process_, &QProcess::finished, this,
             [ this ]( int exitCode, QProcess::ExitStatus status ) {
                 onProcessFinished( exitCode, status == QProcess::CrashExit );
             } );
    connect( process_, &QProcess::errorOccurred, this, [ this ]( QProcess::ProcessError error ) {
        // No finished() follows a failed start
        if ( error == QProcess::FailedToStart ) {
            appendText( tr( "%1 could not be started: %2\n" )
                            .arg( shell_, process_->errorString() ) );
            ++failures_;
            if ( !isVisible() ) {
                show();
            }
            QTimer::singleShot( 0, this, &CommandOutputWindow::runNext );
        }
    } );

    resize( 820, 460 );
    updateTitle();
}

CommandOutputWindow::~CommandOutputWindow()
{
    if ( process_->state() != QProcess::NotRunning ) {
        process_->disconnect( this );
#ifdef Q_OS_UNIX
        killGroup( process_ );
#endif
        process_->kill();
        process_->waitForFinished( 1000 );
    }
}

void CommandOutputWindow::start()
{
    if ( command_.output == FileCommand::Output::Always ) {
        show();
    }
    runNext();
}

void CommandOutputWindow::stop()
{
    if ( !isRunning() ) {
        return;
    }
    stopped_ = true;
#ifdef Q_OS_UNIX
    killGroup( process_ );
#endif
    process_->kill();
}

bool CommandOutputWindow::isRunning() const
{
    return !done_;
}

QString CommandOutputWindow::outputText() const
{
    return text_->toPlainText() + pending_;
}

void CommandOutputWindow::closeEvent( QCloseEvent* event )
{
    stop();
    event->accept();
}

void CommandOutputWindow::runNext()
{
    if ( done_ ) {
        return;
    }
    if ( stopped_ || next_ >= paths_.size() ) {
        onAllFinished();
        return;
    }
    const auto path = paths_.at( next_++ );
    if ( paths_.size() > 1 ) {
        appendText( QString( "%1== %2 ==\n" )
                        .arg( next_ > 1 ? "\n" : "", QDir::toNativeSeparators( path ) ) );
    }
    updateTitle();

    const auto call = filecommands::invocation( shell_, command_.command );
    decoder_.emplace( isCmd( shell_ ) ? QStringDecoder::System : QStringDecoder::Utf8 );
    process_->setProgram( call.program );
    process_->setArguments( call.arguments );
#ifdef Q_OS_WIN
    process_->setNativeArguments( call.nativeArguments );
#endif
    process_->setProcessEnvironment( filecommands::environment( path, root_ ) );
    process_->setWorkingDirectory( filecommands::workingDirectory( path ) );
    LOG_INFO << "File command " << command_.name << " on " << path;
    process_->start();
    // A command waiting for input gets the end of it at once
    process_->closeWriteChannel();
}

void CommandOutputWindow::readOutput()
{
    const auto bytes = process_->readAllStandardOutput();
    if ( truncated_ || bytes.isEmpty() ) {
        return;
    }
    pending_ += ( *decoder_ )( bytes );
    if ( !flushTimer_->isActive() ) {
        flushTimer_->start();
    }
}

void CommandOutputWindow::appendText( const QString& text )
{
    pending_ += text;
    flushOutput();
}

void CommandOutputWindow::flushOutput()
{
    flushTimer_->stop();
    if ( pending_.isEmpty() ) {
        return;
    }
    if ( !truncated_ && shownChars_ + pending_.size() > MaxOutputChars ) {
        pending_.truncate( std::max<qsizetype>( 0, MaxOutputChars - shownChars_ ) );
        pending_ += tr( "\n[The output goes on: only the first %1 characters are shown]\n" )
                        .arg( MaxOutputChars );
        truncated_ = true;
    }
    auto* bar = text_->verticalScrollBar();
    const auto atEnd = bar->value() == bar->maximum();
    QTextCursor cursor( text_->document() );
    cursor.movePosition( QTextCursor::End );
    cursor.insertText( pending_ );
    shownChars_ += pending_.size();
    pending_.clear();
    if ( atEnd ) {
        bar->setValue( bar->maximum() );
    }
}

void CommandOutputWindow::onProcessFinished( int exitCode, bool crashed )
{
    readOutput();
    if ( stopped_ ) {
        appendText( tr( "\n[Stopped]\n" ) );
    }
    else if ( crashed || exitCode != 0 ) {
        ++failures_;
        appendText( crashed ? tr( "\n[The command crashed]\n" )
                            : tr( "\n[Exit code %1]\n" ).arg( exitCode ) );
        if ( !isVisible() ) {
            show();
        }
    }
    else {
        flushOutput();
    }
    runNext();
}

void CommandOutputWindow::onAllFinished()
{
    done_ = true;
    flushOutput();
    stopButton_->setEnabled( false );
    updateTitle();
    Q_EMIT finished();
    if ( command_.output == FileCommand::Output::OnFailure && failures_ == 0 && !stopped_ ) {
        close();
    }
}

void CommandOutputWindow::updateTitle()
{
    const auto subject = paths_.size() == 1 ? QFileInfo( paths_.first() ).fileName()
                                            : tr( "%1 items" ).arg( paths_.size() );
    setWindowTitle( QString( "%1 - %2" ).arg( command_.name, subject ) );

    if ( !done_ ) {
        status_->setText( paths_.size() > 1 ? tr( "Running %1 of %2..." )
                                                  .arg( next_ )
                                                  .arg( paths_.size() )
                                            : tr( "Running..." ) );
    }
    else if ( stopped_ ) {
        status_->setText( tr( "Stopped" ) );
    }
    else if ( failures_ > 0 ) {
        status_->setText( paths_.size() > 1
                              ? tr( "Finished, %1 of %2 failed" ).arg( failures_ ).arg( paths_.size() )
                              : tr( "Failed" ) );
    }
    else {
        status_->setText( tr( "Finished" ) );
    }
}
