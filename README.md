# DweepD2D

讓 1999 年的益智遊戲 **Dweep Gold**（Dexterity Software）在現代 Windows 上順暢執行，而且**不修改遊戲本身**。

- 以 Direct2D / Direct3D 11 取代 DirectDraw，以 XAudio2 取代 DirectSound，不需要 dgVoodoo 等外部包裝器。
- 視窗或無邊框全螢幕，可保持 4:3 或拉伸，提供清晰縮放濾鏡。
- **GPU 繪圖層（Dweep_D2D.exe）**：遊戲所有繪圖函式都改由 GPU 以螢幕解析度重畫，文字改用 DirectWrite，不再是把 640×480 畫面放大。
- 設定和過關進度改存在遊戲目錄的 `DweepSave.ini`，不寫入註冊表。

> 本專案**不包含**任何遊戲檔案。你需要自己擁有一份 Dweep Gold（`Dweep.exe` 版本 1.40）。

---

## 兩種用法

| | A. 相容層 | B. GPU 繪圖層 |
|---|---|---|
| 執行 | 原版 `Dweep.exe` | `Dweep_D2D.exe` |
| 需要的檔案 | `DDraw.dll`、`DSOUND.dll` | `Dweep_D2D.exe`、`DWGFX.dll`、`DSOUND.dll` |
| 畫面 | 640×480 畫面放大到視窗 | 依螢幕解析度直接繪製（例如 3 倍 = 1920×1440） |
| 文字 | GDI 點陣字放大 | DirectWrite 向量字 |
| 原理 | 取代 DirectDraw / DirectSound | 另外攔截遊戲的 21 個繪圖函式，在 GPU 上重做 |

兩種可以同時安裝，共用同一份 `DweepD2D.ini` 和 `DweepSave.ini`。`Dweep_D2D.exe` 是由你的 `Dweep.exe` 產生的複本，**只改了匯入表裡的一個 DLL 名稱**（`DDRAW.dll` → `DWGFX.dll`），原版 `Dweep.exe` 不會被修改。

## 安裝

1. 從 [Releases](../../releases) 下載 `DweepD2D-版本.zip` 並解壓縮，或自行編譯（見下方）。
2. 執行 `install.cmd`，有三種方式：
   - 雙擊後選擇遊戲資料夾
   - 把遊戲資料夾拖到 `install.cmd` 上
   - 執行 `install.cmd "C:\你的\Dweep Gold 資料夾"`
3. 執行 `Dweep.exe`（用法 A）或 `Dweep_D2D.exe`（用法 B）。

安裝程式會做這些事：

- 檢查 `Dweep.exe` 是否為測試過的 1.40 版（比對 SHA-256）。版本不同時只會警告。
- 把 `DDraw.dll`、`DSOUND.dll`、`DWGFX.dll` 複製到遊戲資料夾。`DweepD2D.ini` 只在不存在時才複製；你的設定和進度（`DweepSave.ini`）不會被覆蓋。
- 用你的 `Dweep.exe` 產生 `Dweep_D2D.exe`：只把匯入的 `DDRAW.dll` 改名為 `DWGFX.dll`，原版 `Dweep.exe` 不會被修改。這個檔案是原版遊戲的複本，因此不隨本專案散布，而是在安裝時產生。

移除：刪掉 `DDraw.dll`、`DSOUND.dll`、`DWGFX.dll`、`Dweep_D2D.exe`（和 `DweepD2D.ini`）即可，遊戲本身沒有被改動。

### 關於防毒軟體

這些 DLL 會攔截遊戲的函式，用法 B 還會在記憶體中改寫遊戲程式碼的開頭，有些防毒軟體可能因此誤判。

- 發行版的 zip 內附 `SHA256SUMS.txt`，可以在 PowerShell 用 `Get-FileHash 檔名` 核對每個檔案。
- 也可以從原始碼自行編譯。

## 設定：`DweepD2D.ini`

