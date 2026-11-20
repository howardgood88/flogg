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

#include "dirsearch.h"

#include <QCollator>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTextCodec>

#include <algorithm>

#include "containers.h"
#include "encodingdetector.h"

namespace dirsearch {

namespace {

constexpr int MibUtf8 = 106;
constexpr int MibAscii = 3;
// Read at once, then cut after the last whole line
constexpr qint64 BlockSize = 1024 * 1024;
// What is read to guess the encoding
constexpr qint64 HeadSize = 64 * 1024;
// A line longer than this is not searched (a binary file without zeros)
constexpr qsizetype MaxLineLength = 64 * 1024 * 1024;

QString tr( const char* text )
{
    return QCoreApplication::translate( "DirSearch", text );
}

bool isCancelled( const std::atomic_bool* cancelled )
{
    return cancelled && cancelled->load( std::memory_order_relaxed );
}

// The file read in blocks of whole lines, each one given in its bytes and as text
class LineBlocks {
  public:
    bool open( const QString& path, QString* error )
    {
        file_.setFileName( path );
        if ( !file_.open( QIODevice::ReadOnly ) ) {
            *error = tr( "The file can not be read: %1" ).arg( file_.errorString() );
            return false;
        }

        const auto head = file_.peek( HeadSize );
        // A byte order mark gives the encoding, it is kept out of the text
        if ( auto* utf = QTextCodec::codecForUtfText( head, nullptr ) ) {
            codec_ = utf;
            const auto mib = utf->mibEnum();
            bom_ = head.left( mib == MibUtf8 ? 3 : ( mib == 1017 || mib == 1018 || mib == 1019 ) ? 4 : 2 );
        }
        else if ( !head.isEmpty() ) {
            klogg::vector<char> block( head.begin(), head.end() );
            codec_ = EncodingDetector::getInstance().detectEncoding( block );
        }
        if ( codec_ == nullptr ) {
            codec_ = QTextCodec::codecForMib( MibUtf8 );
        }
        utf8_ = codec_->mibEnum() == MibUtf8 || codec_->mibEnum() == MibAscii;
        const EncodingParameters parameters( codec_ );
        lineFeedWidth_ = parameters.lineFeedWidth;
        lineFeedIndex_ = parameters.lineFeedIndex;

        // Zeros in a file of single byte line feeds: a binary file
        if ( lineFeedWidth_ == 1 && head.indexOf( '\0' ) >= 0 ) {
            binary_ = true;
            return false;
        }
        if ( !bom_.isEmpty() ) {
            file_.read( bom_.size() );
        }
        return true;
    }

    bool isBinary() const
    {
        return binary_;
    }
    const QByteArray& bom() const
    {
        return bom_;
    }
    QTextCodec* codec() const
    {
        return codec_;
    }
    bool isUtf8() const
    {
        return utf8_;
    }
    qint64 size() const
    {
        return file_.size();
    }

    // The next block of whole lines (the last one may have no end of line), false at the
    // end of the file or on an error
    bool next( QByteArray* bytes, QString* text, QString* error )
    {
        while ( true ) {
            if ( file_.atEnd() ) {
                if ( carry_.isEmpty() ) {
                    return false;
                }
                *bytes = std::exchange( carry_, {} );
                *text = decode( *bytes );
                return true;
            }
            auto block = file_.read( BlockSize );
            if ( block.isEmpty() && file_.error() != QFileDevice::NoError ) {
                *error = tr( "The file can not be read: %1" ).arg( file_.errorString() );
                return false;
            }
            if ( !carry_.isEmpty() ) {
                block.prepend( std::exchange( carry_, {} ) );
            }
            const auto end = lastLineEnd( block );
            if ( end == 0 ) {
                if ( block.size() > MaxLineLength ) {
                    *error = tr( "A line is too long to be searched." );
                    return false;
                }
                carry_ = std::move( block );
                continue;
            }
            if ( end < block.size() ) {
                carry_ = block.mid( end );
                block.truncate( end );
            }
            *bytes = std::move( block );
            *text = decode( *bytes );
            return true;
        }
    }

