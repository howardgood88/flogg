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

#include "renderview.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QElapsedTimer>
#include <QPainter>
#include <QPixmap>
#include <QScrollBar>
#include <QShortcut>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QMenu>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QAbstractTextDocumentLayout>
#include <QTextBrowser>
#include <QStyleOptionButton>
#include <QTextCodec>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextList>
#include <QTextTable>
#include <QToolButton>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <map>
#include <set>
#include <QTextLength>

#include "configuration.h"
#include "log.h"
#include "logdata.h"
#include "logfiltereddata.h"
#include "readablesize.h"
#include "rendermodels.h"

namespace render {

namespace {

// Anchor of a heading as GitHub makes it: lower case, the punctuation removed, the spaces
// as dashes
QString headingAnchor( const QString& heading )
{
    QString anchor;
    for ( const auto c : heading.trimmed().toLower() ) {
        if ( c.isLetterOrNumber() || c == '-' || c == '_' ) {
            anchor.append( c );
        }
        else if ( c.isSpace() ) {
            anchor.append( '-' );
        }
    }
    return anchor;
}

// Markdown laid out like GitHub shows it: room around the headings (a rule under the
// first two levels), the paragraphs and the tables, code in boxes. Colors with alpha, for the
// light and dark themes. Run once in the loading thread: linear in the number of blocks.
// Parts of a Markdown document drawn by the browser (MarkdownBrowser), under the text
constexpr int CodeBoxProperty = QTextFormat::UserProperty + 1;    // frame: rounded box
constexpr int RuleProperty = QTextFormat::UserProperty + 2;       // block: 1 thin, 2 thick
constexpr int TaskProperty = QTextFormat::UserProperty + 3;       // block: 1 open, 2 done
constexpr int QuoteColorProperty = QTextFormat::UserProperty + 4; // block: bar of an alert
constexpr int QuoteIndent = 20;

void styleMarkdown( QTextDocument* document )
{
    const QColor subtle( 127, 127, 127, 36 );
    const QColor border( 127, 127, 127, 90 );
    const auto fixedFont = QFontDatabase::systemFont( QFontDatabase::FixedFont );

    QTextCharFormat codeCharacters;
    codeCharacters.setFontFamilies( fixedFont.families() );
    codeCharacters.setFontFixedPitch( true );

    // Changes moving the text (frames, rules) are made after, from the end
    struct Range {
        int start = 0;
        int end = 0;
    };
    std::vector<Range> codeBlocks;
    std::vector<int> rulesAfter;
    // The alerts of GitHub (> [!NOTE]...): the marker made a title
    struct Alert {
        int position = 0;
        int length = 0;
        QString title;
        QColor color;
    };
    std::vector<Alert> alerts;
    std::set<QTextList*> lists;

    QTextCursor cursor( document );
    cursor.beginEditBlock();

    // A line break (<br />) is followed by the space of the end of the line
    const auto lineBreak = QString( QChar::LineSeparator ) + QLatin1Char( ' ' );
    for ( auto found = document->find( lineBreak ); !found.isNull();
          found = document->find( lineBreak, found ) ) {
        found.insertText( QString( QChar::LineSeparator ) );
    }

    bool previousCode = false;
    std::map<QString, int> anchors;

    for ( auto block = document->begin(); block.isValid(); block = block.next() ) {
        auto format = block.blockFormat();
        QTextCursor blockCursor( block );

        // A rule (---): drawn by the browser, thick and light like GitHub's
        if ( format.hasProperty( QTextFormat::BlockTrailingHorizontalRulerWidth ) ) {
            format.clearProperty( QTextFormat::BlockTrailingHorizontalRulerWidth );
            format.setProperty( RuleProperty, 2 );
            format.setTopMargin( 24 );
            format.setBottomMargin( 24 );
            format.setLineHeight( 4, QTextBlockFormat::FixedHeight );
            blockCursor.setBlockFormat( format );
            previousCode = false;
            continue;
        }
        const auto code
            = format.hasProperty( QTextFormat::BlockCodeFence ) || format.nonBreakableLines();

        if ( code ) {
            const auto end = block.position() + block.length() - 1;
            if ( previousCode && block.blockFormat().hasProperty( QTextFormat::BlockCodeFence )
                 == document->findBlock( codeBlocks.back().start )
                        .blockFormat()
                        .hasProperty( QTextFormat::BlockCodeFence ) ) {
                codeBlocks.back().end = end;
            }
            else {
                codeBlocks.push_back( { block.position(), end } );
            }
            format.setTopMargin( 0 );
            format.setBottomMargin( 0 );
            format.setLineHeight( 140, QTextBlockFormat::ProportionalHeight );
            blockCursor.setBlockFormat( format );
            blockCursor.select( QTextCursor::BlockUnderCursor );
            blockCursor.mergeCharFormat( codeCharacters );
        }
        else if ( format.headingLevel() > 0 ) {
            const auto level = format.headingLevel();
            format.setTopMargin( 24 );
            // The first two levels: the rule under them, like GitHub (0.3em, a line, 16px)
            format.setBottomMargin( level <= 2 ? 0 : 16 );
            blockCursor.setBlockFormat( format );

            // The anchor GitHub gives the heading, for the links to #a-section
            auto anchor = headingAnchor( block.text() );
            const auto count = anchors[ anchor ]++;
            if ( count > 0 ) {
                anchor += QStringLiteral( "-%1" ).arg( count );
            }
            QTextCharFormat anchorFormat;
            anchorFormat.setAnchor( true );
            anchorFormat.setAnchorNames( { anchor } );
            QTextCursor headingText( block );
            headingText.movePosition( QTextCursor::EndOfBlock, QTextCursor::KeepAnchor );
            headingText.mergeCharFormat( anchorFormat );
            if ( level <= 2 ) {
                rulesAfter.push_back( block.position() + block.length() - 1 );
            }
        }
        else {
            // A line break of the Markdown in an item of a list leaves an item of its own:
            // an image (its bullet at the bottom), or nothing. Kept at the place of the
            // item, without a bullet.
            if ( auto* list = block.textList() ) {
                const auto text = block.text().trimmed();
                const auto onlyImages
                    = !text.isEmpty() && std::all_of( text.begin(), text.end(), []( QChar c ) {
                          return c == QChar::ObjectReplacementCharacter || c.isSpace();
                      } );
                if ( text.isEmpty() || onlyImages ) {
                    const auto indent = list->format().indent();
                    list->remove( block );
                    format = block.blockFormat();
                    format.setIndent( indent );
                    if ( text.isEmpty() ) {
                        format.setTopMargin( 0 );
                        format.setBottomMargin( 0 );
                        format.setLineHeight( 1, QTextBlockFormat::FixedHeight );
                        blockCursor.setBlockFormat( format );
                        previousCode = false;
                        continue;
                    }
                }
            }

            // Paragraphs, items of lists, quotes; the cells of the tables have their padding.
            // Only a margin below, like GitHub: the space after a heading is its own.
            format.setTopMargin( 0 );
            format.setBottomMargin( blockCursor.currentTable() ? 0 : block.textList() ? 4 : 12 );

            if ( auto* list = block.textList() ) {
                // A task: a check box drawn by the browser instead of a bullet. Out of the
                // list, at its place: the bullet is of the whole list in Qt, of each item on
                // GitHub.
                const auto marker = format.marker();
                if ( marker != QTextBlockFormat::MarkerType::NoMarker ) {
                    format.setProperty( TaskProperty,
                                        marker == QTextBlockFormat::MarkerType::Checked ? 2 : 1 );
                    format.setMarker( QTextBlockFormat::MarkerType::NoMarker );
                    format.setIndent( list->format().indent() );
                    format.setTopMargin( 0 );
                    list->remove( block );
                    format.setObjectIndex( -1 );
                    // The list is deleted with its last item: not used any more
                    list = nullptr;
                }
            }
            if ( auto* list = block.textList() ) {

                // Close items, the bullets of the levels as GitHub draws them
                format.setTopMargin( 0 );
                if ( lists.insert( list ).second ) {
                    auto listFormat = list->format();
                    const auto style = listFormat.style();
                    if ( style == QTextListFormat::ListDisc || style == QTextListFormat::ListCircle
                         || style == QTextListFormat::ListSquare ) {
                        const auto level = std::max( 1, listFormat.indent() );
                        listFormat.setStyle( level == 1   ? QTextListFormat::ListDisc
                                             : level == 2 ? QTextListFormat::ListCircle
                                                          : QTextListFormat::ListSquare );
                        list->setFormat( listFormat );
                    }
                }
            }

            if ( format.hasProperty( QTextFormat::BlockQuoteLevel ) ) {
                // Quotes in grey, after a bar for each level drawn by the browser
                format.setLeftMargin( QuoteIndent * format.intProperty( QTextFormat::BlockQuoteLevel ) );
                QTextCharFormat quote;
                quote.setForeground( QColor( 127, 127, 127 ) );
                QTextCursor quoteText( block );
                quoteText.movePosition( QTextCursor::EndOfBlock, QTextCursor::KeepAnchor );
                quoteText.mergeCharFormat( quote );

                static const std::map<QString, std::pair<QString, QColor>> kinds{
                    { QStringLiteral( "NOTE" ), { QStringLiteral( "\u24D8 Note" ), QColor( 0x2f, 0x81, 0xf7 ) } },
                    { QStringLiteral( "TIP" ), { QStringLiteral( "\U0001F4A1 Tip" ), QColor( 0x3f, 0xb9, 0x50 ) } },
                    { QStringLiteral( "IMPORTANT" ), { QStringLiteral( "\u2757 Important" ), QColor( 0xa3, 0x71, 0xf7 ) } },
                    { QStringLiteral( "WARNING" ), { QStringLiteral( "\u26A0 Warning" ), QColor( 0xd2, 0x99, 0x22 ) } },
                    { QStringLiteral( "CAUTION" ), { QStringLiteral( "\u26D4 Caution" ), QColor( 0xf8, 0x51, 0x49 ) } },
                };
                static const QRegularExpression marker(
                    QStringLiteral( "^\\[!(NOTE|TIP|IMPORTANT|WARNING|CAUTION)\\]\\s*" ),
                    QRegularExpression::CaseInsensitiveOption );
                if ( const auto match = marker.match( block.text() ); match.hasMatch() ) {
                    const auto& kind = kinds.at( match.captured( 1 ).toUpper() );
                    format.setProperty( QuoteColorProperty, kind.second );
                    alerts.push_back( { block.position(), static_cast<int>( match.capturedLength() ),
                                        kind.first, kind.second } );
                }
            }

            bool hasImage = false;
            for ( auto it = block.begin(); !it.atEnd(); ++it ) {
                const auto fragment = it.fragment();
                if ( !fragment.isValid() ) {
                    continue;
                }
                // Images as wide as the page at most, like in the browser (a screenshot of
                // a high density screen is twice as large as it is shown)
                if ( fragment.charFormat().isImageFormat() ) {
                    hasImage = true;
                    auto image = fragment.charFormat().toImageFormat();
#if QT_VERSION >= QT_VERSION_CHECK( 6, 8, 0 )
                    image.setMaximumWidth( QTextLength( QTextLength::PercentageLength, 100 ) );
#endif
                    QTextCursor imageCursor( document );
                    imageCursor.setPosition( fragment.position() );
                    imageCursor.setPosition( fragment.position() + fragment.length(),
                                             QTextCursor::KeepAnchor );
                    imageCursor.setCharFormat( image );
                    continue;
                }
                // Code in the text: on a grey background
                if ( fragment.charFormat().fontFixedPitch() ) {
                    QTextCursor inline_( document );
                    inline_.setPosition( fragment.position() );
                    inline_.setPosition( fragment.position() + fragment.length(),
                                         QTextCursor::KeepAnchor );
                    QTextCharFormat inlineCode;
                    inlineCode.setBackground( subtle );
                    inline_.mergeCharFormat( inlineCode );
                }
            }
            // The height of a line is proportional to its tallest part: not with an image
            if ( !hasImage ) {
                format.setLineHeight( 150, QTextBlockFormat::ProportionalHeight );
            }
            blockCursor.setBlockFormat( format );
        }
        previousCode = code;
    }

    // Tables: cells with room, thin borders
    for ( auto* frame : document->rootFrame()->childFrames() ) {
        if ( auto* table = qobject_cast<QTextTable*>( frame ) ) {
            auto format = table->format();
            format.setBorder( 1 );
            format.setBorderBrush( border );
            format.setBorderStyle( QTextFrameFormat::BorderStyle_Solid );
            format.setBorderCollapse( true );
            format.setCellSpacing( 0 );
            format.setCellPadding( 6 );
            // The margin of a heading above is not kept before a table (16px on GitHub)
            format.setTopMargin( 16 );
            format.setBottomMargin( 16 );
            table->setFormat( format );

            // Every other row on a grey background
            for ( int row = 2; row < table->rows(); row += 2 ) {
                for ( int column = 0; column < table->columns(); ++column ) {
                    auto cell = table->cellAt( row, column );
                    auto cellFormat = cell.format();
                    cellFormat.setBackground( QColor( 127, 127, 127, 20 ) );
                    cell.setFormat( cellFormat );
                }
            }
        }
    }

    // From the end: a box for the code, a rule under the large headings
    std::vector<std::pair<int, int>> changes; // position, 0 for a rule, 1 + index of the code
    for ( const auto position : rulesAfter ) {
        changes.emplace_back( position, 0 );
    }
    for ( size_t i = 0; i < codeBlocks.size(); ++i ) {
        changes.emplace_back( codeBlocks[ i ].end, static_cast<int>( i ) + 1 );
    }
    // Negative: - 1 - index of the alert
    for ( size_t i = 0; i < alerts.size(); ++i ) {
        changes.emplace_back( alerts[ i ].position, -1 - static_cast<int>( i ) );
    }
    std::sort( changes.begin(), changes.end(),
               []( const auto& a, const auto& b ) { return a.first > b.first; } );
    for ( const auto& change : changes ) {
        if ( change.second < 0 ) {
            // The title of the alert, then its text on the next line
            const auto& alert = alerts[ static_cast<size_t>( -1 - change.second ) ];
            cursor.setPosition( alert.position );
            cursor.setPosition( alert.position + alert.length, QTextCursor::KeepAnchor );
            QTextCharFormat title = cursor.charFormat();
            title.setForeground( alert.color );
            title.setFontWeight( QFont::Bold );
            cursor.insertText( alert.title, title );
            QTextCharFormat text = title;
            text.setForeground( QColor( 127, 127, 127 ) );
            text.setFontWeight( QFont::Normal );
            cursor.insertText( QString( QChar::LineSeparator ), text );
            continue;
        }
        if ( change.second == 0 ) {
            QTextBlockFormat rule;
            rule.setProperty( RuleProperty, 1 );
            // The next paragraph adds its own margin and the leading of its lines
            rule.setTopMargin( 18 );
            rule.setBottomMargin( 4 );
            rule.setLineHeight( 1, QTextBlockFormat::FixedHeight );
            cursor.setPosition( change.first );
            cursor.insertBlock( rule, QTextCharFormat() );
            continue;
        }
        const auto& range = codeBlocks[ static_cast<size_t>( change.second - 1 ) ];
        // The rounded box is drawn by the browser
        QTextFrameFormat box;
        box.setProperty( CodeBoxProperty, true );
        box.setPadding( 12 );
        box.setTopMargin( 4 );
        box.setBottomMargin( 16 );
        box.setBorder( 0 );
        cursor.setPosition( range.start );
        cursor.setPosition( range.end, QTextCursor::KeepAnchor );
        cursor.insertFrame( box );
    }

    // A frame stands between two blocks, empty ones around the code: without height
    QTextBlockFormat flat;
    flat.setTopMargin( 0 );
    flat.setBottomMargin( 0 );
    flat.setLineHeight( 1, QTextBlockFormat::FixedHeight );
    QTextCharFormat tiny;
    tiny.setFontPointSize( 1 );
    for ( auto* frame : document->rootFrame()->childFrames() ) {
        if ( qobject_cast<QTextTable*>( frame ) ) {
            continue;
        }
        for ( const auto position : { frame->firstPosition() - 1, frame->lastPosition() + 1 } ) {
            const auto block = document->findBlock( position );
            if ( block.isValid() && block.length() == 1 && !block.textList() ) {
                QTextCursor around( block );
                around.setBlockFormat( flat );
                around.setBlockCharFormat( tiny );
            }
        }
    }
    cursor.endEditBlock();
}

} // namespace

} // namespace render

