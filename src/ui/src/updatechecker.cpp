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

#include "updatechecker.h"

#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkReply>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QSslConfiguration>
#include <QTemporaryFile>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>

#include "configuration.h"
#include "downloader.h"
#include "klogg_version.h"
#include "log.h"
#include "mainwindow.h"
#include "persistentinfo.h"

namespace {

const QUrl LatestReleaseUrl{ "https://api.github.com/repos/howardgood88/flogg/releases/latest" };
const QUrl ReleasesPage{ "https://github.com/howardgood88/flogg/releases" };
const QUrl LatestReleasePage{ "https://github.com/howardgood88/flogg/releases/latest" };
const QString ChangelogUrl{
    "https://raw.githubusercontent.com/howardgood88/flogg/%1/CHANGELOG.md" };

constexpr QLatin1String LastCheckKey = QLatin1String( "update/lastCheck" );
constexpr QLatin1String SkippedVersionKey = QLatin1String( "update/skippedVersion" );
constexpr QLatin1String AvailableVersionKey = QLatin1String( "update/availableVersion" );

// The start check waits for the windows to settle, and is done at most once a day
constexpr int StartupDelayMs = 5000;
constexpr qint64 CheckIntervalSecs = 20 * 60 * 60;

QSettings& settings()
{
    return PersistentInfo::getSettings( app_settings{} );
}

QNetworkRequest githubRequest( const QUrl& url )
{
    QNetworkRequest request( url );
    request.setRawHeader( "User-Agent", QByteArray( "flogg/" ) + kloggVersion().data() );
    if ( !Configuration::get().verifySslPeers() ) {
        auto sslConfiguration = request.sslConfiguration();
        sslConfiguration.setPeerVerifyMode( QSslSocket::VerifyNone );
        request.setSslConfiguration( sslConfiguration );
    }
    return request;
}

QNetworkRequest apiRequest( const QUrl& url )
{
    auto request = githubRequest( url );
    request.setRawHeader( "Accept", "application/vnd.github+json" );
    return request;
}

#if defined( Q_OS_WIN )
// A literal for the PowerShell scripts
QString psQuote( const QString& text )
{
    return "'" + QString( text ).replace( "'", "''" ) + "'";
}
#else
// A literal for the shell scripts
QString shQuote( const QString& text )
{
    return "'" + QString( text ).replace( "'", "'\\''" ) + "'";
}
#endif

} // namespace

