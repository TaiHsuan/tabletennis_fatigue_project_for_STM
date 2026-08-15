# Table Tennis Fatigue Project (v4/v5)

嵌入式 AI 桌球拍 — STM32F7 韌體端即時擷取 IMU 訊號，並以裝置端 AI（TinyML）推論選手的擊球疲勞階段，透過藍牙即時回傳結果。

Embedded fatigue-monitoring system for a table-tennis racket. STM32F7 firmware captures IMU motion data in real time and runs an on-device CNN+LSTM model (via X-CUBE-AI) to classify the player's fatigue stage, streaming results back over Bluetooth.

> 韌體專案名稱為 `tabletennis_fatigue_project_v4`（`.ioc` / MDK-ARM 專案檔沿用 v4 命名），但所在資料夾為 v5，為既有的版本迭代，非文件錯誤。
> The firmware project itself is still named `tabletennis_fatigue_project_v4` (in the `.ioc` and MDK-ARM project files) even though the containing folder is `v5` — this is an intentional carry-over from iteration, not a typo.

---

## 目錄 / Table of Contents

- [系統架構 / System Architecture](#系統架構--system-architecture)
- [硬體規格 / Hardware](#硬體規格--hardware)
- [韌體流程 / Firmware Flow](#韌體流程--firmware-flow)
- [藍牙通訊協定 / BLE UART Protocol](#藍牙通訊協定--ble-uart-protocol)
- [AI 模型 / AI Model](#ai-模型--ai-model)
- [建置與燒錄 / Build & Flash](#建置與燒錄--build--flash)
- [專案目錄結構 / Project Structure](#專案目錄結構--project-structure)

---

## 系統架構 / System Architecture

```
┌──────────────┐   I2C1 (400kHz)   ┌──────────────┐
│  ICM20948    │◄─────────────────►│              │
│  (Accel+Gyro)│                    │              │
└──────────────┘                    │              │      UART2 (9600, AT)   ┌──────────────┐
                                     │  STM32F767VI │◄────────────────────►│   HM19 BLE   │──► 手機 App
┌──────────────┐   GPIO (EXTI6)     │   (Cortex-M7) │                        │    Module    │   Phone App
│ Power Button │◄─────────────────►│              │                        └──────────────┘
│    (PC6)     │                    │  ┌─────────┐ │
└──────────────┘                    │  │X-CUBE-AI│ │
┌──────────────┐   GPIO (PC7)       │  │CNN+LSTM │ │
│  Status LED  │◄─────────────────►│  └─────────┘ │
└──────────────┘                    └──────────────┘
```

**資料流 / Data pipeline**

```
ICM20948 raw accel/gyro (6 軸)
   → 297 frame 滑動視窗蒐集 (raw_data_buffer)
   → Butterworth band-pass biquad 濾波 + 5 點移動平均 (Apply_ETL)
   → CNN + Bi-LSTM 推論 (X-CUBE-AI network.c)
   → 疲勞階段分類 (3 類) + 機率
   → 透過 HM19 BLE 以 UART 文字協定回傳手機
```

---

## 硬體規格 / Hardware

| 項目 Item | 規格 Spec |
|---|---|
| MCU | STM32F767VIH6 (Cortex-M7, TFBGA100), 192 MHz (HSE 8MHz → PLL, Over-Drive enabled) |
| IMU | InvenSense ICM20948（6 軸加速度計 + 陀螺儀），I2C1，位址 `0x68` |
| 藍牙模組 Bluetooth | HM-19 BLE 模組，經 USART2（9600 baud）以 AT 指令控制，開機時以 `AT+NAMEBT-TEST_746` 設定裝置名稱 |
| 電源架構 Power rails | 3V3（MCU/BT，硬體無法關閉）、1V8（IMU 專用，軟體可切）|
| PCB | `TABLETENNIS_F7_RACKET_BLUETOOTH_V02-0323A`（見 Schematics/Layout/Netlist） |

### 腳位對應 / Pin mapping（來自 `.ioc`）

| Pin | 功能 Function | 說明 Note |
|---|---|---|
| PA2 / PA3 | USART2 TX / RX | 連接 HM19 藍牙模組 |
| PB8 / PB9 | I2C1 SCL / SDA | 連接 ICM20948 |
| PA10 | GPIO Output — `Pin3v3` | 3V3 供電控制腳（實際上硬體無法斷電，僅保留控制邏輯） |
| PA11 | GPIO Output — `Pin1V8` | IMU 1V8 電源開關，唯一可被軟體切斷的電源 |
| PC6 | EXTI（`PW_BTN_PC6`） | 電源按鍵，長按觸發開機/關機 |
| PC7 | GPIO Output — `LED_control` | 狀態 LED，**低電位點亮（active-low）** |
| PA13 / PA14 | SWDIO / SWCLK | 除錯用 SWD |
| PH0 / PH1 | HSE 振盪器 | 8 MHz 外部晶振 |

---

## 韌體流程 / Firmware Flow

原始碼位於 [`Core/Src/main.c`](Core/Src/main.c)。

### 1. 電源管理 / Power management

裝置沒有硬體電源鎖存電路，`PC6` 只是一顆一般按鍵，MCU/BT 的 3V3 電源軌無法被硬體關閉，因此「關機」實際上是：切斷 IMU 的 1V8 電源 + 進入 STOP 低功耗模式，等待下一次長按喚醒。

- **冷開機**：上電後閃一次 LED，即進入 STOP 睡眠，等待長按（≥1.5s）才真正啟動。
- **開機**：`System_PowerOn()` 打開 1V8、初始化 ICM20948。
- **關機**（運行中長按）：`System_PowerOff()` 關閉 1V8、進入 STOP，喚醒後自動重新開機初始化。
- 狀態 LED（PC7）兼作 BLE 連線指示：未連線快閃（150ms）、已連線慢閃（800ms），連線狀態由監聽 HM19 模組主動回報的 `OK+CONN` / `OK+LOST` 文字判斷（模組的 STATE 腳未接到 MCU，因此無法直接讀取硬體狀態）。

### 2. 資料擷取與 AI 推論狀態機 / Acquisition & inference state machine

| `sys_state` | 意義 |
|---|---|
| `0` | Idle，等待指令 |
| `1` | 蒐集中，每個 IMU new-data-ready 中斷寫入 `raw_data_buffer` |
| `2` | 已完成一次推論，結果待手機端讀取（指令 `2`）後清空 |

- 視窗大小固定 `WINDOW_FRAMES = 297` frame × `NUM_AXES = 6`（accel xyz + gyro xyz）。
- 視窗蒐集滿後執行 `Apply_ETL()`：
  1. 對每軸做 Butterworth band-pass 二階 biquad 濾波（係數見 `butterworth_biquad_coeffs`）。
  2. 對濾波後訊號做 5 點（前後各 2 點）移動平均平滑。
- 前處理完的 `processed_buffer` 直接複製進 AI 模型輸入 buffer，呼叫 `ai_network_run()` 推論。
- 取 3 類輸出機率中最大者作為 `ai_best_class`（分期結果）。

---

## 藍牙通訊協定 / BLE UART Protocol

MCU 與手機 App 之間透過 HM19 BLE 模組以純文字 UART 協定溝通（USART2, 9600 baud, `\r\n` 結尾）。

### 手機 → MCU 指令 / Commands

| 指令 Byte | 動作 |
|---|---|
| `'0'` | Ping / 就緒檢查，MCU 回覆 `READY\r\n` |
| `'1'` | 開始蒐集一個視窗的 IMU 資料（僅在 `sys_state==0` 時生效） |
| `'2'` | 讀取推論結果（僅在 `sys_state==2` 時有效，讀取後回到 idle） |

### MCU → 手機訊息 / Notifications

| 訊息格式 | 說明 |
|---|---|
| `READY\r\n` | 回應指令 `'0'` |
| `MSG: Start Collecting...\r\n` | 開始蒐集視窗 |
| `TT_OK\r\n` | 一次視窗蒐集 + AI 推論完成，可送出 `'2'` 取結果 |
| `AI Run Error!\r\n` | 推論失敗，狀態回到 idle |
| `RESULT:stage_<n>\|<p0>,<p1>,<p2>\r\n` | 分期結果：`<n>` 為最高機率類別，`<p0..p2>` 為 3 類機率（softmax 輸出，小數 4 位） |
| `MSG: Dumping Processed Data...\r\n` … `D:<a1>,<a2>,...,<a6>\r\n` ×297 … `MSG: Dump Done.\r\n` | 逐 frame 輸出前處理後的 6 軸資料（除錯 / 資料蒐集用） |

此外，HM19 模組本身會主動送出未經 MCU 過濾的連線通知，MCU 會監看同一條 UART 判斷藍牙連線狀態：

| 模組通知 | 意義 |
|---|---|
| `OK+CONN` | 手機已連線 |
| `OK+LOST` | 連線中斷 |

---

## AI 模型 / AI Model

模型以 Keras 訓練（`CNN_LSTM.keras`），使用 **ST Edge AI Core (X-CUBE-AI 10.2.0)** 轉換為 C 程式碼部署（`X-CUBE-AI/App/network*.c/h`）。

| 項目 | 內容 |
|---|---|
| 模型結構 | Conv1D(64) → BN → ReLU → Conv1D(128) → BN → ReLU → MaxPool → Bidirectional LSTM(64×2) → GlobalAveragePooling → Dense(64, ReLU) → Dense(3, Softmax) |
| 輸入 Input | `f32(1, 297, 6)` — 297 個時間點 × 6 軸（accel xyz + gyro xyz） |
| 輸出 Output | `f32(1, 3)` — 3 類疲勞階段機率（softmax） |
| 參數量 Params | 133,571（521.76 KiB） |
| MACC | 22,407,536 |
| Weights (Flash) | 533,516 B |
| Activations (RAM) | 153,344 B |
| 目標晶片 Target | STM32F7 |

詳細層級報表見 [`X-CUBE-AI/App/network_generate_report.txt`](X-CUBE-AI/App/network_generate_report.txt)。

> 若要更換模型：以 STM32CubeMX 開啟 `tabletennis_fatigue_project_v4.ioc`，於 X-CUBE-AI 元件重新指定 `.keras` 模型並重新產生程式碼，即會覆寫 `X-CUBE-AI/App/network*` 系列檔案。`Core/Src/main.c` 中的 `WINDOW_FRAMES`、`NUM_AXES` 及輸出類別數（目前寫死為 3）需與新模型的輸入/輸出 shape 對應調整。

---

## 建置與燒錄 / Build & Flash

本專案由 **STM32CubeMX** 產生，目標工具鏈為 **Keil MDK-ARM V5.32**。

### 需求 / Prerequisites

- Keil MDK-ARM（含 STM32F7 Device Family Pack）
- ST-Link 或相容除錯燒錄器
- （若需修改硬體腳位/時脈/AI 模型）STM32CubeMX ≥ 6.14.0，並安裝 X-CUBE-AI 10.2.0 套件

### 步驟 / Steps

1. 用 Keil µVision 開啟 [`MDK-ARM/tabletennis_fatigue_project_v4.uvprojx`](MDK-ARM/tabletennis_fatigue_project_v4.uvprojx)。
2. Build（F7）。
3. 連接 ST-Link，Download（F8）燒錄至 STM32F767VI。
4. 上電後短按無效，需**長按電源鍵（PC6）≥ 1.5 秒**開機；LED（PC7）閃爍表示 BLE 未連線／已連線狀態。
5. 用手機 BLE App 連線 HM19（裝置名稱 `TEST_746`），依上方協定送出 `'1'` 開始蒐集、`'2'` 取回結果。

> 若需以 STM32CubeMX 重新產生程式碼，開啟 `tabletennis_fatigue_project_v4.ioc` → Generate Code。`Core/Src/main.c` 中的 `/* USER CODE */` 區塊（IMU 驅動、ETL、電源管理、藍牙協定邏輯）會被保留。

---

## 專案目錄結構 / Project Structure

```
├── Core/                         # STM32CubeMX 產生之核心程式（HAL 初始化 + main.c 應用邏輯）
│   ├── Inc/                      # 標頭檔（gpio/i2c/usart/main）
│   └── Src/                      # main.c 內含 IMU 驅動、ETL、電源管理、BLE 協定、AI 呼叫
├── Drivers/                      # STM32F7 HAL 與 CMSIS 底層驅動（ST 官方）
├── Middlewares/ST/AI/            # X-CUBE-AI runtime library
├── X-CUBE-AI/
│   ├── App/                      # AI 模型產生碼（network.c/h, network_data*.c/h）
│   │   └── network_generate_report.txt   # 模型層級/記憶體用量報表
│   └── constants_ai.h
├── .ai/                           # ST Edge AI Core 產生的模型 metadata（JSON）
├── MDK-ARM/                       # Keil µVision 專案檔（.uvprojx）與編譯輸出
├── TABLETENNIS_F7_RACKET_BLUETOOTH_V02-0323A/   # PCB 硬體資料（Schematic / Layout / Netlist）
└── tabletennis_fatigue_project_v4.ioc            # STM32CubeMX 專案設定檔（腳位/時脈/中介軟體）
```