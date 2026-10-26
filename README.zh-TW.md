# flogg

[English](README.md) | **繁體中文**

flogg 是以 [klogg](https://github.com/variar/klogg) 為基礎的 log 檢視器。它保留了 klogg 快速的檔案讀取與搜尋，並加入客製化程度更高的 [TextAnalysisTool](https://textanalysistool.github.io/) 風格 filter panel。

![顯示 filter panel 的 flogg](docs/images/filter-panel.png)

Windows（x64）與 macOS（Apple Silicon）的安裝檔請到 [Releases 頁面](https://github.com/howardgood88/flogg/releases/latest)下載。

## 功能

### Filter panel：TAT 風格，更方便的 filter 管理器

- Filter 支援巢狀的群組樹狀結構管理。
- 每個 filter 可設定 pattern（text 或 regex）、大小寫敏感、排除、前景／背景顏色，顏色可套用在整行或只套用在符合的文字。
- 可個別、整個群組或一次全部啟用／停用 filter；也可以暫時 suspend（不會改動原本的啟用狀態）。
- 選中 filter 後，在主視窗或過濾視窗按 `F8` / `Shift+F8` 在命中行間跳轉。
- 滑鼠停在 log 檢視的某一行時，會依優先順序列出命中該行的 filter 與其編號。
- 在欄位標題按右鍵可選擇要顯示的欄位，各欄寬度可個別調整。

### 快速過濾：與 klogg 同樣高效的搜尋與過濾

- Hyperscan 本身即支援多 pattern 同時掃描，但 klogg 一次搜尋只產生一份結果；flogg 將其擴充為掃描一次檔案就同時得到每個 filter 各自的結果。
- 每個 filter 維護自己的搜尋結果 Roaring Bitmap（沿用自 klogg），可自動根據命中稠密度調整資料結構，以壓縮所需記憶體空間。
- 最終命中結果為 Roaring Bitmap 彼此做聯集 / 差集，啟用 / 停用 / 暫停 filter 只需重新計算 bitmap 組合結果，不需要重新掃描，新增 / 編輯 filter 也只會重新掃描該 pattern。

### 書籤與檢視

- Filter Panel 與 klogg 原生 pattern 輸入框互斥，可透過過濾視窗上方搜尋列最左邊的 `Filters` 按鈕切換。
- 主視窗與過濾視窗支援 `Ctrl+1-8` 標記書籤，`Alt+1-8` / `Shift+Alt+1-8` 可跳到下一個 / 上一個對應書籤行。
  (建議至 `File > Preferences > Shortcuts` 修改按鍵設定，flogg 預設保留 klogg 原生 shortcuts)
- `Ctrl+H`（macOS：`Control+H`）可隱藏主視窗，只顯示過濾視窗。

### Filter / conf 檔

- Filter 的每次修改都會自動存到目前使用的 JSON filter 檔，預設為 `filters.flogg-filters.json`（位置見下方[命令列](#命令列)）。
- 支援匯入 TextAnalysisTool 的 `.tat` 檔。
- 支援匯入 klogg 的 predefined filters 與 highlighters。
- 應用相關設定（UI、shortcuts 等等）預設儲存於 `flogg.conf`（位置見下方[命令列](#命令列)）。

### 命令列

- `--config <file>`：改用指定的設定檔讀寫設定，不影響預設的 `flogg.conf`；對應 session 存在同目錄的 `<name>_session.conf`。
- `--filters <file>`：以指定的 filter 檔開啟 flogg，但不把它設為預設 filter 檔。
- 搭配 `-m` 可開啟多個視窗，各自使用自己的設定與 filter。
- Windows 版的設定（`flogg.conf`）與預設 filter 檔放在 `flogg.exe` 旁邊，整個資料夾為 portable。
- macOS 版依照系統慣例存放：設定在 `~/Library/Preferences/com.flogg.flogg.plist`，預設 filter 檔在
  `~/Library/Application Support/flogg/filters.flogg-filters.json`；使用 `--config` 時，預設 filter 檔改放在該設定檔旁邊。

### 相較 klogg 移除的功能

Predefined filters、highlighter sets、搜尋結果高亮、清空檔案、版本檢查與 Help 選單連結，已由 filter panel 取代或移除。

## 快捷鍵

以下是 flogg 新增的快捷鍵，全部都可以在 File > Preferences > Shortcuts 中修改。

### 檢視

| 操作 | 快捷鍵 | 其他位置 |
| --- | --- | --- |
| 顯示／隱藏主視窗檢視 | `Ctrl+H`（macOS：`Control+H`） | View 選單 |
| 顯示／隱藏 filter panel | 預設無 | View 與 Tools 選單、panel 右上角的關閉按鈕 |

### Filter panel

以下快捷鍵在 filter panel 取得焦點時有效。

| 操作 | 快捷鍵 | 滑鼠 |
| --- | --- | --- |
| 新增 filter | `Insert` | `+ Filter` 按鈕、右鍵選單 |
| 新增群組 | | `+ Group` 按鈕、右鍵選單 |
| 編輯 filter／重新命名群組 | `F2`、`Enter` | 雙擊 |
| 刪除選取項目 | `Delete` | 右鍵選單 |
| 上移／下移（也可移入、移出群組） | `Alt+Up`／`Alt+Down` | 箭頭按鈕、拖放 |
| 啟用／停用單一 filter | | 勾選框 |
| 啟用／停用（群組內）、全部啟用、全部停用 | | 右鍵選單 |
| Suspend／Resume、全部 Suspend／全部 Resume | | 右鍵選單 |
| 加入搜尋、從搜尋排除、取代搜尋 | | 右鍵選單 |
| 選擇要顯示的欄位 | | 在欄位標題按右鍵 |
| 開啟、儲存、匯入 filter 檔 | | `File` 按鈕 |

### Log 檢視

| 操作 | 快捷鍵 | 滑鼠 |
| --- | --- | --- |
| 跳到選取 filter 的下一個符合行（循環搜尋） | `F8` | Filter panel 右鍵選單 |
| 跳到選取 filter 的上一個符合行（循環搜尋） | `Shift+F8` | Filter panel 右鍵選單 |
| 在選取的行設定／移除書籤 N（1-8） | `Ctrl+1` … `Ctrl+8` | |
| 跳到下一個有書籤 N 的行 | `Alt+1` … `Alt+8` | |
| 跳到上一個有書籤 N 的行 | `Alt+Shift+1` … `Alt+Shift+8` | |
| 將選取文字加為 filter | 預設無 | Log 檢視右鍵選單的 `Add as filter...` |
| 將搜尋文字加為 filter | | 搜尋框右鍵選單的 `Add as filter...` |

## 建置

flogg 的建置方式與 klogg 相同，請見 [BUILD.md](BUILD.md)。CI 建置設定在
[.github/workflows/flogg-build.yml](.github/workflows/flogg-build.yml)，
[bench/README.md](bench/README.md) 說明效能測試用的 log 產生器。

## 致謝與授權

flogg fork 自 Anton Filimonov 的 [klogg](https://github.com/variar/klogg)，而 klogg
最初 fork 自 Nicolas Bonnefon 的 [glogg](https://github.com/nickbnf/glogg)。log 讀取、
搜尋引擎與大部分 UI 都來自 klogg，相關文件與歷史請參考 klogg 的 repository。

採用 GPLv3 或更新版本授權，見 [COPYING](COPYING)。
