# flogg

flogg is a fork of [klogg](https://github.com/variar/klogg) that adds a TextAnalysisTool
style filter panel. klogg's main strength is speed on very large files (GB size, tens of
millions of lines); flogg must keep it.

## Performance rule

Every new feature or change must be evaluated for its impact on klogg's native performance
before it is implemented, and the evaluation must be stated when the work is handed over.

Hot paths to protect:

- File loading and indexing (`src/logdata`: `LogData`, `LogDataWorker`)
- Searching and filtering (`LogFilteredData`, `LogFilteredDataWorker`, the per filter
  result bitmaps and their recombination)
- Drawing the log views (`AbstractLogView::paintEvent`, highlighters, wrapping),
  scrolling and following the file
- Quick find (`QuickFind`)

Guidelines:

- Work that depends on the file size must not run on the UI thread; prefer the cached
  per filter results over reading the file again.
- Per line work in painting or searching must stay constant time; no allocations, regex
  compilations or lookups that grow with the number of filters per drawn line unless
  they are cached.
- Features triggered by the user (a shortcut, a tool tip, a menu) may do bounded work on
  demand, but must not add cost to loading, searching or painting when unused.
- When a change touches a hot path, measure it with the benchmark
  (`FLOGG_BENCH_FILE=<log> klogg_itests "[.benchmark]"`, logs from
  `python3 bench/gen_bench_log.py`, see `bench/README.md`) before and after.
