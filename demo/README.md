# flogg 分析功能展示

這個資料夾的檔案用來實際體驗 flogg 的分析功能：每個檔案都埋了特定的「問題」，照著下面的步驟操作，就能看到每個功能能幫你省下什麼時間。

```sh
python3 demo/make_demo.py               # 產生（或重新產生）展示檔，內容每次都一樣
python3 demo/make_demo.py --big 3000000 # 另外產生約 300 MB 的 big-app.log，體驗大檔速度
```

| 檔案 | 內容 | 適合展示 |
| --- | --- | --- |
| `app-good.log` | 支付服務正常的一天（2026-11-21） | 比較的基準 |
| `app-bad.log` | 同一服務出事的一天（2026-11-22）：資料庫斷線、circuit breaker 打開、重試風暴、OOM 當機、request 卡住、NTP 把時鐘調回 30 秒 | 時間差、耗時、規則、比較 |
| `device-boot.log` | 開機 log：每行同時有「開機後秒數」與牆鐘；牆鐘在 NTP 同步時從 2000 年跳到 2026 年 | 時間差優先用開機秒數 |
| `logcat.txt` | Android logcat（沒有年份），跨年夜啟動 App、一個 ANR | 沒有年份的格式、跨年、App 啟動耗時 |
| `rotated/` | 被輪替成三份的 log：`service.log.2` 最舊、`service.log` 最新；`.1` 結尾沒換行、`.2` 開頭有 BOM | 合併檔案 |
| `rules.json` | 針對上面 log 寫的 8 條規則 | 規則庫 |

建議先開兩個分頁：`app-good.log` 與 `app-bad.log`（以下的行號都是 `app-bad.log` 的）。

---

## 1. 行與行的時間差：選兩行就知道隔多久

**操作**

1. 在 `app-bad.log` 點第 1421 行（第一個 `db error`），再按住 `Ctrl`（macOS：`Cmd`）點第 1477 行（`FATAL EXCEPTION`）。
2. 看工具列右側：`Δ +18.794 s`，滑鼠停上去會看到兩行的時間與格式。
3. 再試：點第 1477 行，`Ctrl`＋點第 1478 行（stack trace 的 `at com.pay...`）→ 工具列顯示 `Δ: No time stamp in the last line`。
4. 跨過時鐘調整：點第 1505 行，`Shift`＋點第 1525 行 → `Δ -29.179 s ⚠`，tooltip 寫著 `The clock goes back 29.929 s at line 1516`，告訴你這個數字不可信、原因在哪一行。
5. 開 `device-boot.log`，點第 1 行再 `Shift`＋點最後一行 → `Δ +7.948472 s`（開機秒數），不是牆鐘算出的 26 年。
6. 開 `logcat.txt`，點第 1 行（12-31 23:59:50）再 `Shift`＋點最後一行（已經是 01-01）→ `Δ +19.974 s`，tooltip 註明 log 沒有年份、視為跨了新年。

**強在哪**

- 以前要自己看兩行時間心算，跨分鐘、跨午夜、跨年都很容易算錯；現在選取就有答案。
- **每一行各自解析**，不靠整個檔案的格式偵測：多執行緒輸出順序亂掉、檔案中途換格式都不影響。
- **時間亂掉時會提醒**：中間最多 10 萬行會在背景檢查時鐘是否倒退（例如 NTP 校時），倒退就標 `⚠` 並指出在哪一行，不會給你一個錯得很有自信的數字。
- 有開機秒數（kernel、dmesg）時優先用它，校時造成的牆鐘跳動完全不受影響。
- 格式不同的兩行不硬算，直接說明原因。

---

## 2. 備註：調查過程直接記在 log 上

**操作**

1. 在第 1421 行按右鍵 → `Add note...`（或按 `Shift+M`），輸入「ledger-db 拒絕連線，問題起點」。
2. 第 1477 行加備註「OOM，可能是重試堆積造成」，再按 `Ctrl+1` 加書籤 1。
3. 書籤欄最右邊會出現黃色的備註圖示；滑鼠停在該行，tooltip 最上方就是你的備註。
4. 關掉分頁再打開 `app-bad.log`：備註和書籤都還在。
5. 右鍵 → `Export bookmarks and notes...` → 得到一份 Markdown，每個書籤或備註都附上原始 log 行，可以直接貼進 bug 單。

**強在哪**

- 調查到一半被打斷，回來時線索都還在；匯出後就是現成的事件紀錄。
- **不會產生垃圾檔**：全部存在 flogg 自己的一個 `notes.json`，log 資料夾裡不會多出檔案。檔案被輪替或覆寫、某行內容改變時，對不上的備註會自動清掉；只保留最近使用的 200 個檔案。

---

## 3. 成對事件耗時：哪個 request 最慢、哪個沒結束

**操作**：`Tools > Analyze Log...` → `Durations` 分頁，在 `app-bad.log` 分頁上執行：

| 欄位 | 填入 |
| --- | --- |
| Start | `request begin id=(\d+)` |
| End | `request end id=(\d+)` |

按 `Measure`：

- 摘要列出 pairs 數、平均、中位數、90%、99%、最大、最小。
- 共約 1100 對，中位數約 172 ms；表格最上面是最慢的 request（最慢 4.766 s，約 2.5–4.8 s 的都在事故之後）。
- 表格最下面是 3 筆紅色的 `no end`：送出後一直沒有回應的 request。
- **雙擊任一筆**：log 會選取開始到結束的行，工具列直接顯示兩者的時間差，前後文一目了然。

再試 `logcat.txt`：Start `START u0 \{cmp=com\.demo/\.(\w+)\}`，End `Displayed com\.demo/\.(\w+)` → 一眼看出 `PayActivity` 有一次啟動花了 4.2 秒。

