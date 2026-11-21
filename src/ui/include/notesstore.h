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

#ifndef FLOGG_NOTESSTORE_H
#define FLOGG_NOTESSTORE_H

#include <cstdint>
#include <optional>
#include <vector>

#include <QString>

// The notes of the log files, kept in one file of flogg (notes.json next to the default
// filter file), never next to the logs. Only lines with a note are kept, with their
// bookmarks; the entry of a file goes when its last note is removed, when the file no
// longer starts the same way, and the least recently used files go past MaxFiles.
class NotesStore {
  public:
    static constexpr int MaxFiles = 200;
    // Bytes of the start of a file that identify it
    static constexpr qint64 HeadSize = 64 * 1024;

    struct Note {
        uint64_t line = 0;
        uint32_t slots = 0;
        QString text;
        // Of the text of the line, to tell the line is still the same one
        uint64_t lineHash = 0;
    };

    // The store of the application
    static NotesStore& get();
    // Another file for the store of the application (tests)
    static void useFile( const QString& path );
    explicit NotesStore( QString path );

    // Notes of the file when it starts with the same bytes as when they were saved (a file
    // that grew still does). When it does not, the notes are dropped and `changed` set.
    std::vector<Note> notes( const QString& fileName, bool* changed = nullptr );
    // Replaces the notes of the file, none removes its entry
    void setNotes( const QString& fileName, const std::vector<Note>& notes );
    bool hasNotes( const QString& fileName );

    struct Head {
        uint64_t digest = 0;
        qint64 size = 0;
    };
    // Digest of the first bytes of the file, up to `size`
    static std::optional<Head> readHead( const QString& fileName, qint64 size = HeadSize );
    static uint64_t lineHash( const QString& line );

    QString path() const
    {
        return path_;
    }

  private:
    struct FileNotes {
        QString path;
        Head head;
        int64_t used = 0;
        std::vector<Note> notes;
    };

    void load();
    void save() const;
    static QString key( const QString& fileName );

    QString path_;
    bool loaded_ = false;
    std::vector<FileNotes> files_;
};

#endif
