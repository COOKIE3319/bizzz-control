# Bizzz Control · ESP32-S3 蜂鸣器简谱播放器

用一块 **ESP32-S3** + 一个三脚无源蜂鸣器，做成能播放数字简谱（简谱/键盘谱）的小播放器，
并配一个 **Web Bluetooth 网页控制台**：播放、上传、编辑、重命名、删除曲谱，实时进度条、
播放模式切换、还能像弹琴一样用键盘演奏。

> 曲谱不写在固件里：存成 `data/*.txt` 放在板载 LittleFS（约 12MB 分区），
> 网页上就能管理，换曲子不用重新烧固件。

## 功能

- 🎵 **简谱播放**：数字谱（`5///`、`+1`、`0` 休止）与键盘谱（`G///`、`Q///`）都支持
- 🎹 **和弦**：`(AGE)` 括号内一起按；单只蜂鸣器用「快速分解和弦」近似
- 🎼 **原神/光遇键盘谱**：`#STYLE keys` 模式下 `/` 变成换气休止，支持整首《春日影》
- 📁 **曲谱管理**：网页里上传 / 编辑 / 重命名 / 删除，板载存储约 12MB
- ▶️ **播放控制**：进度条可拖动、暂停/继续、上一首/下一首、停止
- 🔁 **播放模式**：自动连播 / 单曲循环 / 播完即停
- 🎹 **演奏键盘**：电脑键盘 `QWERTYU` / `ASDFGHJ` / `ZXCVBNM`（同原神风物之诗琴）
- 🔊 **开机音效**：上电自动播放 `boot.txt`（默认 DJI 风格开机音）
- 🧠 **双核 RTOS**：BLE 协议栈跑 core 0，播放器任务固定 core 1，互不阻塞
- 📡 **BLE 协议**：文本命令协议，带完整文档，方便自己写客户端

## 目录结构

```
.
├── ESP32S3 bizzzzzz/        # PlatformIO 固件工程（Arduino core 3.x）
│   ├── src/main.cpp         # 固件：曲谱解析 / 播放器 / BLE 服务 / 文件管理
│   ├── data/                # 曲谱：uploadfs 时打包进 LittleFS
│   │   ├── 00_boot.txt      # 开机音效（文件名以 boot.txt 结尾即生效）
│   │   ├── 01_genshin_keys.txt / 02_genshin_num.txt
│   │   └── 03_haruhikage.txt# 春日影（含和弦的完整键盘谱）
│   ├── partitions_16mb.csv  # 16MB flash 分区表（app 4MB + FS ~12MB）
│   ├── platformio.ini
│   └── README.md            # 固件详细文档（格式/协议/踩坑）
├── hardware/                # 硬件改造方案
│   └── multi_channel_square_mixing.md   # 方案B：多 GPIO 方波电阻混合（和弦）
└── bizzz control/           # 网页控制台（纯前端，Web Bluetooth）
    ├── index.html           # 单文件控制台
    ├── serve.sh             # 本地起 http 服务
    └── README.md            # 使用说明 + BLE 协议文档
```

## 硬件

| 蜂鸣器 | ESP32-S3 |
| :----- | :------- |
| VCC    | 3V3      |
| GND    | GND      |
| IO     | GPIO15（低电平触发，无源） |

> 想从"单音蜂鸣器"升级到"真和弦"，见 **[多 GPIO 方波电阻混合硬件方案](hardware/multi_channel_square_mixing.md)**：
> 3 路 LEDC 方波经等值电阻求和（= 4 电平 DAC），再驱动蜂鸣器，零固件改动即可搭。

## 快速开始

### 1) 固件

```bash
cd "ESP32S3 bizzzzzz"
pio run -t upload      # 编译并烧录固件
pio run -t uploadfs    # 把 data/ 曲谱打包上传到 LittleFS
pio device monitor     # 看串口日志（115200）
```

环境：PlatformIO Core 6.x + espressif32 平台 + Arduino core 3.x（3.1.0 实测）。
详见 [`ESP32S3 bizzzzzz/README.md`](ESP32S3%20bizzzzzz/README.md)。

### 2) 网页控制台

```bash
cd "bizzz control"
python3 -m http.server 8777     # 或 ./serve.sh
```

用 **Chrome / Edge** 打开 <http://localhost:8777>（或在线版 <https://cookie3319.github.io/bizzz-control/>），点「连接蓝牙」选 **BizzBuzzer**
（Web Bluetooth 要求 secure context，所以必须 localhost 或 https；Safari 不支持）。

详见 [`bizzz control/README.md`](bizzz%20control/README.md)（含完整 BLE 协议：UUID、
命令表、回包格式）。

## 简谱格式速查

```
#TITLE 曲名
#BPM 120
#STYLE keys          ← 键盘谱/原神谱模式：/ 变成换气休止
1 2 3 4 5 6 7        ← 数字谱 do~si（C 大调）
+1 / -1              ← 高/低八度
0                    ← 休止符（静音），0// = 静音一拍
(AGE) / (A G E)      ← 和弦，括号内一起按
5///                 ← 数字后 / 的个数 = 八分音符数（duration 模式）
```

内置示例：原神键盘谱、数字谱、春日影（含和弦与休止）。

## 技术要点

- **无源蜂鸣器发不同音高**：LEDC 输出 50% 占空比方波；注意 Arduino core 3.x 中
  已挂载引脚再 `ledcAttach` 会失败，换音要用 `ledcChangeFrequency`
- **和弦近似**：单蜂鸣器单音，和弦按 35ms/音 快速分解后保持最高音
- **双核任务**：Bluedroid 在 core 0，播放器 `xTaskCreatePinnedToCore` 到 core 1，
  命令队列 + 临界区解耦
- **BLE 文件传输**：列表带大小、`READ` 分片读取、`SAVE_BEGIN/DATA/END` 上传（hex 编码，
  单包 ≤20 字节，兼容默认 MTU）
- **16MB 分区**：`partitions_16mb.csv`，app 4MB + LittleFS ~11.94MB

## 许可

暂未添加 LICENSE 文件（默认保留所有权利）。如果想开源发布，建议补一个
MIT / Apache-2.0 许可证。
