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

#include <catch2/catch.hpp>

#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QTextBrowser>
#include <QTimer>

#include "updatechecker.h"

SCENARIO( "Update checker reads the build version", "[update]" )
{
    // Release builds: <tag>.<build number>
    CHECK( updates::releaseVersion( "1.1.0.57" ) == QVersionNumber( 1, 1, 0 ) );
    CHECK( updates::releaseVersion( "1.2.0" ) == QVersionNumber( 1, 2, 0 ) );
    // Development builds keep the klogg version
    CHECK_FALSE( updates::releaseVersion( "24.11.0.57" ) );
    CHECK_FALSE( updates::releaseVersion( "" ) );
}

SCENARIO( "Update checker marks a newer release until it is installed", "[update]" )
{
    CHECK( updates::pendingVersion( "1.1.9", "1.1.8.60" ) == "1.1.9" );
    CHECK( updates::pendingVersion( "1.2.0", "1.1.8" ) == "1.2.0" );
    // Installed, or older than the build
    CHECK( updates::pendingVersion( "1.1.9", "1.1.9.61" ).isEmpty() );
    CHECK( updates::pendingVersion( "1.1.8", "1.1.9" ).isEmpty() );
    // Nothing found, or a development build
    CHECK( updates::pendingVersion( "", "1.1.8" ).isEmpty() );
    CHECK( updates::pendingVersion( "1.1.9", "24.11.0.57" ).isEmpty() );
}

SCENARIO( "Update checker reads the GitHub release", "[update]" )
{
    // Shape of https://api.github.com/repos/howardgood88/flogg/releases/latest
    const auto json = QJsonDocument::fromJson( R"({
        "tag_name": "v1.2.0",
        "html_url": "https://github.com/howardgood88/flogg/releases/tag/v1.2.0",
        "body": "## Changes\n- Faster",
        "assets": [
            { "name": "flogg-1.2.0-macos-arm64.dmg",
              "browser_download_url": "https://example.com/flogg-1.2.0-macos-arm64.dmg" },
            { "name": "flogg-1.2.0-windows-x64.zip",
              "browser_download_url": "https://example.com/flogg-1.2.0-windows-x64.zip" }
        ]
    })" ).object();

    const auto release = updates::parseRelease( json, "-windows-x64.zip" );
    REQUIRE( release );
    CHECK( release->version == QVersionNumber( 1, 2, 0 ) );
    CHECK( release->tag == "v1.2.0" );
    CHECK( release->notes == "## Changes\n- Faster" );
    CHECK( release->assetName == "flogg-1.2.0-windows-x64.zip" );
    CHECK( release->assetUrl == QUrl( "https://example.com/flogg-1.2.0-windows-x64.zip" ) );
    CHECK( release->version > *updates::releaseVersion( "1.1.0.57" ) );

    // No package for this platform: only the page
    const auto other = updates::parseRelease( json, "-linux-x64.tar.gz" );
    REQUIRE( other );
    CHECK( other->assetUrl.isEmpty() );

    // A release without notes (body is null)
    auto noNotes = json;
    noNotes.remove( "body" );
    REQUIRE( updates::parseRelease( noNotes, {} ) );
    CHECK( updates::parseRelease( noNotes, {} )->notes.isEmpty() );

    CHECK_FALSE( updates::parseRelease( QJsonObject{ { "tag_name", "latest" } }, {} ) );
}

