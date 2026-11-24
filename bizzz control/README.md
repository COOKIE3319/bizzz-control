# Bizzz Control · 网页蓝牙控制台

ESP32-S3 简谱播放器的网页控制端：通过 **Web Bluetooth** 连接板子（BLE 名称
`BizzBuzzer`），播放板载曲谱、把曲谱文本发到板子播放、像弹钢琴一样演奏，并且有
**播放进度条 / 暂停 / 上一首 / 下一首 / 停止**。

## 启动

```bash
cd "bizzz control"
python3 -m http.server 8777      # 或 ./serve.sh
```

浏览器打开 **<http://localhost:8777>**（Windows 用 `python -m http.server 8777`）。

- 必须用 **Chrome / Edge**（Safari 不支持 Web Bluetooth）
- 必须通过 `http://localhost` 或 `https://` 打开（Web Bluetooth 只在安全上下文可用）
- 板子上电后 BLE 一直广播；串口里能看到 `[ble] 广播中，设备名: BizzBuzzer`

## 界面功能

| 区域 | 说明 |
| :--- | :--- |
| 连接蓝牙 | 弹窗选 `BizzBuzzer`；连上自动拉取设备信息、曲谱列表和 BPM |
| 板载曲谱 | 「刷新列表」读 LittleFS 里的 *.txt；每首可 **播放 / 编辑 / 重命名 / 删除**；文件名以 `boot.txt` 结尾的会标「开机音效」 |
| 曲谱编辑 / 上传 | 文件名 + 文本 →「上传保存」写入板子；「上传并播放」保存后立即播放；点列表里的「编辑」可读回修改再保存 |
| 演奏 | 三行音键（高/中/低），按住持续发声；电脑键盘 `QWERTYU`/`ASDFGHJ`/`ZXCVBNM` 对应高/中/低八度 |
| 直接播放指定音 | 下拉选音符 + 时长 →「播放」 |
| **底部播放条** | ⏮ 上一首 · ▶/⏸ 播放/暂停 · ⏹ 停止 · ⏭ 下一首；**进度条可点击/拖动跳转**；显示当前曲名和 `已播 / 总长`；右侧**播放模式**下拉（自动连播 / 单曲循环 / 播完即停） |
| 日志 | 蓝牙收发与设备回包，排错用 |

播完一首会自动接下一首（板载列表循环）；蓝牙下发的曲谱播完则停下。

曲谱格式：数字谱 `5///`、`+1///`，键盘谱 `G///`、`Q///`；音符后 `/` 的个数 =
八分音符数；`#BPM 120`、`#TITLE 曲名`、`# 注释`。

**单文件上限 16000 字节**（整首《春日影》约 3.8KB）。

**和弦 `(AGE)`**：括号内按键表示一起按下（原神键盘谱常见写法），也接受 `(A G E)` 带空格。
单只蜂鸣器无法真正多音混合，固件用「快速分解和弦」（每音 35ms 依次奏出、之后保持最高音）来近似。

**休止符 `0`（静音）**：`0` 就是静音音符，后面的 `/` 决定静音时长——
`0/` = 静音八分音符、`0//` = 静音一拍（四分音符）、`0////` = 静音两拍；
例：`3// 5// 0// +1//` = mi → sol →（静音一拍）→ 高音 do。

## 蓝牙协议（自己写客户端时看）

| 项 | 值 |
| :--- | :--- |
| 设备名 | `BizzBuzzer` |
| Service UUID | `b1d0c0de-0001-4000-8000-00805f9b34fb` |
| RX（写入） | `b1d0c0de-0002-4000-8000-00805f9b34fb` |
| TX（通知） | `b1d0c0de-0003-4000-8000-00805f9b34fb` |

命令是 **UTF-8 文本，一行一条，以 `\n` 结尾**；一条命令可以分多次写入（设备会累积
到 `\n` 才处理），所以 **每次写入 ≤ 20 字节** 最稳妥（BLE 默认 MTU 23）。长曲谱按
`SCORE_ADD <片段>` + `SCORE_NL` 分批发。

| 命令 | 说明 |
| :--- | :--- |
| `ON <note>` / `OFF` | 持续发声 / 停止（`1` `+1` `-1` `Q` `A` `Z` …） |
| `PLAY <note> <ms>` | 单发音，20~5000ms |
| `BPM <n>` | 速度 20~400，回 `INFO` |
| `SCORE_BEGIN` | 清空曲谱缓冲 |
| `SCORE_ADD <片段>` | 追加曲谱文本（可多次） |
| `SCORE_NL` | 往缓冲追加换行（保留 `#TITLE` / `#BPM` / 注释行结构） |
| `SCORE_PLAY` | 解析并播放缓冲里的曲谱 |
| `LIST` | 请求板载曲谱列表 |
| `FILE <idx>` | 播放板载第 idx 首 |
| `READ <idx> <off>` | 读取文件分片，回 `DATA <off> <hex>`，越界回 `EOF <len>` |
| `SAVE_BEGIN <name>` | 开始上传（新建或覆盖），回 `OK save_begin <name>` |
| `SAVE_DATA <hex>` | 追加文件内容（hex 编码，每片 ≤40 字节） |
| `SAVE_END` | 落盘，回 `SAVED <idx> <name>` |
| `DELETE <idx>` | 删除，回 `OK deleted <name>` |
| `RENAME <idx> <newname>` | 重命名，回 `OK renamed <newname>` |
| `NEXT` / `PREV` | 下一首 / 上一首 |
| `PAUSE` / `RESUME` | 暂停 / 继续 |
| `STOP` | 停止（位置归零） |
| `SEEK <ms>` | 跳到指定毫秒 |
| `MODE <n>` | 播放模式：`0` 自动连播 / `1` 单曲循环 / `2` 播完即停，回 `INFO` |
| `GETINFO` | 设备信息 |
| `GETFREQ` | 返回 LEDC 当前实际输出频率，回 `FREQ <hz>`（调试用，未发声为 0） |

