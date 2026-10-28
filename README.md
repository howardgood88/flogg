# flogg

**English** | [繁體中文](README.zh-TW.md)

flogg is a log viewer built on [klogg](https://github.com/variar/klogg). It keeps klogg's fast file reading and search, and adds a more customisable [TextAnalysisTool](https://textanalysistool.github.io/) style filter panel.

![flogg with the filter panel](docs/images/filter-panel.png)

Downloads for Windows (x64) and macOS (Apple Silicon) are on the [releases page](https://github.com/howardgood88/flogg/releases/latest).

## Features

### Filter panel: a TAT style, easier to manage filter list

- Filters are organised in a tree of nested groups.
- Each filter has a pattern (text or regex), case sensitivity, an exclude option and foreground/background colours, applied to the whole line or to the match only.
- Enable or disable single filters, whole groups, or everything at once; a filter can also be suspended temporarily without changing whether it is enabled. Saving the filter file keeps both the enabled and the suspended states.
- With filters selected, `F8` / `Shift+F8` in the main or filtered view jumps between their matching lines.
- Hovering a line of the log views lists the filters matching it with their numbers, in priority order.
- Right click the column header to choose the visible columns; each column resizes independently and the widths are saved.

### Fast filtering: searching and filtering as fast as klogg

- Hyperscan can scan for many patterns at once, but a klogg search produces a single result; flogg extends it so that one pass over the file gives every filter its own result.
- Each filter keeps its search result in a Roaring Bitmap (as in klogg), which adapts its layout to how dense the matches are to keep memory use low.
- The final result is the union of the included filters minus the excluded ones. Enabling, disabling or suspending filters only recombines the bitmaps without rescanning, and adding or editing a filter rescans that pattern only.

### Bookmarks and views

- The filter panel and klogg's own search box are used one at a time; switch with the `Filters` button at the left end of the search bar above the filtered view.
- In the main and filtered views, `Ctrl+1-8` sets bookmarks and `Alt+1-8` / `Shift+Alt+1-8` jump to the next / previous line with that bookmark.
  (Shortcuts can be changed in `File > Preferences > Shortcuts`; flogg keeps klogg's own shortcuts by default.)
- `Ctrl+H` (macOS: `Control+H`) hides the main view and shows the filtered view only.
- `Ctrl+click` (macOS: `Cmd+click`) adds or removes single lines from the selection, for copying, marking or saving lines that do not follow each other.
- Context menu entries show their current shortcut.

### Filter and conf files

- Each tab has its own filters: the list, enabled, disabled and suspended states, and the JSON filter file it uses. A newly opened file gets the default filter file, `filters.flogg-filters.json` unless another one was opened or saved last (see [Command line](#command-line) for where it is).
- Filters are saved to their file only with `Save filters` (`Ctrl+S` in the filter panel); a `*` after the file name marks unsaved changes, and closing a tab or flogg asks to save them.
- Import TextAnalysisTool `.tat` files.
- Import klogg predefined filters and highlighters.
- Application settings (UI, shortcuts and so on) are kept in `flogg.conf` by default (see [Command line](#command-line) for where it is).
  `File > Import settings...` and `File > Export settings...` copy them from or to another `.conf` file; the session is not included.

### Command line

- `--config <file>` reads and writes the settings in that file (any name) instead of the default `flogg.conf`; the session goes to `<name>_session.conf` next to it.
- `--filters <file>` opens flogg with that filter file without making it the default one.
- Every start of flogg is a new instance (as with klogg's `-m`), however it is started: double click, taskbar, file association or command line.
- The previous session is not restored by default. With `Load last session` in `File > Preferences`, the first flogg started without a file restores the windows, files and per tab filters of the flogg closed last; `-s` / `-n` force it on or off.
- On Windows the settings (`flogg.conf`) and the default filter file are kept next to `flogg.exe`, so the whole folder is portable.
- On macOS they follow the system conventions: the settings are in `~/Library/Preferences/com.flogg.flogg.plist` and the default filter file in
  `~/Library/Application Support/flogg/filters.flogg-filters.json`; with `--config` the default filter file goes next to that settings file.

### Removed from klogg

Predefined filters, highlighter sets, search highlighting, clear file, the version check and the Help menu links are replaced by the filter panel or dropped.

## Shortcuts

Shortcuts added by flogg. All of them can be changed in File > Preferences > Shortcuts.

### Views

| Action | Shortcut | Also in |
| --- | --- | --- |
| Show/hide main view | `Ctrl+H` (macOS: `Control+H`) | View menu |
| Show/hide filter panel | none by default | View and Tools menus, close button of the panel |

### Filter panel

These work while the filter panel has the focus.

| Action | Shortcut | Mouse |
| --- | --- | --- |
| Add filter | `Insert` | `+ Filter` button, context menu |
| Add group | | `+ Group` button, context menu |
| Edit filter or rename group | `F2`, `Enter` | Double click |
| Delete selection | `Delete` | Context menu |
| Move up / down (also in and out of groups) | `Alt+Up` / `Alt+Down` | Arrow buttons, drag and drop |
| Enable / disable a filter | | Check box |
| Enable / disable (in group), enable all, disable all | | Context menu |
| Suspend / resume, suspend all / resume all | | Context menu |
| Add to search, exclude from search, replace search | | Context menu |
| Choose visible columns | | Right click the column header |
| Save filters | `Ctrl+S` | `File` button |
| Open, save as, import filter files | | `File` button |

### Log views

| Action | Shortcut | Mouse |
| --- | --- | --- |
| Next match of the selected filters (wraps around) | `F8` | Filter panel context menu |
| Previous match of the selected filters (wraps around) | `Shift+F8` | Filter panel context menu |
| Set/remove bookmark N (1-8) on the selected lines | `Ctrl+1` ... `Ctrl+8` | |
| Next line with bookmark N | `Alt+1` ... `Alt+8` | |
| Previous line with bookmark N | `Alt+Shift+1` ... `Alt+Shift+8` | |
| Add selection (or the whole line) as filter | none by default | `Add as filter...` in the log view context menu |
| Add search text as filter | | `Add as filter...` in the search box context menu |

## Building

flogg builds the same way as klogg, see [BUILD.md](BUILD.md). The CI build is in
[.github/workflows/flogg-build.yml](.github/workflows/flogg-build.yml), and
[bench/README.md](bench/README.md) describes the benchmark log generator.

## Credits and license

flogg is a fork of [klogg](https://github.com/variar/klogg) by Anton Filimonov,
which started as a fork of [glogg](https://github.com/nickbnf/glogg) by Nicolas
Bonnefon. All of the log reading, search engine and most of the UI come from
klogg; see the klogg repository for its documentation and history.

Licensed under the GPLv3 or later, see [COPYING](COPYING).
