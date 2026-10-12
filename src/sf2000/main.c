/*
 * main.c - libretro adapter that lets Butterscotch run as a core on the
 *          HiChip JZ4775 handhelds (GB300V2 / SF2000) under the sf2000_multicore
 *          frontend.
 *
 * Division of labour
 * ------------------
 * sf2000_multicore owns the loader, the firmware ABI (fs_*, OSD region writes, the
 * sound task), hotkeys, save states, config layering and the RGB565 blit.  It hands
 * us the plain 25-entry libretro vtable and nothing else, so this file is only
 * responsible for:
 *
 *   - finding data.win next to the ROM stub (the loader strips the extension, so
 *     info->path has no suffix to open directly)
 *   - reporting 320x240 / RGB565 / 22050 Hz mono
 *   - translating the frontend's input into Butterscotch's keyboard+gamepad state
 *   - handing the software renderer's framebuffer to retro_video_refresh_cb
 *   - starting loop() exactly once, from the first retro_run()
 *
 * Deliberately NOT requested: RETRO_ENVIRONMENT_SET_PIXEL_FORMAT.  The frontend's
 * handler only accepts XRGB8888 (and then allocates a full-size conversion buffer);
 * leaving it unset means retro_video_refresh_cb treats our buffer as RGB565 already,
 * which is the panel's native format.  See SET_PIXEL_FORMAT in
 * sf2000_multicore/src/libretro_frontend/core_api.c.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "libretro.h"
#include "loop.h"
#include "platformdefs.h"
#include "runner.h"
#include "runner_gamepad.h"
#include "runner_keyboard.h"
#ifdef ENABLE_SW_RENDERER
#include "sw_renderer.h"
#endif

#include "sf2000_fw.h"
#include "hacker_loader.h"

#define GAME_NAME    "Undertale"
#define LIBRARY_NAME "butterscotch"
#define LIBRARY_VER  "1.0"

#define PANEL_W 320
#define PANEL_H 240
#define SAMPLE_RATE 22050        /* hardware rate; anything else plays back 2x fast */

static retro_environment_t        g_env_cb;
static retro_video_refresh_t      g_video_cb;
static retro_audio_sample_t       g_audio_cb;
static retro_audio_sample_batch_t g_audio_batch_cb;
static retro_input_poll_t         g_input_poll_cb;
static retro_input_state_t        g_input_state_cb;

static bool     g_loaded;
static bool     g_started;
static char     g_dataWinPath[512];
static char     g_saveFolder[512];

/* runtime options, filled from system/configs/butterscotch/*.opt */
static struct {
    int32_t renderScale;       /* 100 = 320x240, 75 = 240x180, 50 = 160x120 */
    int32_t textureCacheKb;
    int32_t audioEnabled;
    int32_t audioCacheKb;
    int32_t frameCap;          /* 0 = uncapped */
    int32_t bilinear;
    int32_t debugOverlay;
} g_opt = { 100, 12288, 1, 1024, 0, 1, 0 };

/* --------------------------------------------------------------------------- */

static void option(const char *key, const char *fallback, char *dst, size_t n) {
    struct retro_variable var = { key, NULL };
    dst[0] = '\0';
    if (g_env_cb && g_env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        snprintf(dst, n, "%s", var.value);
    if (!dst[0])
        snprintf(dst, n, "%s", fallback);
}

static void opt_int(const char *key, const char *fallback, int32_t *out) {
    char buf[64];
    option(key, fallback, buf, sizeof(buf));
    *out = (int32_t)strtol(buf, NULL, 10);
}

static void load_options(void) {
    opt_int("butterscotch_render_scale; internal resolution percent (100=320x240)",
            "100", &g_opt.renderScale);
    opt_int("butterscotch_texture_cache_kb; texture cache budget in KB",
            "12288", &g_opt.textureCacheKb);
    opt_int("butterscotch_audio; audio enabled", "1", &g_opt.audioEnabled);
    opt_int("butterscotch_audio_cache_kb; decoded SFX cache in KB", "1024", &g_opt.audioCacheKb);
    opt_int("butterscotch_frame_cap; target fps (0 = uncapped)", "0", &g_opt.frameCap);
    opt_int("butterscotch_bilinear; bilinear filtering when upscaling sprites",
            "1", &g_opt.bilinear);
    opt_int("butterscotch_debug_overlay; show the fps/heap overlay",
            "0", &g_opt.debugOverlay);
}

/* ------------------------------------------------------------ path discovery -- */

/* Split "/a/b/c" into dir "/a/b" and leaf "c". */
static void split_path(const char *path, char *dir, size_t dirn, char *leaf, size_t leafn) {
    if (path == NULL) return;
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        if (dir && dirn) snprintf(dir, dirn, ".");
        if (leaf && leafn) snprintf(leaf, leafn, "%s", path);
        return;
    }
    size_t dl = (size_t)(slash - path);
    if (dl == 0) dl = 1;                 /* keep "/" usable */
    if (dir && dirn) {
        if (dl >= dirn) dl = dirn - 1;
        memcpy(dir, path, dl);
        dir[dl] = '\0';
    }
    if (leaf && leafn) {
        snprintf(leaf, leafn, "%s", slash + 1);
    }
}

