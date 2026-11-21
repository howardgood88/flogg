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

#include "filemerge.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QVBoxLayout>
#include <QtConcurrent>

#include "readablesize.h"

namespace filemerge {

namespace {
constexpr qint64 BlockSize = 1024 * 1024;

QString translate( const char* text )
{
    return QApplication::translate( "filemerge", text );
}

QString canonical( const QString& path )
{
    const QFileInfo info( path );
    const auto result = info.canonicalFilePath();
    return result.isEmpty() ? info.absoluteFilePath() : result;
}
} // namespace

QString merge( const QStringList& inputs, const QString& output, const Options& options,
               AtomicFlag& cancel, const std::function<void( int percent )>& progress )
{
    if ( inputs.isEmpty() ) {
        return translate( "No files to merge" );
    }
    const auto outputPath = canonical( output );
    qint64 total = 0;
    for ( const auto& input : inputs ) {
        if ( canonical( input ) == outputPath ) {
            return translate( "The merged file can not be one of the files merged: %1" )
                .arg( QDir::toNativeSeparators( input ) );
        }
        const QFileInfo info( input );
        if ( !info.isFile() || !info.isReadable() ) {
            return translate( "Can not read %1" ).arg( QDir::toNativeSeparators( input ) );
        }
        total += info.size();
    }

    QSaveFile file( output );
    if ( !file.open( QIODevice::WriteOnly ) ) {
        return translate( "Can not write %1: %2" )
            .arg( QDir::toNativeSeparators( output ), file.errorString() );
    }

    qint64 done = 0;
    int reported = -1;
    for ( int i = 0; i < inputs.size(); ++i ) {
        QFile input( inputs[ i ] );
        if ( !input.open( QIODevice::ReadOnly ) ) {
            file.cancelWriting();
            return translate( "Can not read %1" ).arg( QDir::toNativeSeparators( inputs[ i ] ) );
        }
        if ( options.fileNameLines ) {
            if ( i > 0 ) {
                file.write( "\n" );
            }
            file.write( "==> " + QFileInfo( inputs[ i ] ).fileName().toUtf8() + " <==\n" );
        }

        bool first = true;
        QByteArray last;
        while ( !input.atEnd() ) {
            if ( cancel ) {
                file.cancelWriting();
                return translate( "Stopped" );
            }
            auto block = input.read( BlockSize );
            if ( block.isEmpty() && input.error() != QFile::NoError ) {
                file.cancelWriting();
                return translate( "Can not read %1: %2" )
                    .arg( QDir::toNativeSeparators( inputs[ i ] ), input.errorString() );
            }
            done += block.size();
            // One byte order mark at the start of the merged file at most
            if ( first && ( i > 0 || options.fileNameLines ) && block.startsWith( "\xEF\xBB\xBF" ) ) {
                block.remove( 0, 3 );
            }
            first = false;
            if ( file.write( block ) != block.size() ) {
                file.cancelWriting();
                return translate( "Can not write %1: %2" )
                    .arg( QDir::toNativeSeparators( output ), file.errorString() );
            }
            if ( !block.isEmpty() ) {
                last = block.right( 2 );
            }
            if ( progress && total > 0 ) {
                const auto percent = static_cast<int>( done * 100 / total );
                if ( percent > reported ) {
                    reported = percent;
                    progress( percent );
                }
            }
        }

        // The next file starts on a line of its own
        if ( !last.isEmpty() && !last.endsWith( '\n' ) && i + 1 < inputs.size() ) {
            input.seek( qMax<qint64>( 0, input.size() - BlockSize ) );
            const auto tail = input.read( BlockSize );
            file.write( tail.contains( "\r\n" ) ? "\r\n" : "\n" );
        }
    }

    if ( !file.commit() ) {
        return translate( "Can not write %1: %2" )
            .arg( QDir::toNativeSeparators( output ), file.errorString() );
    }
    return {};
}

QString defaultOutput( const QStringList& inputs )
{
    const auto dir = inputs.isEmpty() ? QDir::homePath() : QFileInfo( inputs.front() ).absolutePath();
    auto path = dir + "/merged.log";
    for ( int n = 2; QFileInfo::exists( path ); ++n ) {
        path = dir + QString( "/merged-%1.log" ).arg( n );
    }
    return path;
}

} // namespace filemerge