    // Where each line of the block starts, and the end of the block last
    std::vector<qsizetype> lineStarts( const QByteArray& bytes ) const
    {
        std::vector<qsizetype> starts{ 0 };
        const auto* data = bytes.constData();
        const auto size = bytes.size();
        const auto width = lineFeedWidth_;
        for ( qsizetype i = 0; i + width <= size; i += width ) {
            if ( isLineFeed( data + i ) ) {
                starts.push_back( i + width );
            }
        }
        if ( starts.back() != size ) {
            starts.push_back( size );
        }
        return starts;
    }

    QByteArray encode( const QString& text, bool* ok ) const
    {
        if ( utf8_ ) {
            *ok = true;
            return text.toUtf8();
        }
        QTextCodec::ConverterState state( QTextCodec::IgnoreHeader );
        auto bytes = codec_->fromUnicode( text.constData(), static_cast<int>( text.size() ), &state );
        *ok = state.invalidChars == 0;
        return bytes;
    }

  private:
    bool isLineFeed( const char* unit ) const
    {
        for ( int i = 0; i < lineFeedWidth_; ++i ) {
            if ( unit[ i ] != ( i == lineFeedIndex_ ? '\n' : '\0' ) ) {
                return false;
            }
        }
        return true;
    }

    // After the last line feed, 0 when none
    qsizetype lastLineEnd( const QByteArray& block ) const
    {
        if ( lineFeedWidth_ == 1 ) {
            return block.lastIndexOf( '\n' ) + 1;
        }
        const auto width = lineFeedWidth_;
        for ( auto i = ( block.size() / width - 1 ) * width; i >= 0; i -= width ) {
            if ( isLineFeed( block.constData() + i ) ) {
                return i + width;
            }
        }
        return 0;
    }

    QString decode( const QByteArray& bytes ) const
    {
        if ( utf8_ ) {
            return QString::fromUtf8( bytes );
        }
        QTextCodec::ConverterState state( QTextCodec::IgnoreHeader );
        return codec_->toUnicode( bytes.constData(), static_cast<int>( bytes.size() ), &state );
    }

    QFile file_;
    QTextCodec* codec_ = nullptr;
    QByteArray bom_;
    bool utf8_ = true;
    bool binary_ = false;
    int lineFeedWidth_ = 1;
    int lineFeedIndex_ = 0;
    QByteArray carry_;
};

// A match, at base + its position in the text it was found in
struct Hit {
    QRegularExpressionMatch match;
    qsizetype base = 0;

