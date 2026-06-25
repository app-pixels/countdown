/*
 * app_hourglass.cpp — countdown timer with green pixel-raster aesthetic
 *
 * Portrait 368×448, canvas. The screen is an 8 px tile grid (46×56 = 2576
 * tiles). Background tiles are dark green; the time digits "MM:SS" in the
 * centre are painted as lighter-green tiles. As the timer runs, tiles turn
 * red one by one in row-major order from top-left to bottom-right; when the
 * last tile flips, the timer hits 00:00.
 *
 * Long press BOOT (≥800 ms) opens a config screen for setting the countdown
 * minutes and toggling the alarm beep. Config persists in NVS namespace
 * "countdown" and is seeded from setup.txt (COUNTDOWN_MINUTES / COUNTDOWN_BEEP)
 * on first boot.
 *
 * Controls:
 *   BOOT short – start / pause / resume (or reset when done)
 *   BOOT long  – open / close config screen
 *   PWR  short – reset to full
 */

#include "app_countdown.h"
#include "app_common.h"
#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <FS.h>
#include "canvas/Arduino_Canvas.h"
#include "pin_config.h"
#include "HWCDC.h"
#include "TouchDrvFT6X36.hpp"
#include "audio_engine.h"

extern USBCDC USBSerial;
extern Arduino_Canvas *g_canvas;

#define BOOT_BTN       0
#define PWR_POLL_MS   50
#define BOOT_LONG_MS 800

// ── Pixel-raster geometry ────────────────────────────────────────────────────
#define TILE_PX       7      // tile body in pixels
#define TILE_STEP     8      // tile + gap (1 px black gap)
#define COLS         (LCD_WIDTH  / TILE_STEP)   // 46
#define ROWS         (LCD_HEIGHT / TILE_STEP)   // 56
#define TILE_TOTAL   (COLS * ROWS)              // 2576

// ── Colours (Game Boy-ish dark→bright green; saturated red on consume) ──────
#define COL_BG          0x0000
#define COL_TILE_DK     0x0420   // dark forest green (idle tile)
#define COL_TILE_LT     0x57E8   // bright spring green (text tile, green region)
#define COL_TILE_RED    0x8000   // dark red (consumed tile)
#define COL_TILE_RED_LT 0xFEC8   // light salmon (text tile inside consumed/red region)
#define COL_CONFIG_FG   0xFFFF
#define COL_CONFIG_DIM  0x7BEF
#define COL_CONFIG_ACC  0x07E0

// ── 5×7 bitmap font for digits 0-9 (MSB → leftmost column) ──────────────────
static const uint8_t DIGIT_5x7[10][7] = {
    { 0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110 }, // 0
    { 0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110 }, // 1
    { 0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111 }, // 2
    { 0b11110, 0b00001, 0b00001, 0b01110, 0b00001, 0b00001, 0b11110 }, // 3
    { 0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010 }, // 4
    { 0b11111, 0b10000, 0b10000, 0b11110, 0b00001, 0b00001, 0b11110 }, // 5
    { 0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110 }, // 6
    { 0b11111, 0b00001, 0b00010, 0b00100, 0b00100, 0b01000, 0b01000 }, // 7
    { 0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110 }, // 8
    { 0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100 }, // 9
};

// Scale: each font pixel = SCALE×SCALE tiles. Effective digit: 10×14 tiles.
#define FONT_SCALE      2
#define DIGIT_TILES_W   (5 * FONT_SCALE)   // 10
#define DIGIT_TILES_H   (7 * FONT_SCALE)   // 14
#define COLON_TILES_W   2
// Layout MM:SS = 10 + 1 + 10 + 1 + 2 + 1 + 10 + 1 + 10 = 46 (full width)

// ── State ────────────────────────────────────────────────────────────────────
static Arduino_Canvas  *canvas      = nullptr;
static TouchDrvFT6X36   s_touch;
static Preferences      s_prefs;

static uint32_t  s_minutes  = 25;     // configured countdown duration (1..180)
static bool      s_beep     = true;
static bool      s_running  = false;
static bool      s_done     = false;
static uint32_t  s_elapsed  = 0;      // accumulated ms while running
static uint32_t  s_startMs  = 0;
static bool      s_bootWas  = false;
static uint32_t  s_bootDownAt   = 0;
static bool      s_bootLongFired = false;
static uint32_t  s_lastPwr  = 0;
static uint32_t  s_lastDraw = 0;
static int       s_lastConsumed = -1;
static bool      s_inConfig = false;
static bool      s_touchWas = false;
static bool      s_audioReady = false;

