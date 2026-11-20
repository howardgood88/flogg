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

#include "editview.h"

#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QShortcut>
#include <QStackedWidget>
#include <QTextCodec>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent>

#include "ILoader.h"
#include "Scintilla.h"

#include "configuration.h"
#include "logdata.h"
#include "readablesize.h"

using Scintilla::sptr_t;
using Scintilla::uptr_t;

namespace {

// Matches of the find bar (the indicators below 8 are for lexers)
constexpr int MatchIndicator = 8;
// A regular expression of the C++ library is slow: Scintilla's own is used above this
constexpr qint64 LargeDocument = 64 * 1024 * 1024;
// Above this the positions need 64 bits
constexpr qint64 TextLargeSize = 1536LL * 1024 * 1024;

bool askBeforeLoading = true;

sptr_t rgb( const QColor& color )
{
    return color.red() | ( color.green() << 8 ) | ( color.blue() << 16 );
}

sptr_t rgba( const QColor& color )
{
    return rgb( color ) | ( static_cast<sptr_t>( color.alpha() ) << 24 );
}

// The keys of the text, kept from the shortcuts of the window
bool isEditorKey( const QKeyEvent* event )
{
    const auto key = event->key();
    if ( ( key >= Qt::Key_F1 && key <= Qt::Key_F35 ) || key == Qt::Key_Escape ) {
        return false;
    }
    const auto modifiers = event->modifiers() & ~Qt::KeypadModifier;
    // Letters, digits, moves, deleting
    if ( modifiers == Qt::NoModifier || modifiers == Qt::ShiftModifier ) {
        return true;
    }
    static const QKeySequence::StandardKey editKeys[] = {
        QKeySequence::Undo,
        QKeySequence::Redo,
        QKeySequence::Cut,
        QKeySequence::Copy,
        QKeySequence::Paste,
        QKeySequence::SelectAll,
        QKeySequence::Save,
        QKeySequence::DeleteStartOfWord,
        QKeySequence::DeleteEndOfWord,
        QKeySequence::DeleteEndOfLine,
        QKeySequence::MoveToNextWord,
        QKeySequence::MoveToPreviousWord,
        QKeySequence::MoveToStartOfLine,
        QKeySequence::MoveToEndOfLine,
        QKeySequence::MoveToStartOfDocument,
        QKeySequence::MoveToEndOfDocument,
        QKeySequence::SelectNextWord,
        QKeySequence::SelectPreviousWord,
        QKeySequence::SelectStartOfLine,
        QKeySequence::SelectEndOfLine,
        QKeySequence::SelectStartOfDocument,
        QKeySequence::SelectEndOfDocument,
    };
    for ( const auto standardKey : editKeys ) {
        if ( event->matches( standardKey ) ) {
            return true;
        }
    }
    // Rectangular selection, moving lines
    const auto arrow = key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up
                       || key == Qt::Key_Down;
    return arrow && ( modifiers & ( Qt::AltModifier | Qt::ControlModifier | Qt::MetaModifier ) );
}

} // namespace

bool EditorWidget::event( QEvent* event )
{
    if ( event->type() == QEvent::ShortcutOverride
         && isEditorKey( static_cast<QKeyEvent*>( event ) ) ) {
        event->accept();
        return true;
    }
    return ScintillaEditBase::event( event );
}

void EditorWidget::keyPressEvent( QKeyEvent* event )
{
    if ( event->matches( QKeySequence::Save ) ) {
        event->accept();
        Q_EMIT saveRequested();
        return;
    }
    ScintillaEditBase::keyPressEvent( event );
}

void EditView::setAskBeforeLoading( bool ask )
{
    askBeforeLoading = ask;
}

