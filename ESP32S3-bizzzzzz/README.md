# ESP32-S3 简谱播放器（曲谱外置版）

一块 ESP32-S3 + 一个三脚蜂鸣器。**曲谱不写在代码里**：工程 `data/` 目录下的
`*.txt` 会被打包成 LittleFS 镜像上传到板子上，上电后固件自动扫描、按文件名排序、
循环播放。换曲子只要重新上传文件系统，**不用重新编译固件**。

## 硬件

| 蜂鸣器 | ESP32-S3 |
| :----- | :------- |
| VCC    | 3V3      |
| GND    | GND      |
| IO     | GPIO15   |

实测：ESP32-S3 (QFN56) rev v0.2，8MB 内置 PSRAM，MAC `AA:BB:CC:DD:EE:FF`（示例，以实机为准），
串口 `/dev/cu.usbmodem101`（原生 USB-Serial/JTAG，VID:PID `303A:1001`）。
蜂鸣器为三脚、低电平触发、**无源**（用方波能奏出音高）。

## 快速开始

```bash
# 1) 编译并烧录固件
pio run -t upload

# 2) 把 data/ 目录下的曲谱打包上传到板载 LittleFS
pio run -t uploadfs

# 3) 看串口日志（Ctrl+C 退出）
pio device monitor
```

> 改曲谱时只需要重跑第 2 步（`uploadfs`）；只有改代码/配置才需要第 1 步。

播放顺序 = 曲谱文件名升序。`data/` 里想放几首放几首（上限 32 个文件），
每首播完停 1.2 秒，一轮结束停 2 秒从头再来。

## 蓝牙控制（Bizzz Control 网页）

固件带 BLE（设备名 `BizzBuzzer`），可以用浏览器网页控制：

```bash
cd "bizzz control"
python3 -m http.server 8777     # 或 ./serve.sh
```

Chrome / Edge 打开 <http://localhost:8777> → 点「连接蓝牙」→ 选 `BizzBuzzer`。

网页上可以：

- 📁 **板载曲谱管理**：读取设备 LittleFS 里的 *.txt，可播放 / 编辑 / 重命名 / 删除
- 📝 **曲谱编辑 / 上传**：贴文本 + 文件名 → 上传保存到板子（或上传并播放）；可回读已有曲谱修改再保存
- 🎹 **演奏**：按住音键/电脑键盘（QWERTYU / ASDFGHJ / ZXCVBNM = 高/中/低八度）
- ▶️ **直接播放指定音**
- 🎛 **底部播放条**：⏮ 上一首 / ▶⏸ 播放暂停 / ⏹ 停止 / ⏭ 下一首 + **可拖动的进度条**（显示已播/总长）
- 📶 文件走 BLE 传输：列表带文件大小，读取/上传按 hex 分片，单文件上限 4000 字节
- 🧾 收发日志，方便排错

固件里的播放器是**非阻塞**的：换曲、暂停、拖动进度都不影响蓝牙响应；
板载曲谱播完自动接下一首，蓝牙下发的曲谱播完即停。

### 双核任务划分（RTOS）

| 任务 | 所在核 | 职责 |
| :--- | :--- | :--- |
| Bluedroid 协议栈任务（ESP-IDF 创建） | **core 0** | BLE 收发、连接管理 |
| 播放器任务（`xTaskCreatePinnedToCore`，优先级 2） | **core 1** | 解析曲谱、推进进度、驱动蜂鸣器 |
| Arduino `loop`（默认） | core 1 | 只做 BLE 状态提示 / 空闲心跳 |

两个子系统之间只通过「命令队列 + 临界区」交互，播放再忙也不会卡蓝牙：
`BLE 回调(core0) --queuePush--> 播放器任务(core1) --bleNotify--> BLE`。
串口日志里可以直接看到：
`[task] BLE 回调运行在 core 0` / `[task] 播放逻辑执行于 core 1`。

协议、UUID、报错处理见 [`bizzz control/README.md`](bizzz%20control/README.md)。

> 默认开机**不自动播放**（等蓝牙指令）；想回到"上电自动循环播放"，
> 把 `src/main.cpp` 里的 `AUTO_PLAY_ON_BOOT` 改成 `1` 重新烧录。

### 开机音效