// ── Persistence ──────────────────────────────────────────────────────────────
static void seedFromSetupTxt(uint32_t &mins, bool &beep) {
    if (!SD_MMC.begin("/sdcard", true)) return;
    File f = SD_MMC.open("/setup/setup.txt");
    if (f) {
        char line[160];
        while (f.available()) {
            int len = f.readBytesUntil('\n', line, sizeof(line) - 1);
            line[len] = '\0';
            const char *p;
            if ((p = strstr(line, "COUNTDOWN_MINUTES")) != nullptr) {
                p += strlen("COUNTDOWN_MINUTES");
                while (*p == ' ' || *p == '=' || *p == '"') p++;
                int v = atoi(p);
                if (v >= 1 && v <= 180) mins = (uint32_t)v;
            } else if ((p = strstr(line, "COUNTDOWN_BEEP")) != nullptr) {
                p += strlen("COUNTDOWN_BEEP");
                while (*p == ' ' || *p == '=' || *p == '"') p++;
                beep = (atoi(p) != 0);
            }
        }
        f.close();
    }
    SD_MMC.end();
}

static void loadConfig() {
    s_prefs.begin("countdown", true);
    bool init = s_prefs.getBool("init", false);
    if (init) {
        s_minutes = s_prefs.getUInt("min", 25);
        s_beep    = s_prefs.getBool("beep", true);
        s_prefs.end();
    } else {
        s_prefs.end();
        seedFromSetupTxt(s_minutes, s_beep);
        s_prefs.begin("countdown", false);
        s_prefs.putUInt("min",  s_minutes);
        s_prefs.putBool("beep", s_beep);
        s_prefs.putBool("init", true);
        s_prefs.end();
    }
    if (s_minutes < 1)   s_minutes = 1;
    if (s_minutes > 180) s_minutes = 180;
}

static void saveConfig() {
    s_prefs.begin("countdown", false);
    s_prefs.putUInt("min",  s_minutes);
    s_prefs.putBool("beep", s_beep);
    s_prefs.putBool("init", true);
    s_prefs.end();
}

// ── Time helpers ─────────────────────────────────────────────────────────────
static uint32_t totalMs()    { return s_minutes * 60000UL; }
static uint32_t elapsedMs()  {
    return s_running ? (s_elapsed + (millis() - s_startMs)) : s_elapsed;
}
static uint32_t remainMs() {
    uint32_t e = elapsedMs(), t = totalMs();
    return (e < t) ? (t - e) : 0;
}

// ── Build text tile-mask for "MM:SS" ─────────────────────────────────────────
static bool s_textMask[ROWS * COLS];

static void buildTextMask(uint32_t rem) {
    memset(s_textMask, 0, sizeof(s_textMask));
    uint32_t mins = rem / 60000UL;
    uint32_t secs = (rem % 60000UL) / 1000UL;
    if (mins > 99) mins = 99;
    int charDigit[5] = { (int)(mins / 10), (int)(mins % 10), -1,
                         (int)(secs / 10), (int)(secs % 10) };
    int charW[5]     = { DIGIT_TILES_W, DIGIT_TILES_W, COLON_TILES_W,
                         DIGIT_TILES_W, DIGIT_TILES_W };

    int totalW = 0;
    for (int c = 0; c < 5; c++) totalW += charW[c];
    totalW += 4;   // 4 gaps × 1 tile

    int colStart = (COLS - totalW) / 2;
    int rowStart = (ROWS - DIGIT_TILES_H) / 2;

    int curCol = colStart;
    for (int c = 0; c < 5; c++) {
        if (charDigit[c] == -1) {
            // Colon: 2-tile-wide block, dots centred vertically within 14 rows
            for (int dx = 0; dx < COLON_TILES_W; dx++) {
                int cx = curCol + dx;
                if (cx < 0 || cx >= COLS) continue;
                int dotRows[2] = { rowStart + 4, rowStart + 9 };
                for (int dy = 0; dy < 2; dy++) {
                    for (int j = 0; j < 2; j++) {
                        int ry = dotRows[dy] + j;
                        if (ry < 0 || ry >= ROWS) continue;
                        s_textMask[ry * COLS + cx] = true;
                    }
                }
            }
        } else {
            int d = charDigit[c];
            for (int fr = 0; fr < 7; fr++) {
                uint8_t bits = DIGIT_5x7[d][fr];
                for (int fc = 0; fc < 5; fc++) {
                    if (!(bits & (1 << (4 - fc)))) continue;
                    int baseR = rowStart + fr * FONT_SCALE;
                    int baseC = curCol  + fc * FONT_SCALE;
                    for (int dy = 0; dy < FONT_SCALE; dy++) {
                        for (int dx = 0; dx < FONT_SCALE; dx++) {
                            int ry = baseR + dy, rx = baseC + dx;
                            if (ry < 0 || ry >= ROWS) continue;
                            if (rx < 0 || rx >= COLS) continue;
                            s_textMask[ry * COLS + rx] = true;
                        }
                    }
                }
            }
        }
        curCol += charW[c] + 1;
    }
}

