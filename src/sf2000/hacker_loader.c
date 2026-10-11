/*
 * On-screen boot log for Butterscotch on SF2000 / GB300.
 *
 * Modeled after froggykvm, re3-sf2000 and xash3d-gb300.
 * Instead of staying pitch black during the multi-second WAD load, this renders
 * a scrolling CRT terminal directly to the screen via retro_video_refresh_cb.
 * Any crash, out-of-memory or missing file remains visible on the LCD.
 */

#include "hacker_loader.h"
#include "font8x8.h"
#include "sf2000_fw.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define SCREEN_W 320
#define SCREEN_H 240
#define HACKER_MAX_LOGS 14
#define HACKER_LINE_MAX 42

#define COLOR_BLACK        0x0000
#define COLOR_SCANLINE     0x0842  /* subtle dark grey scanline banding */
#define COLOR_MATRIX_GREEN 0x07E0
#define COLOR_AMBER        0xFFE0
#define COLOR_WHITE        0xFFFF
#define COLOR_CYAN         0x07FF
#define COLOR_RED          0xF800

static uint16_t s_loader_fb[SCREEN_W * SCREEN_H];
static retro_video_refresh_t s_video_cb = NULL;

static char s_hacker_logs[HACKER_MAX_LOGS][HACKER_LINE_MAX];
static int  s_hacker_log_count = 0;
static char s_rom_name[48] = {0};
static uint32_t s_boot_start_ms = 0;
static bool s_boot_active = false;
static int  s_hacker_progress = 0;

void hacker_loader_set_video_cb(retro_video_refresh_t cb) {
    s_video_cb = cb;
}

bool hacker_loader_is_active(void) {
    return s_boot_active;
}

static void draw_char8x8(int x, int y, char c, uint16_t fg, uint16_t bg) {
    if (c < 32 || c > 126) c = ' ';
    const uint8_t *glyph = font8x8_basic[c - 32];
    for (int row = 0; row < 8; row++) {
        int py = y + row;
        if (py < 0 || py >= SCREEN_H) continue;
        uint16_t *dst = &s_loader_fb[py * SCREEN_W + x];
        uint8_t line = glyph[row];
        for (int col = 0; col < 8; col++) {
            int px = x + col;
            if (px >= 0 && px < SCREEN_W) {
                if (line & (0x80 >> col)) dst[col] = fg;
                else if (bg != 0) dst[col] = bg;
            }
        }
    }
}

static void draw_text(int x, int y, const char *str, uint16_t fg, uint16_t bg) {
    if (!str) return;
    int cur_x = x;
    while (*str) {
        if (*str == '\n') {
            cur_x = x;
            y += 10;
        } else {
            draw_char8x8(cur_x, y, *str, fg, bg);
            cur_x += 8;
            if (cur_x + 8 > SCREEN_W) break;
        }
        str++;
    }
}

static void fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    if (w <= 0 || h <= 0) return;

    for (int row = 0; row < h; row++) {
        uint16_t *line = &s_loader_fb[(y + row) * SCREEN_W + x];
        for (int col = 0; col < w; col++)
            line[col] = color;
    }
}

static void hacker_render_screen(void) {
    /* 1. CRT Scanlines Background */
    for (int y = 0; y < SCREEN_H; y++) {
        uint16_t line_color = (y & 3) ? COLOR_BLACK : COLOR_SCANLINE;
        uint16_t *line = &s_loader_fb[y * SCREEN_W];
        for (int x = 0; x < SCREEN_W; x++)
            line[x] = line_color;
    }

    /* 2. Top headers */
    char rom_line[42];
    snprintf(rom_line, sizeof(rom_line), "ROM:  %.32s", s_rom_name[0] ? s_rom_name : "-");
    draw_text(8, 6, rom_line, COLOR_CYAN, 0);

    draw_text(8, 18, "CORE: Butterscotch (SF2000/GB300)", COLOR_AMBER, 0);

    /* Separator line */
    fill_rect(8, 29, SCREEN_W - 16, 1, COLOR_SCANLINE);

    /* 3. Log stream */
    int log_start_y = 34;
    for (int i = 0; i < s_hacker_log_count && i < HACKER_MAX_LOGS; i++) {
        int text_y = log_start_y + i * 11;
        uint16_t text_color = (i == s_hacker_log_count - 1) ? COLOR_WHITE : COLOR_MATRIX_GREEN;
        if (strstr(s_hacker_logs[i], "[ERR ]") || strstr(s_hacker_logs[i], "[HALT]")) {
            text_color = COLOR_RED;
        }
        draw_text(8, text_y, s_hacker_logs[i], text_color, 0);
    }

    /* 4. Bottom Progress Bar & Percentage */
    fill_rect(8, 193, SCREEN_W - 16, 1, COLOR_SCANLINE);

    char status_str[32];
    snprintf(status_str, sizeof(status_str), "BOOTING: %3d%%", s_hacker_progress);
    draw_text(8, 197, status_str, COLOR_CYAN, 0);

    /* Signature line, bottom center */
    const char *sig = "Synaps33";
    int sig_w = (int)strlen(sig) * 8;
    draw_text((SCREEN_W - sig_w) / 2, SCREEN_H - 11, sig, COLOR_WHITE, 0);

    int bar_x = 8, bar_y = 210;
    int bar_w = SCREEN_W - 16;
    int bar_h = 10;

    fill_rect(bar_x, bar_y, bar_w, 1, COLOR_SCANLINE);
    fill_rect(bar_x, bar_y + bar_h - 1, bar_w, 1, COLOR_SCANLINE);
    fill_rect(bar_x, bar_y, 1, bar_h, COLOR_SCANLINE);
    fill_rect(bar_x + bar_w - 1, bar_y, 1, bar_h, COLOR_SCANLINE);

    int max_fill = bar_w - 4;
    int fill_w = (s_hacker_progress * max_fill) / 100;
    if (fill_w > max_fill) fill_w = max_fill;
    if (fill_w > 0)
        fill_rect(bar_x + 2, bar_y + 2, fill_w, bar_h - 4, COLOR_MATRIX_GREEN);
}

