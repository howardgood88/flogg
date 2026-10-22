# Benchmark

用來量測 flogg 在大檔案上的搜尋與 filter 效能。

## 測試檔案

| 檔案 | 大小 | 行數 |
|---|---|---|
| `bench/big.log` | 1.8 GB | 3,000 萬行 |
| `bench/mid.log` | 308 MB | 500 萬行 |

兩個 `.log` 都已加入 `.gitignore`，不會進 git。檔案不見時，可以重新產生（亂數種子固定，內容每次都一樣，大約需要 1 分鐘）：

```sh
python3 bench/gen_bench_log.py            # 輸出到 bench/
python3 bench/gen_bench_log.py /some/dir  # 輸出到指定目錄
```

每一行的格式：

```
2026-10-22 10:00:00.003 INFO  [wlan1] tid=7955 crash in module 9156
```

## 建置

benchmark 寫在 UI 測試執行檔 `klogg_itests` 裡，需要用 `-DBUILD_TESTS=ON` 建置（完整的 cmake 參數見 `BUILD.md` 和專案的建置說明）：

```sh
cd build_root
ninja klogg_itests
```

## 執行

benchmark 是隱藏的 test case（tag 為 `[.benchmark]`），一般跑測試時不會執行，要明確指定：

```sh
cd build_root
FLOGG_BENCH_FILE=../bench/big.log ./output/klogg_itests -platform offscreen "[.benchmark]"
```

想快速試跑，可以改用 `mid.log`。

### 環境變數

| 變數 | 說明 |
|---|---|
| `FLOGG_BENCH_FILE` | **必填**。要測試的 log 檔。沒設定的話，測試只會印出警告就結束。 |
| `FLOGG_BENCH_SKIP_BASELINE` | 設成任意值，就會跳過「klogg 原生 boolean 搜尋」這項對照測試，節省時間。 |
| `FLOGG_BENCH_BLOCK_LINES` | 覆寫搜尋時每個區塊讀取的行數下限（對應設定 `searchReadBufferSizeLines`），用來實驗不同的區塊大小。 |

## 輸出說明

結果用 Catch2 的 `warning` 印出，每一項是一行：

| 項目 | 量測內容 |
|---|---|
| `Indexing N lines` | 開檔並建立行索引 |
| `klogg boolean search, 30 patterns` | 對照組：用 klogg 原本的 boolean 搜尋，把 30 條 filter 組成一個表達式來搜 |
| `klogg single pattern search` | 對照組：klogg 一般的單一 pattern 搜尋 |
| `Single filter search` | 只有 1 條 filter 的 filter 搜尋 |
| `First filter search, 30 filters` | 30 條 filter 第一次掃描（快取是空的，要掃整個檔案） |
| `Toggle exclude filter off` | 關掉 exclude filter：只重新組合快取裡的 bitmap，不讀檔 |
| `Toggle 10 include filters off` | 關掉 10 條 include filter：同上 |
| `Toggle everything back on` | 全部重新打開，並確認結果和第一次掃描一致 |
| `Edit one filter (rescan 1 pattern)` | 修改 1 條 filter：只重掃那一條 pattern |

測試會檢查 filter 搜尋的命中數和 klogg 原生搜尋一致；不一致的話，測試會失敗。

## 參考結果

環境：Apple Silicon Mac，`big.log`，RelWithDebInfo 建置（2026-10-22）。

| 項目 | 結果 |
|---|---|
| 建立索引 | 約 0.7 秒 |
| klogg 原生 boolean 搜尋（30 個 pattern） | 1.5 秒（修正讀檔瓶頸前是 78 秒） |
| klogg 原生單一 pattern 搜尋 | 0.5 秒 |
| 30 條 filter 第一次掃描 | 1.4 秒（修正前是 132 秒），22,495,530 筆命中 |
| 開關 filter | 4～14 ms |
| 修改 1 條 filter | 約 0.5 秒 |