| 區段 | 鍵 | 值 |
|---|---|---|
| `[Display]` | `Mode` | `fullscreen`（無邊框全螢幕）/ `window` |
| | `WindowScale` | 視窗模式的大小倍率，預設 `2` |
| | `Filter` | `sharp`（清晰、均勻，建議）/ `nearest` / `linear` / `integer`（只用整數倍） |
| | `Aspect` | `keep`（保持 4:3，有黑邊）/ `stretch`（填滿） |
| | `VSync` | `1` 等待螢幕更新，`0` 關閉 |
| `[Render]`（只有 B） | `Scale` | 內部解析度倍率，`auto` 依螢幕決定 |
| | `Verify` | `0` 只用 GPU（預設）；`1` 同時執行原版 CPU 繪圖並逐幀比對；`2` 逐筆比對（慢，強制 Scale 1） |
| | `Gpu` | `0` 改回顯示原版 CPU 畫面 |
| `[Input]` | `MouseSpeed` | `auto`（預設）：和原版 640×480 全螢幕一樣，滑鼠每移動一步就是一個遊戲像素；`1`：游標跟著 Windows 游標走，畫面放大時會覺得比較慢；其他數字：放大的倍率，例如 `1.5` |

使用繪圖板或遠端桌面等「絕對座標」裝置時，請把 `MouseSpeed` 設為 `1`。

快捷鍵（只有 B）：**F12** 把畫面存成 BMP；**F11** 在 `Verify` 為 1 或 2 時切換顯示原版 CPU 畫面。

錯誤和診斷訊息寫在遊戲資料夾的 `DweepD2D_log.txt`。

## 存檔：`DweepSave.ini`

原版遊戲把設定和進度寫在 `HKLM\Software\Dexterity Software\Dweep`。沒有管理員權限時，Windows 會把它轉存到 VirtualStore。遊戲每次啟動還會嘗試在 `HKEY_CLASSES_ROOT` 登記 `.dwp` 副檔名。

兩個 DLL 都會攔截這些註冊表呼叫：

- 遊戲的設定機碼改由 `DweepSave.ini` 提供：
  - `[Options]`：開關類數值（Sound、Music、Hints…）
  - `[Data]`：字串（各關卡組的過關紀錄，例如 `Original Game=1111…`，以及 `Path`）
- 其他機碼一律回報「拒絕存取」，註冊表完全不會被寫入。
- 檔案不存在時會建立新的空檔，進度從頭開始，不會從註冊表匯入。

## 編譯

需求：

- Visual Studio 2022 以上的「使用 C++ 的桌面開發」（需要 x86 編譯器和 Windows 10/11 SDK）
- Python 3（選用）：讓 `build.cmd` 也在 `out\` 產生 `Dweep_D2D.exe`；沒有 Python 時由 `install.cmd` 產生

步驟：

1. （選用）把你自己的 `Dweep.exe` 複製到 `game\Dweep.exe`。只有在 `build.cmd` 產生 `Dweep_D2D.exe` 或做逆向分析時才需要。`game\` 已列在 `.gitignore`，不會被提交。
2. 執行：

   ```
   build.cmd
   ```

   - 輸出到 `out\`：`DDraw.dll`、`DSOUND.dll`、`DWGFX.dll`、`Dweep_D2D.exe`、`DweepD2D.ini`。
   - 如果 Visual Studio 不在預設位置，修改 `build.cmd` 開頭的 `VCVARS`。
3. 安裝到遊戲資料夾：執行 `install.cmd`，它會自動使用 `out\` 裡的檔案。

### 打包發行版

```
package.cmd 1.0
```

會先編譯，再產生：

- `dist\DweepD2D-1.0.zip`：內含三個 DLL、`DweepD2D.ini`、`install.cmd`、`install.ps1`、`README.md`、`LICENSE`、`SHA256SUMS.txt`
- `dist\SHA256SUMS.txt`：zip 檔本身的 SHA-256

發行版只包含本專案的檔案，不含 `Dweep_D2D.exe` 或任何遊戲檔案。

## 專案結構

```
build.cmd            編譯全部到 out\
install.cmd / .ps1   安裝到遊戲資料夾，並產生 Dweep_D2D.exe（不需要 Python）
package.cmd          編譯並打包 Release zip 到 dist\
config\DweepD2D.ini  設定檔範本
src\
  proxy\             用法 A：ddraw.cpp（Direct2D 版 DirectDraw）、dsound.cpp（XAudio2 版 DirectSound）
  gfx\               用法 B：ddemu.cpp（軟體 DirectDraw + 視窗/滑鼠處理）、gfx.cpp（GPU 繪圖層）
  common\regsave.h   註冊表 → DweepSave.ini（A、B 共用）
tools\
  patch_exe.py       由 Dweep.exe 產生 Dweep_D2D.exe（只改匯入的 DLL 名稱）
  dwp_pack.py        把單關自訂關卡 (.DWP) 打包成關卡組
  logger\            早期用的記錄型 DDraw/DSOUND 代理：只記錄遊戲呼叫了什麼
  test\test.cpp      DDraw.dll / DSOUND.dll 冒煙測試