SCENARIO( "Update checker reads the release pages", "[update]" )
{
    CHECK( updates::tagOfReleasePage(
               QUrl( "https://github.com/howardgood88/flogg/releases/tag/v1.2.0" ) )
           == "v1.2.0" );
    CHECK( updates::tagOfReleasePage( QUrl( "https://github.com/howardgood88/flogg/releases" ) )
               .isEmpty() );
    CHECK( updates::tagOfReleasePage( QUrl( "https://github.com/howardgood88/flogg/tag" ) )
               .isEmpty() );

    const auto release = updates::releaseOfTag( "v1.2.0" );
    REQUIRE( release );
    CHECK( release->version == QVersionNumber( 1, 2, 0 ) );
    CHECK( release->page
           == QUrl( "https://github.com/howardgood88/flogg/releases/tag/v1.2.0" ) );
    CHECK_FALSE( updates::releaseOfTag( "" ) );

    const QString changelog = "# flogg changes\r\n\r\n"
                              "## Unreleased\r\n\r\n- Next\r\n\r\n"
                              "## 1.2.0 (2026-11-03)\r\n\r\n- Faster\r\n- Smaller\r\n\r\n"
                              "## 1.1.0 (2026-11-02)\r\n\r\n- Older\r\n";
    CHECK( updates::changelogSection( changelog, QVersionNumber( 1, 2, 0 ) )
           == "- Faster\n- Smaller" );
    CHECK( updates::changelogSection( changelog, QVersionNumber( 1, 1, 0 ) ) == "- Older" );
    CHECK( updates::changelogSection( changelog, QVersionNumber( 1, 1 ) ).isEmpty() );
    CHECK( updates::changelogSection( changelog, QVersionNumber( 1, 3, 0 ) ).isEmpty() );

    // The versions after the installed one, up to the latest: not "Unreleased"
    CHECK( updates::changelogSince( changelog, QVersionNumber( 1, 1, 0 ), QVersionNumber( 1, 2, 0 ) )
           == "- Faster\n- Smaller" );
    CHECK( updates::changelogSince( changelog, QVersionNumber( 1, 0, 0 ), QVersionNumber( 1, 2, 0 ) )
           == "### 1.2.0 (2026-11-03)\n\n- Faster\n- Smaller\n\n### 1.1.0 (2026-11-02)\n\n- Older" );
    CHECK( updates::changelogSince( changelog, QVersionNumber( 1, 0, 0 ), QVersionNumber( 1, 1, 0 ) )
           == "- Older" );
    CHECK( updates::changelogSince( changelog, QVersionNumber( 1, 2, 0 ), QVersionNumber( 1, 2, 0 ) )
               .isEmpty() );
}

// Hidden, asks GitHub for the latest release without the API:
// klogg_itests "[.network]"
SCENARIO( "Update checker without the GitHub API", "[.network]" )
{
    QNetworkAccessManager network;
    network.setRedirectPolicy( QNetworkRequest::NoLessSafeRedirectPolicy );

    std::optional<updates::Release> release;
    QString error;
    QEventLoop loop;
    updates::fetchLatestWithoutApi( network, "-windows-x64.zip",
                                    [ & ]( auto latest, const QString& failure ) {
                                        release = latest;
                                        error = failure;
                                        loop.quit();
                                    } );
    QTimer::singleShot( 30000, &loop, &QEventLoop::quit );
    loop.exec();

    INFO( error.toStdString() );
    REQUIRE( release );
    WARN( release->tag.toStdString() + " " + release->assetUrl.toString().toStdString() + "\n"
          + release->notes.toStdString() );
    CHECK( release->version >= QVersionNumber( 1, 1, 1 ) );
    CHECK_FALSE( release->notes.isEmpty() );
    CHECK( release->assetName.endsWith( "-windows-x64.zip" ) );
}

// Hidden, saves how the changelog of Help > What's new looks:
// FLOGG_RENDER_DIR=<dir> FLOGG_CHANGELOG=<CHANGELOG.md> klogg_itests "[.render]"
SCENARIO( "Changelog rendering", "[.render]" )
{
    const auto renderDir = qEnvironmentVariable( "FLOGG_RENDER_DIR" );
    QFile changes( qEnvironmentVariable( "FLOGG_CHANGELOG" ) );
    if ( renderDir.isEmpty() || !changes.open( QIODevice::ReadOnly | QIODevice::Text ) ) {
        WARN( "FLOGG_RENDER_DIR or FLOGG_CHANGELOG is not set" );
        return;
    }

    QTextBrowser text;
    text.resize( 640, 560 );
    text.setMarkdown( QString::fromUtf8( changes.readAll() ) );
    REQUIRE( text.grab().save( renderDir + "/changelog.png" ) );
}
