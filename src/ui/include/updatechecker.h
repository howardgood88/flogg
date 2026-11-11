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

#ifndef KLOGG_UPDATECHECKER_H
#define KLOGG_UPDATECHECKER_H

#include <functional>
#include <optional>

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QVersionNumber>

class QWidget;

namespace updates {

// The latest release published on GitHub
struct Release {
    QVersionNumber version;
    QString tag;
    QString notes;
    // CHANGELOG.md of the tag when it was already fetched, empty otherwise
    QString changelog;
    QUrl page;
    // The package of this platform, empty when there is none
    QString assetName;
    QUrl assetUrl;
};

// Version of a release build (1.2.0 from 1.2.0.<build>), nothing for a development build
// (numbered after klogg, 24.11.0.<build>)
std::optional<QVersionNumber> releaseVersion( const QString& buildVersion );

// The release in the reply of the GitHub releases API, the package matching the suffix
std::optional<Release> parseRelease( const QJsonObject& json, const QString& assetSuffix );

// The release of a tag (v1.2.0), without notes nor package
std::optional<Release> releaseOfTag( const QString& tag );

// The tag of a release page (https://github.com/<repo>/releases/tag/v1.2.0)
QString tagOfReleasePage( const QUrl& page );

// The changes of a version in CHANGELOG.md: the lines below "## 1.2.0 (date)"
QString changelogSection( const QString& changelog, const QVersionNumber& version );

// The changes of the versions after `installed` up to `latest` in CHANGELOG.md, newest first.
// A single version gives its lines only, several keep their "## 1.2.0 (date)" titles (as
// "###"). Empty when CHANGELOG.md has none of them.
QString changelogSince( const QString& changelog, const QVersionNumber& installed,
                        const QVersionNumber& latest );

// The latest release without the GitHub API, which answers only 60 requests an hour to an
// address (often shared by a whole office): the page releases/latest redirects to, the notes
// from CHANGELOG.md of its tag and the package of this platform when it exists
using FetchCallback = std::function<void( std::optional<Release> release, QString error )>;
void fetchLatestWithoutApi( QNetworkAccessManager& network, const QString& assetSuffix,
                            FetchCallback done );

// The newer release remembered by the last check, empty when the build (its version
// as kloggVersion) has it already or is a development build
QString pendingVersion( const QString& remembered, const QString& buildVersion );

// Package suffix of this platform, empty when releases have none
QString platformAssetSuffix();

} // namespace updates

// Looks for a newer release on GitHub, once a day at start or from the Help menu, and lets the
// user install it: the package is downloaded and replaces flogg once it is closed
class UpdateChecker : public QObject {
    Q_OBJECT
  public:
    static UpdateChecker& instance();

    // A few seconds after start, when enabled and not done in the last day
    void scheduleStartupCheck();

    // From the Help menu: also tells when there is nothing new
    void checkNow( QWidget* parent );

    // Newer release found by the last check and not installed yet, empty when there is none.
    // Kept in the settings, the check is done at most once a day.
    QString availableVersion() const;

  Q_SIGNALS:
    void availableVersionChanged();

  private:
    void setAvailableVersion( const QString& version );

    UpdateChecker();

    void check( bool manual );
    void onReply( bool manual );
    void onRelease( const updates::Release& release, bool manual );
    void fail( const QString& error, bool manual );
    void offer( const updates::Release& release, bool manual );
    // The notes of every version since the installed one, then the dialog
    void fetchNotesAndShow( updates::Release release, QVersionNumber installed );
    void showOffer( const updates::Release& release, const QVersionNumber& installed );
    void download( const updates::Release& release );
    // Prepared to run when flogg quits, then asks every window to close
    bool prepareInstall( const QString& package );
    QWidget* dialogParent() const;

  private:
    QNetworkAccessManager network_;
    QPointer<QWidget> parent_;
    bool checking_ = false;
    bool installPending_ = false;
};

#endif // KLOGG_UPDATECHECKER_H