namespace updates {

std::optional<QVersionNumber> releaseVersion( const QString& buildVersion )
{
    const auto version = QVersionNumber::fromString( buildVersion );
    // Development builds keep the version of klogg they are forked from
    if ( version.isNull() || version.majorVersion() >= 20 ) {
        return {};
    }
    return QVersionNumber( version.majorVersion(), version.minorVersion(), version.microVersion() );
}

QString pendingVersion( const QString& remembered, const QString& buildVersion )
{
    const auto available = QVersionNumber::fromString( remembered );
    const auto current = releaseVersion( buildVersion );
    // Installed since, or a development build
    if ( available.isNull() || !current || available <= *current ) {
        return {};
    }
    return available.toString();
}

std::optional<Release> releaseOfTag( const QString& tag )
{
    auto versionText = tag;
    if ( versionText.startsWith( 'v' ) ) {
        versionText.remove( 0, 1 );
    }
    Release release;
    release.version = QVersionNumber::fromString( versionText );
    if ( release.version.isNull() ) {
        return {};
    }
    release.tag = tag;
    release.page = QUrl( ReleasesPage.toString() + "/tag/" + tag );
    return release;
}

QString tagOfReleasePage( const QUrl& page )
{
    const auto parts = page.path().split( '/', Qt::SkipEmptyParts );
    const auto index = parts.indexOf( "tag" );
    if ( index < 1 || index + 1 >= parts.size() || parts[ index - 1 ] != "releases" ) {
        return {};
    }
    return parts[ index + 1 ];
}

QString changelogSection( const QString& changelog, const QVersionNumber& version )
{
    const auto versionText = version.toString();
    QStringList section;
    bool found = false;
    for ( auto line : changelog.split( '\n' ) ) {
        if ( line.endsWith( '\r' ) ) {
            line.chop( 1 );
        }
        if ( line.startsWith( "## " ) ) {
            if ( found ) {
                break;
            }
            found = line.mid( 3 ).section( ' ', 0, 0, QString::SectionSkipEmpty ) == versionText;
        }
        else if ( found ) {
            section.append( line );
        }
    }
    return section.join( '\n' ).trimmed();
}

std::optional<Release> parseRelease( const QJsonObject& json, const QString& assetSuffix )
{
    auto parsed = releaseOfTag( json[ "tag_name" ].toString() );
    if ( !parsed ) {
        return {};
    }
    auto& release = *parsed;
    release.notes = json[ "body" ].toString().trimmed();
    const QUrl page( json[ "html_url" ].toString() );
    if ( page.isValid() && !page.isEmpty() ) {
        release.page = page;
    }

    if ( !assetSuffix.isEmpty() ) {
        for ( const auto& assetValue : json[ "assets" ].toArray() ) {
            const auto asset = assetValue.toObject();
            const auto name = asset[ "name" ].toString();
            if ( name.endsWith( assetSuffix ) ) {
                release.assetName = name;
                release.assetUrl = QUrl( asset[ "browser_download_url" ].toString() );
                break;
            }
        }
    }
    return parsed;
}

void fetchLatestWithoutApi( QNetworkAccessManager& network, const QString& assetSuffix,
                            FetchCallback done )
{
    auto* latest = network.head( githubRequest( LatestReleasePage ) );
    QObject::connect( latest, &QNetworkReply::finished, latest, [ &network, latest, assetSuffix,
                                                                  done ] {
        latest->deleteLater();
        if ( latest->error() != QNetworkReply::NoError ) {
            done( {}, latest->errorString() );
            return;
        }
        // The url after the redirects
        const auto release = releaseOfTag( tagOfReleasePage( latest->url() ) );
        if ( !release ) {
            done( {}, QCoreApplication::translate( "UpdateChecker",
                                                   "the latest release page has no version" ) );
            return;
        }

        auto* notes = network.get( githubRequest( QUrl( ChangelogUrl.arg( release->tag ) ) ) );
        QObject::connect( notes, &QNetworkReply::finished, notes, [ &network, notes,
                                                                    release = *release,
                                                                    assetSuffix, done ]() mutable {
            notes->deleteLater();
            // Without notes the dialog links the release page
            if ( notes->error() == QNetworkReply::NoError ) {
                release.notes
                    = changelogSection( QString::fromUtf8( notes->readAll() ), release.version );
            }
            if ( assetSuffix.isEmpty() ) {
                done( release, {} );
                return;
            }

            // Named by the release job
            const auto name = "flogg-" + release.version.toString() + assetSuffix;
            const QUrl url( ReleasesPage.toString() + "/download/" + release.tag + "/" + name );
            auto* asset = network.head( githubRequest( url ) );
            QObject::connect( asset, &QNetworkReply::finished, asset,
                              [ asset, release, name, url, done ]() mutable {
                                  asset->deleteLater();
                                  if ( asset->error() == QNetworkReply::NoError ) {
                                      release.assetName = name;
                                      release.assetUrl = url;
                                  }
                                  done( release, {} );
                              } );
        } );
    } );
}

QString platformAssetSuffix()
{
#if defined( Q_OS_WIN ) && defined( Q_PROCESSOR_X86_64 )
    return "-windows-x64.zip";
#elif defined( Q_OS_MACOS ) && defined( Q_PROCESSOR_ARM_64 )
    return "-macos-arm64.dmg";
#elif defined( Q_OS_LINUX ) && defined( Q_PROCESSOR_X86_64 )
    return "-linux-x64.AppImage";
#else
    return {};
#endif
}

} // namespace updates

UpdateChecker& UpdateChecker::instance()
{
    static auto* checker = new UpdateChecker;
    return *checker;
}

UpdateChecker::UpdateChecker()
{
    network_.setRedirectPolicy( QNetworkRequest::NoLessSafeRedirectPolicy );
}

