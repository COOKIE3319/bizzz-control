#include <Arduino.h>
#include <LittleFS.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEService.h>
#include <BLECharacteristic.h>
#include <BLE2902.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/* ============================================================================
 *  ESP32-S3 简谱播放器 · 蓝牙控制版（带播放进度 / 暂停 / 上下曲）
 * ----------------------------------------------------------------------------
 *  硬件：蜂鸣器 VCC -> 3V3   GND -> GND   IO -> GPIO15（低电平触发 / 无源）
 *
 *  BLE：
 *    设备名         BizzBuzzer
 *    Service UUID   b1d0c0de-0001-4000-8000-00805f9b34fb
 *    RX (write)     b1d0c0de-0002-4000-8000-00805f9b34fb
 *    TX (notify)    b1d0c0de-0003-4000-8000-00805f9b34fb
 *
 *  协议（UTF-8 文本，一行一条，以 \n 结尾；单条可分包多次写入）：
 *    ON <note> / OFF           持续发声 / 停止
 *    PLAY <note> <ms>          单发音
 *    BPM <n>                   设置速度 20~400
 *    SCORE_BEGIN/ADD/NL/PLAY   通过蓝牙下发曲谱并播放（NL = 换行）
 *    LIST                      板载曲谱列表（含文件大小）
 *    FILE <idx>                播放第 idx 首
 *    READ <idx> <off>          读取文件内容（每次 40 字节，hex 返回）
 *    SAVE_BEGIN <name>         新建/覆盖一个曲谱文件
 *    SAVE_DATA <hex>           追加内容（hex 编码）
 *    SAVE_END                  落盘
 *    DELETE <idx>              删除曲谱
 *    RENAME <idx> <newname>    重命名曲谱
 *    NEXT / PREV               下一首 / 上一首
 *    PAUSE / RESUME / STOP     暂停 / 继续 / 停止
 *    SEEK <ms>                 拖动进度
 *    GETINFO                   设备信息
 *  回包（每条以 \n 结尾）：
 *    INFO fw=1.0 files=N bpm=B
 *    FILE <idx> <size> <name> … LIST_END <N>
 *    DATA <off> <hex> / EOF <len>   文件内容分片
 *    SAVED <idx> <name> / OK … / ERR …
 *    TRACK <idx> <name>        开始播放某首（idx=255 表示蓝牙下发的曲谱）
 *    POS <posMs> <totalMs> <state>   state: 0=停止 1=播放中 2=暂停
 *    DONE <name> / ERR <msg>
 * ============================================================================ */

/* ------------------------------ 硬件配置 ------------------------------ */
#define BUZZER_PIN          15
#define BUZZER_LOW_TRIGGER  1
#define BUZZER_IS_ACTIVE    0

/* ------------------------------ 播放配置 ------------------------------ */
#define DEFAULT_BPM         120
#define ARTIC_MS            18
#define MAX_NOTES           4000
#define MAX_FILES           64
#define POS_NOTIFY_MS       300          // 进度上报间隔
#define AUTO_PLAY_ON_BOOT   0            // 1 = 上电自动播放第一首

/* ------------------------------ BLE 配置 ------------------------------ */
#define BLE_NAME            "BizzBuzzer"
#define UUID_SERVICE        "b1d0c0de-0001-4000-8000-00805f9b34fb"
#define UUID_RX             "b1d0c0de-0002-4000-8000-00805f9b34fb"
#define UUID_TX             "b1d0c0de-0003-4000-8000-00805f9b34fb"
#define CMD_QUEUE_LEN       16
#define SCORE_BUF_LEN       16384

/* ------------------------------ 音高表 ------------------------------ */
static const uint16_t NOTE_FREQ[8] = {
    0, 262, 294, 330, 349, 392, 440, 494,   // 0 休止, 1~7 = do~si (C4 起)
};

#if ESP_ARDUINO_VERSION_MAJOR < 3
static const uint8_t LEDC_CH = 0;
#endif
static bool s_toneAttached = false;

/* --------------------------- 蜂鸣器底层驱动 --------------------------- */
static inline int buzzerIdleLevel()  { return BUZZER_LOW_TRIGGER ? HIGH : LOW;  }
static inline int buzzerDriveLevel() { return BUZZER_LOW_TRIGGER ? LOW  : HIGH; }

static void buzzerOff() {
#if !BUZZER_IS_ACTIVE
    if (s_toneAttached) {
    #if ESP_ARDUINO_VERSION_MAJOR >= 3
        ledcDetach(BUZZER_PIN);
    #else
        ledcDetachPin(BUZZER_PIN);
    #endif
        s_toneAttached = false;
    }
#endif
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, buzzerIdleLevel());
}

// 返回是否成功设置该频率
static bool buzzerOn(uint16_t freq) {
#if BUZZER_IS_ACTIVE
    (void)freq;
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, buzzerDriveLevel());
    return true;
#else
    #if ESP_ARDUINO_VERSION_MAJOR >= 3
    // 3.x：引脚已挂载时只能改定时器频率，再次 ledcAttach 会失败
    if (s_toneAttached) {
        return ledcChangeFrequency(BUZZER_PIN, freq, 10) != 0;
    }
    if (!ledcAttach(BUZZER_PIN, freq, 10)) {
        ledcDetach(BUZZER_PIN);              // 兜底：清掉残留挂载再试一次
        if (!ledcAttach(BUZZER_PIN, freq, 10)) return false;
    }
    ledcWrite(BUZZER_PIN, 512);
    #else
    if (s_toneAttached) {                    // 2.x：通道可重复配置
        ledcSetup(LEDC_CH, freq, 10);
        ledcWrite(LEDC_CH, 512);
        return true;
    }
    ledcSetup(LEDC_CH, freq, 10);
    ledcAttachPin(BUZZER_PIN, LEDC_CH);
    ledcWrite(LEDC_CH, 512);
    #endif
    s_toneAttached = true;
    return true;