static void hacker_flush(void) {
    hacker_render_screen();
    if (s_video_cb) {
        s_video_cb(s_loader_fb, SCREEN_W, SCREEN_H, SCREEN_W * sizeof(uint16_t));
    }
}

void hacker_loader_init(const char *rom_path) {
    s_boot_active = true;
    s_hacker_log_count = 0;
    s_hacker_progress = 5;
    s_boot_start_ms = os_get_tick_count();

    if (rom_path && *rom_path) {
        const char *b = strrchr(rom_path, '/');
        if (b) b++; else b = rom_path;
        snprintf(s_rom_name, sizeof(s_rom_name), "%.40s", b);
    } else {
        snprintf(s_rom_name, sizeof(s_rom_name), "Undertale");
    }

    hacker_loader_log("SYS", "CORE INIT: JZ4775 MIPS32", 5);
    hacker_loader_log("VFS", "MOUNT /mnt/sda1 ... [OK]", 8);
}

void hacker_loader_log(const char *tag, const char *msg, int pct) {
    if (!s_boot_active) return;
    hacker_loader_log_force(tag, msg, pct);
}

/* Same as hacker_loader_log() but ignores s_boot_active.  Split out because the
 * handover to the game (platformSwapBuffers -> hacker_loader_stop) is the one
 * moment where a silent no-op log would hide a real hang. */
void hacker_loader_log_force(const char *tag, const char *msg, int pct) {
    if (pct > s_hacker_progress) {
        s_hacker_progress = pct;
        if (s_hacker_progress > 100) s_hacker_progress = 100;
    }

    uint32_t now = os_get_tick_count();
    uint32_t elapsed_ms = (now >= s_boot_start_ms) ? (now - s_boot_start_ms) : 0;
    uint32_t sec = elapsed_ms / 1000;
    uint32_t msec = (elapsed_ms % 1000) / 10;

    char formatted[HACKER_LINE_MAX];
    snprintf(formatted, sizeof(formatted), "[%u.%02us] [%-4.4s] %.24s",
             (unsigned)sec, (unsigned)msec, tag ? tag : "SYS", msg ? msg : "");

    /* Mirror to hardware xlog as well */
    xlog("[hacker] %s\n", formatted);

    if (s_hacker_log_count < HACKER_MAX_LOGS) {
        strncpy(s_hacker_logs[s_hacker_log_count], formatted, HACKER_LINE_MAX - 1);
        s_hacker_logs[s_hacker_log_count][HACKER_LINE_MAX - 1] = '\0';
        s_hacker_log_count++;
    } else {
        memmove(&s_hacker_logs[0], &s_hacker_logs[1], (HACKER_MAX_LOGS - 1) * HACKER_LINE_MAX);
        strncpy(s_hacker_logs[HACKER_MAX_LOGS - 1], formatted, HACKER_LINE_MAX - 1);
        s_hacker_logs[HACKER_MAX_LOGS - 1][HACKER_LINE_MAX - 1] = '\0';
    }

    hacker_flush();
}

/* Keep-alive for wait loops: rewrites the newest line in place, so a loop that is
 * still running shows a moving counter and a loop that has died stops moving. */
void hacker_loader_spin(const char *tag, const char *msg) {
    static uint32_t s_last = 0;
    static int s_count = 0;
    uint32_t now = os_get_tick_count();
    if (now == s_last) return;
    s_last = now;
    s_count++;

    uint32_t elapsed_ms = (now >= s_boot_start_ms) ? (now - s_boot_start_ms) : 0;
    char formatted[HACKER_LINE_MAX];
    snprintf(formatted, sizeof(formatted), "[%u.%02us] [%-4.4s] %.24s",
             (unsigned)(elapsed_ms / 1000), (unsigned)((elapsed_ms % 1000) / 10),
             tag ? tag : "WAIT", msg ? msg : "");
    snprintf(formatted + 30, sizeof(formatted) - 30, " n=%d", s_count);

    if (s_hacker_log_count > 0) {
        strncpy(s_hacker_logs[s_hacker_log_count - 1], formatted, HACKER_LINE_MAX - 1);
        s_hacker_logs[s_hacker_log_count - 1][HACKER_LINE_MAX - 1] = '\0';
    } else {
        strncpy(s_hacker_logs[0], formatted, HACKER_LINE_MAX - 1);
        s_hacker_logs[0][HACKER_LINE_MAX - 1] = '\0';
        s_hacker_log_count = 1;
    }
    hacker_flush();
}

void hacker_loader_halt(const char *tag, const char *msg) {
    s_boot_active = true;
    hacker_loader_log(tag ? tag : "ERR", msg ? msg : "CRITICAL ERROR", 0);
    hacker_loader_log("HALT", "SYS HALTED. SELECT+START", 0);

    /* Loop and keep frame flushed so user can read the error */
    for (int i = 0; i < 300; i++) {
        hacker_flush();
        dly_tsk(100);
    }
}

void hacker_loader_stop(void) {
    s_boot_active = false;
}