void UpdateChecker::scheduleStartupCheck()
{
    if ( !Configuration::get().checkForUpdates() ) {
        return;
    }

    const auto lastCheck = settings().value( LastCheckKey ).toDateTime();
    if ( lastCheck.isValid()
         && lastCheck.secsTo( QDateTime::currentDateTimeUtc() ) < CheckIntervalSecs ) {
        return;
    }

    QTimer::singleShot( StartupDelayMs, this, [ this ] { check( false ); } );
}

QString UpdateChecker::availableVersion() const
{
    return updates::pendingVersion( settings().value( AvailableVersionKey ).toString(),
                                    kloggVersion() );
}

void UpdateChecker::setAvailableVersion( const QString& version )
{
    if ( settings().value( AvailableVersionKey ).toString() == version ) {
        return;
    }
    if ( version.isEmpty() ) {
        settings().remove( AvailableVersionKey );
    }
    else {
        settings().setValue( AvailableVersionKey, version );
    }
    Q_EMIT availableVersionChanged();
}

void UpdateChecker::checkNow( QWidget* parent )
{
    parent_ = parent;
    check( true );
}

void UpdateChecker::check( bool manual )
{
    if ( checking_ ) {
        return;
    }
    checking_ = true;

    LOG_INFO << "Checking for a new flogg release";
    auto* reply = network_.get( apiRequest( LatestReleaseUrl ) );
    connect( reply, &QNetworkReply::finished, this, [ this, manual ] { onReply( manual ); } );
}

void UpdateChecker::onReply( bool manual )
{
    checking_ = false;
    auto* reply = qobject_cast<QNetworkReply*>( sender() );
    reply->deleteLater();

    if ( reply->error() != QNetworkReply::NoError ) {
        // 403 or 429 when this address asked the API too often, the release pages still answer
        const auto status = reply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt();
        const auto apiError
            = status == 403 || status == 429
                  ? tr( "GitHub limits how often it is asked from this network, "
                        "try again in an hour." )
                  : reply->errorString();
        LOG_INFO << "GitHub API failed: " << apiError << ", using the release pages";

        checking_ = true;
        updates::fetchLatestWithoutApi(
            network_, updates::platformAssetSuffix(),
            [ this, manual, apiError ]( std::optional<updates::Release> release,
                                        const QString& error ) {
                checking_ = false;
                if ( release ) {
                    onRelease( *release, manual );
                }
                else {
                    LOG_WARNING << "Release pages failed: " << error;
                    fail( apiError, manual );
                }
            } );
        return;
    }

    const auto json = QJsonDocument::fromJson( reply->readAll() ).object();
    const auto release = updates::parseRelease( json, updates::platformAssetSuffix() );
    if ( !release ) {
        fail( tr( "the reply of GitHub has no release version" ), manual );
        return;
    }
    onRelease( *release, manual );
}

void UpdateChecker::onRelease( const updates::Release& release, bool manual )
{
    settings().setValue( LastCheckKey, QDateTime::currentDateTimeUtc() );
    offer( release, manual );
}

void UpdateChecker::fail( const QString& error, bool manual )
{
    LOG_WARNING << "Update check failed: " << error;
    if ( manual ) {
        QMessageBox::warning( dialogParent(), tr( "flogg update" ),
                              tr( "Could not check for a new version:\n%1" ).arg( error ) );
    }
}

