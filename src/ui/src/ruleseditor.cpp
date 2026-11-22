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

#include "ruleseditor.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSpinBox>
#include <QSplitter>
#include <QVBoxLayout>

using analysis::Rule;

namespace {
QColor severityColor( const QString& severity )
{
    return severity == "error"  ? QColor( 0xd0, 0x30, 0x30 )
           : severity == "info" ? QColor( 0x30, 0x70, 0xd0 )
                                : QColor( 0xc0, 0x80, 0x00 );
}
} // namespace

RulesEditor::RulesEditor( const QString& fileName, QWidget* parent )
    : QDialog( parent )
    , fileName_( fileName )
{
    setWindowTitle( tr( "Edit Rules" ) );
    setSizeGripEnabled( true );

    if ( !fileName_.isEmpty() ) {
        QFile file( fileName_ );
        if ( file.open( QIODevice::ReadOnly ) ) {
            rules_ = analysis::parseRules( file.readAll() ).rules;
        }
    }

    // The list of the rules
    list_ = new QListWidget( this );
    list_->setMinimumWidth( 220 );
    auto* add = new QPushButton( tr( "Add" ), this );
    duplicate_ = new QPushButton( tr( "Duplicate" ), this );
    remove_ = new QPushButton( tr( "Remove" ), this );
    up_ = new QPushButton( tr( "Up" ), this );
    down_ = new QPushButton( tr( "Down" ), this );
    connect( add, &QPushButton::clicked, this, [ this ] {
        Rule rule;
        rule.name = tr( "New rule" );
        addRule( rule );
        name_->setFocus();
        name_->selectAll();
    } );
    connect( duplicate_, &QPushButton::clicked, this, [ this ] {
        if ( current_ >= 0 ) {
            auto rule = rules_[ static_cast<size_t>( current_ ) ];
            rule.name += tr( " (copy)" );
            addRule( rule );
        }
    } );
    connect( remove_, &QPushButton::clicked, this, [ this ] {
        if ( current_ < 0 ) {
            return;
        }
        rules_.erase( rules_.begin() + current_ );
        const auto row = qMin( current_, static_cast<int>( rules_.size() ) - 1 );
        current_ = -1;
        updateList();
        showRule( row );
    } );
    connect( up_, &QPushButton::clicked, this, [ this ] { move( -1 ); } );
    connect( down_, &QPushButton::clicked, this, [ this ] { move( 1 ); } );
    connect( list_, &QListWidget::currentRowChanged, this, &RulesEditor::showRule );

    auto* listButtons = new QHBoxLayout();
    for ( auto* button : { add, duplicate_, remove_, up_, down_ } ) {
        listButtons->addWidget( button );
    }
    auto* left = new QWidget( this );
    auto* leftLayout = new QVBoxLayout( left );
    leftLayout->setContentsMargins( 0, 0, 0, 0 );
    leftLayout->addWidget( list_, 1 );
    leftLayout->addLayout( listButtons );

    // The rule selected
    form_ = new QWidget( this );
    name_ = new QLineEdit( form_ );
    description_ = new QLineEdit( form_ );
    description_->setPlaceholderText( tr( "What the problem is, what to do" ) );
    type_ = new QComboBox( form_ );
    type_->addItem( tr( "Match: a line" ), static_cast<int>( Rule::Type::Match ) );
    type_->addItem( tr( "Sequence: lines in this order" ), static_cast<int>( Rule::Type::Sequence ) );
    type_->addItem( tr( "Absence: a line not followed by another" ),
                    static_cast<int>( Rule::Type::Absence ) );
    type_->addItem( tr( "Count: a line repeated" ), static_cast<int>( Rule::Type::Count ) );
    typeHint_ = new QLabel( form_ );
    typeHint_->setWordWrap( true );
    severity_ = new QComboBox( form_ );
    severity_->addItem( tr( "Error" ), "error" );
    severity_->addItem( tr( "Warning" ), "warning" );
    severity_->addItem( tr( "Info" ), "info" );
    patterns_ = new QPlainTextEdit( form_ );
    patterns_->setTabChangesFocus( true );
    patterns_->setLineWrapMode( QPlainTextEdit::NoWrap );
    auto font = patterns_->font();
    font.setFamily( "monospace" );
    font.setStyleHint( QFont::Monospace );
    patterns_->setFont( font );
    patterns_->setFixedHeight( patterns_->fontMetrics().lineSpacing() * 5 + 12 );
    patternsHint_ = new QLabel( form_ );
    patternsHint_->setWordWrap( true );
    caseSensitive_ = new QCheckBox( tr( "Match case" ), form_ );
    within_ = new QCheckBox( tr( "Within" ), form_ );
    seconds_ = new QDoubleSpinBox( form_ );
    seconds_->setRange( 0.001, 86400 );
    seconds_->setDecimals( 3 );
    seconds_->setValue( 5 );
    seconds_->setSuffix( tr( " s" ) );
    count_ = new QSpinBox( form_ );
    count_->setRange( 1, 1000000 );
    countLabel_ = new QLabel( tr( "At least" ), form_ );
    link_ = new QLineEdit( form_ );
    link_->setPlaceholderText( tr( "https://... (a runbook, a ticket), opened by a double click" ) );
    problem_ = new QLabel( form_ );
    problem_->setWordWrap( true );
    problem_->setStyleSheet( "color: #d03030" );

    auto* time = new QHBoxLayout();
    time->addWidget( within_ );
    time->addWidget( seconds_ );
    time->addSpacing( 16 );
    time->addWidget( countLabel_ );
    time->addWidget( count_ );
    time->addStretch();

    auto* formColumn = new QVBoxLayout( form_ );
    auto* form = new QFormLayout();
    formColumn->addLayout( form );
    formColumn->addStretch( 1 );
    for ( auto* hint : { typeHint_, patternsHint_ } ) {
        auto hintPalette = hint->palette();
        hintPalette.setColor( QPalette::WindowText,
                              hintPalette.color( QPalette::PlaceholderText ) );
        hint->setPalette( hintPalette );
    }
    form->addRow( tr( "Name:" ), name_ );
    form->addRow( tr( "Description:" ), description_ );
    form->addRow( tr( "Kind:" ), type_ );
    form->addRow( {}, typeHint_ );
    form->addRow( tr( "Severity:" ), severity_ );
    form->addRow( tr( "Patterns:" ), patterns_ );
    form->addRow( {}, patternsHint_ );
    form->addRow( {}, caseSensitive_ );
    form->addRow( tr( "Time:" ), time );
    form->addRow( tr( "Link:" ), link_ );
    form->addRow( {}, problem_ );

    for ( auto* edit : { name_, description_, link_ } ) {
        connect( edit, &QLineEdit::textEdited, this, &RulesEditor::storeRule );
    }
    connect( patterns_, &QPlainTextEdit::textChanged, this, &RulesEditor::storeRule );
    connect( type_, QOverload<int>::of( &QComboBox::currentIndexChanged ), this,
             &RulesEditor::storeRule );
    connect( severity_, QOverload<int>::of( &QComboBox::currentIndexChanged ), this,
             &RulesEditor::storeRule );
    connect( caseSensitive_, &QCheckBox::toggled, this, &RulesEditor::storeRule );
    connect( within_, &QCheckBox::toggled, this, &RulesEditor::storeRule );
    connect( seconds_, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), this,
             &RulesEditor::storeRule );
    connect( count_, QOverload<int>::of( &QSpinBox::valueChanged ), this, &RulesEditor::storeRule );

    auto* splitter = new QSplitter( this );
    splitter->addWidget( left );
    splitter->addWidget( form_ );
    splitter->setStretchFactor( 1, 1 );

    file_ = new QLabel( this );
    auto* buttons = new QDialogButtonBox( QDialogButtonBox::Close, this );
    auto* saveAndCheck = buttons->addButton( tr( "Save and Check" ), QDialogButtonBox::ActionRole );
    auto* saveButton = buttons->addButton( tr( "Save" ), QDialogButtonBox::ActionRole );
    auto* saveAsButton = buttons->addButton( tr( "Save As..." ), QDialogButtonBox::ActionRole );
    saveAndCheck->setToolTip( tr( "Saves the rules, then checks them on the current tab" ) );
    connect( saveButton, &QPushButton::clicked, this, [ this ] {
        if ( fileName_.isEmpty() ? saveAs() : save( fileName_ ) ) {
            Q_EMIT saved( fileName_, false );
        }
    } );
    connect( saveAsButton, &QPushButton::clicked, this, [ this ] {
        if ( saveAs() ) {
            Q_EMIT saved( fileName_, false );
        }
    } );
    connect( saveAndCheck, &QPushButton::clicked, this, [ this ] {
        if ( fileName_.isEmpty() ? saveAs() : save( fileName_ ) ) {
            Q_EMIT saved( fileName_, true );
        }
    } );
    connect( buttons, &QDialogButtonBox::rejected, this, &QDialog::reject );

    auto* layout = new QVBoxLayout( this );
    layout->addWidget( splitter, 1 );
    layout->addWidget( file_ );
    layout->addWidget( buttons );

    resize( 900, 560 );
    updateList();
    showRule( rules_.empty() ? -1 : 0 );
}