EditView::EditView( std::shared_ptr<LogData> logData, QWidget* parent )
    : QWidget( parent )
    , logData_( std::move( logData ) )
    , messageLabel_( new QLabel )
    , saveButton_( new QPushButton( tr( "Save" ) ) )
    , saveAsButton_( new QPushButton( tr( "Save as…" ) ) )
    , reloadButton_( new QPushButton( tr( "Reload" ) ) )
    , pages_( new QStackedWidget )
    , placeholder_( new QLabel )
    , editor_( new EditorWidget )
    , findBar_( new QWidget )
    , findEdit_( new QLineEdit )
    , replaceEdit_( new QLineEdit )
    , caseButton_( new QToolButton )
    , regexButton_( new QToolButton )
    , findStatus_( new QLabel )
    , progressTimer_( new QTimer( this ) )
{
    messageLabel_->setTextInteractionFlags( Qt::TextSelectableByMouse );
    saveButton_->setToolTip( QKeySequence( QKeySequence::Save ).toString( QKeySequence::NativeText ) );
    reloadButton_->setToolTip( tr( "Read the file on disk again" ) );

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins( 4, 2, 4, 2 );
    bar->addWidget( messageLabel_, 1 );
    bar->addWidget( reloadButton_ );
    bar->addWidget( saveAsButton_ );
    bar->addWidget( saveButton_ );

    placeholder_->setAlignment( Qt::AlignCenter );
    placeholder_->setWordWrap( true );
    pages_->addWidget( placeholder_ );
    pages_->addWidget( editor_ );

    // The find bar of the edit mode, with the replacement
    findEdit_->setPlaceholderText( tr( "Find" ) );
    findEdit_->setClearButtonEnabled( true );
    replaceEdit_->setPlaceholderText( tr( "Replace with" ) );
    replaceEdit_->setClearButtonEnabled( true );
    caseButton_->setText( QStringLiteral( "Aa" ) );
    caseButton_->setToolTip( tr( "Match case" ) );
    regexButton_->setText( QStringLiteral( ".*" ) );
    regexButton_->setToolTip( tr( "Regular expression (ECMAScript, the basic syntax of "
                                  "Scintilla in documents over 64 MiB); \\1 in the replacement" ) );
    auto* previousButton = new QToolButton;
    previousButton->setArrowType( Qt::UpArrow );
    previousButton->setToolTip( tr( "Previous match" ) );
    auto* nextButton = new QToolButton;
    nextButton->setArrowType( Qt::DownArrow );
    nextButton->setToolTip( tr( "Next match" ) );
    auto* replaceButton = new QPushButton( tr( "Replace" ) );
    auto* replaceAllButton = new QPushButton( tr( "Replace all" ) );
    auto* closeButton = new QToolButton;
    closeButton->setText( QStringLiteral( "✕" ) );
    closeButton->setToolTip( tr( "Close" ) );
    for ( auto* button : { caseButton_, regexButton_ } ) {
        button->setCheckable( true );
    }
    for ( auto* button : { caseButton_, regexButton_, previousButton, nextButton, closeButton } ) {
        button->setAutoRaise( true );
        button->setFocusPolicy( Qt::NoFocus );
    }
    auto* findLayout = new QHBoxLayout( findBar_ );
    findLayout->setContentsMargins( 4, 2, 4, 2 );
    findLayout->addWidget( findEdit_, 2 );
    findLayout->addWidget( caseButton_ );
    findLayout->addWidget( regexButton_ );
    findLayout->addWidget( previousButton );
    findLayout->addWidget( nextButton );
    findLayout->addWidget( replaceEdit_, 1 );
    findLayout->addWidget( replaceButton );
    findLayout->addWidget( replaceAllButton );
    findLayout->addWidget( findStatus_ );
    findLayout->addWidget( closeButton );
    findBar_->hide();

    auto* layout = new QVBoxLayout( this );
    layout->setContentsMargins( 0, 0, 0, 0 );
    layout->setSpacing( 0 );
    layout->addLayout( bar );
    layout->addWidget( pages_, 1 );
    layout->addWidget( findBar_ );

    setupEditor();

    connect( editor_, &EditorWidget::saveRequested, this, [ this ] { save(); } );
    connect( editor_, &ScintillaEditBase::savePointChanged, this,
             [ this ]( bool dirty ) { Q_EMIT modifiedChanged( dirty ); } );
    connect( editor_, &ScintillaEditBase::updateUi, this, [ this ] { updateHighlights(); } );
    connect( editor_, &ScintillaEditBase::linesAdded, this, [ this ] {
        // Room for the line numbers
        const auto digits = QString::number( editor_->send( SCI_GETLINECOUNT ) ).size();
        const auto sample = QByteArray( static_cast<int>( std::max<qsizetype>( digits, 3 ) ) + 1, '9' );
        editor_->send( SCI_SETMARGINWIDTHN, 0,
                       editor_->send( SCI_TEXTWIDTH, STYLE_LINENUMBER,
                                      reinterpret_cast<sptr_t>( sample.constData() ) ) );
    } );
    connect( saveButton_, &QPushButton::clicked, this, [ this ] { save(); } );
    connect( saveAsButton_, &QPushButton::clicked, this, &EditView::saveAs );
    connect( reloadButton_, &QPushButton::clicked, this, &EditView::reloadFile );
    connect( progressTimer_, &QTimer::timeout, this, &EditView::updateProgress );
    progressTimer_->setInterval( 100 );

    connect( findEdit_, &QLineEdit::returnPressed, this, &EditView::findNext );
    connect( findEdit_, &QLineEdit::textChanged, this, [ this ] { updateHighlights(); } );
    connect( caseButton_, &QToolButton::toggled, this, [ this ] { updateHighlights(); } );
    connect( regexButton_, &QToolButton::toggled, this, [ this ] { updateHighlights(); } );
    connect( previousButton, &QToolButton::clicked, this, &EditView::findPrevious );
    connect( nextButton, &QToolButton::clicked, this, &EditView::findNext );
    connect( replaceEdit_, &QLineEdit::returnPressed, this,
             [ this ] { replaceNext( replaceEdit_->text() ); } );
    connect( replaceButton, &QPushButton::clicked, this,
             [ this ] { replaceNext( replaceEdit_->text() ); } );
    connect( replaceAllButton, &QPushButton::clicked, this,
             [ this ] { replaceAll( replaceEdit_->text() ); } );
    connect( closeButton, &QToolButton::clicked, this, [ this ] {
        findBar_->hide();
        updateHighlights();
        editor_->setFocus();
    } );
    auto* previousKey = new QShortcut( QKeySequence( Qt::SHIFT | Qt::Key_Return ), findEdit_ );
    previousKey->setContext( Qt::WidgetShortcut );
    connect( previousKey, &QShortcut::activated, this, &EditView::findPrevious );
    for ( auto* edit : { findEdit_, replaceEdit_ } ) {
        auto* escapeKey = new QShortcut( QKeySequence( Qt::Key_Escape ), edit );
        escapeKey->setContext( Qt::WidgetShortcut );
        connect( escapeKey, &QShortcut::activated, closeButton, &QToolButton::click );
    }
    for ( const auto& key : QKeySequence::keyBindings( QKeySequence::FindNext ) ) {
        auto* shortcut = new QShortcut( key, this );
        shortcut->setContext( Qt::WidgetWithChildrenShortcut );
        connect( shortcut, &QShortcut::activated, this, &EditView::findNext );
    }
    for ( const auto& key : QKeySequence::keyBindings( QKeySequence::FindPrevious ) ) {
        auto* shortcut = new QShortcut( key, this );
        shortcut->setContext( Qt::WidgetWithChildrenShortcut );
        connect( shortcut, &QShortcut::activated, this, &EditView::findPrevious );
    }

    connect( logData_.get(), &LogData::fileChanged, this, &EditView::fileChangedOnDisk );

    reloadButton_->hide();
    saveButton_->setEnabled( false );
    saveAsButton_->setEnabled( false );
    placeholder_->setText( tr( "Loading…" ) );
}