回包（notify，**每条以 `\n` 结尾**，客户端按行切分）：

| 回包 | 说明 |
| :--- | :--- |
| `INFO fw=1.0 files=2 bpm=120 state=0` | 设备信息 |
| `FILE <idx> <size> <name>` | 曲谱列表项；最后一行 `LIST_END <count>` |
| `DATA <off> <hex>` / `EOF <len>` | 文件内容分片 / 读取结束 |
| `SAVED <idx> <name>` / `OK <...>` | 上传、删除、重命名结果 |
| `TRACK <idx> <name>` | 开始播放某首；`idx=255` 表示蓝牙下发的曲谱 |
| `POS <posMs> <totalMs> <state>` | 播放进度，约每 300ms 一条；state 0=停止 1=播放 2=暂停 |
| `DONE <name>` | 播放结束 |
| `FREQ <hz>` | `GETFREQ` 的回应，当前实际音高 |
| `ERR <msg>` | 错误 |

最小 Web Bluetooth 示例：

```js
const SERVICE = 'b1d0c0de-0001-4000-8000-00805f9b34fb';
const RX = 'b1d0c0de-0002-4000-8000-00805f9b34fb';
const TX = 'b1d0c0de-0003-4000-8000-00805f9b34fb';

const device = await navigator.bluetooth.requestDevice({
  filters: [{ namePrefix: 'Bizz' }],
  optionalServices: [SERVICE],
});
const service = await (await device.gatt.connect()).getPrimaryService(SERVICE);
const rx = await service.getCharacteristic(RX);
const tx = await service.getCharacteristic(TX);

await tx.startNotifications();
tx.addEventListener('characteristicvaluechanged', (e) => {
  for (const line of new TextDecoder().decode(e.target.value).split('\n')) {
    if (line) console.log('←', line);          // 设备回包（\n 结尾）
  }
});

async function send(line) {                     // 单包 ≤20 字节，长行自动分包
  const data = new TextEncoder().encode(line + '\n');
  for (let i = 0; i < data.length; i += 20) {
    await rx.writeValue(data.subarray(i, i + 20));
  }
}

await send('GETINFO');
await send('FILE 0');       // 播放第 1 首，随后会收到 TRACK / POS
await send('PAUSE');
await send('RESUME');
await send('SEEK 5000');
```

## 常见问题

| 现象 | 原因 / 处理 |
| :--- | :--- |
| 点「连接蓝牙」没有弹窗 | 不是 Chrome/Edge，或页面不是 localhost/https |
| 选不到 `BizzBuzzer` | 板子没在广播（串口看 `[ble] 广播中`）、离太远、已被别的设备连着 |
| 列表/状态一直不出来 | 旧版固件回包不带换行；本版固件已修（回包以 `\n` 结尾），更新固件后刷新页面 |
| 上传/发送慢 | 按分包传输（自适应 20~160 字节），日志里有字节进度，属正常 |
| 上传后听不到 | 确认日志里有 `SAVED`；文件列表里点「播放」试试 |
| 想改开机音效 | 编辑 `00_boot.txt`（要保留 `boot.txt` 结尾）→ 上传保存，下次上电生效；`#BPM` 控制快慢 |
| 曲谱会不会丢 | 编辑框有本地草稿自动保存（localStorage）；但 **`uploadfs`/换分区会覆盖板上文件**，重要曲谱记得导出文本备份 |
| 想要停顿/间隔 | 用休止符 `0`，例如 `3// 5// 0// +1//`；`0` 后面 `/` 越多静音越久 |
| 发送成功但不响 | 查蜂鸣器接线（VCC/GND/IO→GPIO15）、日志里是否有 `TRACK` / `POS` |
| 曲谱太长 | 设备缓冲 4000 字节，超出会截断，拆成多个文件 |
| 进度条不动 | 只在播放时有 `POS` 上报；暂停/停止时进度停住属正常 |
| 同时按多个键后一直响 | 已修：Chrome 不允许并发 GATT 写（并发 `writeValue` 会被丢弃，`OFF` 丢了就长鸣）。现在所有写入串行排队，并且空闲时每 2 秒补发一次 `OFF` 兜底 |