**強在哪**

- 括號群組自動配對（同一個 id 的開始與結束），交錯、亂序的併發 request 也能正確配對。
- 搜尋和 filter 共用同一套多 pattern 引擎，一次掃完整個檔案，只回頭讀命中的行。用 `--big` 產生的大檔一樣能跑。
- 不只算平均，還有百分位與「沒結束的」：效能問題和卡住的請求同時浮現。

---

## 4. 問題特徵規則庫：把「看過的問題」變成一鍵檢查

**操作**：`Durations` 旁邊的 `Rules` 分頁 → `Open...` 選 `demo/rules.json` → `Check`。在 `app-bad.log` 上會找到：

| 規則 | 類型 | 找到什麼 |
| --- | --- | --- |
| Crash | match | 第 1477 行的 OOM |
| Database connection never established | absence | 開始連線後 5 秒內沒有 `db connected` |
| Request never answered | absence | 3 個 10 秒內沒有回應的 request |
| Login then logout within 1 s | sequence | 事故後幾個使用者登入後 1 秒內就登出（同一個 user） |
| Retry storm | count | `r77` 在 60 秒內重試 5 次 |
| Database errors open the circuit breaker | sequence | `db error` 之後 30 秒內 breaker 打開；雙擊規則可開 runbook 連結 |
| Clock set | match | NTP 調整時鐘的那一行 |

在 `app-good.log` 上跑同一份規則，什麼都沒找到：規則只對真正的問題起反應。在 `logcat.txt` 上則會找到 ANR。

**強在哪**

- 4 種規則涵蓋常見的「問題長相」：**某行出現**、**依序出現**、**該出現卻沒出現**、**短時間內重複太多次**。
- 括號群組把同一件事的行綁在一起（同一個連線、同一個使用者），不會把 A 連線的開始和 B 連線的成功誤配。
- 規則檔是 JSON，可以像 filter 檔一樣在團隊間分享：資深同事踩過的坑，新人一鍵就能檢查。
- `New from example...` 會產生每種規則各一條的範本，照著改就好。

---

## 5. 比較：好的 log 和壞的 log 差在哪

**操作**：在 `app-bad.log` 分頁開 `Tools > Analyze Log...` → `Compare`，`With:` 選 `app-good.log`，兩邊都選 `Whole file` → `Compare templates`：

- 最上面是 **only there**（只在好 log）：`circuit breaker CLOSED for service=<*>`，代表壞 log 裡 breaker 一直沒恢復。
- 接著是 **only here**（只出現在壞 log）：`retry request=<*> attempt=<*>`（5 次）、`db error: connection refused host=<*>`、`FATAL EXCEPTION...`、`clock adjusted...`、`circuit breaker OPEN...`，以及 stack trace 的每一行。整天幾千行裡，事故相關的事件全部浮到最上面。
- 下拉選單選 `Fewer there`：`cache miss` 在好 log 比例低得多（壞 log 121 次、好 log 53 次），也就是事故後快取失效變嚴重。
- 雙擊任一列，會跳到該模板第一次出現的位置（點 `There` 欄就跳到另一個分頁）。

換成 `Compare line by line`，並在兩個分頁各選一段相近的行（`Selected lines`），會看到像 diff 的逐行比較：時間與數字不同不算差異，只標出真正多出或少掉的行。

**強在哪**

- 兩份幾千、幾百萬行的 log，人眼不可能逐行比；**模板化**之後，時間戳、id、數字不同的同類行會歸成一類，差異只剩真正「新出現」或「消失」的事件。
- 用比例而不是次數判斷變多變少，兩份 log 長度不同也公平。
- 貼上的文字也能比：先用 `File > Paste from clipboard` 貼成一個分頁，再選它。

---

## 6. 合併檔案：把輪替的 log 接回一份

**操作**

1. `File > Open Folder...` 開啟 `demo/rotated`，在 file panel 選取三個檔案 → 右鍵 `Merge 3 Files...`。
2. 預設順序跟 file panel 一樣；依名稱排序時是 `service.log`、`.1`、`.2`，時間反了。
3. 選 `service.log.2` 按 `Up` 兩次，或直接拖曳，排成 `.2`、`.1`、`service.log`。或者先把 file panel 排序改成 `Oldest first`（`⇅` 按鈕），再開合併視窗，順序就直接是對的。
4. `Merge and Open` → 合併後的檔案自動在新分頁打開：18 行、時間連續；`.1` 結尾缺的換行已補上，`.2` 的 BOM 沒有跑到檔案中間。勾選 `A line with the file name before each file` 則會在每段前加上 `==> 檔名 <==`。

**強在哪**

- 順序直接沿用 file panel 的排序（名稱、時間、大小），再用拖曳微調。
- 處理了手動 `cat` 容易出的錯：缺換行導致兩個檔案的行黏在一起、BOM 出現在檔案中間、輸出檔剛好是輸入檔之一。
- 背景以 1 MB 區塊寫入暫存檔，完成才改名；中途失敗或按停止都不會留下半個檔案。

---

## 大檔體驗（選用）

```sh
python3 demo/make_demo.py --big 3000000
```

開啟 `demo/big-app.log`（約 300 萬行）：

- 用 `Durations`（`request begin id=(\d+)` / `request end id=(\d+)`）：大部分 request 在 300 ms 內，約萬分之五花了 5 秒，全部排在表格最上面。
- 用 `Compare templates`（With 選 `(nothing: the templates of this tab)`）列出所有模板，最罕見的排最前面：只佔萬分之一的 `FATAL EXCEPTION` 馬上浮到最上面。

在 1.8 GB、3000 萬行的檔案上，模板分析約 2 秒；耗時分析在 pattern 命中約 2500 萬行的最壞情況下約 8.5 秒。