上电时会自动播放 LittleFS 里**文件名以 `boot.txt` 结尾**的曲谱（默认 `00_boot.txt`，
DJI 风格开机音效：三声上行短音 + 一声高音长音，约 2.1 秒），播完即停、不会自动
接下一首。想换音效：

1. 网页「板载曲谱」里点 `00_boot.txt` 的「编辑」
2. 改简谱文本（`#BPM` 控制快慢），「上传保存」
3. 下次上电就是新音效，不需要重新烧固件

## 曲谱 txt 格式

一个文件 = 一首曲子。纯文本，支持两种写法（同一文件里也可以混用）：

### 数字谱

```
5///+1///+1/+1/+3//+2///+1//+2//+3//+1///
```

- `1`~`7`：中音 do re mi fa sol la si（C 大调，1 = 262Hz）
- `(AGE)` / `(A G E)`：**和弦**，括号内按键一起按（原神键盘谱写法）
- `+1`~`+7`：高八度；`-1`~`-7`：低八度（前缀可叠加，如 `++1`）
- `0`：休止符（静音），后面 `/` 越多静音越久（`0//` = 静音一拍）

### 键盘谱（原神风物之诗琴 / 光遇琴的 21 键记法）

```
G///Q///Q/Q/E//W///Q//W//E//Q///Q//E//T///Y///
```

| 键位 | 含义 |
| :--- | :--- |
| `Q W E R T Y U` | 高八度 do~si |
| `A S D F G H J` | 中音 do~si |
| `Z X C V B N M` | 低音 do~si |

字母大小写都可以。

### 时值（节奏）

音符后面 `/` 的个数 = 八分音符数：

| 写法 | 时值（4/4 拍） |
| :--- | :--- |
| `5/` | 八分音符 |
| `5//` | 四分音符（1 拍） |
| `5///` | 附点四分（1.5 拍） |
| `5////` | 二分音符（2 拍） |
| 不带 `/` | 按 1 个八分音符处理 |

注意：所有时值都按比例缩放，所以**觉得整体太快/太慢时改 BPM 就行**，
节奏比例不会变。

### 元信息（可选，`#` 开头的行）

```
#TITLE 原神键盘谱示例
#BPM 120
# 这是一行注释，不会被解析
```

| 写法 | 含义 |
| :--- | :--- |
| `#BPM 120` | 速度，默认 120；范围 20~400 |
| `#TITLE 曲名` 或 `#曲名 曲名` | 曲名，只在串口日志显示 |
| `# 任意内容` | 注释 |

其他约定：小节线 `|`、空格、换行、中文等非 ASCII 字符都会被忽略（标题务必写在
`#` 开头的行里，否则字母会被当成音符解析）。

### 关于和弦

原神键盘谱里 `(AGE)` 表示三个键同时按下。但**单只蜂鸣器一次只能发一个频率**，
所以固件把和弦近似成「快速分解和弦」：和弦内各音按 35ms 依次奏出，然后保持最高音。
听感接近"扫弦和弦"，但不是真正的多音混合。想做真和弦需要多个蜂鸣器（各接一个
GPIO，用不同 LEDC 通道同时发声）或者换真正的喇叭+功放。

## 目录结构

```
ESP32S3 bizzzzzz/
├── platformio.ini          # 工程配置（USB CDC、LittleFS、串口）
├── src/main.cpp            # 固件：文件系统扫描 + 曲谱解析 + 蜂鸣器驱动
├── data/                   # ★ 曲谱放这里，uploadfs 时打包进 LittleFS
│   ├── 00_boot.txt         # 开机音效（文件名以 boot.txt 结尾即生效，上电自动播）
│   ├── 01_genshin_keys.txt # 键盘谱示例（原神键盘谱）
│   ├── 03_haruhikage.txt   # 春日影（含和弦的完整键盘谱）
│   └── 02_genshin_num.txt  # 数字谱示例（原神数字谱）
└── README.md

../bizzz control/           # 网页蓝牙控制台（独立目录）
├── index.html              # 单文件 Web Bluetooth 控制页
├── serve.sh                # 一键起本地 http 服务
└── README.md               # 使用说明 + BLE 协议文档
```

## 可调参数（`src/main.cpp` 顶部）