// The browser of the Markdown documents: draws under the text what QTextDocument does not,
// like GitHub: rounded boxes of code, bars of the quotes, light rules, check boxes of tasks.
// Only the parts shown are looked at, the document is not laid out further for them.
class MarkdownBrowser : public QTextBrowser {
  public:
    using QTextBrowser::QTextBrowser;

    // The parts of the document, once it is shown
    void findDecorations()
    {
        codeBoxes_.clear();
        blocks_.clear();
        auto* document = this->document();
        if ( document == nullptr ) {
            return;
        }
        for ( auto* frame : document->rootFrame()->childFrames() ) {
            if ( frame->frameFormat().boolProperty( render::CodeBoxProperty ) ) {
                codeBoxes_.push_back( frame );
            }
        }
        for ( auto block = document->begin(); block.isValid(); block = block.next() ) {
            const auto format = block.blockFormat();
            if ( format.hasProperty( render::RuleProperty ) || format.hasProperty( render::TaskProperty )
                 || format.hasProperty( QTextFormat::BlockQuoteLevel ) ) {
                blocks_.push_back( block );
            }
        }
    }

  protected:
    void paintEvent( QPaintEvent* event ) override
    {
        if ( !codeBoxes_.empty() || !blocks_.empty() ) {
            QPainter painter( viewport() );
            painter.setRenderHint( QPainter::Antialiasing );
            paintDecorations( painter );
        }
        QTextBrowser::paintEvent( event );
    }

  private:
    // Where the block is, only when it is laid out already: the layout of the document is
    // not asked for here (frameBoundingRect lays all of it out, blockBoundingRect up to the
    // block), it would change while the browser paints it and the text would not be drawn
    static std::optional<QRectF> laidOut( const QTextBlock& block )
    {
        const auto* textLayout = block.isValid() ? block.layout() : nullptr;
        if ( textLayout == nullptr || textLayout->lineCount() == 0 ) {
            return std::nullopt;
        }
        return QRectF( textLayout->position(), textLayout->boundingRect().size() );
    }

    void paintDecorations( QPainter& painter )
    {
        auto* document = this->document();
        const QPointF offset( -horizontalScrollBar()->value(), -verticalScrollBar()->value() );
        // A page above and below what is shown
        const auto top = -offset.y() - viewport()->height();
        const auto bottom = -offset.y() + 2 * viewport()->height();
        const auto shown = [ top, bottom ]( qreal from, qreal to ) {
            return to >= top && from <= bottom;
        };
        const QColor box( 127, 127, 127, 36 );
        const QColor line( 127, 127, 127, 70 );
        const auto left = document->documentMargin();
        const auto width = std::max( viewport()->width(), static_cast<int>( document->size().width() ) )
                           - 2 * document->documentMargin();

        // The code boxes: between the empty blocks around their frame
        for ( auto* frame : codeBoxes_ ) {
            const auto before = laidOut( document->findBlock( frame->firstPosition() - 1 ) );
            const auto after = laidOut( document->findBlock( frame->lastPosition() + 1 ) );
            if ( !before || !after ) {
                continue;
            }
            const QRectF rect( left, before->bottom() + 4, width,
                               after->top() - before->bottom() - 4 - 16 );
            if ( !shown( rect.top(), rect.bottom() ) ) {
                continue;
            }
            painter.setPen( Qt::NoPen );
            painter.setBrush( box );
            painter.drawRoundedRect( rect.translated( offset ), 6, 6 );
        }

        for ( const auto& block : blocks_ ) {
            const auto rect = laidOut( block );
            if ( !rect || !shown( rect->top(), rect->bottom() ) ) {
                continue;
            }
            const auto format = block.blockFormat();
            const auto* textLayout = block.layout();

            if ( const auto rule = format.intProperty( render::RuleProperty ); rule > 0 ) {
                // On the line of the block, between its margins
                const auto thickness = rule == 2 ? 4.0 : 1.0;
                const auto y = rect->top() + textLayout->lineAt( 0 ).y();
                painter.setPen( Qt::NoPen );
                painter.setBrush( line );
                painter.drawRect( QRectF( left, y, width, thickness ).translated( offset ) );
                continue;
            }

            if ( const auto task = format.intProperty( render::TaskProperty ); task > 0 ) {
                const auto firstLine = textLayout->lineAt( 0 );
                const auto size = 13.0;
                const QRectF check( rect->left() + firstLine.x() - size - 6,
                                    rect->top() + firstLine.y() + ( firstLine.height() - size ) / 2,
                                    size, size );
                QStyleOptionButton option;
                option.initFrom( this );
                option.rect = check.translated( offset ).toRect();
                option.state = ( task == 2 ? QStyle::State_On : QStyle::State_Off )
                               | QStyle::State_Enabled;
                style()->drawPrimitive( QStyle::PE_IndicatorCheckBox, &option, &painter, this );
                continue;
            }

            // A bar for each level of quote, the colour of an alert for the last one, down to
            // the next block of the quote: one bar for its paragraphs
            const auto level = format.intProperty( QTextFormat::BlockQuoteLevel );
            const auto alert = format.property( render::QuoteColorProperty ).value<QColor>();
            auto barBottom = rect->bottom();
            const auto next = block.next();
            if ( next.isValid() && next.blockFormat().intProperty( QTextFormat::BlockQuoteLevel ) > 0 ) {
                if ( const auto nextRect = laidOut( next ) ) {
                    barBottom = nextRect->top();
                }
            }
            painter.setPen( Qt::NoPen );
            for ( int i = 0; i < level; ++i ) {
                painter.setBrush( i + 1 == level && alert.isValid() ? alert : line );
                painter.drawRect( QRectF( left + i * render::QuoteIndent, rect->top(), 4,
                                          barBottom - rect->top() )
                                      .translated( offset ) );
            }
        }
    }

    std::vector<QTextFrame*> codeBoxes_;
    std::vector<QTextBlock> blocks_;
};

