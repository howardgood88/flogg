# flogg

[English](README.md) | **繁體中文**

flogg 是以 [klogg](https://github.com/variar/klogg) 為基礎的 log 檢視器。它保留
klogg 快速的檔案讀取與搜尋，並以 [TextAnalysisTool](https://textanalysistool.github.io/)
風格的 filter panel 取代原本的搜尋列操作方式：由一組 filter 同時決定哪些行出現在
過濾檢視中，以及主檢視的著色方式。

![顯示 filter panel 的 flogg](docs/images/filter-panel.png)

Windows（x64）與 macOS（Apple Silicon）的安裝檔請到
[Releases 頁面](https://github.com/howardgood88/flogg/releases/latest)下載。

## 功能

### Filter panel

- Filter 以可巢狀的群組樹狀結構管理。
- 每個 filter 可設定 pattern（純文字或正規表示式）、大小寫敏感、排除（exclude），
  以及前景／背景顏色，顏色可套用在整行或只套用在符合的文字。
- 啟用中的 filter 同時決定過濾檢視的內容與主檢視的著色。
- 可個別、整個群組或一次全部啟用／停用 filter；也可以暫時 suspend，不會改動原本的
  啟用狀態。
- Filter 可以有多行說明（清單中顯示第一行，tool tip 顯示全部）。
- 可用拖放或移動按鈕調整順序，也能移入或移出群組。
- 各欄寬度可獨立調整，欄寬與顯示的欄位會被記住；在欄位標題按右鍵可選擇要顯示的欄位。
- 可從選取的文字或搜尋框內容建立 filter。
- Hits 欄顯示每個 filter 的命中行數；群組顯示其中所有已啟用 filter 的命中數總和。
- 每個 filter 依優先順序有一個編號（`#` 欄，可和其他欄位一樣隱藏）；滑鼠停在 log
  檢視的某一行時，會依優先順序列出命中該行的 filter 與其編號。
- `Light` style（Preferences > View > Style）不論系統外觀都維持淺色；`Dark` 則維持深色。

### 快速過濾

- 每個 filter 保有自己的結果 bitmap：切換 filter 只需重新組合快取結果，編輯 filter
  也只會重新掃描該 pattern。
- 搜尋流程使用更大的讀取區塊，且不再以 sleep 輪詢。

### 書籤與檢視

- 編號 1-8 的書籤，每個編號可標記任意多行，並可逐一跳到下一個／上一個。書籤欄只在
  檔案有書籤時顯示。
- 可隱藏主 log 檢視，讓過濾檢視佔滿整個視窗。
- 所有新增的操作都可自訂快捷鍵，見[快捷鍵](#快捷鍵)。

### Filter 檔

- Filter 存成 JSON 格式的 filter 檔（filter panel 的 File 選單）。
- 可匯入 TextAnalysisTool 的 `.tat` 檔。
- 可匯入 klogg 的 predefined filters 與 highlighters。

### 命令列

- `--config <file>`：設定存在指定的檔案中（session 存在同目錄的
  `<name>_session.conf`）。
- `--filters <file>`：載入 filter 檔，但不把它設為預設 filter 檔。
- 搭配 `-m` 可開啟多個視窗，各自使用自己的設定與 filter。
- Windows 版的設定與預設 filter 檔放在 `flogg.exe` 旁邊，整個資料夾可攜帶使用。

### 相較 klogg 移除的功能

Predefined filters、highlighter sets、搜尋結果高亮、清空檔案、版本檢查與 Help 選單
連結，已由 filter panel 取代或移除。

## 快捷鍵

以下是 flogg 新增的快捷鍵，全部都可以在 Preferences > Shortcuts 中修改。
macOS 上 `Ctrl` 是 Command 鍵、`Alt` 是 Option 鍵；唯一例外是「顯示／隱藏主 log
檢視」使用 Control 鍵（因為 Command+H 會隱藏整個應用程式）。

### 檢視

| 操作 | 快捷鍵 | 其他位置 |
| --- | --- | --- |
| 顯示／隱藏主 log 檢視 | `Ctrl+H`（macOS：`Control+H`） | View 選單 |
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