#endif
}

// 当前正在发声的频率（0 = 静音），用于只在变化时重配 LEDC
static uint16_t s_liveFreq = 0;

static void setTone(uint16_t freq) {
    if (freq == s_liveFreq) return;
    if (freq) {
        if (buzzerOn(freq)) s_liveFreq = freq;
    } else {
        buzzerOff();
        s_liveFreq = 0;
    }
}

/* ------------------------------ 数据结构 ------------------------------ */
#define CHORD_MAX        4      // 一个和弦最多几个键
#define CHORD_STRUM_MS   35     // 单只蜂鸣器：和弦按 35ms/音 快速分解后保持最高音

struct NoteEvent {
    uint16_t f[CHORD_MAX];      // 和弦各音频率；n=0 表示休止
    uint8_t  n;                 // 音数（1 = 单音）
    uint16_t durMs;
};
static NoteEvent s_notes[MAX_NOTES];

static String   s_files[MAX_FILES];
static uint8_t  s_fileCount = 0;
static bool     s_fsOk = false;
static uint16_t s_bpm = DEFAULT_BPM;

/* 蓝牙下发的曲谱文本缓冲 */
static char   s_scoreBuf[SCORE_BUF_LEN + 1];
static size_t s_scoreLen = 0;

/* 文件上传缓冲（SAVE_BEGIN / SAVE_DATA / SAVE_END） */
#define UPLOAD_BUF_LEN 16384
static char   s_upBuf[UPLOAD_BUF_LEN + 1];
static size_t s_upLen = 0;
static String s_upName;

/* BLE 命令队列（BLE 任务 -> loop 任务） */
enum CmdKind : uint8_t {
    CMD_FILE_PLAY = 1,
    CMD_SCORE_PLAY,
    CMD_PAUSE,
    CMD_RESUME,
    CMD_STOP,
    CMD_NEXT,
    CMD_PREV,
    CMD_SEEK,
    CMD_PLAY_NOTE,
};
struct CmdMsg {
    uint8_t  kind;
    uint32_t a;
    uint16_t b;
};
static CmdMsg            s_cmdQ[CMD_QUEUE_LEN];
static volatile uint8_t  s_cmdHead = 0;
static volatile uint8_t  s_cmdTail = 0;
static portMUX_TYPE      s_cmdMux = portMUX_INITIALIZER_UNLOCKED;

static bool queuePush(uint8_t kind, uint32_t a, uint16_t b) {
    bool ok = false;
    portENTER_CRITICAL(&s_cmdMux);
    uint8_t next = (uint8_t)((s_cmdHead + 1) % CMD_QUEUE_LEN);
    if (next != s_cmdTail) {
        s_cmdQ[s_cmdHead].kind = kind;
        s_cmdQ[s_cmdHead].a = a;
        s_cmdQ[s_cmdHead].b = b;
        s_cmdHead = next;
        ok = true;
    }
    portEXIT_CRITICAL(&s_cmdMux);
    return ok;
}

static bool queuePop(CmdMsg *m) {
    bool ok = false;
    portENTER_CRITICAL(&s_cmdMux);
    if (s_cmdTail != s_cmdHead) {
        *m = s_cmdQ[s_cmdTail];
        s_cmdTail = (uint8_t)((s_cmdTail + 1) % CMD_QUEUE_LEN);
        ok = true;
    }
    portEXIT_CRITICAL(&s_cmdMux);
    return ok;
}

/* ------------------------------ 播放器状态 ------------------------------ */
enum PlayState : uint8_t { ST_IDLE = 0, ST_PLAYING = 1, ST_PAUSED = 2 };
enum PlayMode : uint8_t { MODE_AUTO = 0, MODE_REPEAT = 1, MODE_ONCE = 2 };  // 连播/单曲循环/播完即停
enum PlaySource : uint8_t { SRC_NONE = 0, SRC_FILE = 1, SRC_BLE = 2 };

static PlayState  s_state = ST_IDLE;
static PlaySource s_src = SRC_NONE;
static uint16_t   s_trackIdx = 0;          // 板载曲谱下标；BLE 曲谱为 255
static String     s_trackName;
static uint16_t   s_noteCount = 0;
static uint16_t   s_noteIdx = 0;
static uint32_t   s_noteStart = 0;
static uint32_t   s_posMs = 0;
static uint32_t   s_totalMs = 0;
static uint32_t   s_lastTick = 0;
static uint32_t   s_lastPosNotify = 0;
static bool       s_isBootSound = false;   // 正在播开机音效：播完就停，不自动接下一首
static PlayMode   s_mode = MODE_AUTO;      // 播放模式：0=自动连播 1=单曲循环 2=播完即停

/* ------------------------------ BLE 状态 ------------------------------ */
static BLEServer         *s_bleServer = nullptr;
static BLECharacteristic *s_bleTx = nullptr;
static volatile bool      s_bleConnected = false;
static volatile bool      s_bleAnnounced = false;

static void bleNotify(const String &msg) {
    if (!s_bleTx || !s_bleConnected) return;
    String line = msg;
    line += '\n';                  // 统一以换行结尾，客户端按行切分
    s_bleTx->setValue(line);
    s_bleTx->notify();
}

