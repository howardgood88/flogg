/*
 * Copyright (C) 2021 Anton Filimonov and other contributors
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

#ifndef KLOGG_CLI_H
#define KLOGG_CLI_H

#include <cstdint>
#include <iostream>
#include <vector>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QString>
#include <QStringList>

#include "klogg_version.h"
#include "log.h"

struct CliParameters {
    bool new_session = false;
    bool load_session = false;
    bool multi_instance = false;
    bool log_to_file = false;
    bool follow_file = false;

    bool enable_logging = false;
    int log_level = 3;

    std::vector<QString> filenames;

    int window_width = 0;
    int window_height = 0;

    QString pattern;

    QString filters_file;

    static constexpr auto ConfigOptionName = "config";

    // The settings are read before the application object exists, so the configuration
    // file is looked up in the raw arguments (only as --config file or --config=file)
    static QString configFileFromArguments( const QStringList& arguments )
    {
        const auto option = QString( "--" ) + ConfigOptionName;
        for ( auto i = 1; i < arguments.size(); ++i ) {
            const auto& argument = arguments.at( i );
            if ( argument == "--" ) {
                break;
            }
            if ( argument == option && i + 1 < arguments.size() ) {
                return arguments.at( i + 1 );
            }
            if ( argument.startsWith( option + "=" ) ) {
                return argument.mid( option.size() + 1 );
            }
        }
        return {};
    }

    CliParameters( QCoreApplication& app, bool console = false )
    {
        QCommandLineParser parser;
        parser.setApplicationDescription( "flogg log viewer" );
        const auto helpOption = parser.addHelpOption();
        // Shown without the build number, in a message box on Windows
        QCoreApplication::setApplicationVersion( releaseVersion() );
        const auto versionOption = parser.addVersionOption();

        const QCommandLineOption multiInstanceOption(
            QStringList() << "m"
                          << "multi",
            "allow multiple instance of flogg to run simultaneously (use together with -s, "
            "--config or --filters)" );

        const QCommandLineOption configOption(
            ConfigOptionName,
            "keep the settings in this file instead of flogg.conf (the session goes to "
            "<name>_session.conf next to it)",
            "file" );

        const QCommandLineOption filtersOption(
            "filters", "load the filters from this file, without making it the default one",
            "file" );

        const QCommandLineOption loadSessionOption(
            QStringList() << "s"
                          << "load-session",
            "load the previous session (default when no file is passed)" );

        const QCommandLineOption newSessionOption(
            QStringList() << "n"
                          << "new-session",
            "do not load the previous session (default when a file is passed)" );

        const QCommandLineOption logToFileOption( QStringList() << "l"
                                                                << "log",
                                                  "save the log to a file" );

        const QCommandLineOption followOption( QStringList() << "f"
                                                             << "follow",
                                               "follow initial opened files" );

        const QCommandLineOption patternOption( QStringList() << "e"
                                                              << "pattern",
                                                "pattern to search for", "pattern" );

        const QCommandLineOption debugOption(
            QStringList() << "d"
                          << "debug",
            "output more debug (increase number for more verbosity)", "debug_level", "0" );

        parser.addOption( debugOption );

        if ( !console ) {
            const QCommandLineOption windowWidthOption( "window-width", "new window width",
                                                        "1024" );
            const QCommandLineOption windowHeightOption( "window-height", "new window height",
                                                         "768" );
            parser.addOption( multiInstanceOption );
            parser.addOption( loadSessionOption );
            parser.addOption( newSessionOption );
            parser.addOption( logToFileOption );
            parser.addOption( followOption );
            parser.addOption( windowWidthOption );
            parser.addOption( windowHeightOption );
            parser.addOption( configOption );
            parser.addOption( filtersOption );
        }
        else {
            parser.addOption( patternOption );
        }

        parser.process( app );

        if ( parser.isSet( helpOption ) ) {
            parser.showHelp( EXIT_SUCCESS );
        }

        if ( parser.isSet( versionOption ) ) {
            parser.showVersion();
        }

        if (parser.value( debugOption ).toInt() > 0) {
            enable_logging = true;
        }

        log_level += parser.value( debugOption ).toInt();

        if ( !console ) {
            if ( parser.isSet( multiInstanceOption ) ) {
                multi_instance = true;
            }

            if ( parser.isSet( loadSessionOption ) ) {
                load_session = true;
            }

            if ( parser.isSet( newSessionOption ) ) {
                new_session = true;
            }

            if ( parser.isSet( logToFileOption ) ) {
                log_to_file = true;
            }

            if ( parser.isSet( followOption ) ) {
                follow_file = true;
            }

            if ( parser.isSet( filtersOption ) ) {
                filters_file = QFileInfo( parser.value( filtersOption ) ).absoluteFilePath();
            }
        }
        else {

            if ( parser.isSet( patternOption ) ) {
                pattern = parser.value( patternOption );
            }
        }

        for ( const auto& file : parser.positionalArguments() ) {
            const auto fileInfo = QFileInfo( file );
            filenames.emplace_back( fileInfo.absoluteFilePath() );
        }
    }

    // Release version without the build number: 1.0.0 for 1.0.0.3
    static QString releaseVersion()
    {
        const auto parts = QString( kloggVersion() ).split( '.' );
        return parts.mid( 0, 3 ).join( '.' );
    }
};

#endif