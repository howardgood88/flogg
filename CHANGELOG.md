# flogg changes

## 1.1.3 (2026-11-03)

- New flogg icon (log lines cut by a blade) in place of the klogg one, on Windows, macOS and Linux
- Scroll bars have a rounded handle in every style, in colours of the style: easy to see in the
  Dark styles too
- Closing with unsaved filters of one file asks the short Save / Discard / Cancel question, the
  list of filters to save is shown only when there are several

## 1.1.2 (2026-11-03)

- Linux package: an AppImage for x64 (`chmod +x` it and run it), updated like the Windows one
- Updates are found also when GitHub limits the questions from your network (often in offices)
- Command line on Windows: the output of `-L`, `-h` and `--version` takes the place of the prompt
  the shell showed too early, without an empty line after it
- No macOS package for now: the macOS build machines of GitHub are seldom available

## 1.1.1 (2026-11-03)

- Updates: once a day at start, or with Help > Check for updates..., flogg looks for a newer
  release, shows its changes and can install it (it replaces itself when closed and starts again)
- Help > What's new in flogg... shows this list
- A new style applies at once, without restarting flogg
- Dark style: the log views and the filter panel use the colours of the Windows terminal
  (`#0C0C0C` background, `#CCCCCC` text), the other text and the icons are white, pressed and
  hovered buttons stand out like in the Light style
- Selected lines, selected words and selected filters have one colour (white on `#177FD3` by
  default), whatever the filter colours; the other widgets select with it too. It can be changed
  in Preferences > View > Style
- The tab bar is shown from the first open file
- The window title, About and `--version` show the release version without the build number
- Menus: Window comes after File, Filters is only in Tools
- Command line on Windows: `-L`, `-h`, `--version` and errors are printed in the terminal
  without garbled file names, and the prompt comes back after them
- Removed the klogg documentation from the Help menu
- Fixed: closing a window right after a style change could crash flogg

## 1.1.0 (2026-11-02)

- One flogg per settings file: a flogg started later opens its window in the running one
  (`-m` starts a separate flogg)
- Command line options for the windows of the running flogg: list them (`-L`, `--json`),
  target or name a window, load or add filters, use the shared filters, select a line, follow
- Only the first flogg restores the previous session
- Shared filters for tabs of every window; unsaved filters of all windows are asked about once
- Tabs using the same filter file follow its saves
- The same file can be open in several windows
- The filtered view follows the selection of the main view

## 1.0.7 (2026-10-29)

- Changing a filter no longer stalls the window with many filters: highlighters are prepared in
  the background
- Much faster tab switching with hundreds of filters
- Suspended filters and groups are saved in the filter file
- Less memory per tab for unchanged filters

## 1.0.6 (2026-10-28)

- Faster tab switching with many filters on Windows

## 1.0.5 (2026-10-28)

- Smooth tab switching on big files with millions of matches
- Each tab keeps its collapsed groups, selection and scroll position in the filter panel

## 1.0.4 (2026-10-28)

- Every tab has its own filters, also kept in the session. Filters are written with Save filters
  (Ctrl+S in the filter panel); closing a tab, a window or flogg asks about unsaved filters
- F8 / Shift+F8 from the filter panel jump in the filtered view
- Ctrl+click (Cmd+click on macOS) adds or removes single lines from the selection
- Add as filter without selected text uses the whole line
- Context menus show the shortcuts
- File > Import settings... / Export settings...
- Fixed: a dragged tab could stay where it was released

## 1.0.3 (2026-10-26)

- Selected filters keep their colours in the filter panel

## 1.0.2 (2026-10-26)

- F8 / Shift+F8 wrap around the file
- Groups show the hits of their enabled filters
- Hovering a line of the log views lists the filters matching it
- Filters are numbered in priority order (# column)
- Clear buttons for the colours in the filter edit dialog
- New Light style

## 1.0.1 (2026-10-24)

- `--config <file>` and `--filters <file>` command line options
- On Windows the settings and the default filter file are kept next to `flogg.exe`

## 1.0.0 (2026-10-24)

- First release: a TextAnalysisTool style filter panel on top of klogg, with groups, enabled and
  suspended filters, multi-line descriptions and selectable columns
- Windows and macOS packages
- Fixed crashes when closing a tab while its file loads, and when closing watched files on Windows