static void notifyPos(bool force = false) {
    uint32_t now = millis();
    if (!force && now - s_lastPosNotify < POS_NOTIFY_MS) return;
    s_lastPosNotify = now;
    bleNotify("POS " + String(s_posMs) + " " + String(s_totalMs) +
              " " + String((unsigned)s_state));
}

/* ------------------------------ 曲谱解析 ------------------------------ */
static bool keyToNote(char c, uint8_t *degree, int8_t *octave) {
    static const char ROW_HI[8]  = "QWERTYU";
    static const char ROW_MID[8] = "ASDFGHJ";
    static const char ROW_LO[8]  = "ZXCVBNM";

    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (uint8_t i = 0; i < 7; i++) {
        if (c == ROW_HI[i])  { *degree = i + 1; *octave =  1; return true; }
        if (c == ROW_MID[i]) { *degree = i + 1; *octave =  0; return true; }
        if (c == ROW_LO[i])  { *degree = i + 1; *octave = -1; return true; }
    }
    return false;
}

static uint16_t scaleFreq(uint8_t degree, int8_t octave) {
    if (degree < 1 || degree > 7) return 0;
    uint32_t f = NOTE_FREQ[degree];
    if (octave > 0)      f <<= octave;
    else if (octave < 0) f >>= (-octave);
    if (f < 20)    f = 20;
    if (f > 20000) f = 20000;
    return (uint16_t)f;
}

static bool parseSingleNote(const char *s, uint16_t *freq) {
    while (*s == ' ') s++;
    int8_t octave = 0;
    while (*s == '+' || *s == '-') {
        octave += (*s == '+') ? 1 : -1;
        s++;
    }
    if (*s >= '1' && *s <= '7') {
        *freq = scaleFreq((uint8_t)(*s - '0'), octave);
        return *freq > 0;
    }
    uint8_t degree = 0;
    int8_t  keyOct = 0;
    if (keyToNote(*s, &degree, &keyOct)) {
        *freq = scaleFreq(degree, (int8_t)(octave + keyOct));
        return *freq > 0;
    }
    return false;
}

static void parseMeta(const String &src, uint16_t *bpm, String *title, bool *keyStyle = nullptr) {
    int i = 0;
    const int len = (int)src.length();
    while (i < len) {
        int eol = i;
        while (eol < len && src[eol] != '\n') eol++;
        String line = src.substring(i, eol);
        i = eol + 1;
        line.trim();
        if (!line.startsWith("#")) continue;

        String body = line.substring(1);
        body.trim();
        String upper = body;
        upper.toUpperCase();

        if (upper.startsWith("BPM")) {
            long v = body.substring(3).toInt();
            if (v >= 20 && v <= 400) *bpm = (uint16_t)v;
        } else if (upper.startsWith("STYLE")) {
            String v = body.substring(5);
            v.trim();
            v.toUpperCase();
            if (keyStyle) *keyStyle = (v == "KEYS");
        } else if (upper.startsWith("TITLE") || body.startsWith("曲名") || body.startsWith("标题")) {
            int cut = 0;
            if (upper.startsWith("TITLE")) cut = 5;
            else cut = 6;
            String t = body.substring(cut);
            t.trim();
            if (t.startsWith(":") || t.startsWith("：")) {
                t = t.substring(t.startsWith(":") ? 1 : 3);
                t.trim();
            }
            if (t.length()) *title = t;
        }
    }
}

static uint16_t parseScore(const String &src, uint16_t bpm, bool keyStyle,
                           uint16_t *keyCount, uint16_t *numCount) {
    const uint32_t eighthMs = 60000UL / bpm / 2;
    uint16_t n = 0;
    uint16_t unknown = 0;
    bool lineStart = true;
    int i = 0;
    const int len = (int)src.length();

    while (i < len) {
        char c = src[i];

        // #STYLE keys 模式：斜杠本身是休止（换气），合并连续的斜杠
        if (keyStyle && c == '/') {
            uint32_t units = 0;
            while (i < len && src[i] == '/') { units++; i++; }
            uint32_t dur = units * eighthMs;
            if (dur > 60000UL) dur = 60000UL;
            if (n < MAX_NOTES) {
                s_notes[n].n = 0;
                s_notes[n].durMs = (uint16_t)dur;
                n++;
            }
            continue;
        }


        if (c == '\n') { lineStart = true; i++; continue; }
        if (c == '\r' || c == '\t' || c == ' ' || c == '|' || c == '/') { i++; continue; }

        uint8_t uc = (uint8_t)c;
        if (uc >= 0x80) { i++; continue; }

        if (c == '#' && lineStart) {
            while (i < len && src[i] != '\n') i++;
            continue;
        }
        lineStart = false;

        int8_t octave = 0;
        while (i < len && (src[i] == '+' || src[i] == '-')) {
            octave += (src[i] == '+') ? 1 : -1;
            i++;
        }
        if (i >= len) break;

        c = src[i];
        uint16_t chordF[CHORD_MAX];
        uint8_t  chordN = 0;

        if (c == '(') {
            // 和弦：(AGE) / (A G E) / (ZBDQ)
            i++;
            while (i < len && src[i] != ')') {
                char ch = src[i];
                if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == ',') { i++; continue; }
                int8_t koct = 0;
                while (i < len && (src[i] == '+' || src[i] == '-')) {
                    koct += (src[i] == '+') ? 1 : -1;
                    i++;
                }
                ch = src[i];
                if (ch >= '1' && ch <= '7') {
                    uint16_t f = scaleFreq((uint8_t)(ch - '0'), koct);
                    if (f && chordN < CHORD_MAX) chordF[chordN++] = f;
                    i++;
                    if (numCount) (*numCount)++;
                } else {
                    uint8_t dg = 0;
                    int8_t  ko = 0;
                    if (keyToNote(ch, &dg, &ko)) {
                        uint16_t f = scaleFreq(dg, (int8_t)(koct + ko));
                        if (f && chordN < CHORD_MAX) chordF[chordN++] = f;
                        if (keyCount) (*keyCount)++;
                    }
                    i++;
                }
            }
            if (i < len && src[i] == ')') i++;
            if (chordN == 0) continue;              // 空括号，跳过
        } else {
            uint8_t degree = 0;
            int8_t  keyOct = 0;
            if (c >= '1' && c <= '7') {
                degree = (uint8_t)(c - '0');
                i++;
                if (numCount) (*numCount)++;
            } else if (c == '0') {
                degree = 0;
                i++;
                if (numCount) (*numCount)++;
            } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
                if (!keyToNote(c, &degree, &keyOct)) { i++; unknown++; continue; }
                octave += keyOct;
                i++;
                if (keyCount) (*keyCount)++;
            } else {
                i++;
                unknown++;
                continue;
            }
            if (degree >= 1) chordF[chordN++] = scaleFreq(degree, octave);  // 0 = 休止
        }

        uint32_t units = 1;
        if (!keyStyle) {
            units = 0;
            while (i < len && src[i] == '/') { units++; i++; }
            if (units == 0) units = 1;
        }

        uint32_t dur = units * eighthMs;
        if (dur > 60000UL) dur = 60000UL;

        if (n < MAX_NOTES) {
            NoteEvent &ev = s_notes[n];
            ev.n = chordN;
            for (uint8_t k = 0; k < chordN; k++) ev.f[k] = chordF[k];
            ev.durMs = (uint16_t)dur;
            n++;
        } else {
            Serial.println("  [!] 音符数超过上限，后面的被截断");
            break;
        }
    }

    if (unknown) Serial.printf("  [!] 跳过了 %u 个无法识别的字符\n", unknown);
    return n;
}