void RulesEditor::addRule( Rule rule )
{
    const auto row = current_ >= 0 ? current_ + 1 : static_cast<int>( rules_.size() );
    rules_.insert( rules_.begin() + row, std::move( rule ) );
    current_ = -1;
    updateList();
    showRule( row );
}

void RulesEditor::addRuleFor( const QString& text )
{
    Rule rule;
    rule.name = tr( "New rule" );
    // Only the characters special in a regular expression, so the pattern stays readable
    QString pattern;
    for ( const auto c : text.trimmed() ) {
        if ( QStringLiteral( "\\^$.|?*+()[]{}" ).contains( c ) ) {
            pattern += u'\\';
        }
        pattern += c;
    }
    rule.patterns.append( pattern );
    addRule( rule );
    name_->setFocus();
    name_->selectAll();
}

void RulesEditor::updateList()
{
    const QSignalBlocker blocker( list_ );
    list_->clear();
    for ( const auto& rule : rules_ ) {
        auto* item = new QListWidgetItem( rule.name.isEmpty() ? tr( "(no name)" ) : rule.name,
                                          list_ );
        item->setForeground( severityColor( rule.severity ) );
        const auto problem = analysis::checkRule( rule );
        if ( !problem.isEmpty() ) {
            item->setText( item->text() + "  ⚠" );
            item->setToolTip( problem );
        }
    }
    list_->setCurrentRow( current_ );
    remove_->setEnabled( current_ >= 0 );
    duplicate_->setEnabled( current_ >= 0 );
    up_->setEnabled( current_ > 0 );
    down_->setEnabled( current_ >= 0 && current_ + 1 < static_cast<int>( rules_.size() ) );
    file_->setText( fileName_.isEmpty() ? tr( "New rule file, not saved yet" )
                                        : QDir::toNativeSeparators( fileName_ ) );
}

