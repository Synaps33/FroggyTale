/*
 * platform_sf2000.c - Butterscotch backend for the HiChip JZ4775 handhelds
 *                     (GB300V2 and SF2000).
 *
 * The consoles have no GPU, no OS, no threads and no mmap, so every one of the
 * twelve platform* hooks below is either a stub or a thin wrapper over a firmware
 * symbol.  The interesting ones are time (there is no clock_gettime in this
 * newlib) and the frame pacing.
 *
 * Target facts (all measured or read out of the stock firmware):
 *   - JZ4775, MIPS32 little-endian, NO FPU (soft-float), ~810-900 MHz
 *   - 320x240 RGB565 ST7789V panel, ~57.88 Hz
 *   - os_get_tick_count() is a millisecond tick; dly_tsk(ms) sleeps
 *   - the firmware forces stereo to mono before it reaches the speaker
 *
 * NOTE on -march: it must stay plain "mips32".  The JZ4775 has no MIPS32r2
 * bit-manipulation unit, so seb/seh/ins/ext/wsbh/rotr raise ExcCode 10
 * (Reserved Instruction) and kill the core.  -mtune=mips32r2 is only a
 * scheduling hint and is safe.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <libretro.h>
#include "platformdefs.h"
#include "log.h"
#ifdef ENABLE_SW_RENDERER
#include "sw_renderer.h"
#endif
#include "hacker_loader.h"
#include "sf2000_fw.h"
#include "sf2000_audio.h"
#include "runner.h"
#include "runner_keyboard.h"
#include "runner_gamepad.h"

/* Firmware symbols provided by the core linker script
 * (sf2000_multicore/linker_scripts/bisrv_*_V2-core.ld resolves these to absolute
 * addresses inside the stock ROM). */
extern uint32_t os_get_tick_count(void);
extern int dly_tsk(unsigned ms);

/* Provided by the multicore frontend; declared by include/stockfw.h there. */
extern void xlog(const char *fmt, ...);
extern void dbg_show_noblock(unsigned short fg, unsigned short bg, const char *fmt, ...);

#define PANEL_W 320
#define PANEL_H 240

/* ------------------------------------------------------------------ logging -- */

void platformLog(const logType type, const char *format, va_list va) {
    /* xlog() compiles away unless /mnt/sda1/bios/enable_xlog.txt exists, and it
     * fs_sync()s every line, so batch the whole formatted line into one call
     * instead of emitting one per printf fragment. */
    char buf[512];
    vsnprintf(buf, sizeof(buf), format, va);
    xlog("[bs] %s", buf);

    if (hacker_loader_is_active()) {
        if (type == LOG_TYPE_ERROR) {
            hacker_loader_log("ERR", buf, 0);
        } else if (type == LOG_TYPE_WARNING) {
            hacker_loader_log("WARN", buf, 0);
        }
    }
}

/* --------------------------------------------------------------------- time -- */

uint64_t sf2000_now_millis(void) {
    return (uint64_t)os_get_tick_count();
}

void platformSleepUntil(uint64_t timeNanos) {
    /* loop.c drives this with nowNanos() values.  There is no clock_gettime and
     * no nanosleep here, and YIELD() on an unknown arch is a no-op, so spin in
     * 1 ms steps off the firmware tick. */
    uint64_t now = sf2000_now_millis() * 1000000ull;
    if (timeNanos <= now) return;
    while (sf2000_now_millis() * 1000000ull < timeNanos) {
        /* dly_tsk(1) yields to the firmware scheduler; a bare spin starves the
         * sound task and trips the watchdog on long frames. */
        dly_tsk(1);
    }
}

/* ---------------------------------------------------------------- lifecycle -- */

bool platformInit(int32_t reqW, int32_t reqH, const char *title, bool headless) {
    (void)reqW; (void)reqH; (void)headless;
    if (title && title[0])
        xlog("[bs] %s", title);
    return true;
}

static Runner *s_runner = NULL;
static bool s_prevKeys[256];