/* ---------------------------- 文件系统辅助 ---------------------------- */
static String baseName(const String &path) {
    int p = path.lastIndexOf('/');
    return (p >= 0) ? path.substring(p + 1) : path;
}

static void scanScoreFiles() {
    s_fileCount = 0;
    File root = LittleFS.open("/");
    if (!root) {
        Serial.println("[x] 无法打开 LittleFS 根目录");
        return;
    }
    File e = root.openNextFile();
    while (e && s_fileCount < MAX_FILES) {
        if (!e.isDirectory()) {
            String name = baseName(String(e.name()));
            String lower = name;
            lower.toLowerCase();
            if (lower.endsWith(".txt") && !name.startsWith(".")) {
                s_files[s_fileCount++] = "/" + name;
            }
        }
        e = root.openNextFile();
    }
    root.close();

    for (uint8_t i = 1; i < s_fileCount; i++) {
        String key = s_files[i];
        int j = (int)i - 1;
        while (j >= 0 && s_files[j] > key) {
            s_files[j + 1] = s_files[j];
            j--;
        }
        s_files[j + 1] = key;
    }
}

/* ---------------------------- 曲谱文件管理 ---------------------------- */
static String sanitizeName(String n) {
    n.trim();
    String out;
    for (size_t i = 0; i < n.length(); i++) {
        char c = n[i];
        if (c == '/' || c == '\\' || c == ':' || (uint8_t)c < 0x20) continue;
        out += c;
    }
    out.trim();
    if (!out.length()) out = "untitled";
    if (!out.endsWith(".txt")) out += ".txt";
    if (out.length() > 40) out = out.substring(0, 36) + ".txt";
    return out;
}

static int findFileIdx(const String &path) {
    for (uint8_t i = 0; i < s_fileCount; i++) {
        if (s_files[i] == path) return (int)i;
    }
    return -1;
}

static int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 把 hex 字符串解码追加到上传缓冲
static void appendHex(const char *p) {
    size_t len = strlen(p);
    for (size_t i = 0; i + 1 < len; i += 2) {
        int hi = hexVal(p[i]);
        int lo = hexVal(p[i + 1]);
        if (hi < 0 || lo < 0) break;
        if (s_upLen >= UPLOAD_BUF_LEN) break;
        s_upBuf[s_upLen++] = (char)((hi << 4) | lo);
    }
    s_upBuf[s_upLen] = 0;
}

static void saveUploaded() {
    if (!s_upName.length()) { bleNotify("ERR name"); return; }
    String path = "/" + s_upName;
    File f = LittleFS.open(path, "w");
    if (!f) { bleNotify("ERR open"); return; }
    size_t w = f.write((const uint8_t *)s_upBuf, s_upLen);
    f.close();
    if (w != s_upLen) { bleNotify("ERR write"); return; }
    Serial.printf("[fs] 保存 %s (%u 字节)\n", s_upName.c_str(), (unsigned)s_upLen);
    scanScoreFiles();
    int idx = findFileIdx(path);
    bleNotify("SAVED " + String(idx) + " " + s_upName);
}

