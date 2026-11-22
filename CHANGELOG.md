# flogg changes

## Unreleased

- **New: time between lines**: with two or more lines selected (`Ctrl+click`,
  `Shift+click`) the tool bar shows the time from the first to the last one; each line is
  parsed on its own (ISO 8601, logcat, syslog, web server, time of day, Unix time, seconds
  since boot), the first time stamp of the line compared unless another is chosen in its
  right click menu (which also copies it), lines of different formats not compared; up
  to 100 000 lines between are checked for the clock going back; it flashes when it changes;
  `Tools > Time Formats...` lists the formats read and takes your own
  (`dd.MM.yyyy HH:mm:ss.f`)
- **New: notes on lines** (`Add note...`, `Shift+M`): shown beside the bookmarks and in
  the tool tip, kept in `notes.json` of flogg with their bookmarks (never next to the
  logs), dropped when the file or the line changed, 200 files at most; `Export bookmarks
  and notes...` writes the lines with their numbers in a code block, the notes as `//`
  comments above them
- **New: `Tools > Analyze Log...`**, in the background:
  - Durations between a start and an end pattern, paired by their groups: percentiles,
    the slowest pairs, the starts never ended
  - Rules of known problems in a JSON file (match, sequence, absence, count, within a
    time, tied by their groups), edited in a form (`Edit...`, `New...`, `Add as rule...`
    in the log views), with a note whose web addresses are links; `Example...`
  - Compare two tabs (the whole file, the filtered view or the selection) by the
    templates of the lines, or line by line in two columns
- **New: merge files** (`Merge N Files...` in the file panel, `File > Merge Files...`):
  the files one after the other in a new file, in the order of the file panel or another
- The encoding moved to the tool tip of the file size, leaving room in the tool bar
- Bookmarks go from 8 to 5 numbers: `Ctrl+1-5` sets them, `1-5` / `Shift+1-5` jump to the
  next / previous line with one
- New default shortcuts (shortcuts changed in Preferences are kept):
  - `Alt+1-9` highlights the selection with a color (was `Ctrl+Shift+1-9`)
  - The visibility, case, regex, inverse, combining and auto refresh toggles have no key
    any more (were `1-8`, now the bookmarks)
  - `Shift+F` adds the selection as a filter
  - `End` goes to the end of the line, `Shift+End` to the right of the text; `$`, `^`,
    `*` and `/` are no longer bound
  - `Ctrl+R` reloads (was `F5`), `Ctrl+.` also stops loading
  - Find next / previous in the log views is `Ctrl+G`, `N` / `Ctrl+Shift+G`, `Shift+N`
    (no `F3` / `Shift+F3`)
  - Paste from the clipboard also on `Ctrl+Y` (macOS: `Control+Y`), delete in the filter
    panel also on `Ctrl+D` (macOS: `Control+D`)

## 1.3.1 (2026-11-20)

- **New: search and replace in a folder** (`Search in Folder...` in the right click menu of
  a folder of the file panel, the magnifier at its top or the File menu): the files of the
  folder are searched in the background, the matching lines listed under the tree
  - Double click a result to open the log view at its line; the right click menu opens it in
    edit mode instead, copies it or removes it from the results
  - `Open as Log` opens every matching line (not only the ones listed) as `file:line:text`
    in a tab, to filter like any log
  - `Replace` replaces the selected results (all of a selected file), `Replace All` every
    file after asking; files are written back in their encoding, byte order mark and line
    ends, files with changes not saved in edit mode are left alone; `\1` in the replacement
    with a regular expression
- The find bars of edit and render mode have the arrows of the filter panel
- An empty tab area says "Open a file or drop it here"

## 1.3.0 (2026-11-20)