static void update_key(RunnerKeyboardState *kb, int32_t gmlKey, bool isDown) {
    if (gmlKey <= 0 || gmlKey >= 256) return;
    bool wasDown = s_prevKeys[gmlKey];
    if (isDown && !wasDown) {
        RunnerKeyboard_onKeyDown(kb, gmlKey);
        s_prevKeys[gmlKey] = true;
    } else if (!isDown && wasDown) {
        RunnerKeyboard_onKeyUp(kb, gmlKey);
        s_prevKeys[gmlKey] = false;
    }
}

void platformInitFunctions(Runner *runner) {
    s_runner = runner;
    memset(s_prevKeys, 0, sizeof(s_prevKeys));
}

void platformExit(void) {
    xlog("[bs] exit");
}

bool platformHandleEvents(void) {
    sf2000_poll_input();

    if (s_runner == NULL) return false;

    /* Read direct hardware joypad registers from the stock firmware */
    uint32_t pad = g_joy_task_state | g_joy_state;

    /* Up = 0x10, Down = 0x40, Left = 0x80, Right = 0x20
     * Select = 0x1, Start = 0x8
     * A = 0x2000, B = 0x4000, X = 0x400, Y = 0x800, L = 0x1000, R = 0x8000
     */
    bool btn_up     = ((pad & 0x0010) != 0) || (sf2000_get_input_state(RETRO_DEVICE_ID_JOYPAD_UP) != 0);
    bool btn_down   = ((pad & 0x0040) != 0) || (sf2000_get_input_state(RETRO_DEVICE_ID_JOYPAD_DOWN) != 0);
    bool btn_left   = ((pad & 0x0080) != 0) || (sf2000_get_input_state(RETRO_DEVICE_ID_JOYPAD_LEFT) != 0);
    bool btn_right  = ((pad & 0x0020) != 0) || (sf2000_get_input_state(RETRO_DEVICE_ID_JOYPAD_RIGHT) != 0);

    bool btn_a      = ((pad & 0x2000) != 0) || (sf2000_get_input_state(RETRO_DEVICE_ID_JOYPAD_A) != 0);
    bool btn_b      = ((pad & 0x4000) != 0) || (sf2000_get_input_state(RETRO_DEVICE_ID_JOYPAD_B) != 0);
    bool btn_x      = ((pad & 0x0400) != 0) || (sf2000_get_input_state(RETRO_DEVICE_ID_JOYPAD_X) != 0);
    bool btn_y      = ((pad & 0x0800) != 0) || (sf2000_get_input_state(RETRO_DEVICE_ID_JOYPAD_Y) != 0);

    bool btn_l      = ((pad & 0x1000) != 0) || (sf2000_get_input_state(RETRO_DEVICE_ID_JOYPAD_L) != 0);
    bool btn_r      = ((pad & 0x8000) != 0) || (sf2000_get_input_state(RETRO_DEVICE_ID_JOYPAD_R) != 0);
    bool btn_start  = ((pad & 0x0008) != 0) || (sf2000_get_input_state(RETRO_DEVICE_ID_JOYPAD_START) != 0);
    bool btn_select = ((pad & 0x0001) != 0) || (sf2000_get_input_state(RETRO_DEVICE_ID_JOYPAD_SELECT) != 0);

    /* Ergonomic mapping tailored to the GB300 / SF2000 layout:
     * - CONFIRM / INTERACT / ADVANCE DIALOG: A, R (shoulder trigger), START
     * - CANCEL / RUN / SKIP TEXT: B, Y (thumb roll), L (shoulder run)
     * - MENU / ITEMS / STATS: X, SELECT
     */
    bool is_confirm = btn_a || btn_r || btn_start;
    bool is_cancel  = btn_b || btn_y || btn_l;
    bool is_menu    = btn_x || btn_select;

    /* 1. Update GameMaker Keyboard state */
    if (s_runner->keyboard != NULL) {
        RunnerKeyboardState *kb = s_runner->keyboard;

        update_key(kb, VK_UP, btn_up);
        update_key(kb, VK_DOWN, btn_down);
        update_key(kb, VK_LEFT, btn_left);
        update_key(kb, VK_RIGHT, btn_right);

        update_key(kb, 'Z', is_confirm);
        update_key(kb, VK_ENTER, is_confirm);
        update_key(kb, VK_SPACE, is_confirm);

        update_key(kb, 'X', is_cancel);
        update_key(kb, VK_SHIFT, is_cancel);

        update_key(kb, 'C', is_menu);
        update_key(kb, VK_CONTROL, is_menu);
    }

    /* 2. Update GameMaker Gamepad/Joystick state */
    if (s_runner->gamepads != NULL) {
        GamepadSlot *gp = &s_runner->gamepads->slots[0];
        s_runner->gamepads->connectedCount = 1;
        gp->connected = true;

        memset(gp->buttonDown, 0, sizeof(gp->buttonDown));

        gp->buttonDown[0]  = is_confirm; // GP_FACE1 (A / Confirm)
        gp->buttonDown[1]  = is_cancel;  // GP_FACE2 (B / Cancel)
        gp->buttonDown[2]  = btn_y;      // GP_FACE3 (Y)
        gp->buttonDown[3]  = is_menu;    // GP_FACE4 (X / Menu)

        gp->buttonDown[4]  = btn_l;      // GP_SHOULDERL
        gp->buttonDown[5]  = btn_r;      // GP_SHOULDERR

        gp->buttonDown[8]  = btn_select; // GP_SELECT
        gp->buttonDown[9]  = btn_start;  // GP_START

        gp->buttonDown[12] = btn_up;     // GP_PADU
        gp->buttonDown[13] = btn_down;   // GP_PADD
        gp->buttonDown[14] = btn_left;   // GP_PADL
        gp->buttonDown[15] = btn_right;  // GP_PADR

        for (int b = 0; b < GP_BUTTON_COUNT; b++) {
            bool was = gp->buttonDownPrev[b];
            bool now = gp->buttonDown[b];
            if (now && !was) gp->buttonPressed[b] = true;
            if (!now && was) gp->buttonReleased[b] = true;
            gp->buttonValue[b] = now ? 1.0f : 0.0f;
            gp->buttonDownPrev[b] = now;
        }

        gp->axisValue[0] = btn_right ? 1.0f : (btn_left ? -1.0f : 0.0f);
        gp->axisValue[1] = btn_down  ? 1.0f : (btn_up   ? -1.0f : 0.0f);
    }

    return false;
}

