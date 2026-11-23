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

#include <functional>

#include <QAction>
#include <QApplication>
#include <QShortcut>
#include <QWidget>

#include "shortcuts.h"

QStringList getKeyBindings( QKeySequence::StandardKey standardKey )
{
    auto bindings = QKeySequence::keyBindings( standardKey );
    QStringList stringBindings;
    std::transform( bindings.cbegin(), bindings.cend(), std::back_inserter( stringBindings ),
                    []( const auto& keySequence ) { return keySequence.toString(); } );

    return stringBindings;
}

QStringList ShortcutAction::defaultShortcutKeys( const std::string& action )
{
    const auto& shortcuts = defaultShortcutList();
    const auto actionShortcuts = shortcuts.find( action );
    return actionShortcuts != shortcuts.end() ? actionShortcuts->second.keySequence : QStringList{};
}

void ShortcutAction::registerShortcut( const ConfiguredShortcuts& configuredShortcuts,
                                       std::map<QString, QShortcut*>& shortcutsStorage,
                                       QWidget* shortcutsParent, Qt::ShortcutContext context,
                                       const std::string& action,
                                       const std::function<void()>& func )
{
    const auto keysConfiguration = configuredShortcuts.find( action );
    const auto keys = keysConfiguration != configuredShortcuts.end()
                          ? keysConfiguration->second
                          : ShortcutAction::defaultShortcutKeys( action );

    for ( const auto& key : keys ) {
        if ( key.isEmpty() ) {
            continue;
        }

        auto shortcut = shortcutsStorage.extract( key );
        if ( shortcut ) {
            shortcut.mapped()->deleteLater();
        }

        registerShortcut( key, shortcutsStorage, shortcutsParent, context, func );
    }
}

void ShortcutAction::registerShortcut( const QString& key,
                                       std::map<QString, QShortcut*>& shortcutsStorage,
                                       QWidget* shortcutsParent, Qt::ShortcutContext context,
                                       const std::function<void()>& func )
{
    auto newShortcut = new QShortcut( QKeySequence( key ), shortcutsParent );
    newShortcut->setContext( context );
    newShortcut->connect( newShortcut, &QShortcut::activated, shortcutsParent,
                          [ func ] { func(); } );
    shortcutsStorage.emplace( key, newShortcut );
}

QList<QKeySequence> ShortcutAction::shortcutKeys( const std::string& action,
                                                  const ConfiguredShortcuts& configuredShortcuts )
{
    const auto keysConfiguration = configuredShortcuts.find( action );
    const auto keys = keysConfiguration != configuredShortcuts.end()
                          ? keysConfiguration->second
                          : ShortcutAction::defaultShortcutKeys( action );

    QList<QKeySequence> shortcuts;
    std::transform( keys.cbegin(), keys.cend(), std::back_inserter( shortcuts ),
                    []( const QString& hotkeys ) { return QKeySequence( hotkeys ); } );

    return shortcuts;
}

void ShortcutAction::showInMenu( QAction* menuAction, const std::string& action,
                                 const ConfiguredShortcuts& configuredShortcuts )
{
    const auto keys = shortcutKeys( action, configuredShortcuts );
    menuAction->setShortcutContext( Qt::WidgetShortcut );
    menuAction->setShortcut( keys.isEmpty() ? QKeySequence{} : keys.front() );
}