void UpdateChecker::offer( const updates::Release& release, bool manual )
{
    const QString currentText = kloggReleaseVersion();
    const auto current = updates::releaseVersion( kloggVersion() );
    LOG_INFO << "Latest flogg release " << release.version.toString() << ", running "
             << currentText;

    if ( !current ) {
        if ( manual ) {
            QMessageBox::information(
                dialogParent(), tr( "flogg update" ),
                tr( "This is a development build (%1), it is not updated.\n"
                    "The latest release is %2." )
                    .arg( currentText, release.version.toString() ) );
        }
        return;
    }

    // Marked in the menus until it is installed, also when skipped or postponed
    setAvailableVersion( release.version > *current ? release.version.toString() : QString{} );

    if ( release.version <= *current ) {
        if ( manual ) {
            QMessageBox::information( dialogParent(), tr( "flogg update" ),
                                      tr( "flogg %1 is the latest version." )
                                          .arg( current->toString() ) );
        }
        return;
    }

    if ( !manual && settings().value( SkippedVersionKey ).toString() == release.tag ) {
        return;
    }

    QDialog dialog( dialogParent() );
    dialog.setWindowTitle( tr( "flogg update" ) );
    dialog.resize( 560, 420 );

    auto* layout = new QVBoxLayout( &dialog );
    auto* title = new QLabel(
        tr( "<b>flogg %1 is available</b>, this is version %2." )
            .arg( release.version.toString(), current->toString() ) );
    layout->addWidget( title );

    auto* notes = new QTextBrowser;
    notes->setOpenExternalLinks( true );
    if ( release.notes.isEmpty() ) {
        notes->setMarkdown( tr( "No notes for this release, see [the release page](%1)." )
                                .arg( release.page.toString() ) );
    }
    else {
        notes->setMarkdown( release.notes );
    }
    layout->addWidget( notes );

    auto* buttons = new QDialogButtonBox;
    const bool canInstall = !release.assetUrl.isEmpty();
    auto* updateButton = buttons->addButton(
        canInstall ? tr( "Update now" ) : tr( "Open download page" ), QDialogButtonBox::AcceptRole );
    auto* skipButton = buttons->addButton( tr( "Skip this version" ), QDialogButtonBox::RejectRole );
    buttons->addButton( tr( "Later" ), QDialogButtonBox::RejectRole );
    updateButton->setDefault( true );
    layout->addWidget( buttons );

    QAbstractButton* clicked = nullptr;
    connect( buttons, &QDialogButtonBox::clicked, &dialog, [ &dialog, &clicked ]( auto* button ) {
        clicked = button;
        dialog.done( 0 );
    } );
    dialog.exec();

    if ( clicked == skipButton ) {
        settings().setValue( SkippedVersionKey, release.tag );
    }
    else if ( clicked == updateButton ) {
        if ( canInstall ) {
            download( release );
        }
        else {
            QDesktopServices::openUrl( release.page );
        }
    }
}

void UpdateChecker::download( const updates::Release& release )
{
    const auto directory = QDir::temp().filePath( "flogg-update-" + release.tag );
    QDir( directory ).removeRecursively();
    if ( !QDir().mkpath( directory ) ) {
        QMessageBox::warning( dialogParent(), tr( "flogg update" ),
                              tr( "Could not create %1." ).arg( directory ) );
        return;
    }

    const auto package = QDir( directory ).filePath( release.assetName );
    auto* file = new QFile( package, this );
    if ( !file->open( QIODevice::WriteOnly ) ) {
        QMessageBox::warning( dialogParent(), tr( "flogg update" ),
                              tr( "Could not write %1." ).arg( package ) );
        file->deleteLater();
        return;
    }

    auto* progress = new QProgressDialog( tr( "Downloading flogg %1..." )
                                              .arg( release.version.toString() ),
                                          tr( "Cancel" ), 0, 0, dialogParent() );
    progress->setWindowTitle( tr( "flogg update" ) );
    progress->setMinimumDuration( 0 );
    progress->setAutoClose( false );
    progress->setAutoReset( false );

    auto* downloader = new Downloader( this );
    connect( downloader, &Downloader::downloadProgress, progress,
             [ progress ]( qint64 received, qint64 total ) {
                 if ( total > 0 ) {
                     progress->setMaximum( static_cast<int>( total / 1024 ) );
                     progress->setValue( static_cast<int>( received / 1024 ) );
                 }
                 else if ( received == 0 ) {
                     // Size unknown (the curl download): busy
                     progress->setRange( 0, 0 );
                 }
             } );
    connect( progress, &QProgressDialog::canceled, downloader, &Downloader::abort );
    connect( downloader, &Downloader::finished, this,
             [ this, downloader, progress, file, package, release ]( bool success ) {
                 const auto error = downloader->lastError();
                 const auto canceled = progress->wasCanceled();
                 progress->deleteLater();
                 downloader->deleteLater();
                 file->deleteLater();

                 if ( !success ) {
                     if ( !canceled ) {
                         const auto answer = QMessageBox::warning(
                             dialogParent(), tr( "flogg update" ),
                             tr( "The download failed:\n%1\n\n"
                                 "Open the download page to get it with the web browser?" )
                                 .arg( error ),
                             QMessageBox::Open | QMessageBox::Close, QMessageBox::Open );
                         if ( answer == QMessageBox::Open ) {
                             QDesktopServices::openUrl( release.page );
                         }
                     }
                     return;
                 }

                 if ( !prepareInstall( package ) ) {
                     // Installed by the user
                     QMessageBox::information(
                         dialogParent(), tr( "flogg update" ),
                         tr( "flogg can't replace itself here, the new version is in\n%1" )
                             .arg( QDir::toNativeSeparators( package ) ) );
                     QDesktopServices::openUrl(
                         QUrl::fromLocalFile( QFileInfo( package ).absolutePath() ) );
                     return;
                 }

                 const auto answer = QMessageBox::question(
                     dialogParent(), tr( "flogg update" ),
                     tr( "flogg %1 is installed when flogg closes, then it starts again.\n"
                         "Close flogg now?" )
                         .arg( release.version.toString() ),
                     QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes );
                 if ( answer == QMessageBox::Yes ) {
                     // The windows ask for the unsaved filters, the update waits for the next
                     // close if one of them is kept open
                     for ( auto* widget : QApplication::topLevelWidgets() ) {
                         if ( auto* window = qobject_cast<MainWindow*>( widget ) ) {
                             Q_EMIT window->exitRequested();
                             break;
                         }
                     }
                 }
             } );

    downloader->download( release.assetUrl, file );
}