// ── Pixel-raster timer screen ────────────────────────────────────────────────
static void drawTimerScreen() {
    canvas->fillScreen(COL_BG);

    uint32_t rem = remainMs();
    if (s_done) rem = 0;

    int consumed = (int)((float)elapsedMs() / (float)totalMs() * (float)TILE_TOTAL);
    if (consumed < 0) consumed = 0;
    if (consumed > TILE_TOTAL) consumed = TILE_TOTAL;
    if (s_done) consumed = TILE_TOTAL;

    buildTextMask(rem);

    int idx = 0;
    for (int r = 0; r < ROWS; r++) {
        int16_t ty = r * TILE_STEP;
        for (int c = 0; c < COLS; c++, idx++) {
            int16_t tx = c * TILE_STEP;
            uint16_t col;
            if (idx < consumed)            col = s_textMask[idx] ? COL_TILE_RED_LT : COL_TILE_RED;
            else if (s_textMask[idx])      col = COL_TILE_LT;
            else                            col = COL_TILE_DK;
            canvas->fillRect(tx, ty, TILE_PX, TILE_PX, col);
        }
    }

    // Pill labels (anchored to BOOT/PWR — overlay on tiles)
    const char *bootLbl;
    if (s_done)         bootLbl = "reset";
    else if (s_running) bootLbl = "pause";
    else                bootLbl = (s_elapsed > 0) ? "go" : "start";
    draw_pill_label(canvas, 0, 0, bootLbl);
    draw_pill_label(canvas, 0, 1, "reset");

    draw_battery_g(canvas, LCD_WIDTH, LCD_HEIGHT);
    draw_watermark_g(canvas, LCD_WIDTH, LCD_HEIGHT);
    canvas->flush();

    s_lastConsumed = consumed;
}

// ── Config screen ────────────────────────────────────────────────────────────
struct Hit { int16_t x, y, w, h; };
static const Hit HIT_MIN_M5  = { 24,  200, 60, 60 };
static const Hit HIT_MIN_M1  = { 92,  200, 60, 60 };
static const Hit HIT_MIN_P1  = { 216, 200, 60, 60 };
static const Hit HIT_MIN_P5  = { 284, 200, 60, 60 };
static const Hit HIT_BEEP    = { 60,  290, 248, 60 };

static bool inHit(const Hit &h, int16_t x, int16_t y) {
    return x >= h.x && x < h.x + h.w && y >= h.y && y < h.y + h.h;
}

static void drawHitButton(const Hit &h, const char *label, uint16_t fg, uint16_t bg) {
    canvas->fillRoundRect(h.x, h.y, h.w, h.h, 12, bg);
    canvas->drawRoundRect(h.x, h.y, h.w, h.h, 12, COL_CONFIG_DIM);
    canvas->setTextSize(3);
    canvas->setTextColor(fg);
    int n = strlen(label);
    int16_t cw = 18, ch = 24;
    canvas->setCursor(h.x + (h.w - n * cw) / 2, h.y + (h.h - ch) / 2);
    canvas->print(label);
}