- **New: edit mode** (the pencil at the right of the tool bar, `View > Edit mode` or
  `Ctrl+Shift+E`): the file in a text editor (Scintilla, as in Notepad++), at the line of
  the log view; the other modes read the file on disk, again once it is saved
  - Loaded in the background (1.2 s for 1.8 GB) and held in memory, about the size of the
    file; a file larger than half of the memory available asks first; freed when the mode
    is left
  - `Ctrl+S` saves in the background, atomically (a temporary file renamed over it, its
    permissions kept), in the encoding, byte order mark and line ends of the file; a file
    that is not saved unchanged (bytes its encoding can not read) or a character the
    encoding can not write is told before
  - Undo, multiple and rectangular selections, input methods; `Ctrl+F` finds and replaces
    (match case, regular expression with `\1` in the replacement, replace all as one undo),
    the matches around the lines shown on a yellow background
  - Leaving the mode, closing the tab or the window asks to save the changes; the tab shows
    `*` while they are not saved; a change of the file on disk offers to reload it
  - Copies made by flogg (archives, downloads, the clipboard) and files that can not be
    written are read only, `Save as` writes them elsewhere
- **New: render mode** (the eye at the right of the tool bar, `View > Render mode` or
  `Ctrl+Shift+V`): the tab shows the file rendered instead of its lines, the filter panel
  stays
  - Markdown (tables, task lists, code blocks) and HTML as a document, laid out like GitHub:
    the alerts (`> [!NOTE]`), the footnotes, `<details>` (shown open), the images fitting the
    page; `Open in browser` shows mermaid diagrams, math and code highlighting with libraries
    from cdn.jsdelivr.net
  - JSON (also `.har` network logs and `.ipynb` notebooks) and XML as a tree: right click to
    copy a value or its JSON path; an error gives its line and `Show in log view` goes there
  - CSV, TSV and JSON Lines as a table of any size, read from the log as it is scrolled and
    following the file; the separator of a CSV file is guessed, the first line can be the
    header; `Ctrl+C` copies the selected cells
  - `Ctrl+F` opens a find bar in the render mode (match case, regular expression; `Enter`/`F3`
    for the next match, `Shift+Enter`/`Shift+F3` for the previous one), alike in documents,
    trees and tables: every occurrence of the text shown is a match, counted (`3 / 120`), the
    current one is shown like selected text and the others on a yellow background; the find
    starts from where the user is (the cursor, the selected node or cell) and takes the text
    selected; a tree is expanded and a narrow column widened and scrolled to the match
  - The lines of a table are searched like the search line and the matches counted in the
    background (2 s for 30 million lines, `120+` meanwhile); a regular expression with
    anchors or looking around is matched cell by cell; a line matching only outside of its
    cells (the keys of JSON Lines) is left out
  - A cell of a tree or a table opens (double click, `F2`) to select a part of its text;
    rows are selected by their header
  - Files of these formats (by their extension) open rendered, the eye goes back to the
    lines; the format of another file comes from its content and can be changed in the list
    of the view; the tab is restored in the render mode
  - Links of a rendered document open the files in tabs of flogg, rendered, at the section
    of the link (`other.md#section`, `#L120` for a line); web links go to the browser
  - Markdown and JSON are parsed in the background up to a size set in
    `Options > Advanced` (20 MiB for JSON and XML, a quarter for Markdown and HTML)

## 1.2.1 (2026-11-16)

- Buttons at the right of the tool bar show and hide the file tree and the filter panel; the
  panels show their icon in their title bar, and the Tools menu items have icons
- The modification time of the file moved to the tool tip of its size, to make room
- A panel moved to another side (or floated) stays there in the windows opened next, not only
  after the last window is closed

## 1.2.0 (2026-11-15)