static bool exists(const char *p) {
    if (p == NULL) return false;
    if (fs_access(p, 0) == 0) return true;
    FILE *f = fopen(p, "rb");
    if (f != NULL) { fclose(f); return true; }
    return false;
}

/* Multicore lists a game in a ROM dir either by the real file (extension intact) or by
 * a "stub": any file of 251 bytes or less.  For Undertale the WAD lives beside the
 * asset files, so a stub is the natural listing mechanism -- but a stub must never be
 * mistaken for the WAD itself, or data.win parsing fails on a 0-byte file.  Accept a
 * path directly only when it is plausibly a WAD: right extension, or big enough to
 * hold the header we are about to validate. */
#define STUB_MAX_BYTES 251u

static bool wad_size_sane(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f == NULL) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long sz = ftell(f);
    fclose(f);
    return sz > (long)STUB_MAX_BYTES;
}

static bool looks_like_wad(const char *p) {
    size_t n = strlen(p);
    if (n >= 4) {
        const char *e = p + n - 4;
        if (strcasecmp(e, ".unx") == 0 || strcasecmp(e, ".win") == 0) return true;
    }
    return wad_size_sane(p);
}

/*
 * The loader builds info->path as /mnt/sda1/ROMS/<dir>/<name> with the extension
 * already stripped, so there is nothing to open directly.  Probe, in order:
 *   <dir>/data.win  <dir>/game.unx  <dir>/<name>.unx  <dir>/<name>
 * then finally info->path itself, but only if it looks like a real WAD rather than
 * one of multicore's stub entries.
 */
static bool resolve_data_win(const char *romPath) {
    char dir[512], leaf[256], cand[640];

    if (romPath == NULL || romPath[0] == '\0') return false;

    if (exists(romPath) && looks_like_wad(romPath)) {
        snprintf(g_dataWinPath, sizeof(g_dataWinPath), "%s", romPath);
        return true;
    }

    split_path(romPath, dir, sizeof(dir), leaf, sizeof(leaf));
    snprintf(cand, sizeof(cand), "%s/data.win", dir);
    if (exists(cand)) { snprintf(g_dataWinPath, sizeof(g_dataWinPath), "%s", cand); return true; }
    snprintf(cand, sizeof(cand), "%s/game.unx", dir);
    if (exists(cand)) { snprintf(g_dataWinPath, sizeof(g_dataWinPath), "%s", cand); return true; }
    snprintf(cand, sizeof(cand), "%s/%s.unx", dir, leaf);
    if (exists(cand)) { snprintf(g_dataWinPath, sizeof(g_dataWinPath), "%s", cand); return true; }
    /* Stub entry: the listing file is a placeholder, so fall back to the extension-
     * stripped sibling of the very same name. */
    snprintf(cand, sizeof(cand), "%s/%s", dir, leaf);
    if (exists(cand) && looks_like_wad(cand)) {
        snprintf(g_dataWinPath, sizeof(g_dataWinPath), "%s", cand);
        return true;
    }

    /* Fallback: canonical ROM directory on SD card */
    if (exists("/mnt/sda1/ROMS/butterscotch/game.unx")) {
        snprintf(g_dataWinPath, sizeof(g_dataWinPath), "/mnt/sda1/ROMS/butterscotch/game.unx");
        return true;
    }
    if (exists("/mnt/sda1/ROMS/butterscotch/data.win")) {
        snprintf(g_dataWinPath, sizeof(g_dataWinPath), "/mnt/sda1/ROMS/butterscotch/data.win");
        return true;
    }

    return false;
}