static void sendReadChunk(int idx, uint32_t off) {
    if (idx < 0 || idx >= s_fileCount) { bleNotify("ERR index"); return; }
    File f = LittleFS.open(s_files[idx], "r");
    if (!f) { bleNotify("ERR open"); return; }
    uint32_t len = f.size();
    if (off >= len) { f.close(); bleNotify("EOF " + String(len)); return; }
    size_t n = len - off;
    if (n > 40) n = 40;
    if (!f.seek(off)) { f.close(); bleNotify("ERR seek"); return; }
    uint8_t buf[40];
    size_t r = f.read(buf, n);
    f.close();
    if (!r) { bleNotify("ERR read"); return; }
    String hex;
    hex.reserve(r * 2);
    char t[3];
    for (size_t i = 0; i < r; i++) {
        snprintf(t, sizeof(t), "%02x", buf[i]);
        hex += t;
    }
    bleNotify("DATA " + String((unsigned)off) + " " + hex);
}

static void deleteFileByIdx(int idx) {
    if (idx < 0 || idx >= s_fileCount) { bleNotify("ERR index"); return; }
    String name = baseName(s_files[idx]);
    if (!LittleFS.remove(s_files[idx])) { bleNotify("ERR delete"); return; }
    Serial.printf("[fs] 删除 %s\n", name.c_str());
    scanScoreFiles();
    bleNotify("OK deleted " + name);
}

static int findBootSoundIdx() {
    for (uint8_t i = 0; i < s_fileCount; i++) {
        String n = baseName(s_files[i]);
        n.toLowerCase();
        if (n.endsWith("boot.txt")) return (int)i;   // 如 00_boot.txt / boot.txt
    }
    return -1;
}

static void renameFileByIdx(int idx, String newName) {
    if (idx < 0 || idx >= s_fileCount) { bleNotify("ERR index"); return; }
    newName = sanitizeName(newName);
    String np = "/" + newName;
    if (LittleFS.exists(np)) { bleNotify("ERR exists " + newName); return; }
    if (!LittleFS.rename(s_files[idx], np)) { bleNotify("ERR rename"); return; }
    Serial.printf("[fs] 重命名 -> %s\n", newName.c_str());
    scanScoreFiles();
    bleNotify("OK renamed " + newName);
}

/* ------------------------------ 播放器 ------------------------------ */
static void playerResetClock() {
    s_noteIdx = 0;
    s_noteStart = 0;
    s_posMs = 0;
    s_lastTick = millis();
    s_liveFreq = 0;
    buzzerOff();
}

static void playerBegin(const String &name, uint16_t trackIdx, PlaySource src) {
    s_trackName = name;
    s_trackIdx = trackIdx;
    s_src = src;
    s_state = ST_PLAYING;
    playerResetClock();
    static bool taskCoreLogged = false;
    if (!taskCoreLogged) {
        taskCoreLogged = true;
        Serial.printf("[task] 播放逻辑执行于 core %d\n", xPortGetCoreID());
    }
    Serial.printf("\n▶ 播放: %s (%u 音, %.1fs)\n", name.c_str(), s_noteCount, s_totalMs / 1000.0);
    bleNotify("TRACK " + String((unsigned)trackIdx) + " " + name);
    notifyPos(true);
}

// 加载板载曲谱文件
static bool playerLoadFile(uint16_t idx, bool isBoot = false) {
    if (idx >= s_fileCount) { bleNotify("ERR index"); return false; }
    const String &path = s_files[idx];
    File f = LittleFS.open(path, "r");
    if (!f) { bleNotify("ERR open " + baseName(path)); return false; }
    String srcText = f.readString();
    f.close();
    if (!srcText.length()) { bleNotify("ERR empty " + baseName(path)); return false; }

    uint16_t bpm = s_bpm;
    String title;
    bool keyStyle = false;
    parseMeta(srcText, &bpm, &title, &keyStyle);

    uint16_t keyCount = 0, numCount = 0;
    uint16_t n = parseScore(srcText, bpm, keyStyle, &keyCount, &numCount);
    if (!n) { bleNotify("ERR parse " + baseName(path)); return false; }

    s_noteCount = n;
    uint32_t total = 0;
    for (uint16_t i = 0; i < n; i++) total += s_notes[i].durMs;
    s_totalMs = total;

    s_isBootSound = isBoot;
    playerBegin(title.length() ? title : baseName(path), idx, SRC_FILE);
    return true;
}

// 加载蓝牙下发的曲谱
static bool playerLoadBleScore() {
    if (s_scoreLen == 0) { bleNotify("ERR score empty"); return false; }
    String srcText(s_scoreBuf);
    uint16_t bpm = s_bpm;
    String title;
    bool keyStyle = false;
    parseMeta(srcText, &bpm, &title, &keyStyle);

    uint16_t keyCount = 0, numCount = 0;
    uint16_t n = parseScore(srcText, bpm, keyStyle, &keyCount, &numCount);
    if (!n) { bleNotify("ERR parse"); return false; }

    s_noteCount = n;
    uint32_t total = 0;
    for (uint16_t i = 0; i < n; i++) total += s_notes[i].durMs;
    s_totalMs = total;

    playerBegin(title.length() ? title : String("蓝牙曲谱"), 255, SRC_BLE);
    return true;
}

static void playerStop(bool notifyDone) {
    s_isBootSound = false;
    if (notifyDone && s_trackName.length()) bleNotify("DONE " + s_trackName);
    s_state = ST_IDLE;
    s_liveFreq = 0;
    buzzerOff();
    s_posMs = 0;
    notifyPos(true);
}

static void playerIdle() {
    s_state = ST_IDLE;
    s_posMs = 0;
    notifyPos(true);
}

static void playerPause() {
    if (s_state != ST_PLAYING) return;
    s_state = ST_PAUSED;
    s_liveFreq = 0;
    buzzerOff();
    notifyPos(true);
}

static void playerResume() {
    if (s_state != ST_PAUSED) return;
    s_state = ST_PLAYING;
    s_lastTick = millis();
    notifyPos(true);
}