EditView::~EditView()
{
    // A load is stopped (its document freed), a save is let to finish
    if ( future_.isRunning() ) {
        if ( !saving_ ) {
            job_->cancelled = true;
        }
        future_.waitForFinished();
    }
    if ( saving_ ) {
        logData_->restoreFile();
    }
    if ( job_ && job_->loader ) {
        job_->loader->Release();
        job_->loader = nullptr;
    }
}

void EditView::setupEditor()
{
    const auto send = [ this ]( unsigned int message, uptr_t wParam = 0, sptr_t lParam = 0 ) {
        return editor_->send( message, wParam, lParam );
    };

    const auto font = Configuration::get().mainFont();
    const auto family = font.family().toUtf8();
    send( SCI_STYLESETFONT, STYLE_DEFAULT, reinterpret_cast<sptr_t>( family.constData() ) );
    send( SCI_STYLESETSIZEFRACTIONAL, STYLE_DEFAULT,
          qRound( font.pointSizeF() * SC_FONT_SIZE_MULTIPLIER ) );

    const auto colors = palette();
    send( SCI_STYLESETFORE, STYLE_DEFAULT, rgb( colors.color( QPalette::Text ) ) );
    send( SCI_STYLESETBACK, STYLE_DEFAULT, rgb( colors.color( QPalette::Base ) ) );
    send( SCI_STYLECLEARALL );
    send( SCI_STYLESETFORE, STYLE_LINENUMBER,
          rgb( colors.color( QPalette::Disabled, QPalette::Text ) ) );
    send( SCI_STYLESETBACK, STYLE_LINENUMBER, rgb( colors.color( QPalette::Window ) ) );

    send( SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER );
    send( SCI_SETMARGINWIDTHN, 1, 0 );
    send( SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET, rgba( colors.color( QPalette::Text ) ) );
    auto selection = colors.color( QPalette::Highlight );
    send( SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_BACK, rgba( selection ) );
    send( SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_TEXT,
          rgba( colors.color( QPalette::HighlightedText ) ) );
    selection.setAlpha( 110 );
    send( SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_INACTIVE_BACK, rgba( selection ) );
    send( SCI_SETCARETLINELAYER, SC_LAYER_UNDER_TEXT );
    send( SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET_LINE_BACK, rgba( QColor( 127, 127, 127, 28 ) ) );

    send( SCI_SETWRAPMODE, SC_WRAP_NONE );
    send( SCI_SETSCROLLWIDTH, 1 );
    send( SCI_SETSCROLLWIDTHTRACKING, 1 );
    send( SCI_SETMULTIPLESELECTION, 1 );
    send( SCI_SETADDITIONALSELECTIONTYPING, 1 );
    send( SCI_SETMULTIPASTE, SC_MULTIPASTE_EACH );

    // The matches of the find bar, under the text like in the render mode
    send( SCI_INDICSETSTYLE, MatchIndicator, INDIC_FULLBOX );
    send( SCI_INDICSETFORE, MatchIndicator, rgb( QColor( 255, 200, 0 ) ) );
    send( SCI_INDICSETALPHA, MatchIndicator, 110 );
    send( SCI_INDICSETOUTLINEALPHA, MatchIndicator, 0 );
    send( SCI_INDICSETUNDER, MatchIndicator, 1 );
}