namespace render {

QString readText( const QString& fileName, QTextCodec* codec, qint64 sizeLimit, QString* error,
                  bool preferUtf8 )
{
    QFile file( fileName );
    if ( file.size() > sizeLimit ) {
        *error = QCoreApplication::translate( "RenderView",
                                              "The file (%1) is larger than the render limit of the format (%2), "
                                              "the limit can be raised in Options > Advanced." )
                     .arg( readableSize( static_cast<uint64_t>( file.size() ) ),
                           readableSize( static_cast<uint64_t>( sizeLimit ) ) );
        return {};
    }
    if ( !file.open( QIODevice::ReadOnly ) ) {
        *error = QCoreApplication::translate( "RenderView", "The file can not be read: %1" )
                     .arg( file.errorString() );
        return {};
    }
    const auto bytes = file.readAll();

    auto* utf8 = QTextCodec::codecForName( "UTF-8" );
    if ( codec == nullptr ) {
        codec = utf8;
    }
    // Markdown, JSON, HTML are UTF-8 nearly always: a file of a few accents or emoji detected
    // as something else (Windows-1252) is read in UTF-8 when it is valid UTF-8
    if ( preferUtf8 && codec != utf8 ) {
        QTextCodec::ConverterState state;
        utf8->toUnicode( bytes.constData(), static_cast<int>( bytes.size() ), &state );
        if ( state.invalidChars == 0 ) {
            codec = utf8;
        }
    }
    // A decoder of its own, the codec is shared with the UI thread
    std::unique_ptr<QTextDecoder> decoder( codec->makeDecoder() );
    auto text = decoder->toUnicode( bytes );
    if ( text.startsWith( QChar( 0xfeff ) ) ) {
        text.remove( 0, 1 );
    }
    return text;
}

qint64 sizeLimit( RenderFormat format )
{
    const auto limit = static_cast<qint64>( Configuration::get().renderSizeLimitMb() ) * 1024 * 1024;
    // A rich text document takes about 50 times its size (tables, code blocks), JSON 5 times
    const auto richText = format == RenderFormat::Markdown || format == RenderFormat::Html;
    return richText ? limit / 4 : limit;
}

Document loadDocument( const QString& fileName, QTextCodec* codec, RenderFormat format,
                       qint64 sizeLimit, bool preferUtf8 )
{
    Document document;
    document.format = format;

    QString error;
    const auto text = readText( fileName, codec, sizeLimit, &error, preferUtf8 );
    if ( !error.isEmpty() ) {
        document.error = error;
        return document;
    }

    // Made here, used and deleted by the UI thread
    const auto makeTextDocument = [ &fileName ] {
        auto* textDocument = new QTextDocument;
        textDocument->setDocumentMargin( 24 );
        // Text a bit larger than the interface, like a page of the browser
        auto font = QGuiApplication::font();
        font.setPointSizeF( font.pointSizeF() * 1.15 );
        textDocument->setDefaultFont( font );
        // Relative images and links are found next to the file
        textDocument->setBaseUrl(
            QUrl::fromLocalFile( QFileInfo( fileName ).absolutePath() + QLatin1Char( '/' ) ) );
        return textDocument;
    };
    const auto handToUiThread = []( QTextDocument* textDocument ) {
        textDocument->moveToThread( QCoreApplication::instance()->thread() );
        return std::shared_ptr<QTextDocument>( textDocument,
                                               []( QTextDocument* d ) { d->deleteLater(); } );
    };

    switch ( format ) {
    case RenderFormat::Markdown: {
        auto* textDocument = makeTextDocument();
        textDocument->setMarkdown( prepareMarkdown( text ), QTextDocument::MarkdownDialectGitHub );
        styleMarkdown( textDocument );
        document.textDocument = handToUiThread( textDocument );
        document.needsBrowser = needsBrowser( text );
        break;
    }
    case RenderFormat::Html: {
        auto* textDocument = makeTextDocument();
        textDocument->setHtml( text );
        document.textDocument = handToUiThread( textDocument );
        break;
    }
    case RenderFormat::Json: {
        const auto utf8 = text.toUtf8();
        QJsonParseError parseError;
        document.json = QJsonDocument::fromJson( utf8, &parseError );
        if ( parseError.error != QJsonParseError::NoError ) {
            const auto position = lineAndColumn( utf8, parseError.offset );
            document.errorLine = position.first;
            document.error = QCoreApplication::translate(
                                 "RenderView", "JSON error at line %1, column %2: %3." )
                                 .arg( position.first )
                                 .arg( position.second )
                                 .arg( parseError.errorString() );
            document.json = {};
        }
        break;
    }
    case RenderFormat::Xml: {
        auto tree = XmlTree::parse( text );
        if ( !tree->error.isEmpty() ) {
            document.errorLine = tree->errorLine;
            document.error = QCoreApplication::translate(
                                 "RenderView", "XML error at line %1, column %2: %3." )
                                 .arg( tree->errorLine )
                                 .arg( tree->errorColumn )
                                 .arg( tree->error );
        }
        // What was read before the error is shown too
        document.xml = std::move( tree );
        break;
    }
    default:
        break;
    }
    return document;
}

} // namespace render

namespace {

enum Page { PlaceholderPage, BrowserPage, TreePage, TablePage };

// Background of the matches of the find bar, in the documents, trees and tables alike
const QColor MatchBackground( 255, 200, 0, 110 );

} // namespace

// Cells of the trees and tables with the matches of the find bar on a yellow background,
// the current match in the colours of the selection, like the text of a document. Only the
// cells drawn are looked at.
class MatchHighlightDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void setMatcher( std::optional<TextMatcher> matcher )
    {
        matcher_ = std::move( matcher );
        currentMatch_ = QPersistentModelIndex();
    }

    // The cell of the current match, and which of its matches it is
    void setCurrentMatch( const QModelIndex& index, int occurrence )
    {
        currentMatch_ = index;
        currentOccurrence_ = occurrence;
    }

    void paint( QPainter* painter, const QStyleOptionViewItem& option,
                const QModelIndex& index ) const override
    {
        const auto searchable = matcher_ ? index.data( SearchTextRole ) : QVariant{};
        if ( !searchable.isValid() || ( option.state & QStyle::State_Selected ) ) {
            QStyledItemDelegate::paint( painter, option, index );
            return;
        }

        QStyleOptionViewItem item( option );
        initStyleOption( &item, index );
        const auto* widget = item.widget;
        auto* style = widget ? widget->style() : QApplication::style();

        auto textRect = style->subElementRect( QStyle::SE_ItemViewItemText, &item, widget );
        const auto margin = style->pixelMetric( QStyle::PM_FocusFrameHMargin, nullptr, widget ) + 1;
        textRect.adjust( margin, 0, -margin, 0 );
        const auto text
            = item.fontMetrics.elidedText( item.text, item.textElideMode, textRect.width() );
        const auto ranges = matcher_->matchRanges( text );
        if ( ranges.empty() ) {
            QStyledItemDelegate::paint( painter, option, index );
            return;
        }

        // The cell without its text, then the text with the matches
        item.text.clear();
        style->drawControl( QStyle::CE_ItemViewItem, &item, painter, widget );

        const auto isCurrent = currentMatch_.isValid() && currentMatch_ == index;
        QList<QTextLayout::FormatRange> formats;
        for ( const auto& range : ranges ) {
            QTextLayout::FormatRange format;
            format.start = range.first;
            format.length = range.second;
            if ( isCurrent && formats.size() == currentOccurrence_ ) {
                format.format.setBackground( item.palette.brush( QPalette::Highlight ) );
                format.format.setForeground( item.palette.brush( QPalette::HighlightedText ) );
            }
            else {
                format.format.setBackground( MatchBackground );
            }
            formats.append( format );
        }
        QTextLayout layout( text, item.font, painter->device() );
        layout.setFormats( formats );
        layout.beginLayout();
        auto line = layout.createLine();
        line.setLineWidth( textRect.width() );
        layout.endLayout();

        painter->save();
        painter->setClipRect( textRect );
        painter->setPen( item.palette.color( QPalette::Text ) );
        if ( auto brush = index.data( Qt::ForegroundRole ); brush.canConvert<QBrush>() ) {
            painter->setPen( brush.value<QBrush>().color() );
        }
        const auto top = textRect.top() + ( textRect.height() - line.height() ) / 2;
        layout.draw( painter, QPointF( textRect.left(), top ) );
        painter->restore();
    }

    // A read only line edit: a part of the text can be selected and copied
    QWidget* createEditor( QWidget* parent, const QStyleOptionViewItem&,
                           const QModelIndex& ) const override
    {
        auto* editor = new QLineEdit( parent );
        editor->setReadOnly( true );
        editor->setFrame( false );
        return editor;
    }

    void setEditorData( QWidget* editor, const QModelIndex& index ) const override
    {
        if ( auto* lineEdit = qobject_cast<QLineEdit*>( editor ) ) {
            lineEdit->setText( index.data( Qt::EditRole ).toString() );
            lineEdit->setCursorPosition( 0 );
            lineEdit->selectAll();
        }
    }

    void setModelData( QWidget*, QAbstractItemModel*, const QModelIndex& ) const override
    {
        // Read only
    }

  private:
    std::optional<TextMatcher> matcher_;
    QPersistentModelIndex currentMatch_;
    int currentOccurrence_ = 0;
};

namespace {

constexpr int MaxColumnWidth = 400;

QTemporaryDir& browserPagesDir()
{
    // Removed when flogg quits
    static QTemporaryDir dir( QDir::tempPath() + QStringLiteral( "/flogg-render-XXXXXX" ) );
    return dir;
}

QTreeView* makeTree()
{
    auto* tree = new QTreeView;
    tree->setUniformRowHeights( true );
    tree->setAlternatingRowColors( true );
    tree->setContextMenuPolicy( Qt::CustomContextMenu );
    tree->header()->setStretchLastSection( true );
    return tree;
}

} // namespace

