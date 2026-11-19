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

#ifndef KLOGG_EDITDOCUMENT_H
#define KLOGG_EDITDOCUMENT_H

#include <QByteArray>
#include <QString>

#include <atomic>
#include <cstddef>
#include <functional>
#include <optional>

class QTextCodec;

// Reading and writing the file of the edit mode. The editor (Scintilla) holds the text in
// UTF-8: a file in another encoding is converted when it is read and back when it is saved.
// Done in a thread of the pool, in blocks, without the whole file twice in memory.
namespace edit {

// How the file was written, to write it back the same way
struct FileFormat {
    // Name of the codec, empty for UTF-8 (or ASCII), which is not converted
    QByteArray codec;
    // A byte order mark started the file
    QByteArray bom;
    // End of the lines: "\r\n", "\n" or "\r", as the first line of the file
    QByteArray endOfLine = "\n";
    // Bytes that could not be decoded (replaced): saving changes them
    bool lossy = false;

    bool isUtf8() const
    {
        return codec.isEmpty();
    }
};

// Size of the blocks read, converted and given to the editor
constexpr qint64 BlockSize = 1024 * 1024;

// The text of the file, in UTF-8 blocks given to the sink. Stops (false) when the sink or
// the cancel flag says so, or on an error. Progress from 0 to 100.
bool readFile( const QString& fileName, QTextCodec* codec,
               const std::function<bool( const char* data, std::size_t length )>& sink,
               FileFormat* format, QString* error, const std::atomic_bool* cancelled = nullptr,
               std::atomic_int* progress = nullptr );

// Writes the UTF-8 text in the format of the file, atomically: a temporary file next to it
// replaces it once complete (its permissions kept). Fails without writing anything when a
// character can not be encoded in the codec of the file.
bool writeFile( const QString& fileName, const char* utf8, std::size_t length,
                const FileFormat& format, QString* error,
                const std::atomic_bool* cancelled = nullptr, std::atomic_int* progress = nullptr );

// Physical memory available, empty when not known
std::optional<quint64> availableMemory();

// A file flogg made for itself (archive extracted, download, clipboard): read only
bool isTemporaryCopy( const QString& fileName );

} // namespace edit

#endif