bool EditView::isBusy() const
{
    return future_.isRunning() || progressTimer_->isActive();
}

bool EditView::isModified() const
{
    return loaded_ && editor_->send( SCI_GETMODIFY ) != 0;
}

bool EditView::documentLarge() const
{
    return editor_->send( SCI_GETLENGTH ) > LargeDocument;
}

QString EditView::message() const
{
    return messageLabel_->text();
}

QString EditView::text() const
{
    const auto length = editor_->send( SCI_GETLENGTH );
    QByteArray text( static_cast<qsizetype>( length + 1 ), '\0' );
    editor_->send( SCI_GETTEXT, static_cast<uptr_t>( length + 1 ),
                   reinterpret_cast<sptr_t>( text.data() ) );
    text.chop( 1 );
    return QString::fromUtf8( text );
}

void EditView::showMessage( const QString& text, bool warning )
{
    messageLabel_->setText( text );
    messageLabel_->setForegroundRole( warning ? QPalette::Link : QPalette::WindowText );
    auto font = messageLabel_->font();
    font.setBold( warning );
    messageLabel_->setFont( font );
}

void EditView::start( qint64 line )
{
    fileName_ = logData_->attachedFileName();
    startLine_ = line;
    loaded_ = false;
    reloadButton_->hide();
    saveButton_->setEnabled( false );
    saveAsButton_->setEnabled( false );
    pages_->setCurrentWidget( placeholder_ );

    const QFileInfo info( fileName_ );
    const auto size = info.size();
    readOnly_ = !info.isWritable() || edit::isTemporaryCopy( fileName_ );

    // The whole file is held in memory, as in Notepad++
    const auto available = edit::availableMemory();
    if ( askBeforeLoading && available && static_cast<quint64>( size ) > *available / 2 ) {
        const auto answer = QMessageBox::warning(
            this, tr( "Edit mode" ),
            tr( "The file (%1) is held in memory to be edited, %2 of memory is available. Load "
                "it anyway?" )
                .arg( readableSize( static_cast<uint64_t>( size ) ),
                      readableSize( *available ) ),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No );
        if ( answer != QMessageBox::Yes ) {
            placeholder_->setText( tr( "The file was not loaded." ) );
            showMessage( {} );
            return;
        }
    }

    // No styles: no lexer, half the memory
    auto options = SC_DOCUMENTOPTION_STYLES_NONE;
    if ( size > TextLargeSize ) {
        options |= SC_DOCUMENTOPTION_TEXT_LARGE;
    }
    // Room for the edits: a buffer of the size of the file is copied whole by the first key
    const auto room = std::clamp<qint64>( size / 64, 1024 * 1024, 64 * 1024 * 1024 );
    auto* loader = reinterpret_cast<Scintilla::ILoader*>(
        editor_->send( SCI_CREATELOADER, static_cast<uptr_t>( size + room ), options ) );
    if ( loader == nullptr ) {
        placeholder_->setText( tr( "Not enough memory for the file." ) );
        return;
    }

    job_ = std::make_shared<Job>();
    job_->loader = loader;
    auto* codec = logData_->getDisplayEncoding();
    future_ = QtConcurrent::run( [ job = job_, fileName = fileName_, codec ] {
        job->ok = edit::readFile(
            fileName, codec,
            [ &job ]( const char* data, std::size_t length ) {
                return job->loader->AddData( data, static_cast<Sci_Position>( length ) )
                       == SC_STATUS_OK;
            },
            &job->format, &job->error, &job->cancelled, &job->progress );
    } );
    placeholder_->setText( tr( "Loading… 0%" ) );
    showMessage( {} );
    progressTimer_->start();
}

void EditView::updateProgress()
{
    if ( !job_ ) {
        progressTimer_->stop();
        return;
    }
    if ( !future_.isFinished() ) {
        const auto progress = job_->progress.load();
        if ( saving_ ) {
            showMessage( tr( "Saving… %1%" ).arg( progress ) );
        }
        else {
            placeholder_->setText( tr( "Loading… %1%" ).arg( progress ) );
        }
        return;
    }
    progressTimer_->stop();
    if ( saving_ ) {
        saveFinished( job_->ok, job_->error );
    }
    else {
        loadFinished( job_->ok, job_->error );
    }
}

