/*
 * Copyright (C) 2016 -- 2019 Anton Filimonov and other contributors
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

#include <QFileInfo>
#include <QNetworkReply>
#include <QProcess>
#include <QSslConfiguration>
#include <QStandardPaths>

#include "configuration.h"
#include "klogg_version.h"
#include "log.h"

#include "downloader.h"

namespace {
// Attempts in total, and in a row without any new byte
constexpr int MaxAttempts = 100;
constexpr int MaxAttemptsWithoutProgress = 4;
// The attempts without progress after which Qt uses HTTP/1.1 and TLS 1.2
constexpr int SimpleProtocolAfter = 2;
constexpr int MaxCurlRuns = 20;

QByteArray userAgent()
{
    return QByteArray( "flogg/" ) + kloggVersion().data();
}
} // namespace

Downloader::Downloader( QObject* parent )
    : QObject( parent )
{
    manager_.setRedirectPolicy( QNetworkRequest::NoLessSafeRedirectPolicy );
}

QString Downloader::lastError() const
{
    return errors_.join( '\n' );
}

void Downloader::addError( const QString& error )
{
    if ( !error.isEmpty() && !errors_.contains( error ) ) {
        errors_.append( error );
    }
}

void Downloader::abort()
{
    aborted_ = true;
    if ( currentDownload_ ) {
        currentDownload_->abort();
    }
    if ( curl_ ) {
        curl_->kill();
    }
}

void Downloader::download( const QUrl& url, QFile* outputFile )
{
    url_ = url;
    output_ = outputFile;
    received_ = 0;
    total_ = -1;
    attempts_ = 0;
    attemptsWithoutProgress_ = 0;
    aborted_ = false;
    errors_.clear();
    startQt();
}

void Downloader::startQt()
{
    ++attempts_;
    receivedAtStart_ = received_;
    responseChecked_ = false;

    // Every attempt starts at github.com: the redirect gives a fresh signed address
    QNetworkRequest request( url_ );
    request.setRawHeader( "User-Agent", userAgent() );
    if ( received_ > 0 ) {
        // The redirected requests keep the headers and the attributes
        request.setRawHeader( "Range", "bytes=" + QByteArray::number( received_ ) + "-" );
    }

    auto sslConfiguration = QSslConfiguration::defaultConfiguration();
    if ( !Configuration::get().verifySslPeers() ) {
        sslConfiguration.setPeerVerifyMode( QSslSocket::VerifyNone );
    }
    if ( attemptsWithoutProgress_ >= SimpleProtocolAfter ) {
        request.setAttribute( QNetworkRequest::Http2AllowedAttribute, false );
        sslConfiguration.setProtocol( QSsl::TlsV1_2 );
    }
    request.setSslConfiguration( sslConfiguration );

    currentDownload_ = manager_.get( request );

    connect( currentDownload_, &QNetworkReply::finished, this, &Downloader::downloadFinished );
    connect( currentDownload_, &QNetworkReply::readyRead, this, &Downloader::downloadReadyRead );

    LOG_INFO << "Downloading " << url_.toEncoded() << " from byte " << received_ << ", attempt "
             << attempts_;
}

// Checks the status of the response before its first bytes are written,
// false when its body is not the file
bool Downloader::checkResponse()
{
    if ( responseChecked_ ) {
        return true;
    }

    const auto status
        = currentDownload_->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt();
    if ( status == 206 ) {
        // Content-Range: bytes <first>-<last>/<total>
        const auto range = currentDownload_->rawHeader( "Content-Range" );
        const auto first = range.mid( 6, range.indexOf( '-' ) - 6 ).toLongLong();
        if ( first != received_ ) {
            LOG_ERROR << "Unexpected range " << range << ", restarting";
            received_ = 0;
            output_->resize( 0 );
            output_->seek( 0 );
            return false;
        }
        const auto slash = range.lastIndexOf( '/' );
        bool ok = false;
        const auto total = range.mid( slash + 1 ).toLongLong( &ok );
        if ( slash > 0 && ok ) {
            total_ = total;
        }
    }
    else if ( status == 200 ) {
        // The whole file: the server ignored the range
        if ( received_ > 0 ) {
            LOG_INFO << "The server sends the whole file again";
            received_ = 0;
            output_->resize( 0 );
            output_->seek( 0 );
        }
        const auto length = currentDownload_->header( QNetworkRequest::ContentLengthHeader );
        if ( length.isValid() ) {
            total_ = length.toLongLong();
        }
    }
    else {
        return false;
    }

    responseChecked_ = true;
    return true;
}

void Downloader::downloadReadyRead()
{
    if ( !checkResponse() ) {
        currentDownload_->readAll();
        return;
    }

    const auto data = currentDownload_->readAll();
    if ( output_->write( data ) != data.size() ) {
        addError( output_->errorString() );
        aborted_ = true;
        currentDownload_->abort();
        return;
    }
    received_ += data.size();
    Q_EMIT downloadProgress( received_, total_ );
}

void Downloader::downloadFinished()
{
    auto* reply = currentDownload_;
    currentDownload_ = nullptr;
    reply->deleteLater();

    if ( !reply->error() && !responseChecked_ ) {
        // An empty body: no readyRead
        currentDownload_ = reply;
        checkResponse();
        currentDownload_ = nullptr;
    }

    const auto status = reply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt();
    const bool complete = total_ < 0 || received_ == total_;

    if ( !reply->error() && responseChecked_ && complete ) {
        LOG_INFO << "Download done, " << received_ << " bytes";
        output_->close();
        Q_EMIT finished( true );
        return;
    }

    const auto error = reply->error() ? reply->errorString()
                                      : tr( "The download stopped at %1 of %2 bytes" )
                                            .arg( received_ )
                                            .arg( total_ );
    LOG_ERROR << "Download failed at byte " << received_ << ": " << error;

    if ( aborted_ ) {
        fail( error );
        return;
    }

    // Missing file, refused access...: another attempt gets the same answer
    // (416: the range is past the end, the file changed)
    if ( status >= 400 && status != 416 && status < 500 ) {
        fail( error );
        return;
    }
    if ( status == 416 ) {
        received_ = 0;
        output_->resize( 0 );
        output_->seek( 0 );
    }

    addError( error );
    attemptsWithoutProgress_ = received_ > receivedAtStart_ ? 0 : attemptsWithoutProgress_ + 1;
    if ( attempts_ < MaxAttempts && attemptsWithoutProgress_ < MaxAttemptsWithoutProgress ) {
        startQt();
    }
    else {
        output_->close();
        attempts_ = 0;
        startCurl();
    }
}

void Downloader::startCurl()
{
    // In Windows since Windows 10 1803, in macOS and in most Linux systems
    const auto program = QStandardPaths::findExecutable( "curl" );
    if ( program.isEmpty() ) {
        fail( {} );
        return;
    }

    ++attempts_;
    receivedAtStart_ = QFileInfo( output_->fileName() ).size();

    // --continue-at -: from the end of the file written by the previous attempts
    QStringList arguments{ "--location",   "--fail",          "--silent",
                           "--show-error", "--continue-at",   "-",
                           "--user-agent", QString( userAgent() ), "--output",
                           output_->fileName() };
    if ( !Configuration::get().verifySslPeers() ) {
        arguments << "--insecure";
    }
    arguments << url_.toString( QUrl::FullyEncoded );

    curl_ = new QProcess( this );
    curl_->setProcessChannelMode( QProcess::MergedChannels );
    const auto done = [ this ]( bool success, const QString& error ) {
        if ( !curl_ ) {
            return;
        }
        curl_->deleteLater();
        curl_ = nullptr;

        const auto size = QFileInfo( output_->fileName() ).size();
        if ( success && !aborted_ && ( total_ < 0 || size == total_ ) ) {
            LOG_INFO << "Download done with curl, " << size << " bytes";
            Q_EMIT finished( true );
            return;
        }

        LOG_ERROR << "Download with curl failed at byte " << size << ": " << error;
        addError( error );
        if ( !aborted_ && size > receivedAtStart_ && attempts_ < MaxCurlRuns ) {
            // It made progress: go on from there
            startCurl();
        }
        else {
            fail( {} );
        }
    };
    connect( curl_, &QProcess::finished, this,
             [ this, done ]( int exitCode, QProcess::ExitStatus status ) {
                 const auto output = QString::fromLocal8Bit( curl_->readAll() ).trimmed();
                 if ( status == QProcess::NormalExit && exitCode == 0 ) {
                     done( true, {} );
                 }
                 else {
                     // Its messages start with "curl: "
                     done( false, output.isEmpty() ? "curl: " + curl_->errorString() : output );
                 }
             } );
    connect( curl_, &QProcess::errorOccurred, this, [ this, done ]( QProcess::ProcessError error ) {
        if ( error == QProcess::FailedToStart ) {
            done( false, "curl: " + curl_->errorString() );
        }
    } );

    // The progress of curl is not followed: busy
    Q_EMIT downloadProgress( 0, 0 );
    LOG_INFO << "Downloading " << url_.toEncoded() << " with curl from byte " << receivedAtStart_;
    curl_->start( program, arguments );
}

void Downloader::fail( const QString& error )
{
    addError( error );
    if ( currentDownload_ == nullptr ) {
        output_->close();
    }
    output_->remove();
    Q_EMIT finished( false );
}
