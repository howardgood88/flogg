/*
 * Copyright (C) 2011 Nicolas Bonnefon and other contributors
 *
 * This file is part of glogg.
 *
 * glogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * glogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with glogg.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Copyright (C) 2019 Anton Filimonov and other contributors
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

#ifndef KLOGG_PERSISTENTINFO_H
#define KLOGG_PERSISTENTINFO_H

#include <memory>

#include <QSettings>

// Singleton class managing the saving of persistent data to permanent storage
// Clients must implement Persistable

struct app_settings {
};
struct session_settings {
};

class PersistentInfo {
  public:
    static QSettings& getSettings( app_settings );
    static QSettings& getSettings( session_settings );

    // Keeps the settings in the given file instead of the default one,
    // has to be called before the settings are first used
    static void setConfigFile( const QString& path );

    // Folder of the configuration file when the settings are kept in a file of their own
    // (next to the executable or passed on the command line), empty when they are kept
    // in the operating system location
    static QString portableDirectory();

  private:
    static const bool ForcePortable;

    explicit PersistentInfo();
    static PersistentInfo& getInstance();

    void PreparePortableSettings( const QString& portableConfigPath );
    void PrepareOsSettings();

    void UpdateSettings();

    std::unique_ptr<QSettings> appSettings_;
    std::unique_ptr<QSettings> sessionSettings_;
    QString portableDirectory_;
};
#endif