void platformSwapBuffers(void) {
    /* End of frame: hand the software renderer's RGB565 framebuffer to the libretro
     * video callback (sf2000/main.c), which the frontend copies into the OSD region.
     * A no-op under ENABLE_NOOP_RENDERER, which has no framebuffer. */
    if (hacker_loader_is_active()) {
        hacker_loader_stop();
    }
#ifdef ENABLE_SW_RENDERER
    SwRenderer_present();
#endif
    Sf2000AudioSystem_renderFrame();
}

void *platformGetProcAddress(const char *name) {
    (void)name;
    return NULL; /* only consulted under USE_GLAD, which this build does not define */
}

/* ------------------------------------------------------------------ "window" -- */

bool platformGetWindowSize(int32_t *outW, int32_t *outH) {
    if (outW) *outW = PANEL_W;
    if (outH) *outH = PANEL_H;
    return true;
}

bool platformGetScaledWindowSize(int32_t *outW, int32_t *outH) {
    return platformGetWindowSize(outW, outH);
}

void platformSetWindowSize(int32_t width, int32_t height) { (void)width; (void)height; }
void platformSetWindowTitle(const char *title) { if (title) xlog("[bs] %s", title); }

void platformGetMousePos(double *xPos, double *yPos) {
    /* There is no pointer on this hardware.  Report the centre so that anything
     * which divides by a delta cannot produce a NaN; the software renderer clips
     * everything to the panel anyway. */
    if (xPos) *xPos = PANEL_W / 2.0;
    if (yPos) *yPos = PANEL_H / 2.0;
}