RenderView::RenderView( std::shared_ptr<LogData> logData, RenderFormat format, QWidget* parent )
    : QWidget( parent )
    , logData_( std::move( logData ) )
    , format_( format )
    , formatBox_( new QComboBox )
    , headerBox_( new QCheckBox( tr( "First line is the header" ) ) )
    , messageLabel_( new QLabel )
    , showLineButton_( new QPushButton( tr( "Show in log view" ) ) )
    , browserButton_( new QPushButton( tr( "Open in browser" ) ) )
    , reloadButton_( new QPushButton( tr( "Render again" ) ) )
    , pages_( new QStackedWidget )
    , placeholder_( new QLabel )
    , browser_( new MarkdownBrowser )
    , tree_( makeTree() )
    , tablePage_( new QWidget )
    , table_( new QTableView )
    , watcher_( new QFutureWatcher<render::Document>( this ) )
    , findBar_( new QWidget )
    , findEdit_( new QLineEdit )
    , caseButton_( new QToolButton )
    , regexButton_( new QToolButton )
    , findStatus_( new QLabel )
    , treeSearchWatcher_( new QFutureWatcher<render::TreeSearch>( this ) )
    , highlightDelegate_( new MatchHighlightDelegate( this ) )
    , countWatcher_( new QFutureWatcher<int>( this ) )
    , documentSearchWatcher_( new QFutureWatcher<render::DocumentSearch>( this ) )
{
    tree_->setItemDelegate( highlightDelegate_ );
    table_->setItemDelegate( highlightDelegate_ );
    // Cells opened in a read only editor to select a part of their text
    for ( auto* view : std::initializer_list<QAbstractItemView*>{ tree_, table_ } ) {
        view->setEditTriggers( QAbstractItemView::DoubleClicked
                               | QAbstractItemView::EditKeyPressed );
    }
    // Rows are selected by their header, cells one by one or several
    table_->setSelectionMode( QAbstractItemView::ExtendedSelection );
    for ( const auto f : render::allFormats() ) {
        formatBox_->addItem( render::formatName( f ), static_cast<int>( f ) );
    }
    formatBox_->setToolTip( tr( "Format the file is shown as" ) );
    headerBox_->setChecked( true );
    messageLabel_->setWordWrap( true );
    messageLabel_->setTextInteractionFlags( Qt::TextSelectableByMouse );
    browserButton_->setToolTip(
        tr( "Shows the document in the web browser, with diagrams (mermaid), math and code "
            "highlighting. The libraries are loaded from cdn.jsdelivr.net." ) );

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins( 4, 2, 4, 2 );
    bar->addWidget( formatBox_ );
    bar->addWidget( headerBox_ );
    bar->addWidget( messageLabel_, 1 );
    bar->addWidget( showLineButton_ );
    bar->addWidget( reloadButton_ );
    bar->addWidget( browserButton_ );

    placeholder_->setAlignment( Qt::AlignCenter );
    placeholder_->setWordWrap( true );
    placeholder_->setMargin( 24 );

    browser_->setOpenLinks( false );
    browser_->viewport()->installEventFilter( this );
    // The images follow the width of the view once it stops changing
    imageFitTimer_ = new QTimer( this );
    imageFitTimer_->setSingleShot( true );
    imageFitTimer_->setInterval( 100 );
    connect( imageFitTimer_, &QTimer::timeout, this, &RenderView::fitImages );
    browser_->setFrameShape( QFrame::NoFrame );

    table_->setAlternatingRowColors( true );
    table_->setSelectionBehavior( QAbstractItemView::SelectItems );
    table_->setWordWrap( false );
    table_->setHorizontalScrollMode( QAbstractItemView::ScrollPerPixel );
    // Fixed heights keep the header small for millions of rows
    table_->verticalHeader()->setSectionResizeMode( QHeaderView::Fixed );
    table_->verticalHeader()->setDefaultSectionSize( table_->fontMetrics().height() + 6 );
    table_->horizontalHeader()->setSectionResizeMode( QHeaderView::Interactive );
    auto* copyAction = new QAction( tr( "Copy" ), table_ );
    copyAction->setShortcut( QKeySequence::Copy );
    copyAction->setShortcutContext( Qt::WidgetShortcut );
    connect( copyAction, &QAction::triggered, this, &RenderView::copySelectedCells );
    table_->addAction( copyAction );
    table_->setContextMenuPolicy( Qt::ActionsContextMenu );

    auto* tableLayout = new QVBoxLayout( tablePage_ );
    tableLayout->setContentsMargins( 0, 0, 0, 0 );
    tableLayout->addWidget( table_ );

    pages_->insertWidget( PlaceholderPage, placeholder_ );
    pages_->insertWidget( BrowserPage, browser_ );
    pages_->insertWidget( TreePage, tree_ );
    pages_->insertWidget( TablePage, tablePage_ );

    // The find bar, at the bottom like the quick find of the log views
    findEdit_->setPlaceholderText( tr( "Find" ) );
    findEdit_->setClearButtonEnabled( true );
    findEdit_->setToolTip( tr( "Enter: next match, Shift+Enter: previous match, Esc: close" ) );
    caseButton_->setText( QStringLiteral( "Aa" ) );
    caseButton_->setToolTip( tr( "Match case" ) );
    regexButton_->setText( QStringLiteral( ".*" ) );
    regexButton_->setToolTip( tr( "Regular expression" ) );
    auto* previousButton = new QToolButton;
    previousButton->setArrowType( Qt::UpArrow );
    previousButton->setToolTip( tr( "Previous match" ) );
    auto* nextButton = new QToolButton;
    nextButton->setArrowType( Qt::DownArrow );
    nextButton->setToolTip( tr( "Next match" ) );
    auto* closeButton = new QToolButton;
    closeButton->setText( QStringLiteral( "\u2715" ) );
    closeButton->setToolTip( tr( "Close" ) );
    for ( auto* button : { caseButton_, regexButton_ } ) {
        button->setCheckable( true );
    }
    for ( auto* button : { caseButton_, regexButton_, previousButton, nextButton, closeButton } ) {
        button->setAutoRaise( true );
        button->setFocusPolicy( Qt::NoFocus );
    }
    findStatus_->setMinimumWidth( findStatus_->fontMetrics().horizontalAdvance( "000000 / 000000" ) );

    auto* findLayout = new QHBoxLayout( findBar_ );
    findLayout->setContentsMargins( 4, 2, 4, 2 );
    findLayout->addWidget( findEdit_, 1 );
    findLayout->addWidget( caseButton_ );
    findLayout->addWidget( regexButton_ );
    findLayout->addWidget( previousButton );
    findLayout->addWidget( nextButton );
    findLayout->addWidget( findStatus_ );
    findLayout->addWidget( closeButton );
    findBar_->hide();

    auto* layout = new QVBoxLayout( this );
    layout->setContentsMargins( 0, 0, 0, 0 );
    layout->setSpacing( 0 );
    layout->addLayout( bar );
    layout->addWidget( pages_, 1 );
    layout->addWidget( findBar_ );

    connect( findEdit_, &QLineEdit::returnPressed, this, &RenderView::findNext );
    connect( findEdit_, &QLineEdit::textChanged, this, &RenderView::resetSearch );
    connect( caseButton_, &QToolButton::toggled, this, &RenderView::resetSearch );
    connect( regexButton_, &QToolButton::toggled, this, &RenderView::resetSearch );
    connect( previousButton, &QToolButton::clicked, this, &RenderView::findPrevious );
    connect( nextButton, &QToolButton::clicked, this, &RenderView::findNext );
    connect( closeButton, &QToolButton::clicked, this, &RenderView::hideFindBar );
    auto* previousKey = new QShortcut( QKeySequence( Qt::SHIFT | Qt::Key_Return ), findEdit_ );
    previousKey->setContext( Qt::WidgetShortcut );
    connect( previousKey, &QShortcut::activated, this, &RenderView::findPrevious );
    auto* escapeKey = new QShortcut( QKeySequence( Qt::Key_Escape ), findEdit_ );
    escapeKey->setContext( Qt::WidgetShortcut );
    connect( escapeKey, &QShortcut::activated, this, &RenderView::hideFindBar );
    // Ctrl+F comes from the main window, which opens the quick find otherwise
    for ( const auto& key : QKeySequence::keyBindings( QKeySequence::FindNext ) ) {
        auto* shortcut = new QShortcut( key, this );
        shortcut->setContext( Qt::WidgetWithChildrenShortcut );
        connect( shortcut, &QShortcut::activated, this, &RenderView::findNext );
    }
    for ( const auto& key : QKeySequence::keyBindings( QKeySequence::FindPrevious ) ) {
        auto* shortcut = new QShortcut( key, this );
        shortcut->setContext( Qt::WidgetWithChildrenShortcut );
        connect( shortcut, &QShortcut::activated, this, &RenderView::findPrevious );
    }

    connect( browser_->verticalScrollBar(), &QScrollBar::valueChanged, this,
             &RenderView::updateDocumentHighlights );
    connect( browser_->horizontalScrollBar(), &QScrollBar::valueChanged, this,
             &RenderView::updateDocumentHighlights );
    connect( documentSearchWatcher_, &QFutureWatcher<render::DocumentSearch>::finished, this,
             [ this ] {
                 documentSearchRunning_ = false;
                 if ( documentSearchWatcher_->isCanceled()
                      || documentSearchWatcher_->future().resultCount() == 0 || !searched_ ) {
                     return;
                 }
                 showDocumentMatches( documentSearchWatcher_->result() );
             } );
    connect( countWatcher_, &QFutureWatcher<int>::resultsReadyAt, this,
             &RenderView::countProgressed );
    connect( countWatcher_, &QFutureWatcher<int>::finished, this, [ this ] {
        counting_ = false;
        if ( !searched_ || countWatcher_->isCanceled() ) {
            return;
        }
        if ( countPending_ ) {
            // Lines appended meanwhile
            startCounting( countedLines_ / CountBlockLines );
            return;
        }
        updateTableStatus();
    } );

    connect( treeSearchWatcher_, &QFutureWatcher<render::TreeSearch>::finished, this, [ this ] {
        treeSearchRunning_ = false;
        if ( treeSearchWatcher_->isCanceled() || treeSearchWatcher_->future().resultCount() == 0
             || !searched_ ) {
            return;
        }
        treeMatches_ = treeSearchWatcher_->result();
        treeMatch_ = -1;
        stepTree( pendingBackward_ );
    } );

    connect( formatBox_, qOverload<int>( &QComboBox::activated ), this, [ this ]( int index ) {
        const auto chosen = static_cast<RenderFormat>( formatBox_->itemData( index ).toInt() );
        if ( chosen != format_ ) {
            showFormat( chosen );
            Q_EMIT formatChanged( format_ );
        }
    } );
    connect( headerBox_, &QCheckBox::toggled, this, [ this ] { showLineTable(); } );
    connect( reloadButton_, &QPushButton::clicked, this, [ this ] { showFormat( format_ ); } );
    connect( browserButton_, &QPushButton::clicked, this, &RenderView::openInBrowser );
    connect( showLineButton_, &QPushButton::clicked, this, [ this ] {
        if ( errorLine_ > 0 ) {
            Q_EMIT showLineRequested( errorLine_ - 1 );
        }
    } );
    connect( browser_, &QTextBrowser::anchorClicked, this, [ this ]( const QUrl& url ) {
        // A section of this document
        if ( url.scheme().isEmpty() && url.path().isEmpty() && url.hasFragment() ) {
            browser_->scrollToAnchor( QUrl::fromPercentEncoding( url.fragment().toUtf8() ) );
            return;
        }
        const auto target = browser_->document()->baseUrl().resolved( url );
        if ( !target.isLocalFile() ) {
            QDesktopServices::openUrl( target );
            return;
        }
        // A file next to it: in a tab of flogg, at the section; a folder in the file manager
        const QFileInfo file( target.toLocalFile() );
        const auto anchor = QUrl::fromPercentEncoding( target.fragment().toUtf8() );
        if ( file.isFile() && file.absoluteFilePath() == QFileInfo( fileName_ ).absoluteFilePath() ) {
            if ( !anchor.isEmpty() ) {
                browser_->scrollToAnchor( anchor );
            }
        }
        else if ( file.isFile() ) {
            Q_EMIT openFileRequested( file.absoluteFilePath(), anchor );
        }
        else if ( file.isDir() ) {
            QDesktopServices::openUrl( target );
        }
        else {
            showMessage( tr( "%1 does not exist." ).arg( file.absoluteFilePath() ), true );
        }
    } );
    connect( tree_, &QTreeView::customContextMenuRequested, this, &RenderView::showTreeMenu );
    connect( watcher_, &QFutureWatcher<render::Document>::finished, this, [ this ] {
        // A newer read may have been started meanwhile, only the last one is shown
        loadingDocument_ = false;
        if ( !watcher_->isCanceled() && watcher_->future().resultCount() > 0 ) {
            showDocument( watcher_->result() );
        }
    } );

    connect( logData_.get(), &LogData::loadingFinished, this, &RenderView::logDataLoaded );
    connect( logData_.get(), &LogData::fileChanged, this, &RenderView::logDataChanged );

    showMessage( {} );
    placeholder_->setText( tr( "Waiting for the file to be loaded…" ) );
    pages_->setCurrentIndex( PlaceholderPage );
    updateButtons();
}

RenderView::~RenderView()
{
    // The read still running is left to finish, its result is dropped
    watcher_->disconnect( this );
    treeSearchWatcher_->disconnect( this );
    countWatcher_->disconnect( this );
    countWatcher_->cancel();
    documentSearchWatcher_->disconnect( this );
    if ( treeSearchCancelled_ ) {
        *treeSearchCancelled_ = true;
    }
}

QWidget* RenderView::currentViewer() const
{
    return pages_->currentWidget();
}

bool RenderView::isLoadingDocument() const
{
    return loadingDocument_;
}

QString RenderView::message() const
{
    return messageLabel_->text();
}

QAbstractItemModel* RenderView::treeModel() const
{
    return treeModel_;
}

LineTableModel* RenderView::tableModel() const
{
    return tableModel_;
}


QTextDocument* RenderView::textDocument() const
{
    return textDocument_.get();
}

void RenderView::start()
{
    if ( started_ ) {
        return;
    }
    started_ = true;
    fileName_ = logData_->attachedFileName();

    const auto asked = format_;
    auto format = format_;
    if ( format == RenderFormat::None ) {
        QFile file( fileName_ );
        QByteArray head;
        if ( file.open( QIODevice::ReadOnly ) ) {
            head = file.read( 4096 );
        }
        format = render::detectFormat( fileName_, head );
    }
    showFormat( format );
    if ( format != asked ) {
        Q_EMIT formatChanged( format );
    }
}