    qsizetype start() const
    {
        return base + match.capturedStart();
    }
    qsizetype end() const
    {
        return base + match.capturedEnd();
    }
};

// The matches of the lines of a block, line by line: a match does not go past the end of
// its line, and an empty match is not one. Gives the index of the line in the block, its
// start and end (without the line feed) in the text and its matches. One iterator over the
// block: each new match() call would check the whole block is valid UTF-16 again.
template <typename LineCallback>
void forEachMatchingLine( const QString& text, const QRegularExpression& pattern,
                          LineCallback&& callback )
{
    qsizetype line = 0;
    qsizetype lineStart = 0;
    // Where the lines were counted to
    qsizetype counted = 0;
    const auto lineEndFrom = [ &text ]( qsizetype from ) {
        const auto end = text.indexOf( QLatin1Char( '\n' ), from );
        return end < 0 ? text.size() : end;
    };
    const auto moveTo = [ & ]( qsizetype position ) {
        for ( auto feed = text.indexOf( QLatin1Char( '\n' ), counted );
              feed >= 0 && feed < position; feed = text.indexOf( QLatin1Char( '\n' ), feed + 1 ) ) {
            ++line;
            lineStart = feed + 1;
        }
        counted = std::max( counted, position );
    };

    std::vector<Hit> hits;
    qsizetype currentLine = -1;
    qsizetype currentStart = 0;
    qsizetype currentEnd = 0;
    const auto flush = [ & ] {
        if ( !hits.empty() ) {
            callback( currentLine, currentStart, currentEnd, hits );
            hits.clear();
        }
    };
    // The matches before it belong to lines matched alone
    qsizetype handledUntil = 0;

    for ( auto it = pattern.globalMatch( text ); it.hasNext(); ) {
        auto match = it.next();
        if ( match.capturedLength() == 0 || match.capturedStart() < handledUntil ) {
            continue;
        }
        moveTo( match.capturedStart() );
        if ( line != currentLine ) {
            flush();
            currentLine = line;
            currentStart = lineStart;
            currentEnd = lineEndFrom( lineStart );
        }
        if ( match.capturedEnd() <= currentEnd ) {
            hits.push_back( { match, 0 } );
            continue;
        }

        // Crossing the end of the line (\s, [^x]): the lines it spans are matched alone
        hits.clear();
        auto start = currentStart;
        auto index = currentLine;
        while ( true ) {
            const auto end = lineEndFrom( start );
            const auto lineText = text.mid( start, end - start );
            for ( auto lineIt = pattern.globalMatch( lineText ); lineIt.hasNext(); ) {
                auto lineMatch = lineIt.next();
                if ( lineMatch.capturedLength() > 0 ) {
                    hits.push_back( { lineMatch, start } );
                }
            }
            if ( !hits.empty() ) {
                callback( index, start, end, hits );
                hits.clear();
            }
            if ( match.capturedEnd() <= end || end >= text.size() ) {
                handledUntil = end + 1;
                break;
            }
            start = end + 1;
            ++index;
        }
        currentLine = -1;
    }
    flush();
}

LineMatch makeLineMatch( qint64 lineNumber, const QString& text, qsizetype lineStart,
                         qsizetype lineEnd, const std::vector<Hit>& hits )
{
    LineMatch result;
    result.line = lineNumber;
    auto length = lineEnd - lineStart;
    if ( length > 0 && text.at( lineEnd - 1 ) == QLatin1Char( '\r' ) ) {
        --length;
    }
    // Around the first match, with some text before it
    qsizetype from = 0;
    const auto first = hits.front().start() - lineStart;
    if ( length > MaxPreviewLength && first > 40 ) {
        from = std::min( first - 40, length - MaxPreviewLength );
    }
    const auto shown = std::min<qsizetype>( length - from, MaxPreviewLength );
    QString prefix = from > 0 ? QStringLiteral( "…" ) : QString{};
    result.preview = prefix + text.mid( lineStart + from, shown );
    if ( from + shown < length ) {
        result.preview += QStringLiteral( "…" );
    }
    for ( const auto& hit : hits ) {
        const auto start = hit.start() - lineStart - from;
        const auto end = std::min( hit.end() - lineStart - from, shown );
        if ( start >= 0 && start < shown && end > start ) {
            result.ranges.emplace_back( static_cast<int>( start + prefix.size() ),
                                        static_cast<int>( end - start ) );
        }
    }
    return result;
}

void listFolder( const QDir& dir, const QRegularExpression& namePattern, bool showHidden,
                 const std::atomic_bool* cancelled, QStringList* files )
{
    if ( isCancelled( cancelled ) ) {
        return;
    }
    const auto hidden = showHidden ? QDir::Hidden : QDir::Filters{};
    const auto entries = dir.entryInfoList( QDir::Files | QDir::Readable | hidden, QDir::NoSort );
    for ( const auto& entry : entries ) {
        if ( namePattern.pattern().isEmpty() || namePattern.match( entry.fileName() ).hasMatch() ) {
            files->append( entry.absoluteFilePath() );
        }
    }
    // Links to folders are not followed (a loop)
    const auto folders
        = dir.entryInfoList( QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks | hidden,
                             QDir::NoSort );
    for ( const auto& folder : folders ) {
        listFolder( QDir( folder.absoluteFilePath() ), namePattern, showHidden, cancelled, files );
    }
}

} // namespace

QRegularExpression Query::pattern() const
{
    auto options = QRegularExpression::MultilineOption | QRegularExpression::UseUnicodePropertiesOption;
    if ( !caseSensitive ) {
        options |= QRegularExpression::CaseInsensitiveOption;
    }
    QRegularExpression result( regex ? text : QRegularExpression::escape( text ), options );
    result.optimize();
    return result;
}

QStringList listFiles( const QString& folder, const QRegularExpression& namePattern,
                       bool showHidden, const std::atomic_bool* cancelled )
{
    QStringList files;
    listFolder( QDir( folder ), namePattern, showHidden, cancelled, &files );
    QCollator collator;
    collator.setNumericMode( true );
    collator.setCaseSensitivity( Qt::CaseInsensitive );
    std::sort( files.begin(), files.end(), [ &collator ]( const QString& a, const QString& b ) {
        return collator.compare( a, b ) < 0;
    } );
    return files;
}

bool searchFile( const QString& path, const QRegularExpression& pattern, qint64 maxLines,
                 FileResult* result, QString* error, const std::atomic_bool* cancelled )
{
    *result = FileResult{};
    result->path = path;
    const QFileInfo info( path );
    result->size = info.size();
    result->modified = info.lastModified();

    LineBlocks blocks;
    if ( !blocks.open( path, error ) ) {
        return false;
    }

    QByteArray bytes;
    QString text;
    qint64 firstLine = 0;
    while ( blocks.next( &bytes, &text, error ) ) {
        if ( isCancelled( cancelled ) ) {
            return false;
        }
        forEachMatchingLine( text, pattern,
                             [ & ]( qsizetype line, qsizetype lineStart, qsizetype lineEnd,
                                    const std::vector<Hit>& hits ) {
                                 result->matchCount += static_cast<qint64>( hits.size() );
                                 if ( static_cast<qint64>( result->lines.size() ) < maxLines ) {
                                     result->lines.push_back( makeLineMatch(
                                         firstLine + line, text, lineStart, lineEnd, hits ) );
                                 }
                             } );
        const auto feeds = text.count( QLatin1Char( '\n' ) );
        firstLine += feeds;
        if ( !text.endsWith( QLatin1Char( '\n' ) ) ) {
            // The last line, without an end
            result->lineCount = firstLine + 1;
        }
        else {
            result->lineCount = firstLine;
        }
    }
    return error->isEmpty();
}

bool exportMatches( const QStringList& files, const QString& root,
                    const QRegularExpression& pattern, QIODevice* output, QString* error,
                    const std::atomic_bool* cancelled, std::atomic_int* filesDone )
{
    const QDir rootDir( root );
    for ( const auto& path : files ) {
        LineBlocks blocks;
        QString ignored;
        if ( blocks.open( path, &ignored ) ) {
            const auto prefix = QDir::toNativeSeparators( rootDir.relativeFilePath( path ) ) + ':';
            QByteArray bytes;
            QString text;
            QString line;
            qint64 firstLine = 0;
            while ( blocks.next( &bytes, &text, &ignored ) ) {
                if ( isCancelled( cancelled ) ) {
                    return false;
                }
                QByteArray out;
                forEachMatchingLine( text, pattern,
                                     [ & ]( qsizetype index, qsizetype lineStart, qsizetype lineEnd,
                                            const std::vector<Hit>& ) {
                                         auto end = lineEnd;
                                         if ( end > lineStart
                                              && text.at( end - 1 ) == QLatin1Char( '\r' ) ) {
                                             --end;
                                         }
                                         line = prefix + QString::number( firstLine + index + 1 )
                                                + ':';
                                         line += QStringView( text ).mid( lineStart, end - lineStart );
                                         out += line.toUtf8();
                                         out += '\n';
                                     } );
                if ( !out.isEmpty() && output->write( out ) != out.size() ) {
                    *error = output->errorString();
                    return false;
                }
                firstLine += text.count( QLatin1Char( '\n' ) );
            }
        }
        if ( filesDone ) {
            ++*filesDone;
        }
    }
    return true;
}

QString expandReplacement( const QRegularExpressionMatch& match, const QString& replacement )
{
    QString result;
    result.reserve( replacement.size() );
    for ( qsizetype i = 0; i < replacement.size(); ++i ) {
        const auto c = replacement.at( i );
        if ( c == QLatin1Char( '\\' ) && i + 1 < replacement.size() ) {
            const auto next = replacement.at( i + 1 );
            if ( next.isDigit() ) {
                result += match.captured( next.digitValue() );
                ++i;
                continue;
            }
            if ( next == QLatin1Char( '\\' ) ) {
                result += next;
                ++i;
                continue;
            }
        }
        result += c;
    }
    return result;
}

qint64 replaceInFile( const QString& path, const QRegularExpression& pattern,
                      const QString& replacement, bool expandGroups, const QSet<qint64>* lines,
                      QString* error, const std::atomic_bool* cancelled )
{
    LineBlocks blocks;
    if ( !blocks.open( path, error ) ) {
        if ( blocks.isBinary() ) {
            *error = tr( "The file is binary." );
        }
        return -1;
    }

    // Written next to the file, renamed over it once complete (its permissions kept);
    // dropped when nothing was replaced
    QSaveFile output( path );
    if ( !output.open( QIODevice::WriteOnly ) ) {
        *error = tr( "The file can not be written: %1" ).arg( output.errorString() );
        return -1;
    }
    output.write( blocks.bom() );
    const auto fail = [ &output ] {
        output.cancelWriting();
        return -1;
    };

    bool ok = true;
    const auto lineFeed = blocks.encode( QStringLiteral( "\n" ), &ok );
    qint64 replaced = 0;
    qint64 firstLine = 0;
    QByteArray bytes;
    QString text;
    while ( blocks.next( &bytes, &text, error ) ) {
        if ( isCancelled( cancelled ) ) {
            return fail();
        }

        // The lines changed: index in the block, text before and after
        struct Change {
            qsizetype line;
            QString original;
            QString changed;
        };
        std::vector<Change> changes;
        forEachMatchingLine(
            text, pattern,
            [ & ]( qsizetype line, qsizetype lineStart, qsizetype lineEnd,
                   const std::vector<Hit>& hits ) {
                if ( lines && !lines->contains( firstLine + line ) ) {
                    return;
                }
                QString changed;
                auto position = lineStart;
                for ( const auto& hit : hits ) {
                    changed += QStringView( text ).mid( position, hit.start() - position );
                    changed += expandGroups ? expandReplacement( hit.match, replacement )
                                            : replacement;
                    position = hit.end();
                }
                changed += QStringView( text ).mid( position, lineEnd - position );
                replaced += static_cast<qint64>( hits.size() );
                changes.push_back(
                    { line, text.mid( lineStart, lineEnd - lineStart ), std::move( changed ) } );
            } );

        if ( changes.empty() ) {
            output.write( bytes );
            firstLine += text.count( QLatin1Char( '\n' ) );
            continue;
        }

        // The lines not changed are copied byte for byte
        const auto starts = blocks.lineStarts( bytes );
        qsizetype copied = 0;
        for ( const auto& change : changes ) {
            if ( change.line + 1 >= static_cast<qsizetype>( starts.size() ) ) {
                continue;
            }
            const auto lineStart = starts[ static_cast<size_t>( change.line ) ];
            const auto nextStart = starts[ static_cast<size_t>( change.line + 1 ) ];
            auto body = bytes.mid( lineStart, nextStart - lineStart );
            const auto hasFeed = body.endsWith( lineFeed );
            if ( hasFeed ) {
                body.chop( lineFeed.size() );
            }
            // A line with bytes its encoding can not read would not be written back the same
            if ( blocks.encode( change.original, &ok ) != body || !ok ) {
                *error = tr( "Line %1 has bytes that can not be read in %2, the file was not "
                             "changed." )
                             .arg( firstLine + change.line + 1 )
                             .arg( QString::fromLatin1( blocks.codec()->name() ) );
                return fail();
            }
            const auto encoded = blocks.encode( change.changed, &ok );
            if ( !ok ) {
                *error = tr( "Some characters can not be written in %1, the file was not "
                             "changed." )
                             .arg( QString::fromLatin1( blocks.codec()->name() ) );
                return fail();
            }
            output.write( bytes.constData() + copied, lineStart - copied );
            output.write( encoded );
            if ( hasFeed ) {
                output.write( lineFeed );
            }
            copied = nextStart;
        }
        output.write( bytes.constData() + copied, bytes.size() - copied );
        firstLine += text.count( QLatin1Char( '\n' ) );
    }

    if ( !error->isEmpty() ) {
        return fail();
    }
    if ( replaced == 0 ) {
        output.cancelWriting();
        return 0;
    }
    if ( !output.commit() ) {
        *error = tr( "The file can not be written: %1" ).arg( output.errorString() );
        return -1;
    }
    return replaced;
}

} // namespace dirsearch