void EditView::loadFinished( bool ok, const QString& error )
{
    auto* loader = std::exchange( job_->loader, nullptr );
    if ( !ok ) {
        loader->Release();
        placeholder_->setText( error.isEmpty() ? tr( "The file was not loaded." ) : error );
        return;
    }

    auto* document = loader->ConvertToDocument();
    editor_->send( SCI_SETDOCPOINTER, 0, reinterpret_cast<sptr_t>( document ) );
    editor_->send( SCI_RELEASEDOCUMENT, 0, reinterpret_cast<sptr_t>( document ) );
    format_ = job_->format;

    // Settings of the document
    editor_->send( SCI_SETCODEPAGE, SC_CP_UTF8 );
    const auto endOfLine = format_.endOfLine == "\r\n" ? SC_EOL_CRLF
                           : format_.endOfLine == "\r" ? SC_EOL_CR
                                                       : SC_EOL_LF;
    editor_->send( SCI_SETEOLMODE, endOfLine );
    // A loaded document does not collect the undo steps, nor know it is changed
    editor_->send( SCI_SETUNDOCOLLECTION, 1 );
    // Room in the index of the lines too, for the lines added
    const auto lines = editor_->send( SCI_GETLINECOUNT );
    editor_->send( SCI_ALLOCATELINES, static_cast<uptr_t>( lines + lines / 64 + 1024 ) );
    editor_->send( SCI_EMPTYUNDOBUFFER );
    editor_->send( SCI_SETSAVEPOINT );
    editor_->send( SCI_SETREADONLY, readOnly_ ? 1 : 0 );
    Q_EMIT editor_->linesAdded( 0 );

    loaded_ = true;
    pages_->setCurrentWidget( editor_ );
    goToLine( startLine_ );
    saveButton_->setEnabled( !readOnly_ );
    saveAsButton_->setEnabled( true );

    QStringList details;
    details.append( format_.isUtf8() ? QStringLiteral( "UTF-8" )
                                     : QString::fromLatin1( format_.codec ) );
    if ( !format_.bom.isEmpty() ) {
        details.append( QStringLiteral( "BOM" ) );
    }
    details.append( format_.endOfLine == "\r\n" ? QStringLiteral( "CRLF" )
                    : format_.endOfLine == "\r" ? QStringLiteral( "CR" )
                                                : QStringLiteral( "LF" ) );
    if ( readOnly_ ) {
        details.append( edit::isTemporaryCopy( fileName_ )
                            ? tr( "read only: a copy made by flogg, use Save as" )
                            : tr( "read only" ) );
    }
    if ( format_.lossy ) {
        details.append( tr( "some bytes could not be read in this encoding" ) );
    }
    showMessage( details.join( QStringLiteral( " · " ) ), readOnly_ || format_.lossy );
    Q_EMIT loaded();
}

bool EditView::save()
{
    if ( !loaded_ || isBusy() ) {
        return false;
    }
    if ( readOnly_ ) {
        saveAs();
        return false;
    }
    if ( format_.lossy
         && QMessageBox::warning(
                this, tr( "Save" ),
                tr( "Some bytes of the file could not be read in %1, they are written as the "
                    "replacement character. Save anyway?" )
                    .arg( QString::fromLatin1( format_.codec ) ),
                QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel )
                != QMessageBox::Save ) {
        return false;
    }

    // The buffer of the editor is written as it is: it does not change meanwhile
    saving_ = true;
    editor_->send( SCI_SETREADONLY, 1 );
    const auto* data = reinterpret_cast<const char*>( editor_->send( SCI_GETCHARACTERPOINTER ) );
    const auto length = static_cast<std::size_t>( editor_->send( SCI_GETLENGTH ) );

    // Not open in the log view meanwhile, else Windows does not replace it
    logData_->releaseFile();
    job_ = std::make_shared<Job>();
    future_ = QtConcurrent::run( [ job = job_, fileName = fileName_, data, length,
                                   format = format_ ] {
        job->ok = edit::writeFile( fileName, data, length, format, &job->error, &job->cancelled,
                                   &job->progress );
    } );
    showMessage( tr( "Saving…" ) );
    progressTimer_->start();
    return true;
}

void EditView::saveFinished( bool ok, const QString& error )
{
    saving_ = false;
    logData_->restoreFile();
    editor_->send( SCI_SETREADONLY, readOnly_ ? 1 : 0 );
    if ( !ok ) {
        showMessage( error.isEmpty() ? tr( "The file was not saved." ) : error, true );
        return;
    }
    editor_->send( SCI_SETSAVEPOINT );
    // The change of the file that follows is this one
    ownChange_ = true;
    QTimer::singleShot( 3000, this, [ this ] { ownChange_ = false; } );
    showMessage( tr( "Saved" ) );
    Q_EMIT saved();
}