static void playerSeekTo(uint32_t ms) {
    if (s_state == ST_IDLE || s_noteCount == 0) return;
    if (ms > s_totalMs) ms = s_totalMs;
    s_posMs = ms;
    s_noteIdx = 0;
    s_noteStart = 0;
    while (s_noteIdx < s_noteCount &&
           s_posMs - s_noteStart >= s_notes[s_noteIdx].durMs) {
        s_noteStart += s_notes[s_noteIdx].durMs;
        s_noteIdx++;
    }
    s_lastTick = millis();
    s_liveFreq = 0;
    buzzerOff();
    notifyPos(true);
}

static void playerNext() {
    if (!s_fileCount) return;
    uint16_t idx;
    if (s_src == SRC_FILE) idx = (uint16_t)((s_trackIdx + 1) % s_fileCount);
    else                   idx = 0;
    playerLoadFile(idx);
}

static void playerPrev() {
    if (!s_fileCount) return;
    uint16_t idx;
    if (s_src == SRC_FILE) idx = (uint16_t)((s_trackIdx + s_fileCount - 1) % s_fileCount);
    else                   idx = (uint16_t)(s_fileCount - 1);
    playerLoadFile(idx);
}

// 播放结束：板载曲谱自动接下一首，蓝牙曲谱则停下
static void playerFinish() {
    s_liveFreq = 0;
    buzzerOff();
    bleNotify("DONE " + s_trackName);
    if (s_isBootSound) {          // 开机音效永远只播一遍
        s_isBootSound = false;
        playerIdle();
        return;
    }
    switch (s_mode) {
        case MODE_REPEAT:         // 单曲循环
            if (s_src == SRC_FILE && s_fileCount > 0)  playerLoadFile(s_trackIdx);
            else if (s_src == SRC_BLE)                 playerLoadBleScore();
            else                                       playerIdle();
            break;
        case MODE_ONCE:           // 播完即停
            playerIdle();
            break;
        default:                  // 自动连播下一首
            if (s_src == SRC_FILE && s_fileCount > 0) {
                playerLoadFile((uint16_t)((s_trackIdx + 1) % s_fileCount));
            } else {
                playerIdle();
            }
            break;
    }
}

// 每次 loop 推进播放器（非阻塞，所以支持暂停/拖动进度）
static void playerTick() {
    if (s_state != ST_PLAYING) return;

    uint32_t now = millis();
    s_posMs += now - s_lastTick;
    s_lastTick = now;

    while (s_noteIdx < s_noteCount &&
           s_posMs - s_noteStart >= s_notes[s_noteIdx].durMs) {
        s_noteStart += s_notes[s_noteIdx].durMs;
        s_noteIdx++;
    }

    if (s_noteIdx >= s_noteCount) {
        playerFinish();
        return;
    }

    const NoteEvent &ev = s_notes[s_noteIdx];
    uint32_t d = ev.durMs;
    uint32_t rel = s_posMs - s_noteStart;
    uint32_t gap = (d > ARTIC_MS) ? ARTIC_MS : d / 4;
    uint16_t tone = 0;
    if (ev.n > 0 && rel < d - gap) {
        if (ev.n == 1) {
            tone = ev.f[0];
        } else {
            // 单只蜂鸣器：和弦用"快速分解"近似，弹完保持最高音
            uint32_t idx = rel / CHORD_STRUM_MS;
            if (idx >= ev.n) {
                uint16_t top = 0;
                for (uint8_t k = 0; k < ev.n; k++) if (ev.f[k] > top) top = ev.f[k];
                tone = top;
            } else {
                tone = ev.f[idx];
            }
        }
    }
    setTone(tone);

    notifyPos();
}

// 单发音（阻塞，ms 最大 5s）
static void playSingleNote(uint16_t freq, uint16_t ms) {
    uint32_t gap = (ms > ARTIC_MS) ? ARTIC_MS : ms / 4;
    if (buzzerOn(freq)) s_liveFreq = freq;
    delay(ms - gap);
    buzzerOff();
    s_liveFreq = 0;
    if (gap) delay(gap);
}

/* ------------------------------ BLE 命令 ------------------------------ */
static String s_rxLine;

static void bleSendFileList() {
    for (uint8_t i = 0; i < s_fileCount; i++) {
        uint32_t len = 0;
        File f = LittleFS.open(s_files[i], "r");
        if (f) { len = f.size(); f.close(); }
        bleNotify("FILE " + String((unsigned)i) + " " + String((unsigned)len) +
                  " " + baseName(s_files[i]));
    }
    bleNotify("LIST_END " + String((unsigned)s_fileCount));
}

static void bleSendInfo() {
    bleNotify("INFO fw=1.0 files=" + String((unsigned)s_fileCount) +
              " bpm=" + String((unsigned)s_bpm) +
              " state=" + String((unsigned)s_state) +
              " mode=" + String((unsigned)s_mode) +
              " fsused=" + String((unsigned)LittleFS.usedBytes()) +
              " fstotal=" + String((unsigned)LittleFS.totalBytes()));
}