re\scripts\          Ghidra 無頭模式腳本（Java）與 PE 小工具
game\                （不在儲存庫中）放置自己的 Dweep.exe
re\ghidra\           （不在儲存庫中）Ghidra 專案
re\output\           （不在儲存庫中）反編譯輸出
```

## 運作原理

### 用法 A：DDraw.dll / DSOUND.dll

遊戲只用到很小的一部分 DirectDraw：

- 一個主畫面，加上一個 640×480×16 位元的系統記憶體後台緩衝區
- 以 Lock/Unlock 直接寫像素
- 文字透過 GetDC 用 GDI 畫

`DDraw.dll` 用軟體實作這些功能，在畫面更新時把 RGB565 畫面上傳成 Direct2D 點陣圖，再縮放顯示到視窗。

滑鼠和視窗相關的 API（GetCursorPos、GetSystemMetrics、ClipCursor…）透過遊戲的匯入表攔截，讓遊戲以為螢幕仍是 640×480。

`DSOUND.dll` 把靜態 PCM 音效緩衝區對應到 XAudio2 的來源語音。

### 用法 B：DWGFX.dll

遊戲所有繪圖都經過一個圖像管理物件（位址 `0x44f9a8`）的 21 個函式：

- 複製、平鋪、各種精靈圖（RLE16、8 位元調色盤、50%、alpha、紅色閃爍）
- 矩形、斜角按鈕、對話框面板、灰階、地板、雷射、髒矩形複製

`DWGFX.dll` 在載入時：

1. 檢查每個函式開頭的位元組是否符合預期。不符合就停用 GPU 層，回到軟體顯示。
2. 用跳轉指令把這些函式導向自己的版本。
3. 在 Direct2D 上以 S 倍解析度重新實作每一個：
   - 半透明、變暗、變亮用不透明度和加色混合
   - 灰階用 ColorMatrix 效果
   - 雷射用由遊戲亮度表產生的漸層條
4. 遊戲在後台緩衝區畫的 GDI 文字，改用 DirectWrite 在 GPU 上繪製。
5. 原版函式除了畫圖外還有一些副作用，例如把「需要重畫的區域」登記到遊戲的髒矩形清單，這些會照原樣補上。

`Verify=1/2` 時，原版 CPU 函式會照常執行，產生的 640×480 畫面當作參考，與 GPU 結果逐幀或逐筆比對，差異記錄在日誌中。每個效果都是用這個方式確認與原版一致。

## 逆向分析（re\）

1. 安裝 [Ghidra](https://ghidra-sre.org/) 和 JDK。
2. 建立名為 `Dweep` 的專案放在 `re\ghidra\`，匯入 `game\Dweep.exe` 並完成分析。
3. 執行腳本：

   ```powershell
   $env:GHIDRA = 'C:\path\to\ghidra'; $env:JAVA_HOME = 'C:\path\to\jdk'
   re\scripts\run.ps1 Summary.java          # 函式與類別概要
   re\scripts\run.ps1 Decomp.java <位址...>  # 反編譯指定函式
   ```

   `XRefs.java`、`Disasm.java`、`MethodStrings.java` 分別查交叉參照、反組譯和字串。

反編譯結果衍生自原版程式，只保存在本機的 `re\output\`（已列在 `.gitignore`），請勿提交或散布。

## 已知限制

- 只支援 Dweep Gold 1.40 的 `Dweep.exe`；`DWGFX.dll` 偵測到不同的程式碼時會自動停用 GPU 層。
- 用法 B 下，如果顯示卡驅動中途重置，會用背景圖重建畫面，移動中的物件要等遊戲下次重畫才會出現。
- 停用 `.dwp` 副檔名登記後，雙擊 `.dwp` 檔不會開啟遊戲（原版沒有管理員權限時本來也做不到）。

## 法律聲明

Dweep 和 Dweep Gold 是 Dexterity Software 的作品，本專案與其無關。

- 本專案只包含自行撰寫的相容性程式碼，不包含遊戲執行檔、資料、圖像、音效或反編譯原始碼。
- 使用時需要你自己合法取得的遊戲。

## 授權

本專案的程式碼以 [MIT 授權](LICENSE) 釋出。此授權只涵蓋本專案自行撰寫的程式碼，不涵蓋 Dweep Gold 遊戲本身。
