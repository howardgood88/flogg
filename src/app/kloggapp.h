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

#ifndef KLOGG_KLOGGAPP_H
#define KLOGG_KLOGGAPP_H

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <numeric>
#include <qapplication.h>
#include <stack>

#include <QApplication>
#include <vector>

#include <QCborValue>

#include <QColor>
#include <QDir>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkProxyFactory>
#include <QRegularExpression>
#include <QUuid>

#ifdef Q_OS_MAC
#include <QFileOpenEvent>
#endif

#include "configuration.h"
#include "crashhandler.h"
#include "klogg_version.h"
#include "log.h"
#include "session.h"
#include "uuid.h"

#include <kdsingleapplication.h>

#include "filterpanel.h"
#include "filtertree.h"
#include "mainwindow.h"
#include "remotecontrol.h"

class KloggApp : public QApplication {

    Q_OBJECT

  public:
    KloggApp( int& argc, char* argv[] )
        : QApplication( argc, argv)
    {
        QFontDatabase::addApplicationFont( ":/fonts/DejaVuSansMono.ttf" );

        QNetworkProxyFactory::setUseSystemConfiguration( true );

        qRegisterMetaType<LoadingStatus>( "LoadingStatus" );
        qRegisterMetaType<LinesCount>( "LinesCount" );
        qRegisterMetaType<LineNumber>( "LineNumber" );
        qRegisterMetaType<std::vector<LineNumber>>( "std::vector<LineNumber>" );
        qRegisterMetaType<klogg::vector<LineNumber>>( "klogg::vector<LineNumber>" );
        qRegisterMetaType<LineLength>( "LineLength" );
        qRegisterMetaType<Portion>( "Portion" );
        qRegisterMetaType<Selection>( "Selection" );
        qRegisterMetaType<QFNotification>( "QFNotification" );
        qRegisterMetaType<QFNotificationReachedEndOfFile>( "QFNotificationReachedEndOfFile" );
        qRegisterMetaType<QFNotificationReachedBegininningOfFile>(
            "QFNotificationReachedBegininningOfFile" );
        qRegisterMetaType<QFNotificationProgress>( "QFNotificationProgress" );
        qRegisterMetaType<QFNotificationInterrupted>( "QFNotificationInterrupted" );
        qRegisterMetaType<QuickFindMatcher>( "QuickFindMatcher" );
    }

    // The flogg started first with these settings becomes the primary instance, the ones
    // started later send it their command line (see sendToPrimary)
    void startSingleInstance( const QString& name )
    {
        instanceName_ = name;
        singleApplication_ = std::make_unique<KDSingleApplication>( name );
    }

    bool isSecondary() const
    {
        return singleApplication_ && !singleApplication_->isPrimaryInstance();
    }

    bool isPrimary() const
    {
        return singleApplication_ && singleApplication_->isPrimaryInstance();
    }

    qint64 primaryPid() const
    {
        return singleApplication_ ? singleApplication_->primaryPid() : 0;
    }

    // Takes the command lines of the flogg started later
    void startControlServer()
    {
        controlServer_ = std::make_unique<remote::ControlServer>(
            [ this ]( const QJsonObject& request ) { return handleRequest( request ); } );
        controlServer_->listen( controlName() );
    }

    // Nothing when the primary instance does not answer in time
    std::optional<QJsonObject> sendToPrimary( const QJsonObject& request )
    {
#ifdef Q_OS_WIN
        // Lets the primary instance bring its window to the front
        ::AllowSetForegroundWindow( static_cast<DWORD>( primaryPid() ) );
#endif
        return remote::sendRequest( controlName(), request, 10000 );
    }