void RenderView::showFormat( RenderFormat format )
{
    resetSearch();
    xmlTree_.reset();
    format_ = format;
    fileChanged_ = false;
    errorLine_ = 0;
    showMessage( {} );

    const auto index = formatBox_->findData( static_cast<int>( format ) );
    formatBox_->setCurrentIndex( index );

    // The views of the previous format are emptied, their memory is freed
    browser_->setDocument( nullptr );
    images_.clear();
    textDocument_.reset();
    tree_->setModel( nullptr );
    delete treeModel_;
    table_->setModel( nullptr );
    delete tableModel_;
    watcher_->cancel();
    loadingDocument_ = false;

    if ( format == RenderFormat::None ) {
        placeholder_->setText( tr( "The format of this file is not known, choose one in the list "
                                   "above." ) );
        pages_->setCurrentIndex( PlaceholderPage );
        updateButtons();
        return;
    }

    if ( render::isLineFormat( format ) ) {
        showLineTable();
        updateButtons();
        return;
    }

    placeholder_->setText( tr( "Rendering…" ) );
    pages_->setCurrentIndex( PlaceholderPage );

    const auto sizeLimit = render::sizeLimit( format );
    auto* codec = logData_->getDisplayEncoding();
    loadingDocument_ = true;
    watcher_->setFuture(
        QtConcurrent::run( [ fileName = fileName_, codec, format, sizeLimit,
                             preferUtf8 = !encodingChosen_ ] {
            return render::loadDocument( fileName, codec, format, sizeLimit, preferUtf8 );
        } ) );
    updateButtons();
}

void RenderView::showDocument( render::Document document )
{
    if ( document.format != format_ ) {
        return;
    }

    resetSearch();
    errorLine_ = document.errorLine;
    if ( !document.error.isEmpty() ) {
        showMessage( document.error, true );
    }

    switch ( document.format ) {
    case RenderFormat::Markdown:
    case RenderFormat::Html:
        if ( document.textDocument ) {
            textDocument_ = std::move( document.textDocument );
            browser_->setDocument( textDocument_.get() );
            static_cast<MarkdownBrowser*>( browser_ )->findDecorations();
            findImages();
            fitImages();
            // Asked by a link before the document was shown
            if ( !pendingAnchor_.isEmpty() ) {
                const auto anchor = std::exchange( pendingAnchor_, QString{} );
                // Once laid out up to it
                QTimer::singleShot( 0, this, [ this, anchor ] { browser_->scrollToAnchor( anchor ); } );
            }
            pages_->setCurrentIndex( BrowserPage );
            if ( document.needsBrowser ) {
                showMessage( tr( "The document has diagrams or math that are shown as code here, "
                                 "open it in the browser to see them." ) );
            }
        }
        break;
    case RenderFormat::Json:
        if ( !document.json.isNull() ) {
            auto* model = new JsonTreeModel(
                document.json.isObject() ? QJsonValue( document.json.object() )
                                         : QJsonValue( document.json.array() ),
                this );
            treeModel_ = model;
            tree_->setModel( model );
            tree_->expandToDepth( 0 );
            tree_->resizeColumnToContents( JsonTreeModel::KeyColumn );
            pages_->setCurrentIndex( TreePage );
        }
        break;
    case RenderFormat::Xml:
        if ( document.xml && document.xml->nodes.size() > 1 ) {
            xmlTree_ = document.xml;
            auto* model = new XmlTreeModel( document.xml, this );
            treeModel_ = model;
            tree_->setModel( model );
            tree_->expandToDepth( 1 );
            tree_->resizeColumnToContents( XmlTreeModel::NodeColumn );
            pages_->setCurrentIndex( TreePage );
        }
        break;
    default:
        break;
    }

    if ( pages_->currentIndex() == PlaceholderPage ) {
        placeholder_->setText( document.error.isEmpty() ? tr( "Nothing to show." )
                                                        : tr( "The file can not be rendered." ) );
    }
    updateButtons();
}

void RenderView::showLineTable()
{
    if ( !render::isLineFormat( format_ ) ) {
        return;
    }

    resetSearch();
    table_->setModel( nullptr );
    delete tableModel_;

    auto* model = new LineTableModel( logData_.get(), format_, headerBox_->isChecked(), this );
    tableModel_ = model;
    table_->setModel( model );

    // Widths from the rows shown, not the whole file
    table_->resizeColumnsToContents();
    for ( int column = 0; column < model->columnCount(); ++column ) {
        if ( table_->columnWidth( column ) > MaxColumnWidth ) {
            table_->setColumnWidth( column, MaxColumnWidth );
        }
    }
    table_->horizontalHeader()->setStretchLastSection( model->columnCount() == 1 );

    if ( format_ == RenderFormat::Csv ) {
        const auto delimiter = model->delimiter();
        const std::map<QChar, QString> names{ { ',', tr( "comma" ) },
                                              { ';', tr( "semicolon" ) },
                                              { '|', tr( "vertical bar" ) },
                                              { '\t', tr( "tab" ) } };
        const auto name = names.count( delimiter ) ? names.at( delimiter ) : QString( delimiter );
        showMessage( tr( "Separator: %1" ).arg( name ) );
    }
    messageLabel_->setToolTip(
        tr( "One line of the file is one row: a quoted field going over several lines is "
            "shown on several rows." ) );
    pages_->setCurrentIndex( TablePage );
}

void RenderView::scrollToAnchor( const QString& anchor )
{
    if ( anchor.isEmpty() ) {
        return;
    }
    if ( textDocument_ && pages_->currentWidget() == browser_ ) {
        browser_->scrollToAnchor( anchor );
    }
    else {
        pendingAnchor_ = anchor;
    }
}

void RenderView::findImages()
{
    // The images and the room their paragraph leaves them, once per document
    images_.clear();
    auto* document = textDocument_.get();
    if ( document == nullptr ) {
        return;
    }
    for ( auto block = document->begin(); block.isValid(); block = block.next() ) {
        for ( auto it = block.begin(); !it.atEnd(); ++it ) {
            const auto fragment = it.fragment();
            if ( fragment.isValid() && fragment.charFormat().isImageFormat() ) {
                auto indent = block.blockFormat().leftMargin()
                              + block.blockFormat().indent() * document->indentWidth();
                if ( const auto* list = block.textList() ) {
                    indent += list->format().indent() * document->indentWidth();
                }
                images_.push_back( { fragment.position(), fragment.length(), indent } );
            }
        }
    }
}

void RenderView::fitImages()
{
    // No wider than the page: the browser scales them, Qt shows their pixels
    auto* document = textDocument_.get();
    if ( document == nullptr || images_.empty() ) {
        return;
    }
    const auto page = browser_->viewport()->width() - 2 * document->documentMargin();
    QTextCursor cursor( document );
    cursor.beginEditBlock();
    for ( const auto& image : images_ ) {
        cursor.setPosition( image.position );
        cursor.setPosition( image.position + image.length, QTextCursor::KeepAnchor );
        auto format = cursor.charFormat().toImageFormat();
        const auto maximum = std::max( 32.0, page - image.indent - 8 );
#if QT_VERSION >= QT_VERSION_CHECK( 6, 8, 0 )
        format.setMaximumWidth( QTextLength( QTextLength::FixedLength, maximum ) );
#else
        // No maximum width before Qt 6.8: the size is set from the one of the image
        const auto resource
            = document->resource( QTextDocument::ImageResource, QUrl( format.name() ) );
        QSizeF size;
        if ( resource.canConvert<QImage>() ) {
            const auto picture = resource.value<QImage>();
            size = QSizeF( picture.size() ) / picture.devicePixelRatio();
        }
        else if ( resource.canConvert<QPixmap>() ) {
            const auto picture = resource.value<QPixmap>();
            size = QSizeF( picture.size() ) / picture.devicePixelRatio();
        }
        if ( size.isEmpty() ) {
            continue;
        }
        const auto scale = std::min( 1.0, maximum / size.width() );
        format.setWidth( size.width() * scale );
        format.setHeight( size.height() * scale );
#endif
        cursor.setCharFormat( format );
    }
    cursor.endEditBlock();
}

bool RenderView::eventFilter( QObject* watched, QEvent* event )
{
    if ( watched == browser_->viewport() && event->type() == QEvent::Resize
         && !images_.empty() ) {
        imageFitTimer_->start();
    }
    return QWidget::eventFilter( watched, event );
}

void RenderView::showMessage( const QString& text, bool warning )
{
    messageLabel_->setText( text );
    messageLabel_->setToolTip( {} );
    messageLabel_->setForegroundRole( warning ? QPalette::Link : QPalette::WindowText );
    auto font = messageLabel_->font();
    font.setBold( warning );
    messageLabel_->setFont( font );
    updateButtons();
}

void RenderView::updateButtons()
{
    const auto isDocument = format_ == RenderFormat::Markdown || format_ == RenderFormat::Html;
    browserButton_->setVisible( isDocument );
    headerBox_->setVisible( format_ == RenderFormat::Csv || format_ == RenderFormat::Tsv );
    showLineButton_->setVisible( errorLine_ > 0 );
    reloadButton_->setVisible( fileChanged_ );
}

void RenderView::logDataLoaded()
{
    if ( !started_ ) {
        start();
        return;
    }
    // Rows appended (or the file truncated) by the follow mode
    if ( tableModel_ ) {
        const auto rows = tableModel_->rowCount();
        tableModel_->refresh();
        if ( tableModel_->rowCount() < rows ) {
            resetSearch();
        }
        else if ( tableSearch_ && searched_ ) {
            // The new lines are searched and counted too
            tableSearch_->updateSearch( 0_lnum, LineNumber( logData_->getNbLine().get() ) );
            if ( counting_ ) {
                countPending_ = true;
            }
            else {
                startCounting( countedLines_ / CountBlockLines );
            }
        }
    }
}

void RenderView::logDataChanged()
{
    // A document is not read again by itself: a file written all the time would be
    // parsed all the time
    if ( started_ && !render::isLineFormat( format_ ) && format_ != RenderFormat::None ) {
        fileChanged_ = true;
        showMessage( tr( "The file has changed." ) );
    }
}

void RenderView::openInBrowser()
{
    if ( format_ == RenderFormat::Html ) {
        QDesktopServices::openUrl( QUrl::fromLocalFile( fileName_ ) );
        return;
    }
    if ( format_ != RenderFormat::Markdown ) {
        return;
    }

    QString error;
    const auto text = render::readText( fileName_, logData_->getDisplayEncoding(),
                                        render::sizeLimit( format_ ), &error, !encodingChosen_ );
    if ( !error.isEmpty() ) {
        showMessage( error, true );
        return;
    }

    auto& dir = browserPagesDir();
    const QFileInfo info( fileName_ );
    if ( !dir.isValid() ) {
        showMessage( tr( "No temporary folder for the page." ), true );
        return;
    }
    // One page per file, written again each time
    const auto pageName = dir.filePath(
        QStringLiteral( "%1-%2.html" )
            .arg( info.completeBaseName() )
            .arg( qHash( info.absoluteFilePath() ), 8, 16, QLatin1Char( '0' ) ) );
    QFile page( pageName );
    if ( !page.open( QIODevice::WriteOnly | QIODevice::Truncate ) ) {
        showMessage( tr( "The page can not be written: %1" ).arg( page.errorString() ), true );
        return;
    }
    page.write( render::markdownBrowserPage( text, info.fileName(), info.absolutePath() ).toUtf8() );
    page.close();
    QDesktopServices::openUrl( QUrl::fromLocalFile( pageName ) );
}