static void handleCommand(String line) {
    while (line.endsWith("\r")) line.remove(line.length() - 1);
    if (!line.length()) return;

    Serial.printf("[ble] <- %s\n", line.c_str());

    if (line == "OFF") {
        s_liveFreq = 0;
        buzzerOff();
        return;
    }
    if (line == "STOP") {
        queuePush(CMD_STOP, 0, 0);
        return;
    }
    if (line == "PAUSE") { queuePush(CMD_PAUSE, 0, 0); return; }
    if (line == "RESUME") { queuePush(CMD_RESUME, 0, 0); return; }
    if (line == "NEXT") { queuePush(CMD_NEXT, 0, 0); return; }
    if (line == "PREV") { queuePush(CMD_PREV, 0, 0); return; }

    if (line.startsWith("ON ")) {
        uint16_t f = 0;
        if (!parseSingleNote(line.c_str() + 3, &f)) {
            bleNotify("ERR note");
            return;
        }
        if (buzzerOn(f)) s_liveFreq = f;
        else bleNotify("ERR tone");
        return;
    }
    if (line.startsWith("PLAY ")) {
        const char *p = line.c_str() + 5;
        char tok[16];
        int ti = 0;
        while (*p && *p != ' ' && ti < 15) tok[ti++] = *p++;
        tok[ti] = 0;
        uint16_t f = 0;
        if (!parseSingleNote(tok, &f)) { bleNotify("ERR note"); return; }
        long ms = atol(p);
        if (ms < 20) ms = 20;
        if (ms > 5000) ms = 5000;
        if (!queuePush(CMD_PLAY_NOTE, f, (uint16_t)ms)) bleNotify("ERR busy");
        return;
    }
    if (line == "LIST") { bleSendFileList(); return; }
    if (line.startsWith("MODE ")) {
        long v = line.substring(5).toInt();
        if (v < 0 || v > 2) { bleNotify("ERR mode"); return; }
        s_mode = (PlayMode)v;
        Serial.printf("[player] 播放模式 -> %ld\n", v);
        bleSendInfo();
        return;
    }

    // ---- 文件管理 ----
    if (line.startsWith("SAVE_BEGIN ")) {
        s_upName = sanitizeName(line.substring(11));
        s_upLen = 0;
        s_upBuf[0] = 0;
        bleNotify("OK save_begin " + s_upName);
        return;
    }
    if (line.startsWith("SAVE_DATA ")) {
        appendHex(line.c_str() + 10);
        return;
    }
    if (line == "SAVE_END") {
        saveUploaded();
        return;
    }
    if (line.startsWith("READ ")) {
        const char *p = line.c_str() + 5;
        int idx = atoi(p);
        const char *sp = strchr(p, ' ');
        uint32_t off = sp ? (uint32_t)atol(sp + 1) : 0;
        sendReadChunk(idx, off);
        return;
    }
    if (line.startsWith("DELETE ")) {
        deleteFileByIdx(atoi(line.c_str() + 7));
        return;
    }
    if (line.startsWith("RENAME ")) {
        const char *p = line.c_str() + 7;
        int idx = atoi(p);
        const char *sp = strchr(p, ' ');
        if (!sp) { bleNotify("ERR name"); return; }
        renameFileByIdx(idx, String(sp + 1));
        return;
    }
    if (line == "GETINFO") { bleSendInfo(); return; }
    if (line == "GETFREQ") {
        uint32_t actual = 0;
#if !BUZZER_IS_ACTIVE && (ESP_ARDUINO_VERSION_MAJOR >= 3)
        if (s_toneAttached) actual = ledcReadFreq(BUZZER_PIN);
#endif
        bleNotify("FREQ " + String(actual));
        return;
    }
    if (line.startsWith("BPM ")) {
        long v = line.substring(4).toInt();
        if (v < 20) v = 20;
        if (v > 400) v = 400;
        s_bpm = (uint16_t)v;
        bleSendInfo();
        return;
    }
    if (line == "SCORE_BEGIN") { s_scoreLen = 0; return; }
    if (line == "SCORE_NL") {
        if (s_scoreLen < SCORE_BUF_LEN) {
            s_scoreBuf[s_scoreLen++] = '\n';
            s_scoreBuf[s_scoreLen] = 0;
        }
        return;
    }
    if (line.startsWith("SCORE_ADD ")) {
        const char *p = line.c_str() + 10;
        size_t len = strlen(p);
        if (s_scoreLen + len > SCORE_BUF_LEN) len = SCORE_BUF_LEN - s_scoreLen;
        memcpy(s_scoreBuf + s_scoreLen, p, len);
        s_scoreLen += len;
        s_scoreBuf[s_scoreLen] = 0;
        return;
    }
    if (line == "SCORE_PLAY") {
        if (!queuePush(CMD_SCORE_PLAY, 0, 0)) bleNotify("ERR busy");
        return;
    }
    if (line.startsWith("FILE ")) {
        long idx = line.substring(5).toInt();
        if (idx < 0 || idx >= (long)s_fileCount) { bleNotify("ERR index"); return; }
        if (!queuePush(CMD_FILE_PLAY, (uint32_t)idx, 0)) bleNotify("ERR busy");
        return;
    }
    if (line.startsWith("SEEK ")) {
        long ms = line.substring(5).toInt();
        if (ms < 0) ms = 0;
        if (!queuePush(CMD_SEEK, (uint32_t)ms, 0)) bleNotify("ERR busy");
        return;
    }
    Serial.printf("[ble] 未知命令: %s\n", line.c_str());
}

class RxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override {
        static bool coreLogged = false;
        if (!coreLogged) {
            coreLogged = true;
            Serial.printf("[task] BLE 回调运行在 core %d\n", xPortGetCoreID());
        }
        String v = c->getValue();
        if (!v.length()) return;
        s_rxLine += v;
        int nl;
        while ((nl = s_rxLine.indexOf('\n')) >= 0) {
            String line = s_rxLine.substring(0, nl);
            s_rxLine = s_rxLine.substring(nl + 1);
            handleCommand(line);
        }
        if (s_rxLine.length() > 512) s_rxLine = "";
    }
};