    // Reads the window options of a request, error set when they are invalid
    static std::optional<WindowCommand> readCommand( const QJsonObject& request, QString* error )
    {
        WindowCommand command;
        for ( const auto& file : request[ "files" ].toArray() ) {
            const auto path = file.toString();
            if ( !QFileInfo::exists( path ) ) {
                *error = QString( "no such file: %1" ).arg( QDir::toNativeSeparators( path ) );
                return {};
            }
            command.files.push_back( path );
        }
        command.follow = request[ "follow" ].toBool();
        command.line = static_cast<uint64_t>( request[ "line" ].toInteger() );
        command.sharedFilters = request[ "shared" ].toBool();

        // A file that does not exist yet starts empty, it is created when saved
        command.filtersFile = request[ "filters" ].toString();
        if ( QFileInfo::exists( command.filtersFile ) ) {
            QString loadError;
            if ( !filters::loadFromFile( command.filtersFile, &loadError ) ) {
                *error = QString( "can't load filters from %1: %2" )
                             .arg( QDir::toNativeSeparators( command.filtersFile ), loadError );
                return {};
            }
        }

        QColor color;
        const auto colorName = request[ "filterColor" ].toString();
        if ( !colorName.isEmpty() ) {
            color = QColor( colorName );
            if ( !color.isValid() ) {
                *error = QString( "invalid colour: %1" ).arg( colorName );
                return {};
            }
        }

        const auto addFilters = [ & ]( const QJsonArray& patterns, bool regex ) {
            for ( const auto& pattern : patterns ) {
                auto filter = FilterNode::makeFilter( pattern.toString() );
                filter.useRegex = regex;
                filter.backColor = color;
                if ( regex && !QRegularExpression( filter.pattern ).isValid() ) {
                    *error = QString( "invalid regular expression: %1" ).arg( filter.pattern );
                    return false;
                }
                command.addedFilters.push_back( std::move( filter ) );
            }
            return true;
        };
        if ( !addFilters( request[ "addFilters" ].toArray(), false )
             || !addFilters( request[ "addRegexes" ].toArray(), true ) ) {
            return {};
        }

        return command;
    }

    // Applies a command line to a window
    static void runCommand( MainWindow* window, const QJsonObject& request,
                            const WindowCommand& command )
    {
        if ( request.contains( "windowName" ) ) {
            window->setWindowName( request[ "windowName" ].toString() );
        }
        window->runCommand( command );
    }

    void initCrashHandler()
    {
        crashHandler_ = std::make_unique<CrashHandler>();
    }

    MainWindow* reloadSession()
    {
        if ( !session_ ) {
            session_ = std::make_shared<Session>();
        }

        for ( auto&& windowSession : session_->windowSessions() ) {
            auto w = newWindow( std::move( windowSession ) );
            w->reloadGeometry();
            w->reloadSession();
            w->show();
        }

        if ( mainWindows_.empty() ) {
            auto w = newWindow();
            w->show();
        }

        return mainWindows_.back().second;
    }

    MainWindow* newWindow()
    {
        if ( !session_ ) {
            session_ = std::make_shared<Session>();
        }

        const auto previousSessions = session_->windowSessions();

        QByteArray geometry;
        QString screenName;
        if ( !previousSessions.empty() ) {
            previousSessions.back().restoreGeometry( &geometry );
            screenName = previousSessions.back().restoreScreen();
        }

        auto window = newWindow( { session_, generateIdFromUuid(), nextWindowIndex() } );
        window->restoreWindowGeometry( geometry, screenName );

        return window;
    }

    void loadFileNonInteractive( const QString& file )
    {
        auto* window = activeWindow();
        if ( !window ) {
            window = newWindow();
            window->show();
        }

        window->loadFileNonInteractive( file );
    }

#ifdef Q_OS_MAC
    bool event( QEvent* event ) override
    {
        if ( event->type() == QEvent::FileOpen ) {
            QFileOpenEvent* openEvent = static_cast<QFileOpenEvent*>( event );
            LOG_INFO << "File open request " << openEvent->file();

            loadFileNonInteractive( openEvent->file() );
        }

        return QApplication::event( event );
    }
#endif

  private:
    QString controlName() const
    {
        return instanceName_ + "-control";
    }

    std::vector<MainWindow*> windows() const
    {
        std::vector<MainWindow*> windows;
        for ( const auto& [ session, window ] : mainWindows_ ) {
            windows.push_back( window );
        }
        std::sort( windows.begin(), windows.end(), []( const auto* lhs, const auto* rhs ) {
            return lhs->windowNumber() < rhs->windowNumber();
        } );
        return windows;
    }

    MainWindow* activeWindow()
    {
        while ( !activeWindows_.empty() && activeWindows_.top().isNull() ) {
            activeWindows_.pop();
        }
        return activeWindows_.empty() ? nullptr : activeWindows_.top().data();
    }