MergeFilesDialog::MergeFilesDialog( const QStringList& files, QWidget* parent )
    : QDialog( parent )
{
    setWindowTitle( tr( "Merge Files" ) );
    setSizeGripEnabled( true );

    auto* hint = new QLabel( tr( "The files are written one after the other, in this order. "
                                 "Drag them or use the buttons to change it." ),
                             this );
    hint->setWordWrap( true );

    list_ = new QListWidget( this );
    list_->setSelectionMode( QAbstractItemView::ExtendedSelection );
    list_->setDragDropMode( QAbstractItemView::InternalMove );
    for ( const auto& file : files ) {
        auto* item = new QListWidgetItem( QFileInfo( file ).fileName(), list_ );
        item->setData( Qt::UserRole, file );
        item->setToolTip( QDir::toNativeSeparators( file ) + "\n"
                          + readableSize( static_cast<uint64_t>( QFileInfo( file ).size() ) ) );
    }

    auto* add = new QPushButton( tr( "Add..." ), this );
    up_ = new QPushButton( tr( "Up" ), this );
    down_ = new QPushButton( tr( "Down" ), this );
    remove_ = new QPushButton( tr( "Remove" ), this );
    connect( add, &QPushButton::clicked, this, &MergeFilesDialog::addFiles );
    connect( up_, &QPushButton::clicked, this, [ this ] { moveSelected( -1 ); } );
    connect( down_, &QPushButton::clicked, this, [ this ] { moveSelected( 1 ); } );
    connect( remove_, &QPushButton::clicked, this, [ this ] {
        qDeleteAll( list_->selectedItems() );
        updateButtons();
    } );
    connect( list_, &QListWidget::itemSelectionChanged, this, &MergeFilesDialog::updateButtons );
    connect( list_->model(), &QAbstractItemModel::rowsMoved, this,
             &MergeFilesDialog::updateButtons );

    auto* side = new QVBoxLayout();
    side->addWidget( add );
    side->addWidget( up_ );
    side->addWidget( down_ );
    side->addWidget( remove_ );
    side->addStretch();
    auto* listRow = new QHBoxLayout();
    listRow->addWidget( list_, 1 );
    listRow->addLayout( side );

    output_ = new QLineEdit( filemerge::defaultOutput( files ), this );
    auto* browse = new QPushButton( tr( "Browse..." ), this );
    connect( browse, &QPushButton::clicked, this, [ this ] {
        const auto fileName = QFileDialog::getSaveFileName( this, tr( "Merged file" ),
                                                            output_->text() );
        if ( !fileName.isEmpty() ) {
            output_->setText( fileName );
        }
    } );
    auto* outputRow = new QHBoxLayout();
    outputRow->addWidget( new QLabel( tr( "Write to:" ), this ) );
    outputRow->addWidget( output_, 1 );
    outputRow->addWidget( browse );

    fileNames_ = new QCheckBox( tr( "A line with the file name before each file" ), this );

    message_ = new QLabel( this );
    message_->setWordWrap( true );
    progress_ = new QProgressBar( this );
    progress_->setRange( 0, 100 );
    progress_->hide();

    auto* buttons = new QDialogButtonBox( QDialogButtonBox::Cancel, this );
    mergeButton_ = buttons->addButton( tr( "Merge and Open" ), QDialogButtonBox::AcceptRole );
    connect( buttons, &QDialogButtonBox::accepted, this, &MergeFilesDialog::merge );
    connect( buttons, &QDialogButtonBox::rejected, this, [ this ] {
        if ( cancel_ ) {
            cancel_->set();
        }
        if ( !isRunning() ) {
            reject();
        }
    } );

    auto* layout = new QVBoxLayout( this );
    layout->addWidget( hint );
    layout->addLayout( listRow, 1 );
    layout->addLayout( outputRow );
    layout->addWidget( fileNames_ );
    layout->addWidget( message_ );
    layout->addWidget( progress_ );
    layout->addWidget( buttons );

    resize( 560, 420 );
    updateButtons();
}

