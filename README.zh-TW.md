# m5-uarch

**Apple M5 處理器的實測微架構資料，以及產生這些資料、不需要 root 權限的量測工具。**

內容包含約 940 種指令形式的延遲（latency）與吞吐量（throughput）表，以及核心內部各種結構的大小，
M5 的效能核心（系統稱為 "Super"）與節能核心都有。所有數字都是在真實硬體上、用一般權限的行程量出來的：
不用 root、不用核心擴充、不用特殊授權。同一套工具在任何 Apple Silicon Mac 上一行指令就能跑，而且任何人都能
把自己的晶片加進來：`make submit` 量測並打包結果，你把它貼進一個 GitHub issue，機器人自動檢查，網站上就能
把不同晶片並排比較。

[English](README.md) · [設計文件](docs/DESIGN.md) · [給初學者的導讀](docs/導讀.zh-TW.md) ·
[線上瀏覽與比較結果](https://useless-husband.github.io/m5-uarch/) ·
[加入你的 Mac](CONTRIBUTING.zh-TW.md) · [JSON](results/apple-m5/apple-m5.json) · [CSV](results/apple-m5/)

## 為什麼要做這個

x86 有 uops.info，Apple M1 有 Dougall Johnson 的 Firestorm 表。之後的 Apple 晶片只有幾篇分析文章，
而且用的工具都需要 root 才能設定效能計數器。就我所知，寫這份文件時（2026 年 9 月）還沒有人公開 M5
的同類資料，LLVM 也還在用 2013 年 Cyclone 核心的排程模型來處理 `apple-m5`。詳見[相關作品](#相關作品)。

不用 root 也做得到的關鍵：macOS 會替每條執行緒記錄「週期數」與「已執行指令數」，而且依核心種類分開，
行程可以透過 `thread_selfcounts()` 讀到自己的數字。沒有 root 就沒有可設定的事件計數器，所以週期與指令數以外的
一切都要靠時間實驗推論；推論比較間接的地方，結果裡會寫明。

## M5 長什麼樣子

量測機器：Mac17,2（Apple M5，4 個 P 核心 + 6 個 E 核心，macOS 27.0），五次執行的中位數；寫成範圍表示
各次結果不一致。「已發表」欄位是別人對其他晶片的量測，放在這裡對照。

| | M5 P 核心 | M5 E 核心 | M4 P / E（已發表） | M1 P（已發表） |
|---|---|---|---|---|
| 管線寬度（每週期 NOP 數） | 10 | 6 | 10 / 5 | 8 |
| 整數 ALU 數 | 8 | 4 | 8 / 4 | 6 |
| 可設定旗標的 ALU 數 | 4 | 4 | | 3 |
| 整數乘法器 | 3 | 1 | | 2 |
| 每週期載入 / 儲存 | 3 / 2 | 2 / 1 | 3 / 2、2 / 1 | |
| 浮點 / SIMD 單元 | 4 | 3 | 4 / 3 | 4 |
| 每週期可跳躍的分支數 | 2 | 1 | 2 / 1 | 1 |
| 重排緩衝區（以 NOP 計） | 3367 | 1072 | 約 3184 / 約 513 | 約 2310 |
| 重排緩衝區（一般指令混合） | 1334 | 466–486 | | |
| 飛行中的整數暫存器重新命名（64 位元 / 32 位元寫入） | 396 / 809 | 185 / 185 | 約 360 / 約 720 | 約 350–360 |
| 飛行中的浮點 / SIMD 重新命名 | 839 | 202 | | 約 400 |
| 飛行中的旗標重新命名 | 167 | 73 | 約 175 | 約 128 |
| 飛行中的載入 | 486 | 65 | | 約 130 |
| 飛行中的儲存 | 137 | 55 | | 約 60 |
| 飛行中未解析的分支 | 194 | 73 | | 約 144 |
| 分支預測錯誤的代價（週期） | 16.7 | 11.8 | | |
| L1D 容量 / 載入到使用的延遲 | 128 KiB / 3 | 64 KiB / 3 | 128 KiB / 3、64 KiB / 3 | |
| L2 載入延遲（週期） | 12（256–512 KiB）、32–36（2–8 MiB） | 15–19（128 KiB–4 MiB） | | |
| 第一層資料 TLB 項數 | 162 | 133–193 | 160 / 192 | |
| 第二層 TLB 項數 | 3101 | 2144–2435 | 3072 / 1024 | |

比較特別的發現：

- **E 核心的進步比 P 核心大。** 跟已發表的 M4 數字比，P 核心的視窗只深了約 6%（3367 對約 3184 個 NOP），
  E 核心卻寬了一整道指令（6 對 5），視窗能裝的 NOP 多了一倍（1072 對約 513）。
- **每次都載入同一個值的 load，在 P 核心上只要 0.34 個週期，不是 3。** 它的值被預測了，相依關係消失；
  只要有三個以上的值輪流出現，就回到 3.00。教科書量測載入延遲的方法（讓一個記憶體格子指向自己）因此會量出
  錯誤的數字。本工具改追一個 509 個節點的隨機環，得到 3 個週期（用索引暫存器時是 4）。E 核心沒有這種預測。
  （`uarch structure -e spec` 的 `lvp_const_load`）
- **條件從不改變的 `csel` 在 P 核心上會被預測。** 讓相依鏈穿過 `csel`「沒選到」的那個輸入，理論上每一步
  `add`+`csel` 要 2 個週期；五次執行裡有三次只要 0.30 到 0.34，代表那條相依已經不存在（另外兩次是 2.00）。
  只要用相反的條件跑 64 圈，同一段程式碼在五次裡有四次變回 2.00（有一次 0.47）。`csinc` 從頭到尾都是 2.00，E 核心也全部是 2.00。指令表裡登記的是
  資料流延遲（1）：工具在每次量測之間都會用相反的旗標跑一下，確保量到的是這個數字。
- **長得像指標的資料會被預先抓取。** 兩個本來應該排隊的快取未命中，格子裡放索引時花 1.67 倍的時間，
  放指標時只花 1.36 倍（中位數那次執行；各次執行 1.08 到 1.37）：有一個「依資料內容預取」的機制提早把
  第二筆抓進來了。E 核心兩者幾乎一致（1.60 與 1.53）。
  所以視窗實驗一律追索引。（`dmp_pointer_chase`，信心度低：共用機器上的記憶體時間不穩）
- **P 核心上兩個 32 位元的結果共用一個實體暫存器**：`add w` 可以同時有 809 個在飛，`add x` 只有 396 個。
  E 核心兩者都是 185。
- **P 核心的「儲存後馬上載入」幾乎不花時間**：存、讀回、加一，每圈 1.66 個週期，也就是存→讀大約 0.66 個
  週期（E 核心 3.6）。轉送單一位元組，或讀得比存的寬，要 6 個週期。
- **暫存器搬移會被消除，但一連串的搬移不會。** 前面是真正運算的 `mov` 在 P 核心上延遲為零；前面是另一個
  `mov` 的 `mov` 要 0.90 個週期：十個裡只有一個被消除，剛好對應每個 10 指令寬的重新命名群組一個
  （6 指令寬的 E 核心是 0.83）。`add x, x, #0` 也會像 `mov` 一樣被消除。`eor x, x, x` **不是**歸零慣用法：
  它保留相依關係，花一個週期。
- **比較加分支的融合比想像中廣。** `cmp`、`adds`、`subs`、`ands`、`tst` 後面接 `b.cond`，`add`/`and` 後面接
  `cbz`，還有 `adrp`+`add`，相鄰時都明顯比中間隔一道指令快，兩種核心都一樣；同一個測試裡兩組不可能融合的
  對照組結果是 1.00 到 1.03。`aese`+`aesmc` 合成一個 2.2 週期的運算（中間夾一個 NOP 就變 4.2）。
- **P 核心的 ALU 吞吐量跟指令讀什麼有關。** 一串 `add xN, xM, #1` 每週期 7.7 道（用 NOP 稀釋後 7.9：8 個
  單元），兩個暫存器相加 6.7，而每道指令都讀同一個暫存器兩次的 `add xN, x19, x19` 只有 3.2。E 核心全部都是
  剛好 4。
- **P 核心上一串 64 位元的立即數 mov 會落在兩種穩定狀態之一**：每週期約 8.9 道，或是跑滿 10 的寬度；每次進入
  迴圈都重新決定（同一顆核心、同樣的時脈、同樣的指令數）。`mov x, #0` 和 `mov x, x` 永遠是 10，
  `mov w, #imm` 永遠是 8.9。舊版工具把兩種狀態的執行混在一起算，在忙碌的機器上得到 8.2、在閒置的機器上
  得到 13，比寬度還大；實驗與修正見 [docs/DESIGN.md](docs/DESIGN.md#steady-states-a-move-at-13-per-cycle)。
  表格列的是最快的那個狀態（10.06）。
- **浮點加法的延遲不是整數**：相依鏈中 P 核心 2.11 個週期、E 核心 2.50（M1 是 3）。向量整數加法兩種核心都
  剛好是 2；浮點乘法 P 核心 3.00、E 核心 3.50。
- **在整數與浮點暫存器之間搬資料很慢**：`fmov d, x` 再 `fmov x, d`，P 核心 10 個週期，E 核心 8 個。
- P 核心的整數除法不管運算元是什麼都是 7 個週期，每兩個週期可以開始一個。E 核心從被除數算起是 7 個週期、
  商為零時 8 個、從除數算起 9 個，而且一次只能做一個。雙精度浮點除法在 P 核心是 9 個週期、每週期可以開始一個
  （E 核心 10 個週期）。`pacga` 是 7 個週期（E 核心 6 個）。

以上都在[結果檔](results/apple-m5/)裡，附每次執行之間的範圍；結果網站上每個結構數字都可以點開看它是從
哪條曲線讀出來的。

## 加入你的 Mac

```sh
git clone https://github.com/useless-husband/m5-uarch && cd m5-uarch
make submit
```

需要：Apple Silicon Mac、Xcode 命令列工具（`cc`、`make`、`python3`）和一個 GitHub 帳號。不需要 `sudo`，
除了 clone（或直接下載 ZIP）之外也不需要會用 git。`make submit` 會量三次（一兩分鐘，快取未命中實驗會用到約
300 MB 記憶體），把結果打包成一段文字、複製到剪貼簿，並打開這個專案的新增 issue 頁面：

```console
$ make submit
chip        Apple M5 (Mac17,2), 4 P + 6 E cores
macOS       27.0 (26A428)
tool        0.3.0, 3 runs
values      4638 instruction figures per run, 132 structure experiments
size        39570 characters (an issue holds 65536)
not sent    host name, user name, serial number, UUIDs, file paths, memory size, start times
...
```

貼上、打勾、按 Create。幾分鐘內機器人會回覆 **accepted（接受）**、**flagged（標記待審）** 或
**rejected（退回）**，附上每一項檢查的理由；接受的資料會被開成 pull request，由維護者合併。`make submit-pr`
做一樣的事，但產生的是給 pull request 用的檔案。只跑 `make measure` 的話，結果寫在 `results/local/<晶片>/`
（JSON 和 CSV，git 不追蹤），`make site` 會把它和已發布的晶片放在一起顯示。到底送出哪些資料，
[CONTRIBUTING.zh-TW.md](CONTRIBUTING.zh-TW.md) 有完整清單。

實際執行的樣子：

```console
$ build/uarch info
chip        Apple M5 (Mac17,2)
os          macOS 27.0 (26A428)
page size   16384 bytes
counters    thread_selfcounts
level 0     P: "Super", 4 cores, L1I 192 KiB, L1D 128 KiB, L2 16384 KiB shared by 4
level 1     E: "Efficiency", 6 cores, L1I 128 KiB, L1D 64 KiB, L2 6144 KiB shared by 6
pauth       keys inactive in this process: pac*/aut* pass their operand through
reach P     yes (confirmed by the per-level counters)
reach E     yes (confirmed by the per-level counters)

$ build/uarch selftest
P-core  ok   cmp + csinc round trip = 2.000 cycles (want 2: two one-cycle operations)
P-core  ok   add_x_reg    latency = 1.000 cycles, 7 clean runs, instructions per iteration as generated (a 64-bit add takes one cycle)
...
E-core  ok   sub_x_reg    latency = 1.000 cycles, 7 clean runs, instructions per iteration as generated (a 64-bit sub takes one cycle)
800 runs, 0 discarded for migration, 18 discarded for interrupts
selftest passed

$ build/uarch insn -l P -f ldr_x_idx
ldr_x_idx                ldr x0, [x27, x20]                       tp  3.00/c   A0>W0 3.02  R1>W0 4.00
ldr_x_idx_lsl3           ldr x0, [x27, x20, lsl #3]               tp  3.00/c   A0>W0 3.00  R1>W0 4.00
ldr_x_idx_sxtw           ldr x0, [x27, w20, sxtw]                 tp  3.00/c   A0>W0 3.03  R1>W0 4.00
ldr_x_idx_uxtw_lsl3      ldr x0, [x27, w20, uxtw #3]              tp  3.00/c   A0>W0 3.00  R1>W0 4.00

$ build/uarch structure -l P -e spec
  lvp_const_load                    0.34 cycles      (high)  Latency of a load that always returns the same value
  csel_unselected_input             0.32 cycles     [0.32 .. 0.60]  (high)  add + csel chained through the input csel does not select
  csel_unselected_after_flip        2.00 cycles     [2.00 .. 2.00]  (high)  The same chain after 64 iterations of the opposite outcome
  csinc_unselected_input            2.00 cycles     [2.00 .. 2.00]  (high)  The same chain with csinc (control)
  ...
```

`tp` 是每週期可完成幾個；`A0>W0 3.00` 表示從位址運算元到載入的值要 3 個週期。實驗有 `width`、`window`、`elim`、`fusion`、`branch`、`cache`、`tlb`
與 `spec`（`uarch structure -e help`）。

## 運作方式

```
 insns/*.def ──► tools/gen_insns.py ──► 組合語言文字 ──► 系統組譯器 ──► 機器碼 ─┐
 （指令範本）     每道指令產生一個吞吐量區塊，以及每條「輸入→輸出」路徑一條延遲鏈      │
                                                                                ▼
   src/enc.h（小型編碼器，和組譯器逐一對照過）──► 放進 MAP_JIT 記憶體的迴圈
                                                                                │
   thread_selfcounts()：依核心種類分開的週期數與指令數 ◄── 跑 n 圈與 2n 圈 ◄─────┘
                        │
                        ▼
   只保留「乾淨」的執行：(a) 所有週期都算在指定的核心種類上
                        (b) 指令數剛好等於迴圈應該執行的數量
                        │
                        ▼
   乾淨執行的中位數 ──► 每次執行一個 JSON ──► tools/uarch_results.py 合併多次執行
                                            （中位數、最小、最大）──► JSON、CSV、網站
```

- **計數器。** `thread_selfcounts()`（libsystem_kernel 有匯出、沒有標頭檔）回傳呼叫它的執行緒的週期數與
  已執行指令數，每種核心一組。QoS 等級可以引導執行緒到 P 或 E 核心，但那只是請求，所以每次執行都要確認
  另一種核心的計數器沒有動，才算數。
- **乾淨的執行。** 迴圈會執行多少道指令，產生器一清二楚。核心替這條執行緒做的事（中斷、被搶走）只會讓
  指令數變多，所以指令數超過最小值的執行一律丟掉。在這台共用的機器上大約丟掉 4%，剩下的可以重現到三、四位數。
- **兩種長度。** 每個量都是 n 圈與 2n 圈的差，以及迴圈本體 k 份與 2k（或 3k）份的差，這樣計數器的系統呼叫、
  迴圈分支、每圈只發生一次的效應都會抵銷。
- **穩定狀態。** P 核心上的吞吐量迴圈每次執行可能落在幾種穩定狀態之一。吞吐量只取「至少三次執行都達到的
  最快狀態」裡的那些執行，而且兩種迴圈長度推算出的固定成本、迴圈本身的成本都要合理，否則重量
  （DESIGN.md 的 "Steady states"）。
- **編碼。** 指令表的編碼在建置時交給系統組譯器產生，C 程式只負責複製。少數需要在執行時計算運算元的指令
  用一個小編碼器產生，測試會把它的每個函式和組譯器的輸出逐一比對。
- **延遲。** 每個輸入運算元各用一條相依鏈量測。輸出和輸入不在同一組暫存器（旗標、浮點）時，用第二道指令
  把鏈接回來：旗標用 `csinc` 與 `cmp`（各 1 個週期，來回 2.00 個週期可以證明），浮點用 `fmov`。`fmov`
  佔多少無法用時間拆開，所以那些延遲以「來回」公布，並標上 `rt`。
- **結構大小。** 用 Henry Wong 的方法：兩個快取未命中中間隔著越來越多的填充指令，在填充指令把某個結構
  塞滿之前兩個未命中會重疊。每個實驗的細節，以及為了在這顆核心上行得通而做的修改，見
  [docs/DESIGN.md](docs/DESIGN.md)。

不用伺服器的提交流程：

```
 make submit ──► 只有每一輪的數值、只有允許的欄位 ──► gzip+base64 文字（約 40 000 字元）
                                                          │ 貼進 issue
                                                          ▼
 Actions 第一段（唯讀權限）：把 issue 內容當資料解析，從 insns/*.def 重建指令表，
   重算每一個統計值，跑所有檢查 ──► 結論 + 留言
                                                          │
 Actions 第二段：留言、貼標籤、推送 submission/issue-N 分支、開 pull request
                                                          │ 維護者合併
                                                          ▼
 Pages：把每顆晶片的所有資料集合併（中位數、分布範圍、標出離群值）──► 比較網站
```

送出的不是 370 KB 的結果檔，而是合併程式需要的「每一輪的原始數值」（三輪約 40 000 字元、五輪 48 376 字元；
一個 issue 最多 65 536 字元）。指令的文字說明在接收端用同一版工具重新產生，並用摘要值證明兩邊一致。
格式、每一項檢查、離群值規則和它的校準、以及 workflow 的威脅模型，見
[docs/DESIGN.md](docs/DESIGN.md#open-submissions)。

## 驗證

- **基準點。** `uarch selftest` 與 `tools/uarch_results.py check` 要求任何 AArch64 核心都必須成立的事實：
  add、sub、eor 是 1 個週期，比較接 `csinc` 是 2 個週期，單一指令不會超過管線寬度（沒有東西退休得比 NOP
  快；唯一一次量到超過，是把兩種穩定狀態混在一起算了）。不成立的話 `make measure` 會在量測前停下來。
- **指令數完全相符。** 每次量測都檢查計數到的每圈指令數等於產生的指令數。
- **實驗內建對照組。** 融合測試包含不可能融合的指令對（結果 1.00 到 1.03）。推測執行的測試每個效應都有
  對照（`csinc`、打亂的環、用索引取代指標）。快取大小和作業系統回報的值比對：L1D 量到 128 與 64 KiB，
  與回報相同。P 核心 L2 轉折點的中位數是回報的 16 MiB（信心度低：各次執行 11.3 到 16 MiB，而且快取與其他
  忙碌的核心共用）；E 核心的轉折點在 5.7 MiB（有一次是 4 MiB），回報值是 6 MiB。
- **可重現性。** 五次執行共 1 094 794 個計時迴圈；3.5% 因中斷被丟掉，660 個（0.06%）因為在兩種核心之間
  搬移被丟掉。各次執行之間，延遲的差異（最大減最小，除以中位數）的中位數兩種核心都是 0.05%
  （第 99 百分位：P 核心 0.9%、E 核心 3.6%）。E 核心的吞吐量一樣穩（第 99 百分位 1.9%）。P 核心的吞吐量
  有 2.9% 的數字在不同次執行之間差超過 6%（第 99 百分位 9.8%）；先前那份由會混算穩定狀態的舊版量的資料，
  是五個裡就有一個。每個公布的數字都附自己的最小值與最大值。
- **每一份提交都自動檢查**（issue 或 pull request）：格式與隱私、工具版本與指令表摘要、用每一輪的數值重算
  統計值、上面的錨點、內部一致性（單元數不超過寬度、P 核對 E 核、量到的快取大小對系統回報）、量測品質、
  重複提交，以及和這顆晶片既有資料逐值比對的離群值檢查。把 M5 自己的五輪拆成兩組互相比對，落在規則之外的
  數值最多 0.15%；超過 0.5% 才會把整份提交標記待審。`make validate` 會像 CI 一樣重新檢查所有已收錄的資料集。
- **與已發表數字比較。** M4 或 M1 有人用 PMU 計數器量過的項目，M5 的數字要嘛合理地往上長、要嘛完全相同
  （上表：TLB 大小、L1 大小與延遲、單元數、32 位元共用暫存器）。差很多的地方（E 核心的寬度與視窗、
  浮點加法延遲）每次執行都重現。

## 限制

- 只看得到週期數與已執行指令數。沒有微指令數，也沒有連接埠分配；單元數是從持續速率推論的，融合是從
  壓力下的速率推論的。
- 跨暫存器組的延遲是來回值（`rt`），不是單一指令的數字。
- 非整數的延遲是相依鏈上的平均，核心真的是這樣跑的，但不同的指令組合可能看到不同的平均。
- 「一般指令的重排緩衝區」是某一種特定混合（整數加法、浮點加法、比較、分支、儲存，依各自的上限比例混合）
  的容量。Apple 的重排緩衝區一個項目可以放好幾道指令，所以沒有單一的「大小」；NOP 的數字是上限，混合的數字
  比較接近實際。E 核心的混合結果每次執行不太一樣。
- P 核心的「飛行中的載入」（486）是浮點載入被某個東西擋下之前能達到的數量；比預期大，不一定就是載入佇列本身。
- 分支預測錯誤的代價假設隨機位元的分支有一半預測錯誤；這個比例本身讀不到。
- 量測行程是一般的 arm64 行程，macOS 在這種行程裡不啟用指標驗證金鑰，所以 `pacia` 等指令量到的是它們實際
  表現出來的「搬移」行為（`uarch info` 會顯示）。`pacga` 和 `xpac*` 是真的。
- 時間會隨運算元數值改變的指令，只量了各項目註明的數值。
- 資料是在和其他忙碌工作共用的機器上收集的（每次執行開始時負載平均 1.6 到 2.6）。執行過濾器能去掉被中斷
  碰到的執行，去不掉快取或記憶體的競爭，所以記憶體相關的實驗會重複搜尋、回報信心度，也是最不容易重現的
  部分：P 核心的 L2 大小、記憶體延遲、E 核心的 TLB 大小、預取測試都標為低信心度。
- P 核心的 `csel` 與不跳躍分支的數字會隨程式碼位置改變，這些測試無法完全解釋，所以以範圍呈現。先前那份
  資料甚至有一次執行量到 `add`+`csel` 一步只要 0.85 個週期，比單獨的 `add` 還快（`csel_after_flip` = −0.15），
  這些測試無法解釋；現在公布的五次執行沒有出現。
- 吞吐量是迴圈能達到的最快穩定狀態的速率。落在較慢狀態的程式（64 位元立即數 mov：8.9 而不是 10，見上）
  會比較慢。兩種迴圈長度始終對不上的情況（分支很密的迴圈、部分向量與原子操作迴圈），數字是較長那個迴圈
  自己的速率（文字輸出標 `~`；每次執行約 2% 的數字，結果檔裡沒有標記），會隨迴圈長度改變。
- 自動檢查擋得住失誤和粗糙的造假，擋不住有心人：照著每個錨點捏造出來的數據也會通過。真正的防線是同一顆
  晶片有多份獨立的提交；網站會顯示每顆晶片有幾份資料、各自從哪裡來，只有兩份獨立資料一致時才標成
  verified。目前 M5 只有一份。
- 沒有涵蓋 SME/SME2 與 MTE 指令。它們是延伸目標，與其不經同樣的檢驗就發表，不如先不放。

## 相關作品

- **Dougall Johnson，[Apple M1 Firestorm/Icestorm 表](https://dougallj.github.io/applecpu/firestorm.html)**
  （2021–2023）：本專案的範本；只有 M1，用核心的 PMU 權限量測。
- **Jiajie Chen（陳嘉杰），[Apple M4 微架構](https://jia.je/hardware/2025/05/21/apple-m4/)** 與
  [cpu-micro-benchmarks](https://github.com/jiegec/cpu-micro-benchmarks)：M1 到 M4 的結構大小；macOS 上用
  私有的 `kpc` 介面，需要 root。沒有指令表。
- **Fabian Sidler，[Benchmarking M-series Apple CPUs](https://acl.inf.ethz.ch/teaching/fastcode/2025/benchmarking_m_series_apple_cpus.pdf)**
  （蘇黎世聯邦理工學院學期論文，2025）：用核心函式量 M1 Pro 與 M3 Max 的指令延遲與吞吐量。
- **[ocxtal/insn_bench_aarch64](https://github.com/ocxtal/insn_bench_aarch64)**：用計時得到的延遲表，有 M1 的結果。
- **uops.info**（Abel 與 Reineke）：x86 的方法論，包括自動建立相依鏈；本工具在 AArch64 允許的範圍內沿用。
- **Henry Wong，〈Measuring Reorder Buffer Capacity〉**（2013）：雙未命中法。
- **Augury / GoFetch**（依資料內容預取）與 **SLAP / FLOP**（Apple CPU 的載入位址與載入值預測）：
  記錄了這些推測行為的資安論文，本工具必須繞過它們。
- **LLVM** 用 `CycloneModel` 定義 `apple-m5`
  （[commit f85494f](https://github.com/llvm/llvm-project/commit/f85494f6afeb)）。

本專案的不同之處：涵蓋 M5 的兩種核心；不需要任何權限；每個數字都公布各次執行之間的差異；而且任何人都能
加入晶片，每份提交都會自動檢查，並從原始的每輪數值重新計算。

## 建置與測試

```sh
make            # 產生 build/uarch
make test       # 單元、性質、編碼器對照、功能、冒煙測試，以及提交流程的端到端測試
make lint       # 警告視為錯誤、clang 靜態分析、Python 編譯檢查
make bench      # selftest 加上所有結構實驗（約十秒）
make measure    # 完整跑三次並合併到 results/local/<晶片>/（git 不追蹤）
make submit     # 量測（或沿用上次的）並打包成 GitHub issue 用的文字；submit-pr：pull request 用的檔案
make validate   # 用機器人的方式重新檢查所有已收錄的資料集
make site       # 由 results/ 產生 site/data.js 和 site/data/
```

需要計數器的測試在沒有計數器的機器上（虛擬機，包括 GitHub 的 macOS 執行器）會印出原因並略過，其餘照常執行。
提交工具也會在機器人實際執行的 `ubuntu-latest` 上測試，包括 `tests/e2e_submission.sh`：不經過 GitHub，
直接對一份範例 issue 跑 workflow 裡的指令。

## 授權

MIT，見 [LICENSE](LICENSE)。