class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *server) override {
        (void)server;
        s_bleConnected = true;
        s_bleAnnounced = false;
        Serial.println("[ble] 主机已连接");
    }
    void onDisconnect(BLEServer *server) override {
        (void)server;
        s_bleConnected = false;
        s_bleAnnounced = false;
        playerStop(false);
        Serial.println("[ble] 主机断开，重新广播");
        BLEDevice::startAdvertising();
    }
};

static void bleInit() {
    BLEDevice::init(BLE_NAME);
    BLEDevice::setMTU(185);

    s_bleServer = BLEDevice::createServer();
    s_bleServer->setCallbacks(new ServerCallbacks());

    BLEService *svc = s_bleServer->createService(UUID_SERVICE);

    BLECharacteristic *rx = svc->createCharacteristic(
        UUID_RX, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    rx->setCallbacks(new RxCallbacks());

    s_bleTx = svc->createCharacteristic(UUID_TX, BLECharacteristic::PROPERTY_NOTIFY);
    s_bleTx->addDescriptor(new BLE2902());

    svc->start();

    BLEAdvertising *adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(UUID_SERVICE);
    adv->setScanResponse(true);
    adv->setMinPreferred(0x06);
    BLEDevice::startAdvertising();

    Serial.printf("[ble] 广播中，设备名: %s\n", BLE_NAME);
}

/* ------------------------------ 主程序 ------------------------------ */
static void playerTask(void *arg);   // 前向声明（任务在 loop 前定义）

static void handleQueued(const CmdMsg &msg) {
    switch (msg.kind) {
        case CMD_FILE_PLAY:  playerLoadFile((uint16_t)msg.a, msg.b != 0); break;
        case CMD_SCORE_PLAY: playerLoadBleScore(); break;
        case CMD_PAUSE:      playerPause(); break;
        case CMD_RESUME:     playerResume(); break;
        case CMD_STOP:       playerStop(true); break;
        case CMD_NEXT:       playerNext(); break;
        case CMD_PREV:       playerPrev(); break;
        case CMD_SEEK:       playerSeekTo(msg.a); break;
        case CMD_PLAY_NOTE:  playSingleNote((uint16_t)msg.a, msg.b); break;
        default: break;
    }
}

void setup() {
    Serial.begin(115200);
    delay(1200);
    buzzerOff();

    Serial.println();
    Serial.println("==================================================");
    Serial.println(" ESP32-S3 简谱播放器 · BLE 播放器版");
    Serial.println(" 蜂鸣器: GPIO15 · 低电平触发 · 无源");
    Serial.println("==================================================");

    s_fsOk = LittleFS.begin(true);
    if (!s_fsOk) {
        Serial.println("[x] LittleFS 挂载失败");
    } else {
        Serial.printf("[fs] LittleFS: 总 %u 字节, 已用 %u 字节\n",
                      (unsigned)LittleFS.totalBytes(), (unsigned)LittleFS.usedBytes());
        scanScoreFiles();
        Serial.printf("[fs] 找到 %u 个 .txt 曲谱\n", s_fileCount);
        for (uint8_t i = 0; i < s_fileCount; i++) {
            Serial.printf("     - %s\n", baseName(s_files[i]).c_str());
        }
    }

    bleInit();

    Serial.printf("[task] loop/setup 运行在 core %d\n", xPortGetCoreID());
    // 播放器任务固定到 core 1：蓝牙协议栈在 core 0，互不打扰
    xTaskCreatePinnedToCore(playerTask, "player", 6144, nullptr, 2, nullptr, 1);

    // 开机音效：LittleFS 里有 boot.txt 就在上电时播一次
    int bootIdx = findBootSoundIdx();
    if (bootIdx >= 0) {
        Serial.printf("[boot] 播放开机音效: %s\n", baseName(s_files[bootIdx]).c_str());
        queuePush(CMD_FILE_PLAY, (uint32_t)bootIdx, 1);   // b=1: 开机音效，播完即停
    } else {
        Serial.println("[boot] 没有 boot.txt，跳过开机音效");
    }

#if AUTO_PLAY_ON_BOOT
    if (s_fsOk && s_fileCount > 0) queuePush(CMD_FILE_PLAY, 0, 0);
#endif
    Serial.println("就绪，等待蓝牙连接");
}

/* ------------------------- 播放器任务（RTOS / 双核） -------------------------
 *  Bluedroid 协议栈任务由 ESP-IDF 创建并跑在 core 0；
 *  播放器任务显式固定到 core 1（和 Arduino loop 同核，但 loop 基本空闲）。
 *  两个子系统只通过「命令队列 + 临界区」交互，谁都不会阻塞谁：
 *    BLE 回调（core 0）--queuePush--> 播放器任务（core 1）--bleNotify--> BLE
 */
static void playerTask(void *arg) {
    (void)arg;
    Serial.printf("[task] 播放器任务已启动，运行在 core %d\n", xPortGetCoreID());
    for (;;) {
        CmdMsg msg;
        while (queuePop(&msg)) handleQueued(msg);
        playerTick();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void loop() {
    if (s_bleConnected && !s_bleAnnounced) {
        s_bleAnnounced = true;
        bleSendInfo();
    }

    static uint32_t lastBeat = 0;
    if (!s_bleConnected && millis() - lastBeat > 10000) {
        lastBeat = millis();
        Serial.printf("[idle] 等待蓝牙连接 (空闲 %lus, loop core=%d)\n",
                      (unsigned long)(millis() / 1000), xPortGetCoreID());
    }
    delay(50);
}
