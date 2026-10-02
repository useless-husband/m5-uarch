# 參與貢獻

[English](CONTRIBUTING.md)

## 送出你的 Mac 量測結果

任何一台 Apple Silicon Mac 都可以。特別歡迎 M5 以外的晶片，也歡迎第二台 M5：一顆晶片要有兩份互相獨立、而且結果一致的提交，[結果網站](https://useless-husband.github.io/m5-uarch/)上才會標成「verified（已驗證）」。

你需要 Xcode 命令列工具（如果沒有 `cc`，執行 `xcode-select --install`；裡面就有 `make`、`git`、`python3`）和一個 GitHub 帳號。不需要 `sudo`，也不需要會用 git。

### 只用 GitHub 帳號

1. 取得程式碼：`git clone https://github.com/useless-husband/m5-uarch`，或在 GitHub 頁面按
   **Code → Download ZIP** 再解壓縮。在那個資料夾打開「終端機」。
2. 能關的程式先關掉，筆電請插電。然後執行：

   ```sh
   make submit
   ```

   它會編譯工具、量三次（一兩分鐘；如果你之前跑過 `make measure`，就直接沿用），把結果打包、複製到剪貼簿，並在瀏覽器打開這個專案的「新增 issue」頁面。
3. 點一下 **Submission** 欄位，貼上（Cmd-V），勾選授權那一格，按 **Create**。
4. 幾分鐘內機器人會在 issue 底下回覆：**accepted（接受）**、**flagged（標記待審）** 或 **rejected（退回）**，每一項檢查都附理由。接受和標記待審的資料會被放進一個 pull request；維護者合併之後，網站就會出現你的晶片。

如果被退回，留言會說明原因。修正之後再執行一次 `make submit`，然後編輯原本的 issue，把舊的那段文字換成新的：每次編輯都會重新檢查。

如果瀏覽器沒有自動打開，終端機裡有印出連結，要貼的文字在 `build/submission/submission.txt`。

### 用 pull request

```sh
make submit-pr
```

量測和打包都一樣，接著會在 `results/<晶片>/` 底下新增兩個檔案（以資料內容命名：`<id>.json` 和 `<id>.samples.json`），並印出開分支用的 git 指令。推到你的 fork，再開 pull request。每次 push，機器人都會檢查這兩個檔案並留言。請不要手動修改它們：檢查會從 samples 檔重新算出每一個統計值，只要有一點不同就退回。

### 會送出哪些資料

只有重新計算和檢查結果所需要的東西。`build/submission/submission.json` 是同一份資料、方便閱讀的版本。

| 欄位 | 例子 | 為什麼需要 |
|---|---|---|
| `machine.brand` | `Apple M5` | 哪一顆晶片 |
| `machine.model` | `Mac17,2` | 哪一款 Mac；同一顆晶片會出現在好幾款機器 |
| `machine.os_version`、`os_build` | `27.0`、`26A428` | macOS 版本可能改變時序 |
| `machine.page_size` | `16384` | TLB 實驗和它有關 |
| `machine.virtual_machine` | `false` | 虛擬機量不了 |
| `machine.pauth_keys_active` | `false` | 解釋 `pac*` 指令的數字 |
| `machine.levels[]` | `P`、`Super`、4 核、L1/L2 大小 | 這顆晶片上「P 核」「E 核」指的是什麼；快取實驗會拿它來對照 |
| `tool_version`、`spec_sha256` | `0.2.0`、一段摘要值 | 用哪一版工具、哪一份指令表量的 |
| `runs[]` | 秒數、計數器介面、量了幾次／乾淨幾次／丟掉幾次、系統負載、各核心時脈 | 每一輪的量測條件，給品質檢查用 |
| `helpers`、`instructions`、`structure` | 每一輪、每一個數字的值 | 量測本身；所有統計值都從這裡重算 |
| `stats_sha256` | 一段摘要值 | 證明統計值確實是從這些數字算出來的 |

**不會送出：**電腦名稱、使用者名稱、序號、硬體 UUID、檔案路徑、記憶體大小、量測的時間。打包程式只複製上表列出的欄位；任何看起來像路徑、電子郵件、UUID、序號或 `.local` 主機名稱的文字，它都拒絕打包，GitHub 上的檢查也會拒絕。issue 本身是公開的，會顯示你的 GitHub 帳號，這和任何 issue 一樣。

### 機器人檢查什麼

格式與隱私、工具版本與指令表、用每一輪的數字重算統計值、物理錨點（add、sub、eor 要 1 個週期；比較指令接 `csinc` 要 2 個；沒有任何東西比核心的寬度還快）、內部一致性（執行單元數不超過寬度、P 核對 E 核、量到的快取大小對系統回報的大小）、量測品質、重複提交，以及這顆晶片已經有資料時，逐值和舊資料比對。細節和這些檢查能證明到什麼程度：[docs/DESIGN.md](docs/DESIGN.md#open-submissions)。

### 只量測、不送出

`make measure` 會寫到 `results/local/<晶片>/`（git 不追蹤）：合併後的 JSON 和兩個 CSV 檔。接著執行 `make site`，網站就會把你的數字放在已發布的晶片旁邊，標成「local」；打開 `site/index.html` 即可。

如果 `build/uarch info` 顯示 `counters none`，代表這台機器不讓一般程式讀週期計數器（虛擬機就是這樣），什麼都量不了。如果 `make measure` 因為錨點檢查失敗而停下來，請開一個一般的 issue，附上 `build/uarch selftest` 的輸出：那是工具的 bug，不是你晶片的問題。

## 新增指令

一個指令就是 `insns/*.def` 裡的一行：

```
名稱 | 組合語言樣板 | 屬性
```

樣板語法寫在 `tools/gen_insns.py` 開頭的說明裡。新增之後：

```sh
make test                       # 每一筆都會實際執行一次；必須能跑，或丟出 SIGILL
build/uarch insn -f <名稱>      # 看看兩種核心上的數字
```

新的一筆要被接受，條件是：延遲鏈是真正的相依關係（看 JSON 裡的 `chain` 文字）、數值不會飄到特殊值（0、無限大、NaN，除非那正是要測的）、數字可以重現。修改指令表會改變 `spec_sha256`：用舊表量的提交會被拒絕，直到 `tools/uarch_submit.py` 裡接受的工具版本更新為止。

## 程式碼

- C11，除了 macOS SDK 之外沒有相依套件。`make lint` 必須通過（警告視為錯誤，並對 `src/` 跑 clang 靜態分析）。
- Python 工具只用標準函式庫。
- 任何在執行時產生機器碼的東西都要經過 `src/enc.h`，而且裡面每個函式都要在 `tests/enc_gen.c` 有對應的測試案例，跟系統組譯器的結果比對。
- 實驗必須交代數字是怎麼得到的（曲線），也必須能回答「無法判定」。從壞掉的實驗得到一個看起來很合理的數字，正是這個專案要避免的狀況。
- 需要計數器的測試，在沒有計數器時要以狀態碼 77 結束並說明原因。`UARCH_COUNTERS=none make test` 會用虛擬機（CI）看到的方式跑整套測試。

## 給維護者

- 機器人用到的標籤：`submission`（issue 表單自動加上）、`accepted`、`flagged`、`rejected`。
- Settings → Actions → General → Workflow permissions：允許 GitHub Actions 建立 pull request。沒開的話，機器人仍會推送 `submission/issue-<編號>` 分支，並在留言附上比較連結。
- 機器人開的 pull request 不會觸發其他 workflow（這是 GitHub 對它的 token 的規定）；檢查結果寫在 pull request 內文裡，合併後 `main` 上會跑 CI。
- 被標記待審的提交需要人來決定：合併（網站會標出它離群的數值，並且不把它們算進晶片的數字），或附上說明關掉 pull request。
- 建議保護 `main` 分支，讓任何東西都要經過審查才能進去；機器人從不推送到 `main`。