- **New: file tree panel** (`File > Open Folder...`, or drop a folder on the window or on the
  panel): browse a folder beside the tabs and open its files with a double click, `Enter` or by
  dragging them to the log views
  - Each window has its own folder, kept with the session; recent folders, parent folder and
    `Set as Root Folder` to move around
  - Filter the file names (`*.log`, several patterns separated by spaces or `;`, a text
    without `*` matches a part of the name); folders without a matching file are hidden
  - Files open in a tab are shown in bold, the file of the current tab is selected
  - Sort by name, date modified, size or type; show hidden files (`Ctrl+Shift+.`)
  - Right click: reveal in the file manager, copy the path / relative path / file name,
    rename (`F2`) and move to the trash; files open in a tab are not renamed or removed
  - Hovering a file shows its size and modification time
  - `Show/hide file tree panel` and `Open folder in the file tree` can be given a shortcut
- Tabs move between windows without loading the file again, with their filters: drag a tab
  to another window, or out of every window for a new one, or use `Move to new window` /
  `Move to window` in the tab menu
- Tool tips next to a large mouse cursor no longer appear under it

## 1.1.13 (2026-11-13)

- A window maximized on a second screen restores there after it is minimized (or hidden to the
  tray) while running, not only after a restart

## 1.1.12 (2026-11-12)

- A window maximized on a second screen opens there again, and restores from minimized there,
  instead of moving back to the main screen

## 1.1.11 (2026-11-11)

- `Check for updates...` shows the changes of every version since the installed one, not only
  the latest
- The bookmark keys (`Ctrl+1-8`, `Alt+1-8`, `Shift+Alt+1-8`) also work in the filter panel: like
  `F8` they use the line selected in the filtered view, and the focus stays in the panel
- The split between the main and filtered views is remembered: the last position it was dragged
  to is used by every tab opened after, without `Save splitter position`, also for files opened
  before (their own position is no longer restored); the splitter is a bit easier to catch

## 1.1.10 (2026-11-11)

- `Check for updates...` has a red dot while a newer version found at start is not installed,
  also after `Later` or `Skip this version`
- The window title shows the version like the release tags, e.g. `(v1.1.9)` instead of
  `(build 1.1.9)`

## 1.1.9 (2026-11-11)

- New `Preferences > Filter files` tab: rules that give a filter file to the log files whose name
  matches wildcard patterns (e.g. `*.syslog; messages*`, or `*/nginx/*.log` for a path), so such
  files open with their filters without loading them each time; the first matching rule wins,
  other files open with the filter file last opened as before. The `?` button explains the
  patterns with examples

## 1.1.8 (2026-11-06)

- Linux: flogg starts on every x86-64 CPU; the package no longer depends on the CPU of the machine
  that built it, 1.1.7 crashed at start on CPUs without AVX-512
- Windows: `Open containing folder` shows the right folder and selects the file when its path
  holds a space; if the file is gone, it opens the folder the file was in

## 1.1.7 (2026-11-06)

- New `Start in Filter mode` option in `Preferences > General`: when off, a new tab and an empty
  search no longer enter Filter mode, the filter panel only highlights the lines as in klogg
- Shift + mouse wheel scrolls sideways in the log views, the filter panel and every other list
  with a horizontal scroll bar; the log views move twice as far per step sideways
- The list of filters shown when hovering a line has the right numbers right after a filter was
  moved or added
- An empty search removes the highlighted matches of the previous search
- Linux: on Wayland flogg runs through XWayland, so a dragged filter panel follows the mouse;
  `QT_QPA_PLATFORM=wayland` keeps it on Wayland

## 1.1.6 (2026-11-04)

- Icon easier to see on the Windows taskbar: a dark tile again, brighter log lines and blade,
  and three thicker log lines below 30 px
- Windows: the scroll bar handle no longer shows black or broken corners around it, also while
  it is dragged

## 1.1.5 (2026-11-04)

- New icon look: a light slate tile filling the whole square (it no longer sinks into a dark
  taskbar), a shorter and bluer blade, thinner log lines that all cross it without touching it,
  the filter match line in amber

## 1.1.4 (2026-11-04)

- Update downloads that lose the connection go on from where they stopped (some proxies cut the
  start of a download but let the rest through), then try HTTP/1.1 and TLS 1.2, then the curl
  program of the system; when all fail, flogg offers to open the download page

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
