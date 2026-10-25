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

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QToolButton>

#include "filtereditdialog.h"

FilterEditDialog::FilterEditDialog( const FilterNode& filter, QWidget* parent )
    : QDialog( parent )
    , filter_( filter )
{
    setWindowTitle( filter.number > 0
                        ? tr( "Edit filter %1" ).arg( filters::numberText( filter ) )
                        : tr( "New filter" ) );

    patternEdit_ = new QLineEdit( filter.pattern );
    // Several lines for a detailed description, Tab still moves to the next field
    descriptionEdit_ = new QPlainTextEdit( filter.name );
    descriptionEdit_->setTabChangesFocus( true );
    descriptionEdit_->setFixedHeight( descriptionEdit_->fontMetrics().lineSpacing() * 4
                                      + 2 * descriptionEdit_->frameWidth() + 8 );

    regexCheck_ = new QCheckBox( tr( "Regular expression" ) );
    regexCheck_->setChecked( filter.useRegex );
    caseCheck_ = new QCheckBox( tr( "Case sensitive" ) );
    caseCheck_->setChecked( filter.caseSensitive );
    excludeCheck_ = new QCheckBox( tr( "Exclude matching lines" ) );
    excludeCheck_->setChecked( filter.exclude );

    colorModeCombo_ = new QComboBox;
    colorModeCombo_->addItem( tr( "Whole line" ) );
    colorModeCombo_->addItem( tr( "Matched text only" ) );
    colorModeCombo_->setCurrentIndex( filter.colorMode == FilterNode::ColorMode::MatchOnly ? 1
                                                                                         : 0 );

    // Colour button followed by a button resetting the colour to none
    const auto makeColorButton = [ this ]( QColor& color, QToolButton*& clearButton ) {
        auto* button = new QToolButton;
        clearButton = new QToolButton;
        clearButton->setText( tr( "Clear" ) );
        clearButton->setToolTip( tr( "No color" ) );
        connect( button, &QToolButton::clicked, this,
                 [ this, &color, button ] { chooseColor( color, button ); } );
        connect( clearButton, &QToolButton::clicked, this, [ this, &color, button ] {
            color = QColor{};
            updateColorButton( button, color );
            updatePreview();
        } );
        return button;
    };
    foreButton_ = makeColorButton( filter_.foreColor, clearForeButton_ );
    backButton_ = makeColorButton( filter_.backColor, clearBackButton_ );
    updateColorButton( foreButton_, filter_.foreColor );
    updateColorButton( backButton_, filter_.backColor );

    auto* colorsLayout = new QHBoxLayout;
    colorsLayout->addWidget( new QLabel( tr( "Text" ) ) );
    colorsLayout->addWidget( foreButton_ );
    colorsLayout->addWidget( clearForeButton_ );
    colorsLayout->addSpacing( 12 );
    colorsLayout->addWidget( new QLabel( tr( "Background" ) ) );
    colorsLayout->addWidget( backButton_ );
    colorsLayout->addWidget( clearBackButton_ );
    colorsLayout->addStretch();

    preview_ = new QLabel;
    preview_->setAutoFillBackground( true );
    preview_->setMargin( 4 );

    errorLabel_ = new QLabel;
    errorLabel_->setStyleSheet( "color: red" );
    errorLabel_->setWordWrap( true );

    buttons_ = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel );
    connect( buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept );
    connect( buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject );

    auto* form = new QFormLayout;
    if ( filter.number > 0 ) {
        form->addRow( tr( "Number:" ), new QLabel( filters::numberText( filter ) ) );
    }
    form->addRow( tr( "Pattern:" ), patternEdit_ );
    form->addRow( tr( "Description:" ), descriptionEdit_ );
    form->addRow( QString{}, regexCheck_ );
    form->addRow( QString{}, caseCheck_ );
    form->addRow( QString{}, excludeCheck_ );
    form->addRow( tr( "Colors:" ), colorsLayout );
    form->addRow( tr( "Apply colors to:" ), colorModeCombo_ );
    form->addRow( tr( "Preview:" ), preview_ );
    form->addRow( errorLabel_ );
    form->addRow( buttons_ );
    setLayout( form );

    connect( patternEdit_, &QLineEdit::textChanged, this, &FilterEditDialog::updatePreview );
    connect( regexCheck_, &QCheckBox::toggled, this, &FilterEditDialog::updatePreview );
    connect( excludeCheck_, &QCheckBox::toggled, this, &FilterEditDialog::updatePreview );

    resize( 480, sizeHint().height() );
    updatePreview();
}

FilterNode FilterEditDialog::filter() const
{
    auto result = filter_;
    result.pattern = patternEdit_->text();
    result.name = descriptionEdit_->toPlainText();
    result.useRegex = regexCheck_->isChecked();
    result.caseSensitive = caseCheck_->isChecked();
    result.exclude = excludeCheck_->isChecked();
    result.colorMode = colorModeCombo_->currentIndex() == 1 ? FilterNode::ColorMode::MatchOnly
                                                            : FilterNode::ColorMode::WholeLine;
    return result;
}

void FilterEditDialog::chooseColor( QColor& color, QToolButton* button )
{
    const auto newColor
        = QColorDialog::getColor( color.isValid() ? color : QColor( Qt::white ), this );
    if ( newColor.isValid() ) {
        color = newColor;
        updateColorButton( button, color );
        updatePreview();
    }
}

void FilterEditDialog::updateColorButton( QToolButton* button, const QColor& color )
{
    QPixmap pixmap( 24, 14 );
    if ( color.isValid() ) {
        pixmap.fill( color );
    }
    else {
        pixmap.fill( Qt::transparent );
    }
    button->setIcon( QIcon( pixmap ) );
    button->setText( color.isValid() ? color.name() : tr( "None" ) );
    button->setToolButtonStyle( Qt::ToolButtonTextBesideIcon );

    ( button == foreButton_ ? clearForeButton_ : clearBackButton_ )->setEnabled( color.isValid() );
}

void FilterEditDialog::updatePreview()
{
    const auto pattern = patternEdit_->text();
    preview_->setText( pattern.isEmpty() ? tr( "(empty pattern)" ) : pattern );

    auto palette = this->palette();
    // Same text colour as the views
    const auto foreColor = filters::displayForeColor( filter_ );
    if ( foreColor.isValid() ) {
        palette.setColor( QPalette::WindowText, foreColor );
    }
    if ( filter_.backColor.isValid() ) {
        palette.setColor( QPalette::Window, filter_.backColor );
    }
    preview_->setPalette( palette );

    QString error;
    if ( pattern.isEmpty() ) {
        error = tr( "Pattern must not be empty" );
    }
    else if ( regexCheck_->isChecked() ) {
        const QRegularExpression regex( pattern );
        if ( !regex.isValid() ) {
            error = tr( "Invalid regular expression: %1" ).arg( regex.errorString() );
        }
    }

    errorLabel_->setText( error );
    errorLabel_->setVisible( !error.isEmpty() );
    buttons_->button( QDialogButtonBox::Ok )->setEnabled( error.isEmpty() );
}