void RulesEditor::showRule( int row )
{
    current_ = row;
    form_->setEnabled( row >= 0 );
    loading_ = true;
    if ( row >= 0 ) {
        const auto& rule = rules_[ static_cast<size_t>( row ) ];
        name_->setText( rule.name );
        description_->setText( rule.description );
        type_->setCurrentIndex( type_->findData( static_cast<int>( rule.type ) ) );
        const auto severity = severity_->findData( rule.severity );
        severity_->setCurrentIndex( severity >= 0 ? severity : 1 );
        patterns_->setPlainText( rule.patterns.join( "\n" ) );
        caseSensitive_->setChecked( rule.caseSensitive );
        within_->setChecked( rule.withinSeconds.has_value() );
        seconds_->setValue( rule.withinSeconds.value_or( 5 ) );
        count_->setValue( rule.count );
        link_->setText( rule.link );
    }
    else {
        for ( auto* edit : { name_, description_, link_ } ) {
            edit->clear();
        }
        patterns_->clear();
    }
    loading_ = false;
    updateForm();
    updateList();
}

void RulesEditor::storeRule()
{
    if ( loading_ || current_ < 0 ) {
        return;
    }
    auto& rule = rules_[ static_cast<size_t>( current_ ) ];
    rule.name = name_->text();
    rule.description = description_->text();
    rule.type = static_cast<Rule::Type>( type_->currentData().toInt() );
    rule.severity = severity_->currentData().toString();
    rule.patterns.clear();
    for ( const auto& line : patterns_->toPlainText().split( '\n' ) ) {
        if ( !line.trimmed().isEmpty() ) {
            rule.patterns.append( line );
        }
    }
    rule.caseSensitive = caseSensitive_->isChecked();
    rule.withinSeconds
        = within_->isChecked() ? std::optional<double>( seconds_->value() ) : std::nullopt;
    rule.count = count_->value();
    rule.link = link_->text().trimmed();
    updateForm();

    // The name and the warning of the row, without rebuilding the list as it is typed
    if ( auto* item = list_->item( current_ ) ) {
        const auto problem = analysis::checkRule( rule );
        item->setText( ( rule.name.isEmpty() ? tr( "(no name)" ) : rule.name )
                       + ( problem.isEmpty() ? QString{} : QString( "  ⚠" ) ) );
        item->setToolTip( problem );
        item->setForeground( severityColor( rule.severity ) );
    }
}