    // By number (2 or #2) or by name, exact then ignoring the case
    MainWindow* findWindow( const QString& target, QString* error ) const
    {
        const auto all = windows();

        auto number = target.startsWith( '#' ) ? target.mid( 1 ) : target;
        bool isNumber = false;
        const auto id = number.toInt( &isNumber );
        if ( isNumber ) {
            for ( auto* window : all ) {
                if ( window->windowNumber() == id ) {
                    return window;
                }
            }
        }

        for ( const auto caseSensitivity : { Qt::CaseSensitive, Qt::CaseInsensitive } ) {
            std::vector<MainWindow*> named;
            std::copy_if( all.begin(), all.end(), std::back_inserter( named ),
                          [ & ]( const auto* window ) {
                              return !window->windowName().isEmpty()
                                     && window->windowName().compare( target, caseSensitivity )
                                            == 0;
                          } );
            if ( named.size() == 1 ) {
                return named.front();
            }
            if ( named.size() > 1 ) {
                QStringList ids;
                for ( const auto* window : named ) {
                    ids.append( QString( "#%1" ).arg( window->windowNumber() ) );
                }
                *error = QString( "several windows are named %1 (%2), use the id" )
                             .arg( target, ids.join( ", " ) );
                return nullptr;
            }
        }

        *error = QString( "no window %1 (see flogg --list-windows)" ).arg( target );
        return nullptr;
    }

    QJsonArray windowsJson()
    {
        const auto* active = activeWindow();
        QJsonArray list;
        for ( auto* window : windows() ) {
            QJsonObject item;
            item[ "id" ] = window->windowNumber();
            item[ "name" ] = window->windowName();
            item[ "title" ] = window->windowTitle();
            item[ "active" ] = window == active;
            QJsonArray tabs;
            for ( const auto& tab : window->tabInfos() ) {
                QJsonObject tabItem;
                tabItem[ "file" ] = QDir::toNativeSeparators( tab.file );
                tabItem[ "current" ] = tab.current;
                tabItem[ "sharedFilters" ] = tab.sharedFilters;
                tabItem[ "filtersFile" ] = QDir::toNativeSeparators( tab.filtersFile );
                tabs.append( tabItem );
            }
            item[ "tabs" ] = tabs;
            list.append( item );
        }
        return list;
    }

    // #1 wifi (active)
    //   * /logs/a.log  [shared filters]
    //     /logs/b.log
    static QString windowsText( const QJsonArray& windows )
    {
        QString text;
        for ( const auto& value : windows ) {
            const auto window = value.toObject();
            text += QString( "#%1" ).arg( window[ "id" ].toInt() );
            if ( !window[ "name" ].toString().isEmpty() ) {
                text += "  " + window[ "name" ].toString();
            }
            if ( window[ "active" ].toBool() ) {
                text += "  (active)";
            }
            text += '\n';

            const auto tabs = window[ "tabs" ].toArray();
            if ( tabs.isEmpty() ) {
                text += "    (no file)\n";
            }
            for ( const auto& tabValue : tabs ) {
                const auto tab = tabValue.toObject();
                text += tab[ "current" ].toBool() ? "  * " : "    ";
                text += tab[ "file" ].toString();
                if ( tab[ "sharedFilters" ].toBool() ) {
                    text += "  [shared filters]";
                }
                text += '\n';
            }
        }
        return text;
    }

    QJsonObject handleRequest( const QJsonObject& request )
    {
        LOG_INFO << "Command line from another flogg: "
                 << QJsonDocument( request ).toJson( QJsonDocument::Compact );

        QJsonObject reply;
        if ( request[ "list" ].toBool() ) {
            const auto list = windowsJson();
            reply[ "output" ] = request[ "json" ].toBool()
                                    ? QString::fromUtf8( QJsonDocument( list ).toJson() )
                                    : windowsText( list );
            reply[ "ok" ] = true;
            return reply;
        }

        QString error;
        QPointer<MainWindow> window;
        const auto target = request[ "window" ].toString();
        if ( !target.isEmpty() ) {
            window = findWindow( target, &error );
            if ( !window ) {
                reply[ "error" ] = error;
                return reply;
            }
        }

        const auto command = readCommand( request, &error );
        if ( !command ) {
            reply[ "error" ] = error;
            return reply;
        }

        if ( !window ) {
            window = newWindow();
            const auto width = request[ "width" ].toInt();
            const auto height = request[ "height" ].toInt();
            if ( width > 0 && height > 0 ) {
                window->resize( width, height );
            }
            window->show();
        }
        reply[ "ok" ] = true;
        reply[ "window" ] = window->windowNumber();

        // After the reply: the command can ask the user to save changed filters
        QTimer::singleShot( 0, this, [ window, request, command = *command ] {
            if ( window ) {
                // In front first, for the questions about changed filters
                window->bringToFront();
                runCommand( window, request, command );
            }
        } );
        return reply;
    }

