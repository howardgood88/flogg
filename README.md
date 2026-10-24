# flogg

flogg is a log viewer built on [klogg](https://github.com/variar/klogg). It keeps
klogg's fast file reading and search, and replaces its search bar workflow with a
[TextAnalysisTool](https://textanalysistool.github.io/) style filter panel: a list
of filters that decides both which lines appear in the filtered view and how the
main view is coloured.

Downloads for Windows (x64) and macOS (Apple Silicon) are on the
[releases page](https://github.com/howardgood88/flogg/releases/latest).

## Features

### Filter panel

- Filters are organised in a tree of nested groups.
- Each filter has a pattern (plain text or regular expression), case sensitivity,
  an exclude option, and foreground/background colours applied to the whole line
  or to the match only.
- The enabled filters drive both the filtered view and the colouring of the main
  view.
- Enable or disable single filters, whole groups, or everything at once; suspend a
  filter temporarily without losing its state.
- Filters can have multi-line descriptions (first line in the list, all of it in
  the tool tip).
- Move filters up and down, and in and out of groups, from the tool bar.
- Columns resize independently; their widths and visibility are saved. Right click
  the header to choose the visible columns.
- Add the current selection as a new filter.

### Fast filtering

- Each filter keeps its own result bitmap, so toggling filters only recombines
  cached results and editing a filter rescans that pattern only.
- Larger read blocks and no sleep polling in the search pipeline.

### Navigation

- F8 / Shift+F8 jump to the next / previous match of the selected filter.
- Numbered bookmarks 1-8 on any number of lines: Ctrl+N toggles, Alt+N and
  Alt+Shift+N go to the next and previous line with bookmark N. The bookmark
  column is only shown while the file has bookmarks.
- View > Show main log view (Ctrl+H) hides the top view to give the filtered view
  the whole window.
- All new actions have configurable shortcuts in the preferences.

### Filter files

- Filters are saved as JSON filter files.
- Import TextAnalysisTool `.tat` files.
- Import klogg predefined filters and highlighters.

### Command line

- `--config <file>` keeps the settings in that file (and the session in
  `<name>_session.conf` next to it).
- `--filters <file>` loads a filter file without making it the default one.
- Together with `-m` this starts several windows, each with its own settings and
  filters.
- On Windows the settings and the default filter file are kept next to
  `flogg.exe`, so the package is portable.

### Removed from klogg

Predefined filters, highlighter sets, search highlighting, clear file, the version
check and the Help menu links are replaced by the filter panel or dropped.

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