const ShortcutAction::ShortcutList& ShortcutAction::defaultShortcutList()
{
    static ShortcutList defaultShortcutKeys = {
        {
            MainWindowNewWindow,
            {
                QApplication::tr( "Open new window" ),
                QStringList{},
            },
        },
        {
            MainWindowOpenFile,
            {
                QApplication::tr( "Open file" ),
                getKeyBindings( QKeySequence::Open ),
            },
        },
        {
            MainWindowCloseFile,
            {
                QApplication::tr( "Close file" ),
                QStringList{ "Ctrl+W", "Ctrl+F4" },
            },
        },
        {
            MainWindowCloseAll,
            {
                QApplication::tr( "Close all files" ),
                QStringList{},
            },
        },
        {
            MainWindowSelectAll,
            {
                QApplication::tr( "Select all" ),
                QStringList{ "Ctrl+A" },
            },
        },
        {
            MainWindowCopy,
            {
                QApplication::tr( "Copy selection to clipboard" ),
                QStringList{ "Ctrl+C", QKeySequence( Qt::Key_Copy ).toString() },
            },
        },
        {
            MainWindowQuit,
            {
                QApplication::tr( "Exit application" ),
                QStringList{ "Ctrl+Q" },
            },
        },
        {
            MainWindowFullScreen,
            {
                QApplication::tr( "Full Screen" ),
                QStringList{},
            },
        },
        {
            MainWindowMax,
            {
                QApplication::tr( "Maximize window" ),
                QStringList{},
            },
        },
        {
            MainWindowMin,
            {
                QApplication::tr( "Minimize Window" ),
                QStringList{},
            },
        },
        {
            MainWindowPreference,
            {
                QApplication::tr( "Preferences" ),
                QStringList{},
            },
        },
        {
            MainWindowOpenQf,
            {
                QApplication::tr( "Open quick find" ),
                getKeyBindings( QKeySequence::Find ),
            },
        },
        {
            MainWindowOpenQfForward,
            {
                QApplication::tr( "Quick find forward" ),
                QStringList{ QKeySequence( Qt::Key_Apostrophe ).toString() },
            },
        },
        {
            MainWindowOpenQfBackward,
            {
                QApplication::tr( "Quick find backward" ),
                QStringList{ QKeySequence( Qt::Key_QuoteDbl ).toString() },
            },
        },
        {
            MainWindowFocusSearchInput,
            {
                QApplication::tr( "Set focus to search input" ),
                QStringList{ { "Ctrl+S", "Ctrl+Shift+F" } },
            },
        },
        {
            MainWindowOpenContainingFolder,
            {
                QApplication::tr( "Open containing folder" ),
                QStringList{},
            },
        },
        {
            MainWindowOpenInEditor,
            {
                QApplication::tr( "Open file in editor" ),
                QStringList{},
            },
        },
        {
            MainWindowCopyPathToClipboard,
            {
                QApplication::tr( "Copy file path to clipboard" ),
                QStringList{},
            },
        },
        {
            MainWindowOpenFromClipboard,
            {
                QApplication::tr( "Paste text from clipboard" ),
                QStringList{ "Ctrl+V" },
            },
        },
        {
            MainWindowOpenFromUrl,
            {
                QApplication::tr( "Open file from URL" ),
                QStringList{},
            },
        },
        {
            MainWindowFollowFile,
            {
                QApplication::tr( "Monitor file changes" ),
                { { QKeySequence( Qt::Key_F ).toString(),
                    QKeySequence( Qt::Key_F10 ).toString() } },
            },
        },
        {
            MainWindowTextWrap,
            {
                QApplication::tr( "Toggle text wrap" ),
                QStringList{ QKeySequence( Qt::Key_W ).toString() },
            },
        },
        {
            MainWindowReload,
            {
                QApplication::tr( "Reload file" ),
                QStringList{ QKeySequence( Qt::Key_F5 ).toString() },
            },
        },
        {
            MainWindowStop,
            {
                QApplication::tr( "Stop file loading" ),
                QStringList{ QKeySequence( Qt::Key_Escape ).toString(), "Ctrl+." },
            },
        },
        {
            MainWindowScratchpad,
            {
                QApplication::tr( "Open scratchpad" ),
                QStringList{},
            },
        },
        {
            MainWindowSelectOpenFile,
            {
                QApplication::tr( "Switch to file" ),
                QStringList{ "Ctrl+Shift+O" },
            },
        },
        {
            MainWindowToggleMainView,
            {
                QApplication::tr( "Show/hide main log view" ),
#ifdef Q_OS_MAC
                // Cmd+H hides the application on macOS, use the Control key itself
                QStringList{ "Meta+H" },
#else
                QStringList{ "Ctrl+H" },
#endif
            },
        },
        {
            MainWindowToggleFilterPanel,
            {
                QApplication::tr( "Show/hide filter panel" ),
                QStringList{},
            },
        },
        {
            MainWindowRenderMode,
            {
                QApplication::tr( "Toggle render mode" ),
                // Like the Markdown preview of VS Code
                QStringList{ "Ctrl+Shift+V" },
            },
        },
        {
            MainWindowEditMode,
            {
                QApplication::tr( "Toggle edit mode" ),
                QStringList{ "Ctrl+Shift+E" },
            },
        },
        {
            MainWindowToggleFileTree,
            {
                QApplication::tr( "Show/hide file tree panel" ),
                QStringList{},
            },
        },
        {
            MainWindowOpenFolder,
            {
                QApplication::tr( "Open folder in the file tree" ),
                QStringList{},
            },
        },
        {
            MainWindowAnalyze,
            {
                QApplication::tr( "Analyze tools (durations, rules, compare)" ),
                QStringList{},
            },
        },
        {
            MainWindowSearchInFolder,
            {
                QApplication::tr( "Search in the folder of the file tree" ),
                QStringList{},
            },
        },
        {
            FilterPanelFindNext,
            {
                QApplication::tr( "Find next match of selected filters" ),
                QStringList{ QKeySequence( Qt::Key_F8 ).toString() },
            },
        },
        {
            FilterPanelFindPrevious,
            {
                QApplication::tr( "Find previous match of selected filters" ),
                QStringList{ "Shift+F8" },
            },
        },
        {
            FilterPanelAddFilter,
            {
                QApplication::tr( "Filter panel: add filter" ),
                QStringList{ QKeySequence( Qt::Key_Insert ).toString() },
            },
        },
        {
            FilterPanelEdit,
            {
                QApplication::tr( "Filter panel: edit filter or group" ),
                QStringList{ { QKeySequence( Qt::Key_F2 ).toString(),
                               QKeySequence( Qt::Key_Return ).toString() } },
            },
        },
        {
            FilterPanelDelete,
            {
                QApplication::tr( "Filter panel: delete selection" ),
                QStringList{ QKeySequence( Qt::Key_Delete ).toString() },
            },
        },
        {
            FilterPanelMoveUp,
            {
                QApplication::tr( "Filter panel: move filter or group up" ),
                QStringList{ "Alt+Up" },
            },
        },
        {
            FilterPanelMoveDown,
            {
                QApplication::tr( "Filter panel: move filter or group down" ),
                QStringList{ "Alt+Down" },
            },
        },
        {
            FilterPanelSave,
            {
                QApplication::tr( "Filter panel: save filters" ),
                QStringList{ "Ctrl+S" },
            },
        },
        {
            CrawlerChangeVisibilityForward,
            {
                QApplication::tr( "Change filtered lines visibility forward" ),
                QStringList{ QKeySequence( Qt::Key_V ).toString() },
            },
        },
        {
            CrawlerChangeVisibilityBackward,
            {
                QApplication::tr( "Change filtered lines visibility backward" ),
                QStringList{ "Shift+V" },
            },
        },
        {
            CrawlerChangeVisibilityToMarksAndMatches,
            {
                QApplication::tr( "Change filtered lines visibility to marks and matches" ),
                QStringList{},
            },
        },
        {
            CrawlerChangeVisibilityToMarks,
            {
                QApplication::tr( "Change filtered lines visibility to marks" ),
                QStringList{},
            },
        },
        {
            CrawlerChangeVisibilityToMatches,
            {
                QApplication::tr( "Change filtered lines visibility to matches" ),
                QStringList{},
            },
        },
        {
            CrawlerIncreseTopViewSize,
            {
                QApplication::tr( "Increase main view" ),
                QStringList{ QKeySequence( Qt::Key_Plus ).toString() },
            },
        },
        {
            CrawlerDecreaseTopViewSize,
            {
                QApplication::tr( "Decrease main view" ),
                QStringList{ QKeySequence( Qt::Key_Minus ).toString() },
            },
        },
        {
            CrawlerEnableCaseMatching,
            {
                QApplication::tr( "Enable case matching" ),
                QStringList{},
            },
        },
        {
            CrawlerEnableRegex,
            {
                QApplication::tr( "Enable regex" ),
                QStringList{},
            },
        },
        {
            CrawlerEnableInverseMatching,
            {
                QApplication::tr( "Enable inverse matching" ),
                QStringList{},
            },
        },
        {
            CrawlerEnableRegexCombining,
            {
                QApplication::tr( "Enable regex combining" ),
                QStringList{},
            },
        },
        {
            CrawlerEnableAutoRefresh,
            {
                QApplication::tr( "Enable auto refresh" ),
                QStringList{},
            },
        },
        {
            CrawlerKeepResults,
            {
                QApplication::tr( "Keep search results" ),
                QStringList{ QKeySequence( Qt::Key_9 ).toString() },
            },
        },
        // remove by commit 0b75b9d6
        // { QfFindNext, { QApplication::tr( "QuickFind: Find next" ), QStringList{ getKeyBindings(
        // QKeySequence::FindNext ) } } }, { QfFindPrev, { QApplication::tr( "QuickFind: Find
        // previous"
        // ), QStringList{ getKeyBindings( QKeySequence::FindPrevious ) } } },
        {
            LogViewMark,
            {
                QApplication::tr( "Add line mark" ),
                QStringList{ QKeySequence( Qt::Key_M ).toString() },
            },
        },
        {
            LogViewNextMark,
            {
                QApplication::tr( "Jump to next mark" ),
                QStringList{ QKeySequence( Qt::Key_BracketRight ).toString() },
            },
        },
        {
            LogViewPrevMark,
            {
                QApplication::tr( "Jump to previous mark" ),
                QStringList{ QKeySequence( Qt::Key_BracketLeft ).toString() },
            },
        },
        {
            LogViewSelectionUp,
            {
                QApplication::tr( "Move selection up" ),
                { { QKeySequence( Qt::Key_Up ).toString(), QKeySequence( Qt::Key_K ).toString() } },
            },
        },
        {
            LogViewSelectionDown,
            {
                QApplication::tr( "Move selection down" ),
                { { QKeySequence( Qt::Key_Down ).toString(),
                    QKeySequence( Qt::Key_J ).toString() } },
            },
        },
        {
            LogViewScrollUp,
            {
                QApplication::tr( "Scroll up" ),
                QStringList{ "Ctrl+Up" },
            },
        },
        {
            LogViewScrollDown,
            {
                QApplication::tr( "Scroll down" ),
                QStringList{ "Ctrl+Down" },
            },
        },
        {
            LogViewScrollLeft,
            {
                QApplication::tr( "Scroll left" ),
                { { QKeySequence( Qt::Key_Left ).toString(),
                    QKeySequence( Qt::Key_H ).toString() } },
            },
        },
        {
            LogViewScrollRight,
            {
                QApplication::tr( "Scroll right" ),
                { { QKeySequence( Qt::Key_Right ).toString(),
                    QKeySequence( Qt::Key_L ).toString() } },
            },
        },
        {
            LogViewJumpToStartOfLine,
            {
                QApplication::tr( "Jump to the beginning of the current line" ),
                QStringList{ QKeySequence( Qt::Key_Home ).toString() },
            },
        },
        {
            LogViewJumpToEndOfLine,
            {
                QApplication::tr( "Jump to the end start of the current line" ),
                QStringList{ QKeySequence( Qt::Key_End ).toString() },
            },
        },
        {
            LogViewJumpToRightOfScreen,
            {
                QApplication::tr( "Jump to the right of the text" ),
                QStringList{ "Shift+End" },
            },
        },
        {
            LogViewJumpToBottom,
            {
                QApplication::tr( "Jump to the bottom of the text" ),
                QStringList{ { "Ctrl+End", "Shift+G" } },
            },
        },
        {
            LogViewJumpToTop,
            {
                QApplication::tr( "Jump to the top of the text" ),
                QStringList{ "Ctrl+Home" },
            },
        },
        {
            LogViewJumpToLine,
            {
                QApplication::tr( "Jump to line" ),
                QStringList{ "Ctrl+L" },
            },
        },
        {
            LogViewQfForward,
            {
                QApplication::tr( "Main view: find next" ),
                QStringList{ "Ctrl+G", QKeySequence( Qt::Key_N ).toString() },
            },
        },
        {
            LogViewQfBackward,
            { QApplication::tr( "Main view: find previous" ),
              QStringList{ "Ctrl+Shift+G", "Shift+N" } },
        },
        {
            LogViewQfSelectedForward,
            { QApplication::tr( "Set selection to QuickFind and find next" ),
              QStringList{ QKeySequence( Qt::Key_Period ).toString() } },
        },
        {
            LogViewQfSelectedBackward,
            {
                QApplication::tr( "Set selection to QuickFind and find previous" ),
                QStringList{ QKeySequence( Qt::Key_Comma ).toString() },
            },
        },
        {
            LogViewExitView,
            {
                QApplication::tr( "Release focus from view" ),
                QStringList{ QKeySequence( Qt::Key_Space ).toString() },
            },
        },
        {
            LogViewAddColorLabel1,
            {
                QApplication::tr( "Highlight text with color 1" ),
                QStringList{ "Alt+1" },
            },
        },
        {
            LogViewAddColorLabel2,
            {
                QApplication::tr( "Highlight text with color 2" ),
                QStringList{ "Alt+2" },
            },
        },
        {
            LogViewAddColorLabel3,
            {
                QApplication::tr( "Highlight text with color 3" ),
                QStringList{ "Alt+3" },
            },
        },
        {
            LogViewAddColorLabel4,
            {
                QApplication::tr( "Highlight text with color 4" ),
                QStringList{ "Alt+4" },
            },
        },
        {
            LogViewAddColorLabel5,
            {
                QApplication::tr( "Highlight text with color 5" ),
                QStringList{ "Alt+5" },
            },
        },
        {
            LogViewAddColorLabel6,
            {
                QApplication::tr( "Highlight text with color 6" ),
                QStringList{ "Alt+6" },
            },
        },
        {
            LogViewAddColorLabel7,
            {
                QApplication::tr( "Highlight text with color 7" ),
                QStringList{ "Alt+7" },
            },
        },
        {
            LogViewAddColorLabel8,
            {
                QApplication::tr( "Highlight text with color 8" ),
                QStringList{ "Alt+8" },
            },
        },
        {
            LogViewAddColorLabel9,
            {
                QApplication::tr( "Highlight text with color 9" ),
                QStringList{ "Alt+9" },
            },
        },
        {
            LogViewAddNextColorLabel,
            {
                QApplication::tr( "Highlight text with next color" ),
                QStringList{ "Ctrl+D" },
            },
        },
        {
            LogViewClearColorLabels,
            {
                QApplication::tr( "Clear all color labels" ),
                QStringList{ "Ctrl+Shift+0" },
            },
        },
        {
            LogViewSetBookmark1,
            {
                QApplication::tr( "Set/remove bookmark 1" ),
                QStringList{ "Ctrl+1" },
            },
        },
        {
            LogViewSetBookmark2,
            {
                QApplication::tr( "Set/remove bookmark 2" ),
                QStringList{ "Ctrl+2" },
            },
        },
        {
            LogViewSetBookmark3,
            {
                QApplication::tr( "Set/remove bookmark 3" ),
                QStringList{ "Ctrl+3" },
            },
        },
        {
            LogViewSetBookmark4,
            {
                QApplication::tr( "Set/remove bookmark 4" ),
                QStringList{ "Ctrl+4" },
            },
        },
        {
            LogViewSetBookmark5,
            {
                QApplication::tr( "Set/remove bookmark 5" ),
                QStringList{ "Ctrl+5" },
            },
        },
        {
            LogViewJumpToBookmark1,
            {
                QApplication::tr( "Jump to next line with bookmark 1" ),
                QStringList{ "1" },
            },
        },
        {
            LogViewJumpToBookmark2,
            {
                QApplication::tr( "Jump to next line with bookmark 2" ),
                QStringList{ "2" },
            },
        },
        {
            LogViewJumpToBookmark3,
            {
                QApplication::tr( "Jump to next line with bookmark 3" ),
                QStringList{ "3" },
            },
        },
        {
            LogViewJumpToBookmark4,
            {
                QApplication::tr( "Jump to next line with bookmark 4" ),
                QStringList{ "4" },
            },
        },
        {
            LogViewJumpToBookmark5,
            {
                QApplication::tr( "Jump to next line with bookmark 5" ),
                QStringList{ "5" },
            },
        },
        {
            LogViewJumpToPreviousBookmark1,
            {
                QApplication::tr( "Jump to previous line with bookmark 1" ),
                QStringList{ "!" },
            },
        },
        {
            LogViewJumpToPreviousBookmark2,
            {
                QApplication::tr( "Jump to previous line with bookmark 2" ),
                QStringList{ "@" },
            },
        },
        {
            LogViewJumpToPreviousBookmark3,
            {
                QApplication::tr( "Jump to previous line with bookmark 3" ),
                QStringList{ "#" },
            },
        },
        {
            LogViewJumpToPreviousBookmark4,
            {
                QApplication::tr( "Jump to previous line with bookmark 4" ),
                QStringList{ "$" },
            },
        },
        {
            LogViewJumpToPreviousBookmark5,
            {
                QApplication::tr( "Jump to previous line with bookmark 5" ),
                QStringList{ "%" },
            },
        },
        {
            LogViewSendSelectionToScratchpad,
            {
                QApplication::tr( "Send selection to scratchpad" ),
                QStringList{ "Ctrl+Z" },
            },
        },
        {
            LogViewReplaceScratchpadWithSelection,
            {
                QApplication::tr( "Replace scratchpad with selection" ),
                QStringList{ "Ctrl+Shift+Z" },
            },
        },
        {
            LogViewAddToSearch,
            {
                QApplication::tr( "Add selection to search pattern" ),
                QStringList{ "Shift+A" },
            },
        },
        {
            LogViewExcludeFromSearch,
            {
                QApplication::tr( "Exclude selection from search pattern " ),
                QStringList{ "Shift+E" },
            },
        },
        {
            LogViewReplaceSearch,
            {
                QApplication::tr( "Replace search pattern with selection" ),
                QStringList{ "Shift+R" },
            },
        },
        {
            LogViewEditNote,
            {
                QApplication::tr( "Add or edit the note of the line" ),
                QStringList{ "Shift+M" },
            },
        },
        {
            LogViewTimeReference,
            {
                QApplication::tr( "Set the time reference line (on it: clear it)" ),
                QStringList{ "T" },
            },
        },
        {
            LogViewClearTimeReference,
            {
                QApplication::tr( "Clear the time reference line" ),
                QStringList{ "Shift+T" },
            },
        },
        {
            LogViewAddAsFilter,
            {
                QApplication::tr( "Add selection as filter" ),
                QStringList{ "Shift+F" },
            },
        },
        {
            LogViewSelectLinesUp,
            {
                QApplication::tr( "Select lines down" ),
                QStringList{ "Shift+Up" },
            },
        },
        {
            LogViewSelectLinesDown,
            {
                QApplication::tr( "Select lines up" ),
                QStringList{ "Shift+Down" },
            },
        },
    };
    return defaultShortcutKeys;
}