static void drawConfigScreen() {
    canvas->fillScreen(COL_BG);

    // Title
    canvas->setTextSize(3);
    canvas->setTextColor(COL_CONFIG_FG);
    const char *title = "TIMER SETUP";
    int16_t tw = (int16_t)(strlen(title) * 18);
    canvas->setCursor((LCD_WIDTH - tw) / 2, 40);
    canvas->print(title);

    // Minutes label + value
    canvas->setTextSize(2);
    canvas->setTextColor(COL_CONFIG_DIM);
    const char *lbl = "MINUTES";
    int16_t lw = (int16_t)(strlen(lbl) * 12);
    canvas->setCursor((LCD_WIDTH - lw) / 2, 84);
    canvas->print(lbl);

    // Big minutes value
    char minBuf[6];
    snprintf(minBuf, sizeof(minBuf), "%lu", (unsigned long)s_minutes);
    int n = strlen(minBuf);
    canvas->setTextSize(6, 7, 2);
    canvas->setTextColor(COL_CONFIG_ACC);
    int16_t cw = 38;
    canvas->setCursor((LCD_WIDTH - n * cw) / 2, 116);
    canvas->print(minBuf);

    // -/+ buttons (left -5, -1; right +1, +5)
    drawHitButton(HIT_MIN_M5, "-5", COL_CONFIG_FG, 0x18C3);
    drawHitButton(HIT_MIN_M1, "-1", COL_CONFIG_FG, 0x18C3);
    drawHitButton(HIT_MIN_P1, "+1", COL_CONFIG_FG, 0x18C3);
    drawHitButton(HIT_MIN_P5, "+5", COL_CONFIG_FG, 0x18C3);

    // Beep toggle
    char beepBuf[24];
    snprintf(beepBuf, sizeof(beepBuf), "ALARM: %s", s_beep ? "ON" : "OFF");
    drawHitButton(HIT_BEEP, beepBuf, s_beep ? COL_CONFIG_ACC : COL_CONFIG_DIM,
                  s_beep ? 0x0200 : 0x1082);

    // Footer hint
    canvas->setTextSize(2);
    canvas->setTextColor(COL_CONFIG_DIM);
    const char *hint = "long BOOT to save";
    int16_t hw = (int16_t)(strlen(hint) * 12);
    canvas->setCursor((LCD_WIDTH - hw) / 2, 380);
    canvas->print(hint);

    draw_pill_label(canvas, 0, 0, "save");
    draw_pill_label(canvas, 0, 1, "exit");

    draw_battery_g(canvas, LCD_WIDTH, LCD_HEIGHT);
    draw_watermark_g(canvas, LCD_WIDTH, LCD_HEIGHT);
    canvas->flush();
}

static void enterConfig() {
    s_inConfig = true;
    s_running  = false;   // pause when configuring
    drawConfigScreen();
}

static void exitConfig() {
    s_inConfig = false;
    if (s_minutes < 1)   s_minutes = 1;
    if (s_minutes > 180) s_minutes = 180;
    saveConfig();
    // Reset countdown so the new duration takes effect immediately
    s_running   = false;
    s_done      = false;
    s_elapsed   = 0;
    s_startMs   = 0;
    s_lastConsumed = -1;
    drawTimerScreen();
}

// ── Alarm beep ───────────────────────────────────────────────────────────────
static void playBeep() {
    if (!s_beep) return;
    if (!s_audioReady) {
        audio_engine_init();
        s_audioReady = true;
    }
    audio_engine_set_volume(80);
    audio_engine_play();

    // 3 short 1 kHz tones, 200 ms each, with 120 ms gaps
    const int sampleRate = 16000;
    const int toneSamples = sampleRate * 200 / 1000;   // 3200
    const int gapSamples  = sampleRate * 120 / 1000;   // 1920
    int16_t buf[256];
    for (int rep = 0; rep < 3; rep++) {
        int sent = 0;
        while (sent < toneSamples) {
            int chunk = toneSamples - sent;
            if (chunk > (int)(sizeof(buf) / sizeof(buf[0])))
                chunk = sizeof(buf) / sizeof(buf[0]);
            for (int i = 0; i < chunk; i++) {
                int phase = (sent + i) % 16;     // 16-sample period @ 16 kHz = 1 kHz
                buf[i] = (phase < 8) ? 8000 : -8000;
            }
            int pushed = audio_engine_push(buf, chunk);
            sent += pushed;
            if (pushed == 0) delay(5);
        }
        // Gap: feed silence
        sent = 0;
        memset(buf, 0, sizeof(buf));
        while (sent < gapSamples) {
            int chunk = gapSamples - sent;
            if (chunk > (int)(sizeof(buf) / sizeof(buf[0])))
                chunk = sizeof(buf) / sizeof(buf[0]);
            int pushed = audio_engine_push(buf, chunk);
            sent += pushed;
            if (pushed == 0) delay(5);
        }
    }
    // Drain a bit, then stop output (the engine continues silence on idle)
    delay(80);
    audio_engine_stop();
}

// ── App entry points ─────────────────────────────────────────────────────────
void app_countdown_set_config(uint32_t duration_min) {
    if (duration_min >= 1 && duration_min <= 180) s_minutes = duration_min;
}