bool UpdateChecker::prepareInstall( const QString& package )
{
    if ( installPending_ ) {
        return true;
    }

    const auto directory = QFileInfo( package ).absolutePath();
    const auto pid = QString::number( QCoreApplication::applicationPid() );
    QString program;
    QStringList arguments;
    QString script;
    QString scriptPath;

#if defined( Q_OS_WIN )
    const auto appDirectory = QDir::toNativeSeparators( QCoreApplication::applicationDirPath() );
    const auto executable = QDir::toNativeSeparators( QCoreApplication::applicationFilePath() );
    {
        // The files of flogg are replaced in place
        QTemporaryFile probe( QDir( appDirectory ).filePath( "flogg-update-XXXXXX" ) );
        if ( !probe.open() ) {
            return false;
        }
    }

    const auto nativeDirectory = QDir::toNativeSeparators( directory );
    const auto extracted = nativeDirectory + "\\extracted";
    script = QString( "$ErrorActionPreference = 'Stop'\r\n"
                      "try {\r\n"
                      "  Wait-Process -Id %1 -ErrorAction SilentlyContinue\r\n"
                      "  Expand-Archive -LiteralPath %2 -DestinationPath %3 -Force\r\n"
                      "  Copy-Item -Path (Join-Path %3 '*') -Destination %4 -Recurse -Force\r\n"
                      "  Start-Process -FilePath %5\r\n"
                      "  Set-Location $env:TEMP\r\n"
                      "  Remove-Item -LiteralPath %6 -Recurse -Force -ErrorAction "
                      "SilentlyContinue\r\n"
                      "}\r\n"
                      "catch {\r\n"
                      "  $_ | Out-File -FilePath (Join-Path %6 'install.log')\r\n"
                      "  Start-Process -FilePath %5\r\n"
                      "}\r\n" )
                 .arg( pid, psQuote( QDir::toNativeSeparators( package ) ), psQuote( extracted ),
                       psQuote( appDirectory ), psQuote( executable ),
                       psQuote( nativeDirectory ) );
    scriptPath = QDir( directory ).filePath( "install.ps1" );
    program = "powershell.exe";
    arguments = { "-NoProfile",  "-NonInteractive", "-ExecutionPolicy", "Bypass",
                  "-WindowStyle", "Hidden",         "-File",            scriptPath };
#elif defined( Q_OS_MACOS )
    // flogg.app/Contents/MacOS
    QDir bundleDirectory( QCoreApplication::applicationDirPath() );
    if ( !bundleDirectory.cd( "../.." ) || !bundleDirectory.dirName().endsWith( ".app" ) ) {
        return false;
    }
    const auto bundle = bundleDirectory.absolutePath();
    if ( !QFileInfo( QFileInfo( bundle ).absolutePath() ).isWritable()
         || !QFileInfo( bundle ).isWritable() ) {
        return false;
    }

    // The old bundle is put back when the new one can't be copied
    script = QString( "#!/bin/sh\n"
                      "while kill -0 %1 2>/dev/null; do sleep 0.2; done\n"
                      "mnt=$(mktemp -d /tmp/flogg-update.XXXXXX)\n"
                      "if hdiutil attach -nobrowse -readonly -mountpoint \"$mnt\" %2 >/dev/null; "
                      "then\n"
                      "  rm -rf %4\n"
                      "  if mv %3 %4 && ditto \"$mnt/flogg.app\" %3; then\n"
                      "    rm -rf %4\n"
                      "  else\n"
                      "    rm -rf %3\n"
                      "    mv %4 %3\n"
                      "  fi\n"
                      "  hdiutil detach \"$mnt\" >/dev/null\n"
                      "fi\n"
                      "rmdir \"$mnt\"\n"
                      "open %3\n"
                      "rm -rf %5\n" )
                 .arg( pid, shQuote( package ), shQuote( bundle ), shQuote( bundle + ".old" ),
                       shQuote( directory ) );
    scriptPath = QDir( directory ).filePath( "install.sh" );
    program = "/bin/sh";
    arguments = { scriptPath };
#elif defined( Q_OS_LINUX )
    // The AppImage flogg runs from (set by its runtime), replaced by the new one
    const auto appImage = qEnvironmentVariable( "APPIMAGE" );
    if ( appImage.isEmpty() || !QFileInfo( QFileInfo( appImage ).absolutePath() ).isWritable() ) {
        return false;
    }

    script = QString( "#!/bin/sh\n"
                      "while kill -0 %1 2>/dev/null; do sleep 0.2; done\n"
                      "if cp %2 %4 && chmod +x %4 && mv -f %4 %3; then\n"
                      "  rm -rf %5\n"
                      "else\n"
                      "  rm -f %4\n"
                      "fi\n"
                      "%3 >/dev/null 2>&1 &\n" )
                 .arg( pid, shQuote( package ), shQuote( appImage ), shQuote( appImage + ".new" ),
                       shQuote( directory ) );
    scriptPath = QDir( directory ).filePath( "install.sh" );
    program = "/bin/sh";
    arguments = { scriptPath };
#else
    Q_UNUSED( pid );
    Q_UNUSED( directory );
    return false;
#endif

    QFile scriptFile( scriptPath );
    if ( !scriptFile.open( QIODevice::WriteOnly | QIODevice::Truncate )
         || scriptFile.write( script.toUtf8() ) < 0 ) {
        return false;
    }
    scriptFile.close();

    LOG_INFO << "Update prepared in " << directory;
    installPending_ = true;
    connect( qApp, &QCoreApplication::aboutToQuit, this, [ program, arguments ] {
        QProcess process;
        process.setProgram( program );
        process.setArguments( arguments );
#ifdef Q_OS_WIN
        process.setCreateProcessArgumentsModifier( []( QProcess::CreateProcessArguments* args ) {
            args->flags |= 0x08000000; // CREATE_NO_WINDOW
        } );
#endif
        LOG_INFO << "Starting the update " << program << " " << arguments.join( ' ' );
        process.startDetached();
    } );
    return true;
}

QWidget* UpdateChecker::dialogParent() const
{
    if ( parent_ ) {
        return parent_;
    }
    if ( auto* active = QApplication::activeWindow() ) {
        return active;
    }
    for ( auto* widget : QApplication::topLevelWidgets() ) {
        if ( qobject_cast<MainWindow*>( widget ) && widget->isVisible() ) {
            return widget;
        }
    }
    return nullptr;
}