void RenderView::copySelectedCells()
{
    const auto* selection = table_->selectionModel();
    if ( selection == nullptr || !selection->hasSelection() ) {
        return;
    }
    auto indexes = selection->selectedIndexes();
    std::sort( indexes.begin(), indexes.end(), []( const auto& a, const auto& b ) {
        return a.row() != b.row() ? a.row() < b.row() : a.column() < b.column();
    } );

    // Rows by lines, cells by tabs
    QString text;
    int row = indexes.first().row();
    QStringList cells;
    for ( const auto& index : std::as_const( indexes ) ) {
        if ( index.row() != row ) {
            text += cells.join( '\t' ) + '\n';
            cells.clear();
            row = index.row();
        }
        cells.append( index.data( Qt::ToolTipRole ).isValid()
                          ? index.data( Qt::ToolTipRole ).toString()
                          : index.data().toString() );
    }
    text += cells.join( '\t' );
    QApplication::clipboard()->setText( text );
}

void RenderView::showTreeMenu( const QPoint& position )
{
    auto* view = qobject_cast<QTreeView*>( sender() );
    if ( view == nullptr ) {
        return;
    }
    const auto index = view->indexAt( position );
    if ( !index.isValid() ) {
        return;
    }

    QMenu menu;
    if ( auto* json = qobject_cast<JsonTreeModel*>( view->model() ) ) {
        menu.addAction( tr( "Copy value" ), this, [ json, index ] {
            QApplication::clipboard()->setText( json->valueText( index ) );
        } );
        menu.addAction( tr( "Copy path" ), this, [ json, index ] {
            QApplication::clipboard()->setText( json->path( index ) );
        } );
    }
    else {
        menu.addAction( tr( "Copy" ), this, [ index ] {
            const auto value = index.siblingAtColumn( XmlTreeModel::ValueColumn );
            const auto tip = value.data( Qt::ToolTipRole );
            QApplication::clipboard()->setText( tip.isValid() ? tip.toString()
                                                              : value.data().toString() );
        } );
    }
    menu.exec( view->viewport()->mapToGlobal( position ) );
}

/*
 * Find bar
 */
void RenderView::copy()
{
    // A part of a cell in its editor, the find bar
    if ( auto* edit = qobject_cast<QLineEdit*>( QApplication::focusWidget() );
         edit && isAncestorOf( edit ) ) {
        edit->copy();
        return;
    }
    switch ( pages_->currentIndex() ) {
    case BrowserPage:
        browser_->copy();
        break;
    case TablePage:
        copySelectedCells();
        break;
    case TreePage: {
        // The cells selected, by rows
        auto indexes = tree_->selectionModel()->selectedIndexes();
        if ( indexes.isEmpty() && tree_->currentIndex().isValid() ) {
            indexes.append( tree_->currentIndex() );
        }
        QStringList cells;
        for ( const auto& index : std::as_const( indexes ) ) {
            const auto text = index.data( Qt::EditRole ).toString();
            cells.append( text.isEmpty() ? index.data().toString() : text );
        }
        if ( !cells.isEmpty() ) {
            QApplication::clipboard()->setText( cells.join( '\t' ) );
        }
        break;
    }
    default:
        break;
    }
}

QString RenderView::selectedText() const
{
    switch ( pages_->currentIndex() ) {
    case BrowserPage:
        return browser_->textCursor().selectedText();
    case TreePage:
    case TablePage: {
        auto* view = pages_->currentIndex() == TreePage ? static_cast<QAbstractItemView*>( tree_ )
                                                        : table_;
        // A part of a cell selected in its editor
        if ( auto* editor = qobject_cast<QLineEdit*>( QApplication::focusWidget() );
             editor && view->isAncestorOf( editor ) ) {
            return editor->hasSelectedText() ? editor->selectedText() : editor->text();
        }
        // A cell selected: its text; a row: the text of its current cell
        const auto current = view->currentIndex();
        if ( current.isValid() && view->selectionModel()->isSelected( current ) ) {
            return current.data( Qt::EditRole ).toString();
        }
        break;
    }
    default:
        break;
    }
    return {};
}

void RenderView::showFindBar()
{
    // The text selected is searched
    const auto selection = selectedText();
    if ( !selection.isEmpty() && !selection.contains( QChar::ParagraphSeparator )
         && selection.size() <= 1000 ) {
        findEdit_->setText( selection );
    }
    findBar_->show();
    findEdit_->setFocus( Qt::ShortcutFocusReason );
    findEdit_->selectAll();
}

void RenderView::hideFindBar()
{
    resetSearch();
    findBar_->hide();
    if ( auto* viewer = pages_->currentWidget() ) {
        if ( viewer == tablePage_ ) {
            table_->setFocus();
        }
        else {
            viewer->setFocus();
        }
    }
}

void RenderView::setFindText( const QString& text, bool caseSensitive, bool regex )
{
    caseButton_->setChecked( caseSensitive );
    regexButton_->setChecked( regex );
    findEdit_->setText( text );
    findBar_->show();
}

void RenderView::findNext()
{
    find( false );
}

void RenderView::findPrevious()
{
    find( true );
}

QString RenderView::findText() const
{
    return findEdit_->text();
}

QString RenderView::findStatus() const
{
    return findStatus_->text();
}

bool RenderView::isSearching() const
{
    return treeSearchRunning_ || documentSearchRunning_ || counting_
           || ( tableSearch_ && searched_ && !tableSearchDone_ );
}

void RenderView::setFindStatus( const QString& status, bool warning )
{
    findStatus_->setText( status );
    findStatus_->setForegroundRole( warning ? QPalette::Link : QPalette::WindowText );
}

void RenderView::resetSearch()
{
    searched_.reset();
    highlightDelegate_->setMatcher( std::nullopt );
    tree_->viewport()->update();
    table_->viewport()->update();
    documentMatches_.clear();
    documentMatchesCut_ = false;
    browser_->setExtraSelections( {} );
    if ( treeSearchCancelled_ ) {
        *treeSearchCancelled_ = true;
        treeSearchCancelled_.reset();
    }
    treeSearchWatcher_->cancel();
    treeSearchRunning_ = false;
    documentSearchWatcher_->cancel();
    documentSearchRunning_ = false;
    treeMatches_ = {};
    treeMatch_ = -1;
    countWatcher_->cancel();
    counting_ = false;
    countPending_ = false;
    blockCounts_.clear();
    lineCountsBlock_ = NoBlock;
    lineCounts_.clear();
    tableMatch_ = QPersistentModelIndex();
    tableOccurrence_ = 0;
    pendingTableStep_ = false;
    tableCellScan_ = false;
    shownTreeMatch_ = QPersistentModelIndex();
    // The search object is kept, its threads are made once
    if ( tableSearch_ ) {
        tableSearch_->interruptSearch();
        tableSearch_->clearSearch();
    }
    tableSearchDone_ = false;
    setFindStatus( {} );
}

void RenderView::find( bool backward )
{
    if ( !findBar_->isVisible() ) {
        findBar_->show();
    }
    const auto text = findEdit_->text();
    if ( text.isEmpty() ) {
        resetSearch();
        return;
    }

    if ( searched_ ) {
        // Asked again before the first match is found: it goes on in the last direction.
        // The count of the matches of a table does not keep from going to the next ones.
        pendingBackward_ = backward;
        if ( treeSearchRunning_ || documentSearchRunning_ || pendingTableStep_
             || ( pages_->currentIndex() == TablePage && !tableSearchDone_ ) ) {
            return;
        }
        switch ( pages_->currentIndex() ) {
        case BrowserPage:
            stepDocument( backward );
            break;
        case TreePage:
            stepTree( backward );
            break;
        case TablePage:
            stepTable( backward );
            break;
        default:
            break;
        }
        return;
    }

    const TextMatcher matcher( text, caseButton_->isChecked(), regexButton_->isChecked() );
    if ( !matcher.isValid() ) {
        setFindStatus( tr( "Invalid regular expression" ), true );
        return;
    }

    pendingBackward_ = backward;
    highlightDelegate_->setMatcher( matcher );
    tree_->viewport()->update();
    table_->viewport()->update();
    switch ( pages_->currentIndex() ) {
    case BrowserPage:
        searched_ = matcher;
        searchDocument( matcher );
        break;
    case TreePage:
        searched_ = matcher;
        searchTree( matcher );
        break;
    case TablePage:
        searched_ = matcher;
        searchTable( matcher );
        break;
    default:
        setFindStatus( tr( "Nothing to search" ) );
        break;
    }
}

// Document: every match is found and highlighted, at most MaxDocumentMatches
namespace render {

DocumentSearch findInDocument( const QString& rawText, const TextMatcher& matcher,
                               const std::atomic_bool* cancelled )
{
    // The blocks (paragraphs, cells of tables) are searched one by one, like
    // QTextDocument::find: a match does not go over two
    const auto isBlockEnd = []( QChar c ) {
        const auto code = c.unicode();
        return code == QChar::ParagraphSeparator || code == 0xfdd0 || code == 0xfdd1
               || code == 0xfdef || code == QChar::ObjectReplacementCharacter;
    };

    DocumentSearch search;
    qsizetype blockStart = 0;
    for ( qsizetype i = 0; i <= rawText.size(); ++i ) {
        if ( i < rawText.size() && !isBlockEnd( rawText.at( i ) ) ) {
            continue;
        }
        if ( cancelled && cancelled->load( std::memory_order_relaxed ) ) {
            break;
        }
        if ( i > blockStart ) {
            for ( const auto& range :
                  matcher.matchRanges( rawText.mid( blockStart, i - blockStart ) ) ) {
                search.matches.emplace_back( static_cast<int>( blockStart ) + range.first,
                                             range.second );
                if ( search.matches.size() >= MaxDocumentMatches ) {
                    search.cut = true;
                    return search;
                }
            }
        }
        blockStart = i + 1;
    }
    return search;
}

} // namespace render

// Document: every match is found in a thread of the pool, on the text of the document
// taken here, then highlighted
void RenderView::searchDocument( const TextMatcher& matcher )
{
    auto cancelled = std::make_shared<std::atomic_bool>( false );
    treeSearchCancelled_ = cancelled;
    documentSearchRunning_ = true;
    setFindStatus( tr( "Searching…" ) );
    documentSearchWatcher_->setFuture( QtConcurrent::run(
        [ rawText = browser_->document()->toRawText(), matcher, cancelled ] {
            return render::findInDocument( rawText, matcher, cancelled.get() );
        } ) );
}

void RenderView::showDocumentMatches( const render::DocumentSearch& search )
{
    auto* document = browser_->document();

    // Made in an edit block: a cursor moved outside of one lays the document out up to it,
    // all of it for a match at the end
    QTextCursor editBlock( document );
    editBlock.beginEditBlock();
    documentMatches_.reserve( search.matches.size() );
    for ( const auto& match : search.matches ) {
        QTextCursor cursor( document );
        cursor.setPosition( match.first );
        cursor.setPosition( match.first + match.second, QTextCursor::KeepAnchor );
        documentMatches_.push_back( cursor );
    }
    editBlock.endEditBlock();
    documentMatchesCut_ = search.cut;
    stepDocument( pendingBackward_ );
}

void RenderView::updateDocumentHighlights()
{
    if ( documentMatches_.empty() ) {
        return;
    }
    // Only the matches around what is shown: a highlight is laid out to be drawn, those at
    // the end of a long document would lay it all out
    constexpr int Around = 2000;
    const auto* viewport = browser_->viewport();
    const auto first = browser_->cursorForPosition( QPoint( 0, 0 ) ).position() - Around;
    const auto last
        = browser_->cursorForPosition( QPoint( viewport->width(), viewport->height() ) ).position()
          + Around;
    const auto begin = std::lower_bound(
        documentMatches_.begin(), documentMatches_.end(), first,
        []( const QTextCursor& match, int position ) { return match.selectionEnd() < position; } );

    QTextCharFormat highlight;
    highlight.setBackground( MatchBackground );
    QList<QTextEdit::ExtraSelection> highlights;
    for ( auto it = begin; it != documentMatches_.end() && it->selectionStart() <= last; ++it ) {
        highlights.append( { *it, highlight } );
    }
    browser_->setExtraSelections( highlights );
}