void app_countdown_setup(Arduino_OLED *gfx) {
    (void)gfx;
    canvas = g_canvas;

    s_running       = false;
    s_done          = false;
    s_elapsed       = 0;
    s_startMs       = 0;
    s_bootWas       = false;
    s_bootDownAt    = 0;
    s_bootLongFired = false;
    s_lastPwr       = 0;
    s_lastDraw      = 0;
    s_lastConsumed  = -1;
    s_inConfig      = false;
    s_touchWas      = false;
    s_audioReady    = false;

    loadConfig();

    if (!s_touch.begin(Wire, FT6X36_SLAVE_ADDRESS, IIC_SDA, IIC_SCL))
        USBSerial.println("FT6X36 init failed (hourglass)");

    pinMode(BOOT_BTN, INPUT_PULLUP);
    drawTimerScreen();
}

void app_countdown_loop() {
    common_tick();
    uint32_t now = millis();

    // ── BOOT short = action, BOOT long = config toggle ─────────────────────
    bool boot = (digitalRead(BOOT_BTN) == LOW);
    if (boot && !s_bootWas) {
        s_bootDownAt    = now;
        s_bootLongFired = false;
    }
    if (boot && !s_bootLongFired && (now - s_bootDownAt) >= BOOT_LONG_MS) {
        s_bootLongFired = true;
        common_activity();
        if (s_inConfig) exitConfig();
        else            enterConfig();
    }
    if (!boot && s_bootWas) {
        if (!s_bootLongFired && !s_inConfig) {
            common_activity();
            if (s_done) {
                s_done = false; s_running = false; s_elapsed = 0; s_startMs = 0;
                s_lastConsumed = -1;
                drawTimerScreen();
            } else if (!s_running) {
                s_startMs = millis();
                s_running = true;
            } else {
                s_elapsed += millis() - s_startMs;
                s_running  = false;
                drawTimerScreen();
            }
        }
        s_bootDownAt = 0;
    }
    s_bootWas = boot;

    // ── Config-mode handling ───────────────────────────────────────────────
    if (s_inConfig) {
        int16_t tx, ty;
        bool touching = s_touch.getPoint(&tx, &ty, 1);
        if (touching && !s_touchWas) {
            common_activity();
            if (inHit(HIT_MIN_M5, tx, ty)) {
                s_minutes = (s_minutes > 5) ? s_minutes - 5 : 1;
                drawConfigScreen();
            } else if (inHit(HIT_MIN_M1, tx, ty)) {
                if (s_minutes > 1) s_minutes--;
                drawConfigScreen();
            } else if (inHit(HIT_MIN_P1, tx, ty)) {
                if (s_minutes < 180) s_minutes++;
                drawConfigScreen();
            } else if (inHit(HIT_MIN_P5, tx, ty)) {
                s_minutes = (s_minutes + 5 <= 180) ? s_minutes + 5 : 180;
                drawConfigScreen();
            } else if (inHit(HIT_BEEP, tx, ty)) {
                s_beep = !s_beep;
                drawConfigScreen();
            }
        }
        s_touchWas = touching;

        if (common_consume_pwr_short()) {
            common_activity();
            exitConfig();
        }
        delay(20);
        return;
    }

    // ── Countdown completion ───────────────────────────────────────────────
    if (s_running && elapsedMs() >= totalMs()) {
        s_elapsed += millis() - s_startMs;
        s_running  = false;
        s_done     = true;
        drawTimerScreen();
        playBeep();
        return;
    }

    // ── Periodic redraw (when consumed tile count or displayed second changes) ──
    if (s_running && now - s_lastDraw >= 200) {
        s_lastDraw = now;
        static uint32_t s_lastSec = 0xFFFFFFFFUL;
        int consumed = (int)((float)elapsedMs() / (float)totalMs() * (float)TILE_TOTAL);
        uint32_t curSec = remainMs() / 1000UL;
        if (consumed != s_lastConsumed || curSec != s_lastSec) {
            s_lastSec = curSec;
            drawTimerScreen();
        }
    }

    // ── PWR short = reset ──────────────────────────────────────────────────
    if (common_consume_pwr_short()) {
        common_activity();
        s_running       = false;
        s_done          = false;
        s_elapsed       = 0;
        s_startMs       = 0;
        s_lastConsumed  = -1;
        drawTimerScreen();
    }

    delay(10);
}