void RulesEditor::updateForm()
{
    const auto type = static_cast<Rule::Type>( type_->currentData().toInt() );
    switch ( type ) {
    case Rule::Type::Match:
        typeHint_->setText( tr( "Found on every line matching the pattern." ) );
        patternsHint_->setText( tr( "1 regular expression, e.g. FATAL EXCEPTION|ANR in" ) );
        break;
    case Rule::Type::Sequence:
        typeHint_->setText( tr( "Found when the patterns come in this order (within the time, "
                                "if set). The groups in parentheses tie the lines of one case "
                                "together, e.g. the same user." ) );
        patternsHint_->setText(
            tr( "2 or more, one per line, in their order, e.g.\n"
                "login user=(\\w+)\nlogout user=(\\w+)" ) );
        break;
    case Rule::Type::Absence:
        typeHint_->setText( tr( "Found when the first pattern is not followed by the second one "
                                "within the time (by the end of the file without a time). The "
                                "groups in parentheses tie the two lines, e.g. the same id." ) );
        patternsHint_->setText( tr( "2, one per line: what starts, then what must follow, e.g.\n"
                                    "connect start id=(\\d+)\nconnected id=(\\d+)" ) );
        break;
    case Rule::Type::Count:
        typeHint_->setText( tr( "Found when the pattern comes at least this many times (within "
                                "the time, if set), counted apart for each value of its groups." ) );
        patternsHint_->setText( tr( "1 regular expression, e.g. retry request=(\\w+)" ) );
        break;
    }
    count_->setVisible( type == Rule::Type::Count );
    countLabel_->setVisible( type == Rule::Type::Count );
    count_->setSuffix( tr( " times" ) );
    seconds_->setEnabled( within_->isChecked() );

    problem_->setText( current_ >= 0 ? analysis::checkRule( rules_[ static_cast<size_t>( current_ ) ] )
                                     : QString{} );
}

void RulesEditor::move( int delta )
{
    const auto to = current_ + delta;
    if ( current_ < 0 || to < 0 || to >= static_cast<int>( rules_.size() ) ) {
        return;
    }
    std::swap( rules_[ static_cast<size_t>( current_ ) ], rules_[ static_cast<size_t>( to ) ] );
    current_ = to;
    updateList();
}

bool RulesEditor::save( const QString& fileName )
{
    for ( size_t r = 0; r < rules_.size(); ++r ) {
        const auto problem = analysis::checkRule( rules_[ r ] );
        if ( !problem.isEmpty() ) {
            showRule( static_cast<int>( r ) );
            QMessageBox::warning( this, tr( "Edit Rules" ),
                                  tr( "Rule \"%1\": %2" ).arg( rules_[ r ].name, problem ) );
            return false;
        }
    }
    QSaveFile file( fileName );
    if ( !file.open( QIODevice::WriteOnly ) || file.write( analysis::rulesToJson( rules_ ) ) < 0
         || !file.commit() ) {
        QMessageBox::warning( this, tr( "Edit Rules" ),
                              tr( "Can not write %1" ).arg( QDir::toNativeSeparators( fileName ) ) );
        return false;
    }
    fileName_ = fileName;
    updateList();
    return true;
}

bool RulesEditor::saveAs()
{
    const auto fileName = QFileDialog::getSaveFileName(
        this, tr( "Save rules" ), fileName_.isEmpty() ? "rules.json" : fileName_,
        tr( "flogg rules (*.json);;All files (*)" ) );
    return !fileName.isEmpty() && save( fileName );
}