void RenderView::stepDocument( bool backward )
{
    if ( documentMatches_.empty() ) {
        setFindStatus( tr( "No match" ), true );
        return;
    }

    // From the cursor of the browser, the selected match or where the user clicked
    const auto current = browser_->textCursor();
    const auto from = backward ? current.selectionStart() : current.selectionEnd();
    size_t index = 0;
    if ( backward ) {
        index = documentMatches_.size() - 1;
        for ( size_t i = documentMatches_.size(); i > 0; --i ) {
            if ( documentMatches_[ i - 1 ].selectionEnd() <= from
                 && documentMatches_[ i - 1 ].selectionStart() < from ) {
                index = i - 1;
                break;
            }
        }
    }
    else {
        for ( size_t i = 0; i < documentMatches_.size(); ++i ) {
            if ( documentMatches_[ i ].selectionStart() >= from ) {
                index = i;
                break;
            }
        }
    }

    browser_->setTextCursor( documentMatches_[ index ] );
    browser_->ensureCursorVisible();
    updateDocumentHighlights();
    setFindStatus( QStringLiteral( "%1 / %2%3" )
                       .arg( index + 1 )
                       .arg( documentMatches_.size() )
                       .arg( documentMatchesCut_ ? QStringLiteral( "+" ) : QString{} ) );
}

// Tree: the JSON values or the XML nodes, searched in a thread of the pool
void RenderView::searchTree( const TextMatcher& matcher )
{
    auto cancelled = std::make_shared<std::atomic_bool>( false );
    treeSearchCancelled_ = cancelled;
    setFindStatus( tr( "Searching…" ) );

    treeSearchRunning_ = true;
    if ( auto* json = qobject_cast<JsonTreeModel*>( treeModel_ ) ) {
        // The values are shared with the model, read only by both threads
        treeSearchWatcher_->setFuture(
            QtConcurrent::run( [ root = json->root(), matcher, cancelled ] {
                render::TreeSearch search;
                search.json = render::findInJson( root, matcher, cancelled.get() );
                return search;
            } ) );
    }
    else if ( xmlTree_ ) {
        treeSearchWatcher_->setFuture(
            QtConcurrent::run( [ tree = xmlTree_, matcher, cancelled ] {
                render::TreeSearch search;
                search.xml = render::findInXml( *tree, matcher, cancelled.get() );
                return search;
            } ) );
    }
    else {
        treeSearchRunning_ = false;
        setFindStatus( tr( "No match" ), true );
    }
}

void RenderView::stepTree( bool backward )
{
    const auto count = static_cast<int>( treeMatches_.json.empty() ? treeMatches_.xml.size()
                                                                   : treeMatches_.json.size() );
    if ( count == 0 ) {
        setFindStatus( tr( "No match" ), true );
        return;
    }

    // From the current match, or from the node the user went to
    const auto current = tree_->currentIndex();
    const auto fromMatch = treeMatch_ >= 0 && current.isValid() && current == shownTreeMatch_;
    if ( fromMatch ) {
        treeMatch_ = ( treeMatch_ + ( backward ? count - 1 : 1 ) ) % count;
    }
    else if ( current.isValid() ) {
        // The matches are in the order of the rows: the first one after the start of the
        // current cell (forward), the last one before it (backward)
        const auto position = treeMatchPosition( current );
        treeMatch_ = static_cast<int>( position.first );
        if ( backward ) {
            treeMatch_ = treeMatch_ > 0 ? treeMatch_ - 1 : count - 1;
        }
        else if ( treeMatch_ >= count ) {
            treeMatch_ = 0;
        }
    }
    else {
        treeMatch_ = backward ? count - 1 : 0;
    }

    QModelIndex index;
    int occurrence = 0;
    if ( auto* json = qobject_cast<JsonTreeModel*>( treeModel_ ) ) {
        const auto& match = treeMatches_.json[ static_cast<size_t>( treeMatch_ ) ];
        index = json->indexOfPath( match.rows, match.column );
        occurrence = match.occurrence;
    }
    else if ( auto* xml = qobject_cast<XmlTreeModel*>( treeModel_ ) ) {
        const auto& match = treeMatches_.xml[ static_cast<size_t>( treeMatch_ ) ];
        index = xml->indexOfNode( match.node, match.column );
        occurrence = match.occurrence;
    }
    if ( index.isValid() ) {
        // Expands the parents
        tree_->scrollTo( index, QAbstractItemView::PositionAtCenter );
        showCurrentMatch( tree_, index, occurrence );
        shownTreeMatch_ = index;
    }
    setFindStatus( QStringLiteral( "%1 / %2%3" )
                       .arg( treeMatch_ + 1 )
                       .arg( count )
                       .arg( static_cast<size_t>( count ) >= render::MaxTreeMatches
                                 ? QStringLiteral( "+" )
                                 : QString{} ) );
}

std::pair<size_t, bool> RenderView::treeMatchPosition( const QModelIndex& current ) const
{
    // Index of the first match at or after the start of the cell
    if ( !treeMatches_.json.empty() ) {
        std::vector<int> rows;
        for ( auto index = current.siblingAtColumn( 0 ); index.isValid(); index = index.parent() ) {
            rows.insert( rows.begin(), index.row() );
        }
        const auto key = std::make_pair( rows, current.column() );
        const auto it = std::lower_bound(
            treeMatches_.json.begin(), treeMatches_.json.end(), key,
            []( const render::JsonMatch& match, const std::pair<std::vector<int>, int>& cell ) {
                return std::tie( match.rows, match.column ) < std::tie( cell.first, cell.second );
            } );
        return { static_cast<size_t>( it - treeMatches_.json.begin() ), true };
    }
    const auto key = std::make_pair( static_cast<int>( current.internalId() ), current.column() );
    const auto it = std::lower_bound(
        treeMatches_.xml.begin(), treeMatches_.xml.end(), key,
        []( const render::XmlMatch& match, const std::pair<int, int>& cell ) {
            return std::tie( match.node, match.column ) < std::tie( cell.first, cell.second );
        } );
    return { static_cast<size_t>( it - treeMatches_.xml.begin() ), true };
}

// Table: the lines are searched by the search threads of flogg, like the search line, in
// results of their own
void RenderView::searchTable( const TextMatcher& matcher )
{
    if ( !tableSearch_ ) {
        tableSearch_ = logData_->getNewFilteredData();
        connect( tableSearch_.get(), &LogFilteredData::searchProgressed, this,
                 [ this ]( LinesCount, int progress, LineNumber ) {
                     tableSearchProgressed( progress );
                 },
                 Qt::QueuedConnection );
    }
    tableSearchDone_ = false;
    tableCellScan_ = render::needsCellScan( matcher );
    setFindStatus( tr( "Searching…" ) );
    // The matches in the cells are counted meanwhile
    startCounting( 0 );
    if ( tableCellScan_ ) {
        // Anchors or looking around: the lines are not searched as a whole, the counts of
        // the blocks lead to the cells
        tableSearchDone_ = true;
        stepTable( pendingBackward_ );
        return;
    }
    tableSearch_->runSearch( RegularExpressionPattern( matcher.text(), matcher.isCaseSensitive(),
                                                       false, false, !matcher.isRegex() ) );
}

void RenderView::tableSearchProgressed( int progress )
{
    if ( !searched_ || tableSearchDone_ ) {
        return;
    }
    if ( progress < 100 ) {
        setFindStatus( tr( "Searching… %1%" ).arg( progress ) );
        return;
    }
    tableSearchDone_ = true;
    stepTable( pendingBackward_ );
}

void RenderView::showCurrentMatch( QAbstractItemView* view, const QModelIndex& index,
                                   int occurrence )
{
    // Not selected: only the text of the match is shown like the selection, like in a document
    highlightDelegate_->setCurrentMatch( index, occurrence );
    view->selectionModel()->setCurrentIndex( index, QItemSelectionModel::Clear );
    if ( occurrence >= 0 ) {
        revealOccurrence( view, index, occurrence );
    }
    view->viewport()->update();
}

void RenderView::revealOccurrence( QAbstractItemView* view, const QModelIndex& index,
                                   int occurrence )
{
    const auto text = index.data( SearchTextRole ).toString();
    const auto ranges = searched_ ? searched_->matchRanges( text )
                                  : std::vector<std::pair<int, int>>{};
    if ( occurrence >= static_cast<int>( ranges.size() ) ) {
        return;
    }
    const auto& range = ranges[ static_cast<size_t>( occurrence ) ];
    const auto metrics = view->fontMetrics();
    const auto matchStart = metrics.horizontalAdvance( text.left( range.first ) );
    const auto matchEnd = metrics.horizontalAdvance( text.left( range.first + range.second ) );
    // The margins of the text in the cell, and some room after the match
    const auto margin = 2 * ( view->style()->pixelMetric( QStyle::PM_FocusFrameHMargin ) + 1 ) + 16;

    // The column is widened when the match is cut by its width
    auto* header = view == table_ ? table_->horizontalHeader() : tree_->header();
    const auto column = index.column();
    if ( view->visualRect( index ).width() < matchEnd + margin ) {
        if ( header->stretchLastSection() && column == header->count() - 1 ) {
            header->setStretchLastSection( false );
        }
        const auto indentation = view->visualRect( index ).left()
                                 - view->visualRect( index.siblingAtColumn( column ) ).left();
        header->resizeSection( column, matchEnd + margin + std::max( 0, indentation ) );
    }

    // Then scrolled to, the cell may be wider than the view
    const auto cell = view->visualRect( index );
    const auto left = cell.left() + margin / 2 + matchStart;
    const auto right = cell.left() + margin / 2 + matchEnd;
    auto* scrollBar = view->horizontalScrollBar();
    if ( left < 0 || right > view->viewport()->width() ) {
        const auto value = scrollBar->value() + left - view->viewport()->width() / 3;
        // The range follows a widened column later, the view lays itself out after an event
        if ( value > scrollBar->maximum() ) {
            scrollBar->setMaximum( value );
        }
        scrollBar->setValue( value );
    }
}

RenderView::RowSearch RenderView::nextRowBySearch( int fromRow, bool backward,
                                                   const std::function<bool( int )>& hasMatch )
{
    // At most this number of matching lines is looked at for a matching cell at each step:
    // a line can match outside of its cells (the keys of JSON Lines, a separator)
    constexpr quint64 MaxRowsChecked = 1000;

    auto* model = tableModel_.data();
    const auto matches = tableSearch_->getNbMatches().get();
    const auto headerMatched = model->hasHeader() && matches > 0
                               && tableSearch_->getMatchingLineNumber( 0_lnum ).get() == 0;
    const auto count = matches - ( headerMatched ? 1 : 0 );
    if ( count == 0 ) {
        return { RowSearch::None };
    }

    // Index of the matches list, the header (first match if matched) left out
    const auto first = headerMatched ? 1u : 0u;
    const auto last = matches - 1;
    const auto lineOf = [ this ]( quint64 index ) {
        return tableSearch_->getMatchingLineNumber(
                               LineNumber( static_cast<LineNumber::UnderlyingType>( index ) ) )
            .get();
    };

    quint64 index = backward ? last : first;
    if ( fromRow >= 0 ) {
        const auto fromLine = static_cast<quint64>( model->lineOfRow( fromRow ) );
        // Last match at or before the line
        const auto at = tableSearch_->getLineIndexNumber( LineNumber( fromLine ) ).get();
        if ( lineOf( at ) > fromLine ) {
            // No match before the line
            index = backward ? last : at;
        }
        else if ( backward ) {
            index = lineOf( at ) < fromLine ? at : ( at > first ? at - 1 : last );
        }
        else {
            index = at < last ? at + 1 : first;
        }
        if ( index < first ) {
            index = backward ? last : first;
        }
    }

    // The next lines found by the search, until one has a cell with matches
    const auto checked = std::min<quint64>( count, MaxRowsChecked );
    int lastRow = -1;
    for ( quint64 i = 0; i < checked; ++i ) {
        const auto row = model->rowOfLine( static_cast<qint64>( lineOf( index ) ) );
        if ( row >= 0 && row < model->rowCount() ) {
            if ( hasMatch( row ) ) {
                return { RowSearch::Found, row };
            }
            lastRow = row;
        }
        index = backward ? ( index > first ? index - 1 : last ) : ( index < last ? index + 1 : first );
    }
    if ( checked < count ) {
        return { RowSearch::Limit, lastRow, checked };
    }
    // Found only outside of the cells
    return { RowSearch::None };
}