| 宏 | 默认 | 说明 |
| :-- | :-- | :-- |
| `BUZZER_PIN` | 15 | 蜂鸣器 IO |
| `BUZZER_LOW_TRIGGER` | 1 | 低电平触发 |
| `BUZZER_IS_ACTIVE` | 0 | 0=无源(能奏音高) 1=有源(只有节奏) |
| `DEFAULT_BPM` | 120 | 曲谱没写 `#BPM` 时的默认速度 |
| `ARTIC_MS` | 18 | 相邻音符断音时长 |
| `MAX_NOTES` / `MAX_FILES` | 4000 / 64 | 单曲音符上限 / 文件数上限 |
| 存储 | ~11.94MB | LittleFS 分区大小（16MB flash 分区表） |
| 单文件上限 | 16KB | 受上传缓冲限制 |
| `AUTO_PLAY_ON_BOOT` | 0 | 1 = 上电自动播放第一首；0 = 只等蓝牙控制 |
| `POS_NOTIFY_MS` | 300 | 进度（POS）上报间隔 |
| `BLE_NAME` | `BizzBuzzer` | 蓝牙广播名称 |

## 串口日志示例

```
==================================================
 ESP32-S3 简谱播放器 · 曲谱外置版
 蜂鸣器: GPIO15 · 低电平触发 · 无源(可奏音高)
==================================================
[fs] LittleFS 已挂载: 总 1572864 字节, 已用 1184 字节
[fs] 找到 2 个 .txt 曲谱
     - 01_genshin_keys.txt
     - 02_genshin_num.txt
开始播放 ...

=============== 第 1 轮：2 首 ===============
(1/2)
♪ 原神键盘谱示例  <键盘谱>
   文件: 01_genshin_keys.txt | 56 音 (键盘 56 / 数字 0) | 约 29.5s @ 120 BPM
(2/2)
♪ 原神数字谱示例  <数字谱>
   文件: 02_genshin_num.txt | 56 音 (键盘 56 / 数字 0) | 约 29.5s @ 120 BPM
```

## 踩坑记录

- **串口看不到 `Serial` 输出**：本板 `ARDUINO_USB_MODE=1`，PlatformIO 默认
  `ARDUINO_USB_CDC_ON_BOOT=0`，`Serial` 会落到 UART0（GPIO43/44，USB 上收不到），
  只有 ESP_LOG 能通过 USB-JTAG console 看到。已在 `platformio.ini` 加
  `build_flags = -DARDUINO_USB_CDC_ON_BOOT=1` 解决。
- **卡在 "连上了一个新设备" 之类的弹窗 / 上传失败**：USB-Serial/JTAG 端口
  `/dev/cu.usbmodem101` 同一时刻只能被一个程序占用，先关掉串口监视器再烧录。
- **文件系统分区**：板子实际是 **16MB flash**，现在用自定义分区表
  `partitions_16mb.csv`（app 4MB + LittleFS **约 11.94MB**），
  `board_build.filesystem = littlefs` 让 `pio run -t uploadfs` 生成并烧写镜像。
- **LEDC API**：本环境实际是 espressif32 `53.03.10` + Arduino core `3.1.0`，
  `ledcSetup/ledcAttachPin` 已移除；代码用 `#if ESP_ARDUINO_VERSION_MAJOR >= 3`
  同时兼容 3.x（`ledcAttach/ledcWrite/ledcDetach`）与 2.x。
- **换音高不生效（重要坑）**：core 3.x 里对**已挂载的引脚**再次 `ledcAttach()`
  会返回 false，如果直接 return，声音会一直停在第一次挂载的频率上（表现为"按 A
  再按 S 永远是 A 的音"）。正确做法是已挂载时调用
  `ledcChangeFrequency(pin, freq, 10)`，只有首次才 attach；调试可用
  `ledcReadFreq(pin)` 读实际输出频率（本工程提供了 `GETFREQ` 蓝牙命令验证）。
- **USB-JTAG 自动复位**：抓日志时不要手动乱拉 DTR/RTS，否则芯片会被留在
  download 模式、串口无输出；直接用 `pio device monitor` 即可。
- **换分区表会格式化文件系统**：`uploadfs` 之后板上原有曲谱会被 `data/` 覆盖，
  改分区前先备份。