    MainWindow* newWindow( WindowSession&& session )
    {
        session_->addInstanceWindow( session.windowId() );
        mainWindows_.emplace_back( session, new MainWindow( session ) );

        auto& window = mainWindows_.back().second;

        activeWindows_.push( QPointer<MainWindow>( window ) );

        LOG_INFO << "Window " << &window << " created";
        connect( window, &MainWindow::newWindow, [ = ]() { newWindow()->show(); } );
        connect( window, &MainWindow::windowActivated,
                 [ this, window ]() { onWindowActivated( *window ); } );
        connect( window, &MainWindow::windowClosed,
                 [ this, window ]() { onWindowClosed( *window ); } );
        connect( window, &MainWindow::exitRequested, [ this ] { exitApplication(); } );

        return window;
    }

    void onWindowActivated( MainWindow& window )
    {
        LOG_INFO << "Window " << &window << " activated";
        activeWindows_.push( QPointer<MainWindow>( &window ) );
    }

    void onWindowClosed( MainWindow& window )
    {
        LOG_INFO << "Window " << &window << " closed";
        auto w = std::find_if( mainWindows_.begin(), mainWindows_.end(),
                               [ &window ]( const auto& p ) { return p.second == &window; } );

        if ( w != mainWindows_.end() ) {
            session_->removeInstanceWindow( w->first.windowId() );
            mainWindows_.erase( w );
        }
    }

    void exitApplication()
    {
        LOG_INFO << "exit application";

        // One question for the unsaved filters of every window, the shared ones listed once
        std::vector<UnsavedFilters> unsaved;
        for ( const auto& [ session, window ] : mainWindows_ ) {
            Q_UNUSED( session );
            for ( auto& filters : window->unsavedFiltersOnExit() ) {
                if ( !filters.shared
                     || std::none_of( unsaved.begin(), unsaved.end(),
                                      []( const auto& other ) { return other.shared; } ) ) {
                    unsaved.push_back( std::move( filters ) );
                }
            }
        }
        if ( !confirmUnsavedFilters( activeWindow(),
                                     tr( "Quitting: these filters have unsaved changes." ),
                                     unsaved ) ) {
            return;
        }

        session_->setExitRequested( true );
        session_->keepOnlyInstanceWindows();
        auto mainWindows = mainWindows_;
        mainWindows.reverse();
        for ( const auto& [ session, window ] : mainWindows ) {
            Q_UNUSED( session );
            // Kept open when it is only hidden in the tray
            window->setFiltersConfirmed( true );
            if ( !window->close() ) {
                session_->setExitRequested( false );
                return;
            }
        }

        QTimer::singleShot( 100, this, &QCoreApplication::quit );
    }

    size_t nextWindowIndex() const
    {
        if ( mainWindows_.empty() ) {
            return 0;
        }
        else {
            const auto windowWithMaxIndex = std::max_element(
                mainWindows_.begin(), mainWindows_.end(), []( const auto& lhs, const auto& rhs ) {
                    return lhs.first.windowIndex() < rhs.first.windowIndex();
                } );
            return windowWithMaxIndex->first.windowIndex() + 1;
        }
    }

  private:
    std::unique_ptr<CrashHandler> crashHandler_;

    QString instanceName_;
    std::unique_ptr<KDSingleApplication> singleApplication_;
    std::unique_ptr<remote::ControlServer> controlServer_;

    std::shared_ptr<Session> session_;

    std::list<std::pair<WindowSession, MainWindow*>> mainWindows_;
    std::stack<QPointer<MainWindow>> activeWindows_;

};

#endif // KLOGG_KLOGGAPP_H