void EditView::saveAs()
{
    if ( !loaded_ || isBusy() ) {
        return;
    }
    const auto target = QFileDialog::getSaveFileName( this, tr( "Save as" ), fileName_ );
    if ( target.isEmpty() ) {
        return;
    }
    QString error;
    const auto* data = reinterpret_cast<const char*>( editor_->send( SCI_GETCHARACTERPOINTER ) );
    const auto length = static_cast<std::size_t>( editor_->send( SCI_GETLENGTH ) );
    // The file of the log view itself: not open meanwhile (Windows)
    const auto sameFile = QFileInfo( target ) == QFileInfo( fileName_ );
    if ( sameFile ) {
        logData_->releaseFile();
    }
    QApplication::setOverrideCursor( Qt::WaitCursor );
    const auto ok = edit::writeFile( target, data, length, format_, &error );
    QApplication::restoreOverrideCursor();
    if ( sameFile ) {
        logData_->restoreFile();
    }
    showMessage( ok ? tr( "Saved as %1" ).arg( target ) : error, !ok );
}

bool EditView::confirmClose()
{
    if ( saving_ ) {
        future_.waitForFinished();
        updateProgress();
    }
    if ( !isModified() ) {
        return true;
    }
    const auto answer = QMessageBox::question(
        this, tr( "Edit mode" ),
        tr( "%1 has changes that are not saved." ).arg( QFileInfo( fileName_ ).fileName() ),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save );
    if ( answer == QMessageBox::Discard ) {
        return true;
    }
    if ( answer != QMessageBox::Save || !save() ) {
        return false;
    }
    future_.waitForFinished();
    updateProgress();
    return !isModified();
}

void EditView::fileChangedOnDisk()
{
    if ( !loaded_ || saving_ || ownChange_ ) {
        return;
    }
    showMessage( isModified() ? tr( "The file has changed on disk, reloading it discards the "
                                    "changes made here." )
                              : tr( "The file has changed on disk." ),
                 true );
    reloadButton_->show();
}

void EditView::reloadFile()
{
    if ( isModified()
         && QMessageBox::question( this, tr( "Reload" ), tr( "Discard the changes made here?" ),
                                   QMessageBox::Discard | QMessageBox::Cancel,
                                   QMessageBox::Cancel )
                != QMessageBox::Discard ) {
        return;
    }
    const auto line = editor_->send( SCI_LINEFROMPOSITION, editor_->send( SCI_GETCURRENTPOS ) );
    start( line );
}

void EditView::goToLine( qint64 line )
{
    if ( !loaded_ ) {
        startLine_ = line;
        return;
    }
    editor_->send( SCI_GOTOLINE, static_cast<uptr_t>( std::max<qint64>( line, 0 ) ) );
    // A third from the top, like the jumps of the log view
    const auto onScreen = editor_->send( SCI_LINESONSCREEN );
    editor_->send( SCI_SETFIRSTVISIBLELINE,
                   static_cast<uptr_t>( std::max<qint64>( 0, line - onScreen / 3 ) ) );
    editor_->setFocus();
}

/*
 * Find and replace
 */
void EditView::showFindBar()
{
    // The selection is searched when it is on one line
    const auto start = editor_->send( SCI_GETSELECTIONSTART );
    const auto end = editor_->send( SCI_GETSELECTIONEND );
    if ( loaded_ && end > start && end - start <= 1000 ) {
        QByteArray selected( static_cast<qsizetype>( end - start + 1 ), '\0' );
        editor_->send( SCI_GETSELTEXT, 0, reinterpret_cast<sptr_t>( selected.data() ) );
        const auto text = QString::fromUtf8( selected.constData() );
        if ( !text.contains( '\n' ) && !text.contains( '\r' ) ) {
            findEdit_->setText( text );
        }
    }
    findBar_->show();
    findEdit_->setFocus( Qt::ShortcutFocusReason );
    findEdit_->selectAll();
    updateHighlights();
}

void EditView::setFindText( const QString& text, bool caseSensitive, bool regex )
{
    caseButton_->setChecked( caseSensitive );
    regexButton_->setChecked( regex );
    findEdit_->setText( text );
    findBar_->show();
    updateHighlights();
}

QString EditView::findStatus() const
{
    return findStatus_->text();
}

void EditView::setFindStatus( const QString& status, bool warning )
{
    findStatus_->setText( status );
    findStatus_->setForegroundRole( warning ? QPalette::Link : QPalette::WindowText );
}