/* ------------------------------------------------------------------- run glue -- */

/* Called from platformSwapBuffers() once per frame by loop(). */
void sf2000_present_frame(const void *pixels, unsigned w, unsigned h, unsigned pitch) {
    if (g_video_cb && pixels != NULL && w != 0 && h != 0)
        g_video_cb(pixels, w, h, (size_t)pitch * 2u);
}

/* Hand the software renderer the panel size, the cache budget and the directory
 * holding ATLAS.BIN.  Called before loop(), because SWRenderer_create() runs inside
 * it and needs both. */
static void configure_renderer(void) {
    SwRendererConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    switch (g_opt.renderScale) {
        case 75: cfg.width = 240; cfg.height = 180; break;
        case 50: cfg.width = 160; cfg.height = 120; break;
        default:  cfg.width = PANEL_W; cfg.height = PANEL_H; break;
    }

    /* The whole heap is 58 MiB and game.unx already claims a large slice of it, so
     * the atlas cache is a fixed budget rather than "as much as fits". */
    int32_t kb = g_opt.textureCacheKb;
    if (kb < 512) kb = 512;
    if (kb > 24576) kb = 24576;      /* 24 MiB hard ceiling */
    cfg.cacheBytes = kb * 1024;
    cfg.bilinear = g_opt.bilinear != 0;
    cfg.debugOverlay = g_opt.debugOverlay != 0;

    SwRenderer_setConfig(&cfg);

    /* ATLAS.BIN / ATLAS.RGBA4444 sit next to the WAD, i.e. next to info->path. */
    SwRenderer_setAssetDir(g_saveFolder);
}

static void build_args(CommandLineArgs *args) {
    args->exitAtFrame = -1;
    args->speedMultiplier = 1.0;
    args->fastForwardSpeed = 0.0;
    args->osType = OS_WINDOWS;
    args->profilerFramesBetween = 0;

    /*
     * Per-chunk loading plus lazy rooms/textures is not an optimisation here, it is
     * a requirement: game.unx is 60 MB and the console's whole heap is 58 MiB.  The
     * lazy paths use fopen + fseek + fread, which is all the firmware has (no mmap).
     */
    args->loadType = DATAWINLOADTYPE_LOAD_PER_CHUNK;
    args->lazyRooms = true;
    args->lazyTextures = true;
    args->lazyAudio = true;    /* AUDO chunk is loaded lazily on demand by sf2000_audio */

    args->renderer = SOFTWARE; /* our RGB565 software renderer, see sw_renderer.c */

    args->dataWinPath = g_dataWinPath;
    args->saveFolder = g_saveFolder;
}

/* ------------------------------------------------------------- libretro vtable -- */

void retro_init(void) {
    load_options();
    xlog("[bs] retro_init scale=%d texcache=%dK audio=%d\n",
         (int)g_opt.renderScale, (int)g_opt.textureCacheKb, (int)g_opt.audioEnabled);
}

void retro_deinit(void) {
    xlog("[bs] retro_deinit");
    g_loaded = false;
}

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof(*info));
    info->library_name     = LIBRARY_NAME;
    info->library_version  = LIBRARY_VER;
    info->valid_extensions = "win";
    info->need_fullpath    = true;   /* we want the real path, not the loader's blob */
    info->block_extract    = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info) {
    memset(info, 0, sizeof(*info));
    int w = PANEL_W, h = PANEL_H;
    switch (g_opt.renderScale) {
        case 75: w = 240; h = 180; break;
        case 50: w = 160; h = 120; break;
        default: break;
    }
    info->geometry.base_width   = (unsigned)w;
    info->geometry.base_height  = (unsigned)h;
    info->geometry.max_width    = (unsigned)PANEL_W;
    info->geometry.max_height   = (unsigned)PANEL_H;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
    info->timing.fps            = 60.0;
    info->timing.sample_rate    = 22050.0;
}

