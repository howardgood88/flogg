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

#ifndef KLOGG_DOWNLOADER_H
#define KLOGG_DOWNLOADER_H

#include <QFile>
#include <QNetworkAccessManager>
#include <QStringList>
#include <QUrl>

class QProcess;

// Downloads a file. A lost connection is resumed where it stopped with a Range request
// (some proxies cut the downloads from the start of a package but let the rest through),
// switching to HTTP/1.1 and TLS 1.2 when the attempts make no progress, then to the curl
// program of the system
class Downloader : public QObject {
    Q_OBJECT
  public:
    explicit Downloader( QObject* parent = nullptr );

    void download( const QUrl& url, QFile* outputFile );

    QString lastError() const;

    // Ends the download in progress, finished( false ) follows
    void abort();

  Q_SIGNALS:
    void downloadProgress( qint64 bytesReceived, qint64 bytesTotal );
    void finished( bool );

  private Q_SLOTS:
    void downloadFinished();
    void downloadReadyRead();

  private:
    void startQt();
    bool checkResponse();
    void startCurl();
    void fail( const QString& error );
    void addError( const QString& error );

    QNetworkAccessManager manager_;
    QNetworkReply* currentDownload_ = nullptr;
    QProcess* curl_ = nullptr;

    QUrl url_;
    QFile* output_ = nullptr;

    qint64 received_ = 0;
    qint64 total_ = -1;
    qint64 receivedAtStart_ = 0;
    bool responseChecked_ = false;
    int attempts_ = 0;
    int attemptsWithoutProgress_ = 0;
    bool aborted_ = false;
    QStringList errors_;
};

#endif // KLOGG_DOWNLOADER_H