RenderView::RowSearch RenderView::nextRowByBlocks( int fromRow, bool backward )
{
    auto* model = tableModel_.data();
    const auto blocks = static_cast<qint64>( blockCounts_.size() );
    if ( blocks == 0 ) {
        return { RowSearch::None };
    }
    const auto lines = static_cast<qint64>( countedLines_ );
    const qint64 fromLine
        = fromRow >= 0 ? model->lineOfRow( fromRow ) : ( backward ? lines : -1 );
    const auto blockLines = static_cast<qint64>( CountBlockLines );
    const auto startBlock = std::clamp<qint64>( fromLine < 0 ? 0 : fromLine / blockLines, 0,
                                                blocks - 1 );

    // Around the blocks, the block of the line twice: after the line, then before it
    for ( qint64 visit = 0; visit <= blocks; ++visit ) {
        const auto block
            = ( ( backward ? startBlock - visit : startBlock + visit ) % blocks + blocks ) % blocks;
        const auto blockCount = blockCounts_[ static_cast<size_t>( block ) ];
        if ( blockCount < 0 ) {
            return { RowSearch::Pending };
        }
        if ( blockCount == 0 ) {
            continue;
        }
        const auto& counts = lineCountsOfBlock( static_cast<quint64>( block ) );
        const auto size = static_cast<qint64>( counts.size() );
        for ( qint64 i = 0; i < size; ++i ) {
            const auto offset = backward ? size - 1 - i : i;
            const auto line = block * blockLines + offset;
            if ( visit == 0 && ( backward ? line >= fromLine : line <= fromLine ) ) {
                continue;
            }
            if ( visit == blocks && ( backward ? line < fromLine : line > fromLine ) ) {
                continue;
            }
            if ( counts[ static_cast<size_t>( offset ) ] > 0 ) {
                const auto row = model->rowOfLine( line );
                if ( row >= 0 && row < model->rowCount() ) {
                    return { RowSearch::Found, row };
                }
            }
        }
    }
    return { RowSearch::None };
}

void RenderView::stepTable( bool backward )
{
    if ( !tableModel_ || !searched_ || ( !tableCellScan_ && !tableSearch_ ) ) {
        return;
    }
    auto* model = tableModel_.data();
    const auto columns = model->columnCount();
    const auto occurrences = [ this, model ]( int row, int column ) {
        return static_cast<int>(
            searched_->matchRanges( model->cellDisplayText( row, column ) ).size() );
    };
    // First (last) cell with matches of the row from the column on (back)
    const auto matchingColumn = [ & ]( int row, int from ) {
        for ( int column = from; column >= 0 && column < columns;
              column += backward ? -1 : 1 ) {
            if ( occurrences( row, column ) > 0 ) {
                return column;
            }
        }
        return -1;
    };
    const auto showMatch = [ &, this ]( int row, int column, int occurrence ) {
        const auto cell = model->index( row, column );
        table_->scrollTo( cell, QAbstractItemView::PositionAtCenter );
        tableMatch_ = cell;
        tableOccurrence_ = occurrence;
        showCurrentMatch( table_, cell, occurrence );
        updateTableStatus();
    };
    const auto firstOrLast = [ & ]( int row, int column ) {
        showMatch( row, column, backward ? occurrences( row, column ) - 1 : 0 );
    };

    // In the current row first
    const auto current = table_->currentIndex();
    if ( current.isValid() ) {
        const auto row = current.row();
        // From the current match, or from the start of the cell the user went to
        const auto fromMatch = tableMatch_.isValid() && tableMatch_ == current;
        if ( fromMatch ) {
            const auto next = tableOccurrence_ + ( backward ? -1 : 1 );
            if ( next >= 0 && next < occurrences( row, current.column() ) ) {
                showMatch( row, current.column(), next );
                return;
            }
        }
        else if ( !backward && occurrences( row, current.column() ) > 0 ) {
            showMatch( row, current.column(), 0 );
            return;
        }
        const auto column = matchingColumn( row, current.column() + ( backward ? -1 : 1 ) );
        if ( column >= 0 ) {
            firstOrLast( row, column );
            return;
        }
    }

    // Then the next rows, from the lines found by the search or from the counts of the blocks
    const auto fromRow = current.isValid() ? current.row() : -1;
    const auto next = tableCellScan_
                          ? nextRowByBlocks( fromRow, backward )
                          : nextRowBySearch( fromRow, backward, [ & ]( int row ) {
                                return matchingColumn( row, backward ? columns - 1 : 0 ) >= 0;
                            } );
    switch ( next.state ) {
    case RowSearch::Found:
        firstOrLast( next.row, matchingColumn( next.row, backward ? columns - 1 : 0 ) );
        break;
    case RowSearch::Pending:
        // Stepped again when the blocks are counted
        pendingTableStep_ = true;
        pendingBackward_ = backward;
        setFindStatus( tr( "Searching…" ) );
        break;
    case RowSearch::Limit: {
        // Goes on from there the next time
        const auto cell = model->index( next.row, backward ? 0 : columns - 1 );
        tableMatch_ = QPersistentModelIndex();
        showCurrentMatch( table_, cell, -1 );
        table_->scrollTo( cell, QAbstractItemView::PositionAtCenter );
        setFindStatus( tr( "No match in the cells of the next %1 rows" ).arg( next.checked ),
                       true );
        break;
    }
    case RowSearch::None:
        setFindStatus( tr( "No match" ), true );
        break;
    }
}

/*
 * Count of the matches of a table, in blocks of lines counted by the threads of the pool.
 * Only the counts of the blocks are kept.
 */
namespace {

int countBlock( const LogData& logData, const render::LineFormat& format,
                const TextMatcher& matcher, bool cellScan, bool header, quint64 block,
                quint64 lines )
{
    const auto firstLine = block * RenderView::CountBlockLines;
    if ( firstLine >= lines ) {
        return 0;
    }
    const auto number = std::min( RenderView::CountBlockLines, lines - firstLine );
    const auto read = logData.getLines(
        LineNumber( static_cast<LineNumber::UnderlyingType>( firstLine ) ),
        LinesCount( static_cast<LinesCount::UnderlyingType>( number ) ) );
    int count = 0;
    for ( size_t i = 0; i < read.size(); ++i ) {
        if ( header && firstLine + i == 0 ) {
            continue;
        }
        count += format.countMatches( read[ i ], matcher, cellScan );
    }
    return count;
}

} // namespace

void RenderView::startCounting( quint64 fromBlock )
{
    if ( !tableModel_ || !searched_ ) {
        return;
    }
    const auto lines = logData_->getNbLine().get();
    const auto blocks = ( lines + CountBlockLines - 1 ) / CountBlockLines;
    blockCounts_.resize( static_cast<size_t>( blocks ), -1 );
    std::fill( blockCounts_.begin() + static_cast<qsizetype>( fromBlock ), blockCounts_.end(),
               -1 );
    countedLines_ = lines;
    countFirstBlock_ = fromBlock;
    countPending_ = false;
    counting_ = true;
    if ( lineCountsBlock_ >= fromBlock ) {
        lineCountsBlock_ = NoBlock;
    }

    QList<quint64> toCount;
    for ( auto block = fromBlock; block < blocks; ++block ) {
        toCount.append( block );
    }
    countWatcher_->setFuture( QtConcurrent::mapped(
        std::move( toCount ),
        [ logData = logData_, format = tableModel_->lineFormat(), matcher = *searched_,
          cellScan = tableCellScan_, header = tableModel_->hasHeader(), lines ]( quint64 block ) {
            return countBlock( *logData, format, matcher, cellScan, header, block, lines );
        } ) );
}

void RenderView::countProgressed( int begin, int end )
{
    if ( !searched_ || countWatcher_->isCanceled() ) {
        return;
    }
    for ( int i = begin; i < end; ++i ) {
        const auto block = countFirstBlock_ + static_cast<quint64>( i );
        if ( block < blockCounts_.size() ) {
            blockCounts_[ block ] = countWatcher_->resultAt( i );
        }
    }
    if ( std::exchange( pendingTableStep_, false ) ) {
        stepTable( pendingBackward_ );
        return;
    }
    updateTableStatus();
}

std::optional<quint64> RenderView::matchesBeforeLine( quint64 line )
{
    const auto block = line / CountBlockLines;
    if ( block >= blockCounts_.size() ) {
        return std::nullopt;
    }
    quint64 before = 0;
    for ( quint64 b = 0; b < block; ++b ) {
        if ( blockCounts_[ b ] < 0 ) {
            return std::nullopt;
        }
        before += static_cast<quint64>( blockCounts_[ b ] );
    }

    const auto& counts = lineCountsOfBlock( block );
    for ( quint64 i = 0; i < line - block * CountBlockLines && i < counts.size(); ++i ) {
        before += static_cast<quint64>( counts[ i ] );
    }
    return before;
}

const std::vector<int>& RenderView::lineCountsOfBlock( quint64 block )
{
    // The lines of one block, counted again here once
    if ( lineCountsBlock_ != block ) {
        const auto firstLine = block * CountBlockLines;
        const auto number
            = firstLine < countedLines_ ? std::min( CountBlockLines, countedLines_ - firstLine ) : 0;
        const auto read = logData_->getLines(
            LineNumber( static_cast<LineNumber::UnderlyingType>( firstLine ) ),
            LinesCount( static_cast<LinesCount::UnderlyingType>( number ) ) );
        const auto& format = tableModel_->lineFormat();
        lineCounts_.assign( read.size(), 0 );
        for ( size_t i = 0; i < read.size(); ++i ) {
            if ( !( tableModel_->hasHeader() && firstLine + i == 0 ) ) {
                lineCounts_[ i ] = format.countMatches( read[ i ], *searched_, tableCellScan_ );
            }
        }
        lineCountsBlock_ = block;
    }
    return lineCounts_;
}

void RenderView::updateTableStatus()
{
    if ( !tableModel_ || !searched_ || !tableMatch_.isValid() ) {
        return;
    }
    quint64 total = 0;
    bool complete = !counting_ && !countPending_;
    for ( const auto count : blockCounts_ ) {
        if ( count < 0 ) {
            complete = false;
        }
        else {
            total += static_cast<quint64>( count );
        }
    }
    const auto totalText = QString::number( total ) + ( complete ? QString{} : QStringLiteral( "+" ) );

    const auto row = tableMatch_.row();
    const auto before
        = matchesBeforeLine( static_cast<quint64>( tableModel_->lineOfRow( row ) ) );
    if ( !before ) {
        setFindStatus( QStringLiteral( "? / %1" ).arg( totalText ) );
        return;
    }
    auto rank = *before + static_cast<quint64>( tableOccurrence_ ) + 1;
    for ( int column = 0; column < tableMatch_.column(); ++column ) {
        rank += searched_->matchRanges( tableModel_->cellDisplayText( row, column ) ).size();
    }
    setFindStatus( QStringLiteral( "%1 / %2" ).arg( rank ).arg( totalText ) );
}