MergeFilesDialog::~MergeFilesDialog()
{
    if ( watcher_ ) {
        cancel_->set();
        watcher_->waitForFinished();
    }
}

QStringList MergeFilesDialog::files() const
{
    QStringList files;
    for ( int i = 0; i < list_->count(); ++i ) {
        files.append( list_->item( i )->data( Qt::UserRole ).toString() );
    }
    return files;
}

QString MergeFilesDialog::output() const
{
    return output_->text();
}

void MergeFilesDialog::setOutput( const QString& output )
{
    output_->setText( output );
}

void MergeFilesDialog::addFiles()
{
    const auto dir = list_->count() > 0
                         ? QFileInfo( list_->item( list_->count() - 1 )->data( Qt::UserRole ).toString() )
                               .absolutePath()
                         : QString{};
    for ( const auto& file : QFileDialog::getOpenFileNames( this, tr( "Add files" ), dir ) ) {
        auto* item = new QListWidgetItem( QFileInfo( file ).fileName(), list_ );
        item->setData( Qt::UserRole, file );
        item->setToolTip( QDir::toNativeSeparators( file ) );
    }
    updateButtons();
}

void MergeFilesDialog::moveSelected( int delta )
{
    std::vector<int> rows;
    for ( auto* item : list_->selectedItems() ) {
        rows.push_back( list_->row( item ) );
    }
    std::sort( rows.begin(), rows.end() );
    if ( rows.empty() || ( delta < 0 && rows.front() == 0 )
         || ( delta > 0 && rows.back() == list_->count() - 1 ) ) {
        return;
    }
    if ( delta > 0 ) {
        std::reverse( rows.begin(), rows.end() );
    }
    for ( const auto row : rows ) {
        auto* item = list_->takeItem( row );
        list_->insertItem( row + delta, item );
        item->setSelected( true );
    }
    updateButtons();
}

void MergeFilesDialog::updateButtons()
{
    const auto selected = list_->selectedItems();
    up_->setEnabled( !selected.isEmpty() && !list_->item( 0 )->isSelected() );
    down_->setEnabled( !selected.isEmpty() && !list_->item( list_->count() - 1 )->isSelected() );
    remove_->setEnabled( !selected.isEmpty() );
    mergeButton_->setEnabled( list_->count() >= 2 && !isRunning() );
}

void MergeFilesDialog::merge()
{
    if ( isRunning() ) {
        return;
    }
    const auto inputs = files();
    const auto output = output_->text().trimmed();
    if ( output.isEmpty() ) {
        message_->setText( tr( "Choose where to write the merged file" ) );
        return;
    }

    cancel_ = std::make_shared<AtomicFlag>();
    progress_->setValue( 0 );
    progress_->show();
    message_->clear();
    mergeButton_->setEnabled( false );

    QPointer<QProgressBar> bar = progress_;
    const std::function<void( int )> progress = [ bar ]( int percent ) {
        QMetaObject::invokeMethod(
            qApp, [ bar, percent ] { if ( bar ) { bar->setValue( percent ); } },
            Qt::QueuedConnection );
    };
    filemerge::Options options;
    options.fileNameLines = fileNames_->isChecked();

    watcher_ = new QFutureWatcher<QString>( this );
    connect( watcher_, &QFutureWatcher<QString>::finished, this, [ this, output ] {
        const auto error = watcher_->result();
        watcher_->deleteLater();
        watcher_ = nullptr;
        progress_->hide();
        updateButtons();
        if ( !error.isEmpty() ) {
            message_->setText( error );
            return;
        }
        Q_EMIT merged( output );
        accept();
    } );
    watcher_->setFuture( QtConcurrent::run( [ inputs, output, options, cancel = cancel_, progress ] {
        return filemerge::merge( inputs, output, options, *cancel, progress );
    } ) );
}