int EditView::searchFlags() const
{
    int flags = 0;
    if ( caseButton_->isChecked() ) {
        flags |= SCFIND_MATCHCASE;
    }
    if ( regexButton_->isChecked() ) {
        flags |= SCFIND_REGEXP;
        // The regular expressions of the C++ library are slow on a large document
        if ( !documentLarge() ) {
            flags |= SCFIND_CXX11REGEX;
        }
    }
    return flags;
}

void EditView::findNext()
{
    find( false );
}

void EditView::findPrevious()
{
    find( true );
}

void EditView::find( bool backward )
{
    if ( !findBar_->isVisible() ) {
        findBar_->show();
    }
    const auto text = findEdit_->text().toUtf8();
    if ( !loaded_ || text.isEmpty() ) {
        setFindStatus( {} );
        return;
    }

    editor_->send( SCI_SETSEARCHFLAGS, static_cast<uptr_t>( searchFlags() ) );
    const auto length = editor_->send( SCI_GETLENGTH );
    const auto search = [ this, &text ]( sptr_t from, sptr_t to ) {
        editor_->send( SCI_SETTARGETRANGE, static_cast<uptr_t>( from ), to );
        return editor_->send( SCI_SEARCHINTARGET, static_cast<uptr_t>( text.size() ),
                              reinterpret_cast<sptr_t>( text.constData() ) );
    };

    // From the selection (the current match), around the document. Backward, the target
    // goes from its end to its start. An invalid regular expression sets the status.
    editor_->send( SCI_SETSTATUS, SC_STATUS_OK );
    const auto start = editor_->send( SCI_GETSELECTIONSTART );
    const auto end = editor_->send( SCI_GETSELECTIONEND );
    auto found = backward ? search( start, 0 ) : search( end, length );
    auto wrapped = false;
    if ( found < 0 ) {
        found = backward ? search( length, end ) : search( 0, start );
        wrapped = true;
    }
    if ( editor_->send( SCI_GETSTATUS ) == SC_STATUS_WARN_REGEX ) {
        editor_->send( SCI_SETSTATUS, SC_STATUS_OK );
        setFindStatus( tr( "Invalid regular expression" ), true );
        return;
    }
    if ( found < 0 ) {
        setFindStatus( tr( "No match" ), true );
        updateHighlights();
        return;
    }

    const auto matchStart = editor_->send( SCI_GETTARGETSTART );
    const auto matchEnd = editor_->send( SCI_GETTARGETEND );
    editor_->send( SCI_SETSEL, static_cast<uptr_t>( matchStart ), matchEnd );
    editor_->send( SCI_SCROLLRANGE, static_cast<uptr_t>( matchEnd ), matchStart );
    const auto line = editor_->send( SCI_LINEFROMPOSITION, static_cast<uptr_t>( matchStart ) );
    setFindStatus( wrapped ? tr( "Line %1, wrapped" ).arg( line + 1 )
                           : tr( "Line %1" ).arg( line + 1 ) );
    updateHighlights();
}

void EditView::replaceNext( const QString& replacement )
{
    if ( !loaded_ || readOnly_ || findEdit_->text().isEmpty() ) {
        return;
    }
    const auto text = findEdit_->text().toUtf8();
    const auto replaceWith = replacement.toUtf8();
    const auto regex = regexButton_->isChecked();

    // The selection is replaced when it is a match, then the next one is found
    const auto start = editor_->send( SCI_GETSELECTIONSTART );
    const auto end = editor_->send( SCI_GETSELECTIONEND );
    editor_->send( SCI_SETSEARCHFLAGS, static_cast<uptr_t>( searchFlags() ) );
    editor_->send( SCI_SETTARGETRANGE, static_cast<uptr_t>( start ), end );
    if ( end > start
         && editor_->send( SCI_SEARCHINTARGET, static_cast<uptr_t>( text.size() ),
                           reinterpret_cast<sptr_t>( text.constData() ) )
                == start
         && editor_->send( SCI_GETTARGETEND ) == end ) {
        editor_->send( regex ? SCI_REPLACETARGETRE : SCI_REPLACETARGET,
                       static_cast<uptr_t>( replaceWith.size() ),
                       reinterpret_cast<sptr_t>( replaceWith.constData() ) );
        const auto after = editor_->send( SCI_GETTARGETEND );
        editor_->send( SCI_SETSEL, static_cast<uptr_t>( after ), after );
    }
    find( false );
}