void retro_set_environment(retro_environment_t cb) { g_env_cb = cb; }
void retro_set_video_refresh(retro_video_refresh_t cb) {
    g_video_cb = cb;
    hacker_loader_set_video_cb(cb);
}
void retro_set_audio_sample(retro_audio_sample_t cb) { g_audio_cb = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { g_audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { g_input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { g_input_state_cb = cb; }

void sf2000_send_audio(const int16_t *samples, size_t frames) {
    if (g_audio_batch_cb && samples && frames > 0) {
        g_audio_batch_cb(samples, frames);
    }
}

void sf2000_poll_input(void) {
    if (g_input_poll_cb) g_input_poll_cb();
}

int16_t sf2000_get_input_state(unsigned id) {
    if (g_input_state_cb) {
        return g_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id);
    }
    return 0;
}

void sf2000_shutdown(void) {
    if (g_env_cb) {
        g_env_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
    }
}
void retro_set_controller_port_device(unsigned port, unsigned device) {
    (void)port; (void)device;
}
void retro_reset(void) { }
size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *data, size_t size) { (void)data; (void)size; return false; }
bool retro_unserialize(const void *data, size_t size) { (void)data; (void)size; return false; }
void retro_cheat_reset(void) { }
void retro_cheat_set(unsigned index, bool enabled, const char *code) {
    (void)index; (void)enabled; (void)code;
}
unsigned retro_get_region(void) { return 0; }
void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }

bool retro_load_game_special(unsigned type, const struct retro_game_info *info,
                                    size_t count) {
    (void)type; (void)info; (void)count;
    return false;
}

bool retro_load_game(const struct retro_game_info *info) {
    if (info == NULL) return false;

    hacker_loader_init(info->path);
    hacker_loader_log("SYS", "CORE INIT: MIPS32 SOFT-FLOAT", 5);
    hacker_loader_log("VFS", "MOUNT /mnt/sda1 ... [OK]", 8);

    if (!resolve_data_win(info->path)) {
        xlog("[bs] data.win not found near '%s'\n", info->path);
        hacker_loader_halt("ERR", "DATA.WIN / GAME.UNX NOT FOUND");
        return false;
    }

    split_path(g_dataWinPath, g_saveFolder, sizeof(g_saveFolder), NULL, 0);
    g_loaded = true;

    hacker_loader_log("WAD", g_dataWinPath, 20);
    hacker_loader_log("VFS", "CONTENT DIR RESOLVED", 25);

    xlog("[bs] data.win  = %s\n", g_dataWinPath);
    xlog("[bs] save     = %s\n", g_saveFolder);
    xlog("[bs] load start, heap base=%p\n", (void *)sbrk(0));

    hacker_loader_log_force("LOAD", "LOAD_GAME OK, RETURNING", 26);
    return true;
}

void retro_unload_game(void) {
    xlog("[bs] unload, heap in use ~%ld bytes\n", (long)(intptr_t)sbrk(0));
    g_loaded = false;
}

/*
 * loop() owns the frame pacing, the VM step and the presentation, and it does not
 * return while the game is running.  So the first retro_run() enters it and the
 * frontend's run_emulator() loop simply spins inside this call until the session
 * ends - which is exactly what a libretro core looks like from the outside.
 */
void retro_run(void) {
    hacker_loader_log_force("RUN", "RETRO_RUN ENTERED", 27);
    if (!g_started) {
        g_started = true;

        hacker_loader_log_force("BOOT", "CONFIGURING RENDERER...", 28);

#ifdef ENABLE_SW_RENDERER
        configure_renderer();
#endif

        CommandLineArgs args = {0};
        build_args(&args);
        args.debug = g_opt.debugOverlay != 0;

        hacker_loader_log_force("BOOT", "STARTING ENGINE LOOP", 30);

        loop(args, LIBRARY_NAME);

        xlog("[bs] engine loop returned, shutting down to frontend\n");
        shutdown_game();
        for (;;) {
            dly_tsk(100);
        }
    }
}
