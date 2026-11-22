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

#include "timeformatsdialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "configuration.h"
#include "timestamp.h"

TimeFormatsDialog::TimeFormatsDialog( const QString& sample, QWidget* parent )
    : QDialog( parent )
    , previous_( timestamps::customFormats() )
{
    setWindowTitle( tr( "Time Formats" ) );
    setSizeGripEnabled( true );

    auto* builtIn = new QTreeWidget( this );
    builtIn->setRootIsDecorated( false );
    builtIn->setHeaderLabels( { tr( "Built-in format" ), tr( "Examples" ) } );
    for ( const auto& format : timestamps::builtInFormats() ) {
        builtIn->addTopLevelItem( new QTreeWidgetItem( { format.name, format.example } ) );
    }
    builtIn->resizeColumnToContents( 0 );
    builtIn->setFixedHeight( builtIn->sizeHintForRow( 0 ) * 8 + builtIn->header()->height() );

    list_ = new QListWidget( this );
    list_->setEditTriggers( QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed
                            | QAbstractItemView::SelectedClicked );
    for ( const auto& format : previous_ ) {
        auto* item = new QListWidgetItem( format, list_ );
        item->setFlags( item->flags() | Qt::ItemIsEditable );
    }
    auto* add = new QPushButton( tr( "Add" ), this );
    remove_ = new QPushButton( tr( "Remove" ), this );
    connect( add, &QPushButton::clicked, this, [ this ] {
        auto* item = new QListWidgetItem( "yyyy-MM-dd HH:mm:ss.f", list_ );
        item->setFlags( item->flags() | Qt::ItemIsEditable );
        list_->setCurrentItem( item );
        list_->editItem( item );
        apply();
    } );
    connect( remove_, &QPushButton::clicked, this, [ this ] {
        delete list_->currentItem();
        apply();
    } );
    connect( list_, &QListWidget::itemChanged, this, &TimeFormatsDialog::apply );
    connect( list_, &QListWidget::currentRowChanged, this, &TimeFormatsDialog::updateButtons );

    auto* listButtons = new QVBoxLayout();
    listButtons->addWidget( add );
    listButtons->addWidget( remove_ );
    listButtons->addStretch();
    auto* listRow = new QHBoxLayout();
    listRow->addWidget( list_, 1 );
    listRow->addLayout( listButtons );

    auto* syntax = new QLabel(
        tr( "Your formats are looked for first. <b>yyyy</b> year, <b>yy</b> year in 2 digits, "
            "<b>MM</b>/<b>M</b> month, <b>MMM</b> Jan..Dec, <b>dd</b>/<b>d</b> day, "
            "<b>HH</b>/<b>H</b> hours, <b>mm</b> minutes, <b>ss</b> seconds, <b>f</b> the digits "
            "after the seconds; any other character as it is written. The hours and the minutes "
            "are needed, the date is not. E.g. <tt>dd.MM.yyyy HH:mm:ss.f</tt>, "
            "<tt>[yy/M/d H:mm:ss]</tt>." ),
        this );
    syntax->setWordWrap( true );

    sample_ = new QLineEdit( sample, this );
    sample_->setPlaceholderText( tr( "Paste a line of your log to try the formats" ) );
    result_ = new QLabel( this );
    result_->setWordWrap( true );
    result_->setTextInteractionFlags( Qt::TextSelectableByMouse );
    problem_ = new QLabel( this );
    problem_->setWordWrap( true );
    problem_->setStyleSheet( "color: #d03030" );
    connect( sample_, &QLineEdit::textChanged, this, &TimeFormatsDialog::apply );

    auto* buttons = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this );
    connect( buttons, &QDialogButtonBox::accepted, this, &TimeFormatsDialog::accept );
    connect( buttons, &QDialogButtonBox::rejected, this, &TimeFormatsDialog::reject );

    auto* layout = new QVBoxLayout( this );
    layout->addWidget( builtIn );
    layout->addWidget( new QLabel( tr( "<b>Your formats</b>" ), this ) );
    layout->addLayout( listRow, 1 );
    layout->addWidget( syntax );
    layout->addWidget( problem_ );
    layout->addWidget( new QLabel( tr( "<b>Try a line</b>" ), this ) );
    layout->addWidget( sample_ );
    layout->addWidget( result_ );
    layout->addWidget( buttons );

    resize( 760, 620 );
    apply();
}

QStringList TimeFormatsDialog::formats() const
{
    QStringList formats;
    for ( int i = 0; i < list_->count(); ++i ) {
        const auto text = list_->item( i )->text().trimmed();
        if ( !text.isEmpty() ) {
            formats.append( text );
        }
    }
    return formats;
}

void TimeFormatsDialog::setFormats( const QStringList& formats )
{
    const QSignalBlocker blocker( list_ );
    list_->clear();
    for ( const auto& format : formats ) {
        auto* item = new QListWidgetItem( format, list_ );
        item->setFlags( item->flags() | Qt::ItemIsEditable );
    }
    apply();
}

QString TimeFormatsDialog::result() const
{
    return result_->text();
}

void TimeFormatsDialog::setSample( const QString& line )
{
    sample_->setText( line );
}

void TimeFormatsDialog::updateButtons()
{
    remove_->setEnabled( list_->currentItem() != nullptr );
}

void TimeFormatsDialog::apply()
{
    // Applied right away, so the line to try uses them; Cancel puts the previous ones back
    QStringList problems;
    QStringList usable;
    for ( const auto& format : formats() ) {
        const auto problem = timestamps::checkCustomFormat( format );
        if ( problem.isEmpty() ) {
            usable.append( format );
        }
        else {
            problems.append( QString( "%1: %2" ).arg( format, problem ) );
        }
    }
    problem_->setText( problems.join( "\n" ) );
    problem_->setVisible( !problems.isEmpty() );
    timestamps::setCustomFormats( usable );

    const auto stamps = timestamps::inOrder( timestamps::parseLine( sample_->text() ) );
    QStringList found;
    for ( const auto& stamp : stamps ) {
        found.append( tr( "<b>%1</b> → %2" )
                          .arg( timestamps::formatName( stamp.format ).toHtmlEscaped(),
                                timestamps::formatTimestamp( stamp ).toHtmlEscaped() ) );
    }
    result_->setText( sample_->text().isEmpty() ? QString{}
                      : found.isEmpty()         ? tr( "No time stamp found in this line" )
                                                : found.join( "<br>" ) );
    updateButtons();
}

void TimeFormatsDialog::accept()
{
    apply();
    auto& config = Configuration::get();
    config.setCustomTimeFormats( timestamps::customFormats() );
    config.save();
    Q_EMIT formatsChanged();
    QDialog::accept();
}

void TimeFormatsDialog::reject()
{
    timestamps::setCustomFormats( previous_ );
    QDialog::reject();
}