int EditView::replaceAll( const QString& replacement )
{
    if ( !loaded_ || readOnly_ || findEdit_->text().isEmpty() ) {
        return 0;
    }
    const auto text = findEdit_->text().toUtf8();
    const auto replaceWith = replacement.toUtf8();
    const auto message = regexButton_->isChecked() ? SCI_REPLACETARGETRE : SCI_REPLACETARGET;

    // One undo step for all of them
    QApplication::setOverrideCursor( Qt::WaitCursor );
    editor_->send( SCI_SETSEARCHFLAGS, static_cast<uptr_t>( searchFlags() ) );
    editor_->send( SCI_BEGINUNDOACTION );
    int count = 0;
    sptr_t from = 0;
    while ( true ) {
        const auto length = editor_->send( SCI_GETLENGTH );
        editor_->send( SCI_SETTARGETRANGE, static_cast<uptr_t>( from ), length );
        if ( editor_->send( SCI_SEARCHINTARGET, static_cast<uptr_t>( text.size() ),
                            reinterpret_cast<sptr_t>( text.constData() ) )
             < 0 ) {
            break;
        }
        const auto matchEmpty
            = editor_->send( SCI_GETTARGETEND ) == editor_->send( SCI_GETTARGETSTART );
        editor_->send( message, static_cast<uptr_t>( replaceWith.size() ),
                       reinterpret_cast<sptr_t>( replaceWith.constData() ) );
        ++count;
        from = editor_->send( SCI_GETTARGETEND );
        if ( matchEmpty ) {
            // An empty match of a regular expression: one character further
            from = editor_->send( SCI_POSITIONAFTER, static_cast<uptr_t>( from ) );
            if ( from >= editor_->send( SCI_GETLENGTH ) ) {
                break;
            }
        }
    }
    editor_->send( SCI_ENDUNDOACTION );
    QApplication::restoreOverrideCursor();
    setFindStatus( tr( "%n replaced", nullptr, count ) );
    updateHighlights();
    return count;
}

// The matches of the find bar around the lines shown, on a yellow background
void EditView::updateHighlights()
{
    // Filling the indicators updates the editor, which asks again
    if ( !loaded_ || updatingHighlights_ ) {
        return;
    }
    const QScopedValueRollback<bool> updating( updatingHighlights_, true );

    const auto length = editor_->send( SCI_GETLENGTH );
    const auto text = findBar_->isVisible() ? findEdit_->text().toUtf8() : QByteArray{};
    const auto firstVisible = editor_->send( SCI_GETFIRSTVISIBLELINE );
    // Nothing moved or changed since the last time
    const auto key = std::make_tuple( text, searchFlags(), firstVisible, length,
                                      editor_->send( SCI_GETSELECTIONSTART ),
                                      editor_->send( SCI_GETSELECTIONEND ),
                                      editor_->send( SCI_GETMODIFY ) );
    if ( highlighted_ && *highlighted_ == key ) {
        return;
    }
    highlighted_ = key;

    editor_->send( SCI_SETINDICATORCURRENT, MatchIndicator );
    editor_->send( SCI_INDICATORCLEARRANGE, 0, length );
    if ( text.isEmpty() ) {
        return;
    }

    const auto firstLine
        = editor_->send( SCI_DOCLINEFROMVISIBLE, static_cast<uptr_t>( firstVisible ) );
    const auto lastLine = firstLine + editor_->send( SCI_LINESONSCREEN ) + 1;
    const auto from = editor_->send( SCI_POSITIONFROMLINE, static_cast<uptr_t>( firstLine ) );
    auto to = editor_->send( SCI_GETLINEENDPOSITION, static_cast<uptr_t>( lastLine ) );
    if ( to < 0 ) {
        to = length;
    }

    // Not the current match: shown as the selection
    const auto selectionStart = editor_->send( SCI_GETSELECTIONSTART );
    const auto selectionEnd = editor_->send( SCI_GETSELECTIONEND );
    // The target is used by the search, it is given back after
    const auto targetStart = editor_->send( SCI_GETTARGETSTART );
    const auto targetEnd = editor_->send( SCI_GETTARGETEND );
    editor_->send( SCI_SETSEARCHFLAGS, static_cast<uptr_t>( searchFlags() ) );
    auto position = from;
    while ( position < to ) {
        editor_->send( SCI_SETTARGETRANGE, static_cast<uptr_t>( position ), to );
        if ( editor_->send( SCI_SEARCHINTARGET, static_cast<uptr_t>( text.size() ),
                            reinterpret_cast<sptr_t>( text.constData() ) )
             < 0 ) {
            break;
        }
        const auto start = editor_->send( SCI_GETTARGETSTART );
        const auto end = editor_->send( SCI_GETTARGETEND );
        if ( end <= start ) {
            position = editor_->send( SCI_POSITIONAFTER, static_cast<uptr_t>( start ) );
            continue;
        }
        if ( start != selectionStart || end != selectionEnd ) {
            editor_->send( SCI_INDICATORFILLRANGE, static_cast<uptr_t>( start ), end - start );
        }
        position = end;
    }
    editor_->send( SCI_SETTARGETRANGE, static_cast<uptr_t>( targetStart ), targetEnd );
}
