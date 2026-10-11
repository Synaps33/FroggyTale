/*
 * sw_renderer.c - the SOFTWARE GraphicsAPI backend: a CPU blitter onto RGB565.
 *
 * There is no GPU on the JZ4775 handhelds, so this implements RendererVtable
 * directly.  Everything lands in one uint16_t framebuffer that main.c hands to
 * retro_video_refresh_cb; the frontend copies that straight into the OSD region
 * (it only speaks XRGB8888, and because we never request it, it never allocates a
 * conversion buffer - see SET_PIXEL_FORMAT in
 * sf2000_multicore/src/libretro_frontend/core_api.c).
 *
 * Design notes, roughly in order of how much they mattered:
 *
 *  - Axis-aligned fast path.  Undertale draws nearly all of its sprites unrotated,
 *    so drawSprite/drawSpritePart check `angle == 0` and run a straight scanline
 *    blit with no trig at all.  Rotated draws pay for sinf/cosf once per call,
 *    never per pixel.  This matters because the CPU is soft-float: sinf() is a
 *    ~200-instruction libgcc routine.
 *
 *  - Texture data stays RGBA4444 and is converted to RGB565 per pixel at blit time
 *    through a 4096-entry table (rgb444To565Lut).  Converting once at load time -
 *    which the first revision did - threw the alpha away entirely and is unusable:
 *    Undertale leans on alpha everywhere.  The table costs 8 KB of the 58 MiB heap
 *    and fits in the JZ4775's 8 KB dcache, and it turns the common
 *    opaque-white-tint blit into a single load.
 *
 *  - hasAlpha is computed over the whole block at load time.  A block with no
 *    partially or fully transparent texel skips the alpha test, and an
 *    effective-alpha==255 && white-tint blit skips the blend.
 *
 *  - View/GUI transforms follow the PS2 GS backend's shape (gs_renderer.c:1137):
 *    subtract the view origin, then scale into the port.  The projection matrix is
 *    ignored; Undertale is 2D and Runner only ever hands over near-identity
 *    matrices.
 */

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "atlas.h"
#include "common.h"
#include "data_win.h"
#include "log.h"
#include "renderer.h"
#include "runner.h"
#include "sw_renderer.h"
#include "text_utils.h"
#include "utils.h"

/* Panel geometry and the heap ceiling live in the SF2000 firmware header; on a host
 * build they are just the native 320x240 panel, which is what sw_renderer targets. */
#ifdef PLATFORM_SF2000
#include "sf2000_fw.h"
#else
#ifndef SF2000_PANEL_W
#define SF2000_PANEL_W 320
#endif
#ifndef SF2000_PANEL_H
#define SF2000_PANEL_H 240
#endif
#endif

#ifdef PLATFORM_SF2000
/* Defined in src/sf2000/main.c; forwards one frame to retro_video_refresh_cb. */
extern void sf2000_present_frame(const void* pixels, unsigned w, unsigned h, unsigned pitch);
#else
/* Host builds (BACKEND=sw) link a test harness that defines this and writes the frame
 * out, so the same renderer code can be diffed against the GL reference output. */
extern void sw_renderer_present_sink(const void* pixels, unsigned w, unsigned h, unsigned pitch);
#endif

/* ------------------------------------------------------- colour conversion -- */

/*
 * Indexed by (R4 << 8) | G4, i.e. 12 bits of colour; alpha is handled separately so
 * that it does not blow the table up to 64 KB.
 *
 * A flat 4-bit value replicated into 5/6 bits lands on the wrong side of the panel's
 * gamma badly enough to band on Undertale's large shaded backgrounds, so each entry
 * is dithered.  B is indexed together with R and G even though the stored pixel
 * carries its own B nibble; reusing R there is harmless because Undertale's art uses
 * blue almost exclusively as a flat secondary and any error is sub-LSB.
 */
static uint16_t rgb444To565Lut[4096];

static void buildColorLut(void) {
    for (int r = 0; r < 16; r++) {
        int r5 = (r << 1) | (r >> 3); /* 4-bit (0..15) -> 5-bit (0..31) */
        for (int g = 0; g < 16; g++) {
            int g6 = (g << 2) | (g >> 2); /* 4-bit (0..15) -> 6-bit (0..63) */
            for (int b = 0; b < 16; b++) {
                int b5 = (b << 1) | (b >> 3); /* 4-bit (0..15) -> 5-bit (0..31) */
                rgb444To565Lut[(r << 8) | (g << 4) | b] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
            }
        }
    }
}

static inline uint16_t bgrToRgb565(uint32_t c) {
    int r5 = (BGR_R(c) >> 3) & 0x1F;
    int g6 = (BGR_G(c) >> 2) & 0x3F;
    int b5 = (BGR_B(c) >> 3) & 0x1F;
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

/* src * a + dst * (255 - a), all 8-bit. */
static inline uint16_t blendRgb565(uint16_t src, uint16_t dst, int32_t a) {
    int32_t r = (((src >> 11) & 0x1F) * a + ((dst >> 11) & 0x1F) * (255 - a)) >> 8;
    int32_t g = (((src >> 5) & 0x3F) * a + ((dst >> 5) & 0x3F) * (255 - a)) >> 8;
    int32_t b = ((src & 0x1F) * a + (dst & 0x1F) * (255 - a)) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* ------------------------------------------------------------------- state -- */

typedef struct {
    Renderer base;        /* must stay first: Runner carries Renderer* everywhere */

    SwAtlas atlas;

    uint16_t* fb;         /* panel framebuffer */
    int32_t fbW, fbH;

    uint16_t* target;     /* where primitives land: fb, or a surface */
    int32_t targetW, targetH;

    /* world -> screen */
    int32_t gameW, gameH;
    int32_t viewX, viewY;
    float   scaleX, scaleY;
    int32_t portX, portY;
    bool    inGui;

    bool    atlasOpen;
    bool    bilinear;
    bool    debugOverlay;

    /* surfaces */
    uint16_t** surfacePixels;
    int32_t*   surfaceW;
    int32_t*   surfaceH;
    bool*      surfaceLive;
    uint32_t   surfaceCount;
    uint32_t   surfaceCapacity;

    /* shadowed GPU state; the getters must answer truthfully */
    bool        blendEnable;
    int32_t     blendMode;
    BlendFactors blendFactors;
    bool        alphaTestEnable;
    uint8_t     alphaTestRef;
    bool        colorWriteR, colorWriteG, colorWriteB, colorWriteA;
    bool        fogEnable;
    uint32_t    fogColor;
} SwRenderer;

static SwRenderer* g_sw;   /* SwRenderer_present() has no Renderer* to work from */
static char g_assetDir[512] = ".";
static bool g_lutBuilt = false;

/* ------------------------------------------------------------------ config -- */

void SwRenderer_setAssetDir(const char* dir) {
    if (dir == NULL || dir[0] == '\0') dir = ".";
    snprintf(g_assetDir, sizeof(g_assetDir), "%s", dir);
}

#ifdef PLATFORM_SF2000
#define SW_ASSET_DIR_ENV 0
#else
/* Host test builds have no ROM directory, so allow SW_ASSET_DIR to point at the
 * packed atlas (tools/pack_assets.py writes ATLAS.BIN + ATLAS.RGBA4444 next to
 * each other).  Unset on the device: src/sf2000/main.c calls SwRenderer_setAssetDir
 * with the directory it discovered next to info->path. */
#define SW_ASSET_DIR_ENV 1
#endif

static SwRendererConfig s_pending_cfg;
static bool s_has_pending_cfg = false;

void SwRenderer_setConfig(const SwRendererConfig* cfg) {
    if (cfg == NULL) return;
    s_pending_cfg = *cfg;
    s_has_pending_cfg = true;
    if (g_sw == NULL) return;
    if (cfg->width > 0) g_sw->fbW = cfg->width;
    if (cfg->height > 0) g_sw->fbH = cfg->height;
    g_sw->bilinear = cfg->bilinear;
    g_sw->debugOverlay = cfg->debugOverlay;
    if (cfg->cacheBytes > 0) g_sw->atlas.cacheBudget = (uint32_t)cfg->cacheBytes;
}

void SwRenderer_present(void) {
    if (g_sw == NULL || g_sw->fb == NULL) return;
#ifdef PLATFORM_SF2000
    sf2000_present_frame(g_sw->fb, (unsigned)g_sw->fbW, (unsigned)g_sw->fbH,
                         (unsigned)g_sw->fbW);
#else
    sw_renderer_present_sink(g_sw->fb, (unsigned)g_sw->fbW, (unsigned)g_sw->fbH,
                             (unsigned)g_sw->fbW);
#endif
}

static bool ensureFramebuffer(SwRenderer* sw, int32_t w, int32_t h) {
    if (w <= 0 || h <= 0) return false;
    if (w > SF2000_PANEL_W) w = SF2000_PANEL_W;
    if (h > SF2000_PANEL_H) h = SF2000_PANEL_H;
    if (sw->fb != NULL && sw->fbW == w && sw->fbH == h) return true;

    uint16_t* fb = (uint16_t*)safeCalloc((size_t)w * (size_t)h, sizeof(uint16_t));
    if (fb == NULL) return false;
    free(sw->fb);
    sw->fb = fb;
    sw->fbW = w;
    sw->fbH = h;
    sw->target = fb;
    sw->targetW = w;
    sw->targetH = h;
    return true;
}

/* --------------------------------------------------------------- transform -- */

static inline int32_t toScreenX(SwRenderer* sw, float wx) {
    if (sw->inGui)
        return (int32_t)lrintf(wx * sw->scaleX + (float)sw->portX);
    return (int32_t)lrintf((wx - (float)sw->viewX) * sw->scaleX + (float)sw->portX);
}

static inline int32_t toScreenY(SwRenderer* sw, float wy) {
    if (sw->inGui)
        return (int32_t)lrintf(wy * sw->scaleY + (float)sw->portY);
    return (int32_t)lrintf((wy - (float)sw->viewY) * sw->scaleY + (float)sw->portY);
}

static inline float worldScaleX(SwRenderer* sw, float v) { return v * sw->scaleX; }
static inline float worldScaleY(SwRenderer* sw, float v) { return v * sw->scaleY; }

/* -------------------------------------------------------------- primitives -- */

static inline void setPixel(SwRenderer* sw, int32_t x, int32_t y, uint32_t c, int32_t a) {
    if (x < 0 || y < 0 || x >= sw->targetW || y >= sw->targetH) return;
    uint16_t* p = sw->target + (size_t)y * sw->targetW + x;
    *p = (a >= 255) ? (uint16_t)c : blendRgb565((uint16_t)c, *p, a);
}

static void fillRect(SwRenderer* sw, int32_t x, int32_t y, int32_t w, int32_t h,
                     uint32_t color, float alpha) {
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > sw->targetW) w = sw->targetW - x;
    if (y + h > sw->targetH) h = sw->targetH - y;
    if (w <= 0 || h <= 0) return;

    uint16_t c = bgrToRgb565(color);
    int32_t a = (int32_t)(alpha * 255.0f);
    if (!sw->colorWriteR && !sw->colorWriteG && !sw->colorWriteB) return;

    for (int32_t row = 0; row < h; row++) {
        uint16_t* d = sw->target + (size_t)(y + row) * sw->targetW + x;
        if (a >= 255) {
            for (int32_t col = 0; col < w; col++) d[col] = c;
        } else if (a > 0) {
            for (int32_t col = 0; col < w; col++) d[col] = blendRgb565(c, d[col], a);
        }
    }
}

static void strokeRect(SwRenderer* sw, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                       uint32_t color, float alpha) {
    int32_t w = x2 - x1, h = y2 - y1;
    if (w < 0) { x1 += w; w = -w; }
    if (h < 0) { y1 += h; h = -h; }
    fillRect(sw, x1, y1, w + 1, 1, color, alpha);
    fillRect(sw, x1, y1 + h, w + 1, 1, color, alpha);
    fillRect(sw, x1, y1, 1, h + 1, color, alpha);
    fillRect(sw, x1 + w, y1, 1, h + 1, color, alpha);
}

/* Bresenham.  Undertale's lines are UI rules and debug overlays, not artwork. */
static void drawLineRaw(SwRenderer* sw, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                        int32_t width, uint32_t color, float alpha) {
    int32_t dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int32_t dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    if (width < 1) width = 1;
    int32_t half = width / 2;

    for (;;) {
        if (half == 0) {
            setPixel(sw, x0, y0, bgrToRgb565(color), (int32_t)(alpha * 255.0f));
        } else {
            fillRect(sw, x0 - half, y0 - half, width, width, color, alpha);
        }
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Convex scanline fill.  Triangle colours are a per-corner gradient in the GL
 * backend; on a 320x240 panel with no GPU the error is invisible, so this flattens
 * to c1. */
static void fillTriangle(SwRenderer* sw, float ax, float ay, float bx, float by,
                         float cx, float cy, uint32_t color, float alpha) {
    float minYf = fminf(ay, fminf(by, cy));
    float maxYf = fmaxf(ay, fmaxf(by, cy));
    int32_t minY = (int32_t)floorf(minYf);
    int32_t maxY = (int32_t)ceilf(maxYf);
    if (minY < 0) minY = 0;
    if (maxY > sw->targetH - 1) maxY = sw->targetH - 1;

    float area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
    if (area == 0.0f) return;

    for (int32_t y = minY; y <= maxY; y++) {
        float sy = (float)y + 0.5f;
        float xs[4];
        int32_t n = 0;

        for (int32_t i = 0; i < 3; i++) {
            int32_t j = (i + 1) % 3;
            float x0 = (i == 0) ? ax : (i == 1 ? bx : cx);
            float y0 = (i == 0) ? ay : (i == 1 ? by : cy);
            float x1 = (j == 0) ? ax : (j == 1 ? bx : cx);
            float y1 = (j == 0) ? ay : (j == 1 ? by : cy);
            if (y0 == y1) continue;

            int32_t k = 3 - i - j;              /* opposite vertex */
            float ox = (k == 0) ? ax : (k == 1 ? bx : cx);
            float oy = (k == 0) ? ay : (k == 1 ? by : cy);

            float t = (sy - y0) / (y1 - y0);
            float x = x0 + t * (x1 - x0);

            /* half-space of the opposite vertex; valid because the triangle is convex */
            float cross = (x1 - x0) * (oy - y0) - (y1 - y0) * (ox - x0);
            float dxv  = (x1 - x0) * (sy - y0) - (y1 - y0) * (x  - x0);
            if ((cross > 0.0f) != (dxv > 0.0f)) continue;

            if (n < 4) xs[n++] = x;
        }
        if (n < 2) continue;

        float lo = fminf(xs[0], xs[1]);
        float hi = fmaxf(xs[0], xs[1]);
        int32_t x0i = (int32_t)ceilf(lo);
        int32_t x1i = (int32_t)ceilf(hi);
        if (x0i < 0) x0i = 0;
        if (x1i > sw->targetW) x1i = sw->targetW;
        if (x1i > x0i) fillRect(sw, x0i, y, x1i - x0i, 1, color, alpha);
    }
}

/* ------------------------------------------------------------------- tint -- */

typedef struct {
    int32_t r8, g8, b8;   /* 0..255 multipliers */
    bool    isWhite;
} SwTint;

static inline SwTint makeTint(uint32_t color) {
    SwTint t;
    t.r8 = BGR_R(color);
    t.g8 = BGR_G(color);
    t.b8 = BGR_B(color);
    t.isWhite = (t.r8 == 255 && t.g8 == 255 && t.b8 == 255);
    return t;
}

/* RGBA4444 texel -> RGB565, honouring an 8-bit colour tint. */
static inline uint16_t shadeTexel(uint16_t p, SwTint tint) {
    if (tint.isWhite)
        return rgb444To565Lut[(p >> 4) & 0x0FFF];

    int32_t r = (int32_t)(((p >> 12) & 0xF) * 17) * tint.r8 >> 8;
    int32_t g = (int32_t)(((p >> 8) & 0xF) * 17) * tint.g8 >> 8;
    int32_t b = (int32_t)(((p >> 4) & 0xF) * 17) * tint.b8 >> 8;
    return (uint16_t)((((r * 31) >> 5) << 11) | (((g * 63) >> 6) << 5) | ((b * 31) >> 5));
}

/* 64-entry perceptual gamma coverage LUT for 2x2 downscaling (a_sum: 0..60).
 * Prevents thin 1-pixel strokes from disappearing while avoiding character fusion. */
static const uint8_t g_downscaleLut[64] = {
      0,  26,  39,  50,  60,  68,  76,  83,  90,  97, 103, 110, 115, 121, 127, 132,
    137, 142, 147, 152, 157, 161, 166, 170, 175, 179, 183, 188, 192, 196, 200, 204,
    208, 212, 216, 219, 223, 227, 230, 234, 238, 241, 245, 248, 252, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
};

/* --------------------------------------------------------------- the blit -- */

/*
 * Source sub-rect (sx, sy, sw_, sh_) in block pixels -> dest rect in screen pixels.
 * Axis-aligned only; the rotated path lives in blitBlockRotated.
 */
static void blitBlock(SwRenderer* sw, SwAtlasBlock* blk,
                      int32_t sx, int32_t sy, int32_t sw_, int32_t sh_,
                      int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                      bool flipX, bool flipY, SwTint tint, float alpha) {
    if (dw <= 0 || dh <= 0 || sw_ <= 0 || sh_ <= 0) return;

    int32_t x0 = dx, y0 = dy, x1 = dx + dw, y1 = dy + dh;
    if (x1 <= 0 || y1 <= 0 || x0 >= sw->targetW || y0 >= sw->targetH) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > sw->targetW) x1 = sw->targetW;
    if (y1 > sw->targetH) y1 = sw->targetH;
    int32_t w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0) return;

    /* 16.16 fixed point step and starting sub-pixel positions; avoids float divide per pixel. */
    int32_t stepX = (int32_t)(((int64_t)sw_ << 16) / dw);
    int32_t stepY = (int32_t)(((int64_t)sh_ << 16) / dh);
    int32_t startXFix = (sx << 16) + (int32_t)(((int64_t)(x0 - dx) * (int64_t)sw_ << 16) / dw);
    int32_t startYFix = (sy << 16) + (int32_t)(((int64_t)(y0 - dy) * (int64_t)sh_ << 16) / dh);

    int32_t gAlpha = (int32_t)(alpha * 255.0f);
    bool blendOn = sw->blendEnable;
    int32_t testRef = sw->alphaTestEnable ? (int32_t)sw->alphaTestRef : 0;

    const bool simple = !blk->hasAlpha && !sw->alphaTestEnable &&
                        !flipX && !flipY && stepX == 65536 && stepY == 65536;

    if (simple && (!blendOn || gAlpha >= 255) && tint.isWhite) {
        /* Fastest case: opaque block, 1:1, no tint. One load, no test, no blend. */
        int32_t sx0 = sx + (int32_t)(((int64_t)(x0 - dx) * sw_) / dw);
        int32_t sy0 = sy + (int32_t)(((int64_t)(y0 - dy) * sh_) / dh);
        for (int32_t row = 0; row < h; row++) {
            const uint16_t* s = blk->pixels + (size_t)(sy0 + row) * blk->w + sx0;
            uint16_t* d = sw->target + (size_t)(y0 + row) * sw->targetW + x0;
            for (int32_t col = 0; col < w; col++)
                d[col] = rgb444To565Lut[(s[col] >> 4) & 0x0FFF];
        }
        return;
    }

    int32_t currYFix = startYFix;
    bool downscale = (stepX >= 98304 || stepY >= 98304);

    if (downscale && blk->hasAlpha) {
        /* 2x2 area-coverage filter with gamma LUT for text and downscaled sprites.
         * Ensures 1-pixel strokes (dots on 'i', crossbars on 't'/'e') are never dropped,
         * while proper alpha blending prevents characters from fusing together. */
        for (int32_t row = 0; row < h; row++, currYFix += stepY) {
            int32_t syPix0 = currYFix >> 16;
            if (flipY) syPix0 = sy + (sh_ - 1) - (syPix0 - sy);
            if (syPix0 < sy) syPix0 = sy;
            if (syPix0 >= sy + sh_) syPix0 = sy + sh_ - 1;

            int32_t syPix1 = syPix0 + 1;
            if (syPix1 >= sy + sh_) syPix1 = syPix0;

            const uint16_t* s0 = blk->pixels + (size_t)syPix0 * blk->w;
            const uint16_t* s1 = blk->pixels + (size_t)syPix1 * blk->w;

            uint16_t* d = sw->target + (size_t)(y0 + row) * sw->targetW + x0;
            int32_t currXFix = startXFix;

            for (int32_t col = 0; col < w; col++, currXFix += stepX) {
                int32_t sxi0 = currXFix >> 16;
                if (flipX) sxi0 = sx + (sw_ - 1) - (sxi0 - sx);
                if (sxi0 < sx) sxi0 = sx;
                if (sxi0 >= sx + sw_) sxi0 = sx + sw_ - 1;

                int32_t sxi1 = sxi0 + 1;
                if (sxi1 >= sx + sw_) sxi1 = sxi0;

                uint16_t p00 = s0[sxi0];
                uint16_t p01 = s0[sxi1];
                uint16_t p10 = s1[sxi0];
                uint16_t p11 = s1[sxi1];

                int32_t a00 = p00 & 0xF;
                int32_t a01 = p01 & 0xF;
                int32_t a10 = p10 & 0xF;
                int32_t a11 = p11 & 0xF;

                int32_t a_sum = a00 + a01 + a10 + a11;
                if (a_sum == 0) continue;

                uint16_t bestP = p00;
                int32_t maxA = a00;
                if (a01 > maxA) { maxA = a01; bestP = p01; }
                if (a10 > maxA) { maxA = a10; bestP = p10; }
                if (a11 > maxA) { maxA = a11; bestP = p11; }

                int32_t effA = (int32_t)g_downscaleLut[a_sum > 63 ? 63 : a_sum];
                if (gAlpha < 255) effA = (effA * gAlpha) >> 8;
                if (effA <= testRef) continue;

                uint16_t c = shadeTexel(bestP, tint);
                d[col] = (!blendOn || effA >= 255) ? c : blendRgb565(c, d[col], effA);
            }
        }
        return;
    }

    for (int32_t row = 0; row < h; row++, currYFix += stepY) {
        int32_t syPix = currYFix >> 16;
        if (flipY) syPix = sy + (sh_ - 1) - (syPix - sy);
        if (syPix < sy) syPix = sy;
        if (syPix >= sy + sh_) syPix = sy + sh_ - 1;
        const uint16_t* s = blk->pixels + (size_t)syPix * blk->w;

        uint16_t* d = sw->target + (size_t)(y0 + row) * sw->targetW + x0;
        int32_t currXFix = startXFix;

        for (int32_t col = 0; col < w; col++, currXFix += stepX) {
            int32_t sxi = currXFix >> 16;
            if (flipX) sxi = sx + (sw_ - 1) - (sxi - sx);
            if (sxi < sx) sxi = sx;
            if (sxi >= sx + sw_) sxi = sx + sw_ - 1;

            uint16_t p = s[sxi];
            int32_t a4 = p & 0xF;
            if (a4 == 0) continue;
            if (a4 * 17 <= testRef) continue;              /* GL-style GREATER test */

            uint16_t c = shadeTexel(p, tint);
            int32_t effA = (a4 < 15) ? ((gAlpha * (a4 * 17)) >> 8) : gAlpha;
            if (effA <= 0) continue;

            d[col] = (!blendOn || effA >= 255) ? c : blendRgb565(c, d[col], effA);
        }
    }
}

/* Rare path: arbitrary rotation, sampled by inverse-mapping the dest rect. */
static void blitBlockRotated(SwRenderer* sw, SwAtlasBlock* blk,
                             int32_t sx, int32_t sy, int32_t sw_, int32_t sh_,
                             int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                             float cosA, float sinA,
                             SwTint tint, float alpha) {
    if (dw <= 0 || dh <= 0 || sw_ <= 0 || sh_ <= 0) return;
    if (dx >= sw->targetW || dy >= sw->targetH) return;
    if (dx + dw <= 0 || dy + dh <= 0) return;

    int32_t gAlpha = (int32_t)(alpha * 255.0f);
    bool blendOn = sw->blendEnable;
    int32_t testRef = sw->alphaTestEnable ? (int32_t)sw->alphaTestRef : 0;
    float invW = 1.0f / (float)dw, invH = 1.0f / (float)dh;

    for (int32_t row = 0; row < dh; row++) {
        int32_t py = dy + row;
        if (py < 0 || py >= sw->targetH) continue;
        uint16_t* d = sw->target + (size_t)py * sw->targetW;

        for (int32_t col = 0; col < dw; col++) {
            int32_t px = dx + col;
            if (px < 0 || px >= sw->targetW) continue;

            float ox = (float)col + 0.5f - (float)dw * 0.5f;
            float oy = (float)row + 0.5f - (float)dh * 0.5f;
            float u = (ox * cosA + oy * sinA) * invW + 0.5f;
            float v = (-ox * sinA + oy * cosA) * invH + 0.5f;
            if (u < 0.0f || v < 0.0f || u >= 1.0f || v >= 1.0f) continue;

            int32_t sxi = sx + (int32_t)(u * (float)sw_);
            int32_t syi = sy + (int32_t)(v * (float)sh_);
            if (sxi >= blk->w) sxi = blk->w - 1;
            if (syi >= blk->h) syi = blk->h - 1;

            uint16_t p = blk->pixels[(size_t)syi * blk->w + sxi];
            int32_t a4 = p & 0xF;
            if (a4 == 0 || a4 * 17 <= testRef) continue;

            uint16_t c = shadeTexel(p, tint);
            int32_t effA = (a4 < 15) ? ((gAlpha * (a4 * 17)) >> 8) : gAlpha;
            if (effA <= 0) continue;

            d[px] = (!blendOn || effA >= 255) ? c : blendRgb565(c, d[px], effA);
        }
    }
}

/* ---------------------------------------------------- sprite entry points -- */

/*
 * Shared body for drawSprite / drawSpritePart.
 *
 *   srcX/srcY/srcW/srcH  source sub-rect, in source-rect pixels (0,0,0,0 = all)
 *   localX/localY       offset of the drawn content from the anchor, in world units
 *   useTargetSize       size the content draws at: targetWidth/Height (draw_sprite)
 *                       versus the sub-rect itself (draw_sprite_part)
 */
static void blitSprite(SwRenderer* sw, int32_t tpagIndex,
                       float x, float y, float localX, float localY,
                       float destW, float destH,
                       float srcX, float srcY, float srcW, float srcH,
                       float xscale, float yscale, float angleDeg,
                       float pivotX, float pivotY,
                       uint32_t color, float alpha) {
    SwAtlasBlock* blk = SwAtlas_block(&sw->atlas, tpagIndex);
    if (blk == NULL) return;
    const SwAtlasRecord* rec = SwAtlas_record(&sw->atlas, tpagIndex);
    if (rec == NULL) { SwAtlasCache_release(&sw->atlas, blk); return; }

    /* Map source-rect coordinates onto block pixels.  A capped block is smaller than
     * the original rect, and this is the only place that knows it. */
    float toBlockX = (rec->srcWidth > 0) ? (float)blk->w / (float)rec->srcWidth : 1.0f;
    float toBlockY = (rec->srcHeight > 0) ? (float)blk->h / (float)rec->srcHeight : 1.0f;

    int32_t sx = (int32_t)lrintf(srcX * toBlockX);
    int32_t sy = (int32_t)lrintf(srcY * toBlockY);
    int32_t sW = (int32_t)lrintf(srcW * toBlockX);
    int32_t sH = (int32_t)lrintf(srcH * toBlockY);
    if (sW <= 0 || sH <= 0) { SwAtlasCache_release(&sw->atlas, blk); return; }
    if (sW > blk->w) sW = blk->w;
    if (sH > blk->h) sH = blk->h;

    bool flipX = xscale < 0.0f;
    bool flipY = yscale < 0.0f;

    float sxScreen = worldScaleX(sw, destW);
    float syScreen = worldScaleY(sw, destH);
    int32_t dw = (int32_t)lrintf(fabsf(sxScreen));
    int32_t dh = (int32_t)lrintf(fabsf(syScreen));
    if (dw <= 0 || dh <= 0) { SwAtlasCache_release(&sw->atlas, blk); return; }

    /* Anchor in screen space; the rotation pivot is applied relative to it. */
    float anchorX = x + localX * xscale;
    float anchorY = y + localY * yscale;
    int32_t axScreen = toScreenX(sw, anchorX);
    int32_t ayScreen = toScreenY(sw, anchorY);
    int32_t dx = axScreen;
    int32_t dy = ayScreen;

    if (angleDeg == 0.0f) {
        if (flipX) dx -= dw;
        if (flipY) dy -= dh;
        blitBlock(sw, blk, sx, sy, sW, sH, dx, dy, dw, dh, flipX, flipY,
                  makeTint(color), alpha);
    } else {
        /* Rotate the destination rect's top-left about (pivotX, pivotY) in world
         * space, matching glDrawSpritePartColor's construction. */
        float rad = -angleDeg * ((float)M_PI / 180.0f);
        float cs = cosf(rad), sn = sinf(rad);
        float px = pivotX * sw->scaleX, py = pivotY * sw->scaleY;
        float ox = (float)dx - px, oy = (float)dy - py;
        int32_t rdx = (int32_t)lrintf(cs * ox - sn * oy + px);
        int32_t rdy = (int32_t)lrintf(sn * ox + cs * oy + py);
        blitBlockRotated(sw, blk, sx, sy, sW, sH, rdx, rdy, dw, dh, cs, sn,
                         makeTint(color), alpha);
    }

    SwAtlasCache_release(&sw->atlas, blk);
}

static void swDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y,
                         float originX, float originY, float xscale, float yscale,
                         float angleDeg, uint32_t color, float alpha) {
    SwRenderer* sw = (SwRenderer*)renderer;
    const SwAtlasRecord* rec = SwAtlas_record(&sw->atlas, tpagIndex);
    if (rec == NULL) return;

    /* Mirrors glDrawSprite: the drawn content is targetWidth/Height, positioned by
     * targetX/targetY relative to the sprite origin. */
    float localX = (float)rec->contentX - originX;
    float localY = (float)rec->contentY - originY;
    blitSprite(sw, tpagIndex, x, y, localX, localY,
               (float)rec->contentW * xscale, (float)rec->contentH * yscale,
               0.0f, 0.0f, (float)rec->srcWidth, (float)rec->srcHeight,
               xscale, yscale, angleDeg, x, y, color, alpha);
}

static void swDrawSpritePart(Renderer* renderer, int32_t tpagIndex,
                             float srcOffX, float srcOffY, float srcW, float srcH,
                             float x, float y, float xscale, float yscale,
                             float angleDeg, float pivotX, float pivotY,
                             uint32_t color, float alpha) {
    SwRenderer* sw = (SwRenderer*)renderer;
    /* draw_sprite_part ignores the sprite origin: the sub-rect's own top-left is the
     * anchor, and dest size is srcW*scale (see glDrawSpritePartColor). */
    blitSprite(sw, tpagIndex, x, y, 0.0f, 0.0f,
               srcW * xscale, srcH * yscale,
               srcOffX, srcOffY, srcW, srcH,
               xscale, yscale, angleDeg, pivotX, pivotY, color, alpha);
}

static void swDrawSpritePartColor(Renderer* renderer, int32_t tpagIndex,
                                  float srcOffX, float srcOffY, float srcW, float srcH,
                                  float x, float y, float xscale, float yscale,
                                  float angleDeg, float pivotX, float pivotY,
                                  uint32_t c1, MAYBE_UNUSED uint32_t c2,
                                  MAYBE_UNUSED uint32_t c3, MAYBE_UNUSED uint32_t c4,
                                  float alpha) {
    /* Per-corner gradients are a GPU nicety; c1 is representative enough that
     * flattening is invisible at 320x240. */
    swDrawSpritePart(renderer, tpagIndex, srcOffX, srcOffY, srcW, srcH,
                     x, y, xscale, yscale, angleDeg, pivotX, pivotY, c1, alpha);
}

static void swDrawSpritePos(Renderer* renderer, int32_t tpagIndex,
                            float x1, float y1, float x2, float y2,
                            float x3, float y3, float x4, float y4, float alpha) {
    SwRenderer* sw = (SwRenderer*)renderer;
    SwAtlasBlock* blk = SwAtlas_block(&sw->atlas, tpagIndex);
    if (blk == NULL) return;
    const SwAtlasRecord* rec = SwAtlas_record(&sw->atlas, tpagIndex);
    if (rec == NULL) { SwAtlasCache_release(&sw->atlas, blk); return; }

    /* Two triangles across the arbitrary quad, using screen-space affine mapping.
     * draw_sprite_pos is used by Undertale for perspective-ish effects only. */
    SwTint tint = makeTint(0xFFFFFF);
    int32_t gAlpha = (int32_t)(alpha * 255.0f);
    bool blendOn = sw->blendEnable;

    const float xs4[4] = { x1, x2, x3, x4 };
    const float ys4[4] = { y1, y2, y3, y4 };
    for (int32_t tri = 0; tri < 2; tri++) {
        int32_t i0 = tri == 0 ? 0 : 1;
        int32_t i1 = tri == 0 ? 1 : 2;
        int32_t i2 = tri == 0 ? 2 : 3;
        if (tri == 1) { i0 = 1; i1 = 3; i2 = 2; }

        float p0x = xs4[i0], p0y = ys4[i0];
        float p1x = xs4[i1], p1y = ys4[i1];
        float p2x = xs4[i2], p2y = ys4[i2];
        float det = (p1x - p0x) * (p2y - p0y) - (p1y - p0y) * (p2x - p0x);
        if (det == 0.0f) continue;

        float minXf = fminf(p0x, fminf(p1x, p2x)), maxXf = fmaxf(p0x, fmaxf(p1x, p2x));
        float minYf = fminf(p0y, fminf(p1y, p2y)), maxYf = fmaxf(p0y, fmaxf(p1y, p2y));
        int32_t y0 = (int32_t)floorf(minYf), y1i = (int32_t)ceilf(maxYf);
        if (y0 < 0) y0 = 0;
        if (y1i > sw->targetH - 1) y1i = sw->targetH - 1;

        for (int32_t y = y0; y <= y1i; y++) {
            float py = (float)y + 0.5f;
            for (int32_t x = (int32_t)floorf(minXf); x <= (int32_t)ceilf(maxXf); x++) {
                if (x < 0 || x >= sw->targetW) continue;
                float px = (float)x + 0.5f;
                float w0 = ((p1x - p0x) * (py - p0y) - (p1y - p0y) * (px - p0x)) / det;
                float w1 = ((px - p0x) * (p2y - p0y) - (py - p0y) * (p2x - p0x)) / det;
                float w2 = 1.0f - w0 - w1;
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;

                float u = w1, v = w2;
                int32_t sxi = (int32_t)(u * (float)blk->w);
                int32_t syi = (int32_t)(v * (float)blk->h);
                if (sxi >= blk->w) sxi = blk->w - 1;
                if (syi >= blk->h) syi = blk->h - 1;

                uint16_t p = blk->pixels[(size_t)syi * blk->w + sxi];
                int32_t a4 = p & 0xF;
                if (a4 == 0) continue;
                uint16_t c = shadeTexel(p, tint);
                int32_t effA = (a4 < 15) ? ((gAlpha * (a4 * 17)) >> 8) : gAlpha;
                if (effA <= 0) continue;
                uint16_t* dp = sw->target + (size_t)y * sw->targetW + x;
                *dp = (!blendOn || effA >= 255) ? c : blendRgb565(c, *dp, effA);
            }
        }
    }
    SwAtlasCache_release(&sw->atlas, blk);
    (void)rec;
}

static void swDrawSpriteTiled(Renderer* renderer, int32_t tpagIndex,
                              MAYBE_UNUSED float originX, MAYBE_UNUSED float originY,
                              float x, float y, float xscale, float yscale,
                              bool tileX, bool tileY, float roomW, float roomH,
                              uint32_t color, float alpha) {
    SwRenderer* sw = (SwRenderer*)renderer;
    const SwAtlasRecord* rec = SwAtlas_record(&sw->atlas, tpagIndex);
    if (rec == NULL) return;

    float w = (float)rec->contentW * fabsf(xscale);
    float h = (float)rec->contentH * fabsf(yscale);
    if (w <= 0.0f || h <= 0.0f) return;

    float startX = x, endX = x + (tileX ? roomW : w);
    float startY = y, endY = y + (tileY ? roomH : h);

    if (tileX && sw->scaleX > 0.0f) {
        float viewLeft = (float)sw->viewX;
        float viewRight = viewLeft + (float)sw->targetW / sw->scaleX;
        float offset = fmodf(viewLeft - x, w);
        if (offset < 0.0f) offset += w;
        startX = fmaxf(x, viewLeft - offset);
        endX = fminf(x + roomW, viewRight + w);
    }

    if (tileY && sw->scaleY > 0.0f) {
        float viewTop = (float)sw->viewY;
        float viewBottom = viewTop + (float)sw->targetH / sw->scaleY;
        float offset = fmodf(viewTop - y, h);
        if (offset < 0.0f) offset += h;
        startY = fmaxf(y, viewTop - offset);
        endY = fminf(y + roomH, viewBottom + h);
    }

    for (float curY = startY; curY < endY; curY += h) {
        for (float curX = startX; curX < endX; curX += w) {
            swDrawSprite(renderer, tpagIndex, curX, curY,
                         0.0f, 0.0f, xscale, yscale, 0.0f, color, alpha);
        }
    }
}

/* ------------------------------------------------------------------- text -- */

static void drawTextInternal(SwRenderer* sw, const char* text, float x, float y,
                             float xscale, float yscale, float angleDeg,
                             int32_t c1, int32_t c2, int32_t c3, int32_t c4,
                              float alpha, float lineSeparation) {
    DataWin* dw = sw->base.dataWin;
    if (text == NULL) return;
    int32_t fontIndex = sw->base.drawFont;
    if (fontIndex < 0 || (uint32_t)fontIndex >= dw->font.count) return;

    Font* font = &dw->font.fonts[fontIndex];
    int32_t textLen = (int32_t)strlen(text);
    if (textLen == 0) return;

    int32_t lineCount = TextUtils_countLines(text, textLen);
    float lineStride = (lineSeparation < 0.0f)
                     ? TextUtils_lineStride(font)
                     : (lineSeparation / (font->scaleY != 0.0f ? font->scaleY : 1.0f));

    float valignOffset = 0.0f;
    if (sw->base.drawValign == 1) valignOffset = -((float)lineCount * lineStride) / 2.0f;
    else if (sw->base.drawValign == 2) valignOffset = -((float)lineCount * lineStride);

    bool lerpColors = (c1 != c2 || c2 != c3 || c3 != c4);
    float scaleX = xscale * font->scaleX;
    float scaleY = yscale * font->scaleY;

    int32_t lineStart = 0;
    float cursorY = valignOffset - (float)font->ascenderOffset;

    for (int32_t lineIdx = 0; lineIdx < lineCount; lineIdx++) {
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(text[lineEnd])) lineEnd++;
        int32_t lineLen = lineEnd - lineStart;

        float cursorX = 0.0f;
        if (sw->base.drawHalign != 0) {
            float lineWidth = TextUtils_measureLineWidth(font, text + lineStart, lineLen);
            if (sw->base.drawHalign == 1) cursorX = -lineWidth / 2.0f;
            else if (sw->base.drawHalign == 2) cursorX = -lineWidth;
        }

        float lineWidth = 0.0f;
        if (lerpColors) lineWidth = TextUtils_measureLineWidth(font, text + lineStart, lineLen);
        float gradientX = 0.0f;

        int32_t pos = 0;
        uint16_t ch = 0;
        bool hasCh = (lineLen > 0);
        if (hasCh) ch = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);

        while (hasCh) {
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            uint16_t nextCh = 0;
            bool hasNext = lineLen > pos;
            if (hasNext) nextCh = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);

            if (glyph != nullptr) {
                float gx = x + (cursorX + (float)glyph->offset) * scaleX;
                float gy = y + cursorY * scaleY;

                uint32_t col = (uint32_t)c1;
                if (lerpColors) {
                    float f = (lineWidth > 0.0f) ? (gradientX / lineWidth) : 0.0f;
                    col = (uint32_t)Color_lerp(c1, c2, f);
                }

                if (font->isSpriteFont && font->spriteIndex >= 0 &&
                    (uint32_t)font->spriteIndex < dw->sprt.count) {
                    Sprite* sp = &dw->sprt.sprites[font->spriteIndex];
                    int32_t gi = (int32_t)(glyph - font->glyphs);
                    if (gi >= 0 && (uint32_t)gi < (uint32_t)sp->textureCount) {
                        int32_t tpagIdx = sp->tpagIndices[gi];
                        if (tpagIdx >= 0)
                            swDrawSprite((Renderer*)sw, tpagIdx, gx, gy, 0.0f, 0.0f,
                                         scaleX, scaleY, angleDeg, col, alpha);
                    }
                } else if (font->tpagIndex >= 0 && glyph->sourceWidth != 0 &&
                           glyph->sourceHeight != 0) {
                    swDrawSpritePart((Renderer*)sw, font->tpagIndex,
                                     (float)glyph->sourceX, (float)glyph->sourceY,
                                     (float)glyph->sourceWidth, (float)glyph->sourceHeight,
                                     gx, gy,
                                     scaleX, scaleY, angleDeg, 0.0f, 0.0f, col, alpha);
                }
                gradientX += (float)glyph->shift;
                cursorX += (float)glyph->shift;
            }

            if (hasNext) {
                float kern = (glyph != nullptr) ? TextUtils_getKerningOffset(glyph, nextCh) : 0.0f;
                cursorX += kern;
            }
            ch = nextCh;
            hasCh = hasNext;
        }
        lineStart = TextUtils_skipNewline(text, lineEnd, textLen);
        cursorY += lineStride;
    }
}

static void swDrawText(Renderer* renderer, const char* text, float x, float y,
                       float xscale, float yscale, float angleDeg, float lineSeparation) {
    SwRenderer* sw = (SwRenderer*)renderer;
    uint32_t c = renderer->drawColor;
    drawTextInternal(sw, text, x, y, xscale, yscale, angleDeg,
                     (int32_t)c, (int32_t)c, (int32_t)c, (int32_t)c,
                     renderer->drawAlpha, lineSeparation);
}

static void swDrawTextColor(Renderer* renderer, const char* text, float x, float y,
                            float xscale, float yscale, float angleDeg,
                            int32_t c1, int32_t c2, int32_t c3, int32_t c4,
                            float alpha, float lineSeparation) {
    drawTextInternal((SwRenderer*)renderer, text, x, y, xscale, yscale, angleDeg,
                     c1, c2, c3, c4, alpha, lineSeparation);
}

/* ---------------------------------------------------------------- surfaces -- */

static void ensureSurfaceCapacity(SwRenderer* sw, uint32_t needed) {
    if (needed <= sw->surfaceCapacity) return;
    uint32_t cap = sw->surfaceCapacity ? sw->surfaceCapacity * 2 : 8;
    while (cap < needed) cap *= 2;
    sw->surfacePixels = (uint16_t**)safeRealloc(sw->surfacePixels, cap * sizeof(uint16_t*));
    sw->surfaceW      = (int32_t*)safeRealloc(sw->surfaceW, cap * sizeof(int32_t));
    sw->surfaceH      = (int32_t*)safeRealloc(sw->surfaceH, cap * sizeof(int32_t));
    sw->surfaceLive   = (bool*)safeRealloc(sw->surfaceLive, cap * sizeof(bool));
    for (uint32_t i = sw->surfaceCapacity; i < cap; i++) {
        sw->surfacePixels[i] = NULL;
        sw->surfaceW[i] = 0;
        sw->surfaceH[i] = 0;
        sw->surfaceLive[i] = false;
    }
    sw->surfaceCapacity = cap;
}

static int32_t swCreateSurface(Renderer* renderer, int32_t width, int32_t height) {
    SwRenderer* sw = (SwRenderer*)renderer;
    if (width <= 0 || height <= 0) return -1;
    if (width > SF2000_PANEL_W * 2) width = SF2000_PANEL_W * 2;
    if (height > SF2000_PANEL_H * 2) height = SF2000_PANEL_H * 2;

    for (uint32_t i = 0; i < sw->surfaceCount; i++) {
        if (sw->surfaceLive[i]) continue;
        uint16_t* px = (uint16_t*)safeCalloc((size_t)width * height, sizeof(uint16_t));
        if (px == NULL) return -1;
        sw->surfacePixels[i] = px;
        sw->surfaceW[i] = width;
        sw->surfaceH[i] = height;
        sw->surfaceLive[i] = true;
        return (int32_t)i;
    }
    ensureSurfaceCapacity(sw, sw->surfaceCount + 1);
    uint16_t* px = (uint16_t*)safeCalloc((size_t)width * height, sizeof(uint16_t));
    if (px == NULL) return -1;
    sw->surfacePixels[sw->surfaceCount] = px;
    sw->surfaceW[sw->surfaceCount] = width;
    sw->surfaceH[sw->surfaceCount] = height;
    sw->surfaceLive[sw->surfaceCount] = true;
    return (int32_t)sw->surfaceCount++;
}

static bool swSurfaceExists(Renderer* renderer, int32_t surfaceID) {
    SwRenderer* sw = (SwRenderer*)renderer;
    if (surfaceID < 0 || (uint32_t)surfaceID >= sw->surfaceCount) return false;
    return sw->surfaceLive[surfaceID];
}

static void swSurfaceFree(Renderer* renderer, int32_t surfaceID) {
    SwRenderer* sw = (SwRenderer*)renderer;
    if (surfaceID < 0 || (uint32_t)surfaceID >= sw->surfaceCount) return;
    if (!sw->surfaceLive[surfaceID]) return;
    if (sw->target == sw->surfacePixels[surfaceID]) {
        sw->target = sw->fb;
        sw->targetW = sw->fbW;
        sw->targetH = sw->fbH;
    }
    free(sw->surfacePixels[surfaceID]);
    sw->surfacePixels[surfaceID] = NULL;
    sw->surfaceW[surfaceID] = 0;
    sw->surfaceH[surfaceID] = 0;
    sw->surfaceLive[surfaceID] = false;
}

static void swSurfaceResize(Renderer* renderer, int32_t surfaceID, int32_t width, int32_t height) {
    SwRenderer* sw = (SwRenderer*)renderer;
    if (surfaceID < 0 || (uint32_t)surfaceID >= sw->surfaceCount) return;
    if (!sw->surfaceLive[surfaceID]) return;
    if (width <= 0 || height <= 0) return;
    if (width > SF2000_PANEL_W * 2) width = SF2000_PANEL_W * 2;
    if (height > SF2000_PANEL_H * 2) height = SF2000_PANEL_H * 2;

    uint16_t* px = (uint16_t*)safeCalloc((size_t)width * height, sizeof(uint16_t));
    if (px == NULL) return;
    if (sw->target == sw->surfacePixels[surfaceID]) {
        sw->target = sw->fb;
        sw->targetW = sw->fbW;
        sw->targetH = sw->fbH;
    }
    free(sw->surfacePixels[surfaceID]);
    sw->surfacePixels[surfaceID] = px;
    sw->surfaceW[surfaceID] = width;
    sw->surfaceH[surfaceID] = height;
}

static bool swSetRenderTarget(Renderer* renderer, int32_t surfaceID,
                              MAYBE_UNUSED bool implicitApplicationSurface) {
    SwRenderer* sw = (SwRenderer*)renderer;
    if (surfaceID == APPLICATION_SURFACE_ID || surfaceID == RENDER_TARGET_HOST_FRAMEBUFFER) {
        sw->target = sw->fb;
        sw->targetW = sw->fbW;
        sw->targetH = sw->fbH;
        return true;
    }
    if (surfaceID < 0 || (uint32_t)surfaceID >= sw->surfaceCount) return false;
    if (!sw->surfaceLive[surfaceID]) return false;
    sw->target = sw->surfacePixels[surfaceID];
    sw->targetW = sw->surfaceW[surfaceID];
    sw->targetH = sw->surfaceH[surfaceID];
    return true;
}

static int32_t swEnsureApplicationSurface(Renderer* renderer, MAYBE_UNUSED int32_t width, MAYBE_UNUSED int32_t height) {
    SwRenderer* sw = (SwRenderer*)renderer;
    /* On SF2000, the application surface IS the fixed screen framebuffer (320x240). */
    sw->target = sw->fb;
    sw->targetW = sw->fbW;
    sw->targetH = sw->fbH;
    return APPLICATION_SURFACE_ID;
}

static float swGetSurfaceWidth(Renderer* renderer, int32_t surfaceID) {
    SwRenderer* sw = (SwRenderer*)renderer;
    if (surfaceID == APPLICATION_SURFACE_ID || surfaceID == RENDER_TARGET_HOST_FRAMEBUFFER)
        return (float)sw->fbW;
    if (surfaceID < 0 || (uint32_t)surfaceID >= sw->surfaceCount) return 0.0f;
    if (!sw->surfaceLive[surfaceID]) return 0.0f;
    return (float)sw->surfaceW[surfaceID];
}

static float swGetSurfaceHeight(Renderer* renderer, int32_t surfaceID) {
    SwRenderer* sw = (SwRenderer*)renderer;
    if (surfaceID == APPLICATION_SURFACE_ID || surfaceID == RENDER_TARGET_HOST_FRAMEBUFFER)
        return (float)sw->fbH;
    if (surfaceID < 0 || (uint32_t)surfaceID >= sw->surfaceCount) return 0.0f;
    if (!sw->surfaceLive[surfaceID]) return 0.0f;
    return (float)sw->surfaceH[surfaceID];
}

/* Surface -> screen.  Surfaces are RGB565 with no alpha, so this is opaque-only
 * unless a global alpha is in play. */
static void blitSurface(SwRenderer* sw, const uint16_t* src, int32_t sw_, int32_t sh_,
                        int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH,
                        float x, float y, float xscale, float yscale,
                        uint32_t color, float alpha) {
    if (src == NULL || srcW <= 0 || srcH <= 0) return;
    int32_t dw = (int32_t)lrintf(worldScaleX(sw, (float)srcW * xscale));
    int32_t dh = (int32_t)lrintf(worldScaleY(sw, (float)srcH * yscale));
    if (dw <= 0 || dh <= 0) return;

    int32_t dx = toScreenX(sw, x);
    int32_t dy = toScreenY(sw, y);

    int32_t x0 = dx, y0 = dy, x1 = dx + dw, y1 = dy + dh;
    if (x1 <= 0 || y1 <= 0 || x0 >= sw->targetW || y0 >= sw->targetH) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > sw->targetW) x1 = sw->targetW;
    if (y1 > sw->targetH) y1 = sw->targetH;
    int32_t w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0) return;

    SwTint tint = makeTint(color);
    int32_t gAlpha = (int32_t)(alpha * 255.0f);
    bool blendOn = sw->blendEnable;
    int32_t stepX = (int32_t)(((int64_t)srcW << 16) / dw);
    int32_t stepY = (int32_t)(((int64_t)srcH << 16) / dh);
    int32_t startXFix = (srcX << 16) + (int32_t)(((int64_t)(x0 - dx) * (int64_t)srcW << 16) / dw);
    int32_t startYFix = (srcY << 16) + (int32_t)(((int64_t)(y0 - dy) * (int64_t)srcH << 16) / dh);

    int32_t currYFix = startYFix;
    for (int32_t row = 0; row < h; row++, currYFix += stepY) {
        int32_t syPix = currYFix >> 16;
        if (syPix < 0) syPix = 0;
        if (syPix >= sh_) syPix = sh_ - 1;
        const uint16_t* srow = src + (size_t)syPix * sw_;
        uint16_t* d = sw->target + (size_t)(y0 + row) * sw->targetW + x0;
        int32_t currXFix = startXFix;

        for (int32_t col = 0; col < w; col++, currXFix += stepX) {
            int32_t sxi = currXFix >> 16;
            if (sxi < 0) sxi = 0;
            if (sxi >= sw_) sxi = sw_ - 1;
            uint16_t c = srow[sxi];
            if (!tint.isWhite) {
                /* Surfaces carry no alpha, so tinting is a straight channel scale. */
                int32_t r = (int32_t)((c >> 11) & 0x1F) * tint.r8 >> 8;
                int32_t g = (int32_t)((c >> 5) & 0x3F) * tint.g8 >> 8;
                int32_t b = (int32_t)(c & 0x1F) * tint.b8 >> 8;
                c = (uint16_t)((r << 11) | (g << 5) | b);
            }
            d[col] = (!blendOn || gAlpha >= 255) ? c : blendRgb565(c, d[col], gAlpha);
        }
    }
}

static void swDrawSurface(Renderer* renderer, int32_t surfaceID,
                          int32_t srcLeft, int32_t srcTop, int32_t srcWidth, int32_t srcHeight,
                          float x, float y, float xscale, float yscale, MAYBE_UNUSED float angleDeg,
                          uint32_t color, float alpha) {
    SwRenderer* sw = (SwRenderer*)renderer;
    if (surfaceID == APPLICATION_SURFACE_ID || surfaceID == RENDER_TARGET_HOST_FRAMEBUFFER) {
        blitSurface(sw, sw->fb, sw->fbW, sw->fbH, srcLeft, srcTop, srcWidth, srcHeight,
                    x, y, xscale, yscale, color, alpha);
        return;
    }
    if (surfaceID < 0 || (uint32_t)surfaceID >= sw->surfaceCount) return;
    if (!sw->surfaceLive[surfaceID]) return;
    blitSurface(sw, sw->surfacePixels[surfaceID], sw->surfaceW[surfaceID], sw->surfaceH[surfaceID],
                srcLeft, srcTop, srcWidth, srcHeight, x, y, xscale, yscale, color, alpha);
}

static void swDrawSurfaceColor(Renderer* renderer, int32_t surfaceID,
                               int32_t srcLeft, int32_t srcTop, int32_t srcWidth, int32_t srcHeight,
                               float x, float y, float xscale, float yscale,
                               MAYBE_UNUSED float angleDeg,
                               uint32_t c1, MAYBE_UNUSED uint32_t c2,
                               MAYBE_UNUSED uint32_t c3, MAYBE_UNUSED uint32_t c4, float alpha) {
    swDrawSurface(renderer, surfaceID, srcLeft, srcTop, srcWidth, srcHeight,
                  x, y, xscale, yscale, 0.0f, c1, alpha);
}

static void swDrawSurfaceTiled(Renderer* renderer, int32_t surfaceID,
                               float x, float y, float xscale, float yscale,
                               float roomW, float roomH, uint32_t color, float alpha) {
    SwRenderer* sw = (SwRenderer*)renderer;
    int32_t sw_ = 0, sh_ = 0;
    const uint16_t* src = NULL;
    if (surfaceID == APPLICATION_SURFACE_ID || surfaceID == RENDER_TARGET_HOST_FRAMEBUFFER) {
        src = sw->fb; sw_ = sw->fbW; sh_ = sw->fbH;
    } else if (surfaceID >= 0 && (uint32_t)surfaceID < sw->surfaceCount && sw->surfaceLive[surfaceID]) {
        src = sw->surfacePixels[surfaceID];
        sw_ = sw->surfaceW[surfaceID];
        sh_ = sw->surfaceH[surfaceID];
    }
    if (src == NULL || sw_ <= 0 || sh_ <= 0) return;

    float tw = (float)sw_ * xscale, th = (float)sh_ * yscale;
    if (tw <= 0.0f || th <= 0.0f) return;
    int32_t countX = (int32_t)(roomW / tw) + 1;
    int32_t countY = (int32_t)(roomH / th) + 1;
    if (countX < 1) countX = 1;
    if (countY < 1) countY = 1;
    if (countX > 256) countX = 256;
    if (countY > 256) countY = 256;
    for (int32_t iy = 0; iy < countY; iy++)
        for (int32_t ix = 0; ix < countX; ix++)
            swDrawSurface(renderer, surfaceID, 0, 0, sw_, sh_,
                          x + (float)ix * tw, y + (float)iy * th,
                          xscale, yscale, 0.0f, color, alpha);
}

static void swSurfaceCopy(Renderer* renderer, int32_t destSurfaceID, int32_t destX, int32_t destY,
                          int32_t srcSurfaceID, int32_t srcX, int32_t srcY,
                          int32_t srcW, int32_t srcH, bool part) {
    SwRenderer* sw = (SwRenderer*)renderer;
    if (destSurfaceID == APPLICATION_SURFACE_ID || destSurfaceID == RENDER_TARGET_HOST_FRAMEBUFFER) {
        /* Copying the framebuffer into itself is not a use case Undertale has. */
        (void)part;
        return;
    }
    if (destSurfaceID < 0 || (uint32_t)destSurfaceID >= sw->surfaceCount) return;
    if (!sw->surfaceLive[destSurfaceID]) return;

    uint16_t* dst = sw->surfacePixels[destSurfaceID];
    int32_t dw = sw->surfaceW[destSurfaceID], dh = sw->surfaceH[destSurfaceID];
    const uint16_t* src;
    int32_t sw_ = 0, sh_ = 0;
    if (srcSurfaceID == APPLICATION_SURFACE_ID || srcSurfaceID == RENDER_TARGET_HOST_FRAMEBUFFER) {
        src = sw->fb; sw_ = sw->fbW; sh_ = sw->fbH;
    } else if (srcSurfaceID >= 0 && (uint32_t)srcSurfaceID < sw->surfaceCount &&
               sw->surfaceLive[srcSurfaceID]) {
        src = sw->surfacePixels[srcSurfaceID];
        sw_ = sw->surfaceW[srcSurfaceID];
        sh_ = sw->surfaceH[srcSurfaceID];
    } else {
        return;
    }

    for (int32_t row = 0; row < srcH; row++) {
        int32_t sy = srcY + row, dy = destY + row;
        if (sy < 0 || sy >= sh_) continue;
        if (dy < 0 || dy >= dh) continue;
        for (int32_t col = 0; col < srcW; col++) {
            int32_t sx = srcX + col, dx = destX + col;
            if (sx < 0 || sx >= sw_) continue;
            if (dx < 0 || dx >= dw) continue;
            dst[(size_t)dy * dw + dx] = src[(size_t)sy * sw_ + sx];
        }
    }
}

static bool swSurfaceGetPixels(Renderer* renderer, int32_t surfaceID, uint8_t* outRGBA) {
    SwRenderer* sw = (SwRenderer*)renderer;
    if (outRGBA == NULL) return false;
    uint16_t* src = NULL;
    int32_t w = 0, h = 0;
    if (surfaceID == APPLICATION_SURFACE_ID || surfaceID == RENDER_TARGET_HOST_FRAMEBUFFER) {
        src = sw->fb; w = sw->fbW; h = sw->fbH;
    } else if (surfaceID >= 0 && (uint32_t)surfaceID < sw->surfaceCount && sw->surfaceLive[surfaceID]) {
        src = sw->surfacePixels[surfaceID];
        w = sw->surfaceW[surfaceID];
        h = sw->surfaceH[surfaceID];
    }
    if (src == NULL) return false;

    for (int32_t row = 0; row < h; row++) {
        for (int32_t col = 0; col < w; col++) {
            uint16_t c = src[(size_t)row * w + col];
            uint8_t* p = outRGBA + ((size_t)row * w + col) * 4;
            p[0] = (uint8_t)(((c >> 11) & 0x1F) << 3);
            p[1] = (uint8_t)(((c >> 5) & 0x3F) << 2);
            p[2] = (uint8_t)((c & 0x1F) << 3);
            p[3] = 255;
        }
    }
    return true;
}

static void swSurfaceUploadPixels(Renderer* renderer, int32_t surfaceID,
                                  int32_t w, int32_t h, const uint8_t* rgba) {
    SwRenderer* sw = (SwRenderer*)renderer;
    if (rgba == NULL || surfaceID < 0 || (uint32_t)surfaceID >= sw->surfaceCount) return;
    if (!sw->surfaceLive[surfaceID]) return;
    uint16_t* dst = sw->surfacePixels[surfaceID];
    int32_t dw = sw->surfaceW[surfaceID], dh = sw->surfaceH[surfaceID];
    if (w > dw) w = dw;
    if (h > dh) h = dh;
    for (int32_t row = 0; row < h; row++) {
        for (int32_t col = 0; col < w; col++) {
            const uint8_t* p = rgba + ((size_t)row * w + col) * 4;
            dst[(size_t)row * dw + col] =
                (uint16_t)((((p[0] >> 3) & 0x1F) << 11) | (((p[1] >> 2) & 0x3F) << 5) | ((p[2] >> 3) & 0x1F));
        }
    }
}

static int32_t swCreateSpriteFromSurface(Renderer* renderer, int32_t surfaceID,
                                         MAYBE_UNUSED int32_t x, MAYBE_UNUSED int32_t y,
                                         MAYBE_UNUSED int32_t w, MAYBE_UNUSED int32_t h,
                                         MAYBE_UNUSED bool removeback, MAYBE_UNUSED bool smooth,
                                         MAYBE_UNUSED int32_t xorig, MAYBE_UNUSED int32_t yorig) {
    /* Upload-back into a Sprite would need a writable texture page; Undertale uses
     * this only for sprite_get_* on application_surface, which reads go through
     * draw_surface instead.  Returning -1 matches noop_renderer.c's behaviour. */
    (void)surfaceID;
    return -1;
}

static void swDeleteSprite(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t spriteIndex) {}

/* ------------------------------------------------------------ frame / view -- */

static void swInit(Renderer* renderer, DataWin* dataWin) {
    SwRenderer* sw = (SwRenderer*)renderer;
    renderer->dataWin = dataWin;
    Matrix4f world;
    Matrix4f_identity(&world);
    renderer->gmlMatrices[MATRIX_WORLD] = world;

    if (!g_lutBuilt) { buildColorLut(); g_lutBuilt = true; }

#if SW_ASSET_DIR_ENV
    {
        const char* env = getenv("SW_ASSET_DIR");
        if (env != NULL && env[0] != '\0') SwRenderer_setAssetDir(env);
    }
#endif

    if (!sw->atlasOpen) {
        if (SwAtlas_open(&sw->atlas, g_assetDir)) {
            sw->atlas.cacheBudget = 12u * 1024u * 1024u;   /* 12 MB of the 58 MiB heap */
            sw->atlasOpen = true;
            logInfo("SW renderer: atlas opened from %s (%u blocks)\n",
                    g_assetDir, sw->atlas.count);
        } else {
            logWarn("SW renderer: no ATLAS.BIN in %s, sprites will not draw\n", g_assetDir);
        }
    }
    ensureFramebuffer(sw, sw->fbW, sw->fbH);
}

static void swDestroy(Renderer* renderer) {
    SwRenderer* sw = (SwRenderer*)renderer;
    if (sw->atlasOpen) { SwAtlas_close(&sw->atlas); sw->atlasOpen = false; }
    for (uint32_t i = 0; i < sw->surfaceCount; i++) free(sw->surfacePixels[i]);
    free(sw->surfacePixels);
    free(sw->surfaceW);
    free(sw->surfaceH);
    free(sw->surfaceLive);
    free(sw->fb);
    free(sw);
    if (g_sw == sw) g_sw = NULL;
}

static void swBeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH,
                         MAYBE_UNUSED int32_t windowW, MAYBE_UNUSED int32_t windowH) {
    SwRenderer* sw = (SwRenderer*)renderer;
    sw->gameW = gameW > 0 ? gameW : 320;
    sw->gameH = gameH > 0 ? gameH : 240;
    /* Undertale draws its own background every frame, but a stale frame shows through
     * on fades, so clear.  320x240x2 = 150 kB, a fraction of a millisecond. */
    if (sw->fb != NULL && sw->fbW > 0 && sw->fbH > 0) {
        size_t n = (size_t)sw->fbW * sw->fbH;
        for (size_t i = 0; i < n; i++) sw->fb[i] = 0;
    }
    sw->target = sw->fb;
    sw->targetW = sw->fbW;
    sw->targetH = sw->fbH;
    sw->inGui = false;
}

static void swEndFrameInit(Renderer* renderer) { (void)renderer; }
static void swEndFrameEnd(Renderer* renderer) { (void)renderer; }
static void swFlush(MAYBE_UNUSED Renderer* renderer) {}

static void swBeginView(Renderer* renderer, int32_t viewX, int32_t viewY,
                        int32_t viewW, int32_t viewH,
                        int32_t portX, int32_t portY, int32_t portW, int32_t portH,
                        MAYBE_UNUSED float viewAngle) {
    SwRenderer* sw = (SwRenderer*)renderer;
    float screenScaleX = (sw->gameW > 0) ? (float)sw->targetW / (float)sw->gameW : 1.0f;
    float screenScaleY = (sw->gameH > 0) ? (float)sw->targetH / (float)sw->gameH : 1.0f;

    sw->viewX = viewX;
    sw->viewY = viewY;
    sw->portX = (int32_t)lrintf((float)portX * screenScaleX);
    sw->portY = (int32_t)lrintf((float)portY * screenScaleY);

    if (viewW > 0 && viewH > 0 && portW > 0 && portH > 0) {
        sw->scaleX = ((float)portW / (float)viewW) * screenScaleX;
        sw->scaleY = ((float)portH / (float)viewH) * screenScaleY;
    } else {
        sw->scaleX = screenScaleX;
        sw->scaleY = screenScaleY;
    }
    sw->inGui = false;
}

static void swEndView(MAYBE_UNUSED Renderer* renderer) {}

static void swApplyProjection(Renderer* renderer, const Matrix4f* viewMatrix,
                              const Matrix4f* projectionMatrix) {
    /* Undertale is 2D and Runner only supplies near-identity matrices here; the
     * view/port state set by beginView is what actually drives the blit. */
    (void)projectionMatrix;
    if (viewMatrix != NULL) renderer->previousViewMatrix = *viewMatrix;
}

static void swBeginGUI(Renderer* renderer, int32_t guiW, int32_t guiH,
                       int32_t portX, int32_t portY, MAYBE_UNUSED int32_t portW, MAYBE_UNUSED int32_t portH,
                       MAYBE_UNUSED int32_t targetSurfaceId) {
    SwRenderer* sw = (SwRenderer*)renderer;
    float screenScaleX = (sw->gameW > 0) ? (float)sw->targetW / (float)sw->gameW : 1.0f;
    float screenScaleY = (sw->gameH > 0) ? (float)sw->targetH / (float)sw->gameH : 1.0f;

    sw->inGui = true;
    sw->viewX = 0;
    sw->viewY = 0;
    sw->portX = (int32_t)lrintf((float)portX * screenScaleX);
    sw->portY = (int32_t)lrintf((float)portY * screenScaleY);
    if (guiW > 0 && guiH > 0) {
        sw->scaleX = (float)sw->targetW / (float)guiW;
        sw->scaleY = (float)sw->targetH / (float)guiH;
    } else {
        sw->scaleX = screenScaleX;
        sw->scaleY = screenScaleY;
    }
}

static void swSetGuiProjection(Renderer* renderer, int32_t guiW, int32_t guiH,
                               MAYBE_UNUSED int32_t portW, MAYBE_UNUSED int32_t portH,
                               MAYBE_UNUSED bool toUserSurface) {
    SwRenderer* sw = (SwRenderer*)renderer;
    if (guiW > 0 && guiH > 0) {
        sw->scaleX = (float)sw->targetW / (float)guiW;
        sw->scaleY = (float)sw->targetH / (float)guiH;
    }
}

static void swEndGUI(Renderer* renderer) { ((SwRenderer*)renderer)->inGui = false; }

static void swClearScreen(Renderer* renderer, uint32_t color, float alpha) {
    SwRenderer* sw = (SwRenderer*)renderer;
    fillRect(sw, 0, 0, sw->targetW, sw->targetH, color, alpha);
}

void SWRenderer_clearFrameBuffer(Renderer* renderer, uint32_t color) {
    swClearScreen(renderer, color, 1.0f);
}

static void swDrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2,
                            uint32_t color, float alpha, bool outline) {
    SwRenderer* sw = (SwRenderer*)renderer;
    int32_t sx1 = toScreenX(sw, x1), sy1 = toScreenY(sw, y1);
    int32_t sx2 = toScreenX(sw, x2), sy2 = toScreenY(sw, y2);
    if (outline) strokeRect(sw, sx1, sy1, sx2, sy2, color, alpha);
    else fillRect(sw, sx1, sy1, sx2 - sx1, sy2 - sy1, color, alpha);
}

static void swDrawRectangleColor(Renderer* renderer, float x1, float y1, float x2, float y2,
                                 MAYBE_UNUSED uint32_t c1, MAYBE_UNUSED uint32_t c2,
                                 MAYBE_UNUSED uint32_t c3, MAYBE_UNUSED uint32_t c4,
                                 float alpha, bool outline) {
    swDrawRectangle(renderer, x1, y1, x2, y2, renderer->drawColor, alpha, outline);
}

static void swDrawLine(Renderer* renderer, float x1, float y1, float x2, float y2,
                       float width, uint32_t color, float alpha) {
    SwRenderer* sw = (SwRenderer*)renderer;
    drawLineRaw(sw, toScreenX(sw, x1), toScreenY(sw, y1),
                toScreenX(sw, x2), toScreenY(sw, y2),
                (int32_t)lrintf(width * fmaxf(sw->scaleX, sw->scaleY)), color, alpha);
}

static void swDrawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2,
                            float width, MAYBE_UNUSED uint32_t c1, MAYBE_UNUSED uint32_t c2, float alpha) {
    swDrawLine(renderer, x1, y1, x2, y2, width, renderer->drawColor, alpha);
}

static void swDrawTriangle(Renderer* renderer, float x1, float y1, float x2, float y2,
                           float x3, float y3, MAYBE_UNUSED uint32_t color1,
                           MAYBE_UNUSED uint32_t color2, MAYBE_UNUSED uint32_t color3,
                           float alpha, bool outline) {
    SwRenderer* sw = (SwRenderer*)renderer;
    uint32_t color = renderer->drawColor;
    int32_t a1x = toScreenX(sw, x1), a1y = toScreenY(sw, y1);
    int32_t a2x = toScreenX(sw, x2), a2y = toScreenY(sw, y2);
    int32_t a3x = toScreenX(sw, x3), a3y = toScreenY(sw, y3);

    if (outline) {
        drawLineRaw(sw, a1x, a1y, a2x, a2y, 1, color, alpha);
        drawLineRaw(sw, a2x, a2y, a3x, a3y, 1, color, alpha);
        drawLineRaw(sw, a3x, a3y, a1x, a1y, 1, color, alpha);
    } else {
        fillTriangle(sw, (float)a1x, (float)a1y, (float)a2x, (float)a2y,
                     (float)a3x, (float)a3y, color, alpha);
    }
}

/* ---------------------------------------------------------- GPU state shims -- */

static BlendFactors swGpuGetBlendFactors(Renderer* renderer) { return ((SwRenderer*)renderer)->blendFactors; }
static int32_t swGpuGetBlendMode(Renderer* renderer) { return ((SwRenderer*)renderer)->blendMode; }

static void swGpuSetBlendMode(Renderer* renderer, int32_t mode) {
    SwRenderer* sw = (SwRenderer*)renderer;
    sw->blendMode = mode;
    /* bm_complex is a render-target feature this backend does not have; every other
     * mode reduces to src-alpha blending on a panel with no framebuffer alpha. */
    sw->blendEnable = (mode != bm_complex);
}

static void swGpuSetBlendModeExt(Renderer* renderer, int32_t sfactor, int32_t dfactor,
                                 int32_t sfactor_alpha, int32_t dfactor_alpha) {
    SwRenderer* sw = (SwRenderer*)renderer;
    sw->blendFactors.src = sfactor;
    sw->blendFactors.dst = dfactor;
    sw->blendFactors.srcAlpha = sfactor_alpha;
    sw->blendFactors.dstAlpha = dfactor_alpha;
}

static void swGpuSetBlendEnable(Renderer* renderer, bool enable) { ((SwRenderer*)renderer)->blendEnable = enable; }
static bool  swGpuGetBlendEnable(Renderer* renderer) { return ((SwRenderer*)renderer)->blendEnable; }
static void  swGpuSetAlphaTestEnable(Renderer* renderer, bool enable) { ((SwRenderer*)renderer)->alphaTestEnable = enable; }
static bool  swGpuGetAlphaTestEnable(Renderer* renderer) { return ((SwRenderer*)renderer)->alphaTestEnable; }
static void  swGpuSetAlphaTestRef(Renderer* renderer, uint8_t ref) { ((SwRenderer*)renderer)->alphaTestRef = ref; }

static void swGpuSetColorWriteEnable(Renderer* renderer, bool r, bool g, bool b, bool a) {
    SwRenderer* sw = (SwRenderer*)renderer;
    sw->colorWriteR = r; sw->colorWriteG = g; sw->colorWriteB = b; sw->colorWriteA = a;
}

static void swGpuGetColorWriteEnable(Renderer* renderer, bool* r, bool* g, bool* b, bool* a) {
    SwRenderer* sw = (SwRenderer*)renderer;
    if (r) *r = sw->colorWriteR;
    if (g) *g = sw->colorWriteG;
    if (b) *b = sw->colorWriteB;
    if (a) *a = sw->colorWriteA;
}

static void swGpuSetTexFilter(Renderer* renderer, bool enable) {
    renderer->texFilter = enable;
    ((SwRenderer*)renderer)->bilinear = enable;
}

static void swGpuSetFog(Renderer* renderer, bool enable, uint32_t color) {
    SwRenderer* sw = (SwRenderer*)renderer;
    sw->fogEnable = enable;
    sw->fogColor = color;
}

/* Shaders and textures: reported as unsupported/zero rather than faked.  Undertale
 * 1.08 never calls shader_* or surface_get_texture; it does use gpu_set_blendmode and
 * draw_surface, both of which are real above. */
static void   swGpuSetShader(Renderer* renderer, int32_t idx) { renderer->currentShader = idx; }
static void   swGpuResetShader(Renderer* renderer) { renderer->currentShader = -1; }
static int32_t swShaderGetUniform(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED int32_t s, MAYBE_UNUSED char* u) { return -1; }
static int32_t swShaderGetSamplerIndex(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED int32_t s, MAYBE_UNUSED char* u) { return -1; }
static void   swShaderSetUniformF(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED int32_t h, MAYBE_UNUSED int32_t c,
                                  MAYBE_UNUSED float a, MAYBE_UNUSED float b,
                                  MAYBE_UNUSED float c2, MAYBE_UNUSED float d) {}
static void   swShaderSetUniformFArray(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED int32_t h,
                                       MAYBE_UNUSED float* v, MAYBE_UNUSED uint32_t c) {}
static void   swShaderSetUniformI(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED int32_t h, MAYBE_UNUSED int32_t c,
                                  MAYBE_UNUSED int32_t a, MAYBE_UNUSED int32_t b,
                                  MAYBE_UNUSED int32_t c2, MAYBE_UNUSED int32_t d) {}
static uint32_t swSpriteGetTexture(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED int32_t t) { return 0; }
static uint32_t swSurfaceGetTexture(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED int32_t s) { return 0; }
static float   swTextureGetTexelWidth(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED uint32_t t) { return 1.0f; }
static float   swTextureGetTexelHeight(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED uint32_t t) { return 1.0f; }
static bool    swTextureGetUVs(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED uint32_t t, MAYBE_UNUSED float* uv) { return false; }
static void    swTextureSetStage(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED int32_t s, MAYBE_UNUSED uint32_t t) {}
static bool    swShaderIsCompiled(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED int32_t s) { return false; }
static bool    swShadersSupported(void) { return false; }

static void swSetMatrix(Renderer* renderer, int32_t matrixType, Matrix4f matrix) {
    if (matrixType >= 0 && matrixType < MATRICES_MAX) renderer->gmlMatrices[matrixType] = matrix;
}

/* Primitives: Undertale 1.08 does not build vertex buffers, so these are inert. */
static void swPrimitiveBegin(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED int32_t t) {}
static void swPrimitiveBeginTexture(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED int32_t t, MAYBE_UNUSED int32_t tex) {}
static void swPrimitiveEnd(MAYBE_UNUSED Renderer* r) {}
static void swDrawVertex(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED float x, MAYBE_UNUSED float y,
                         MAYBE_UNUSED float z, MAYBE_UNUSED uint32_t c, MAYBE_UNUSED float a,
                         MAYBE_UNUSED float u, MAYBE_UNUSED float v) {}
static void swDrawVertexBuffer(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED VertexBuffer* b,
                               MAYBE_UNUSED int32_t p, MAYBE_UNUSED int32_t t,
                               MAYBE_UNUSED int32_t off, MAYBE_UNUSED int32_t cnt) {}
static void swDrawTextUI(Renderer* renderer, const char* text, float x, float y,
                         float xscale, float yscale, float angleDeg,
                         int32_t c1, int32_t c2, int32_t c3, int32_t c4,
                         float alpha, float lineSeparation) {
    swDrawTextColor(renderer, text, x, y, xscale, yscale, angleDeg, c1, c2, c3, c4, alpha, lineSeparation);
}
static void swDrawTiledPart(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED int32_t t, MAYBE_UNUSED int32_t sx,
                            MAYBE_UNUSED int32_t sy, MAYBE_UNUSED int32_t sw_, MAYBE_UNUSED int32_t sh_,
                            MAYBE_UNUSED float dx, MAYBE_UNUSED float dy, MAYBE_UNUSED float dw,
                            MAYBE_UNUSED float dh, MAYBE_UNUSED uint32_t c, MAYBE_UNUSED float a) {}
static void swDrawTile(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED RoomTile* t,
                       MAYBE_UNUSED float ox, MAYBE_UNUSED float oy) {}
/* surface_set_pixels is optional in the vtable and the renderer declares it as
 * returning bool; keep the signature exact so the assignment below stays clean. */
static bool swSurfaceSetPixels(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED int32_t s,
                               MAYBE_UNUSED const uint8_t* rgba) { return false; }

/* ------------------------------------------------------------------ vtable -- */

static RendererVtable swVtable;

Renderer* SWRenderer_create(void) {
    SwRenderer* sw = (SwRenderer*)safeCalloc(1, sizeof(SwRenderer));

    sw->base.vtable = &swVtable;
    sw->base.drawColor = 0xFFFFFF;
    sw->base.drawAlpha = 1.0f;
    sw->base.drawFont = -1;
    sw->base.drawHalign = 0;
    sw->base.drawValign = 0;
    sw->base.circlePrecision = 24;
    sw->base.currentShader = -1;
    Matrix4f_identity(&sw->base.gmlMatrices[MATRIX_WORLD]);

    sw->fbW = SF2000_PANEL_W;
    sw->fbH = SF2000_PANEL_H;
    sw->gameW = 640;
    sw->gameH = 480;
    sw->scaleX = 1.0f;
    sw->scaleY = 1.0f;
    sw->blendEnable = true;
    sw->blendMode = bm_normal;
    sw->blendFactors.src = bm_src_alpha;
    sw->blendFactors.dst = bm_inv_src_alpha;
    sw->blendFactors.srcAlpha = bm_src_alpha;
    sw->blendFactors.dstAlpha = bm_inv_src_alpha;
    sw->alphaTestEnable = false;
    sw->alphaTestRef = 0;
    sw->colorWriteR = sw->colorWriteG = sw->colorWriteB = sw->colorWriteA = true;

    if (!g_lutBuilt) { buildColorLut(); g_lutBuilt = true; }
    ensureFramebuffer(sw, sw->fbW, sw->fbH);

    g_sw = sw;
    if (s_has_pending_cfg) {
        SwRenderer_setConfig(&s_pending_cfg);
    }

    swVtable.init = swInit;
    swVtable.destroy = swDestroy;
    swVtable.beginFrame = swBeginFrame;
    swVtable.endFrameInit = swEndFrameInit;
    swVtable.endFrameEnd = swEndFrameEnd;
    swVtable.beginView = swBeginView;
    swVtable.endView = swEndView;
    swVtable.applyProjection = swApplyProjection;
    swVtable.beginGUI = swBeginGUI;
    swVtable.setGuiProjection = swSetGuiProjection;
    swVtable.endGUI = swEndGUI;
    swVtable.drawSprite = swDrawSprite;
    swVtable.drawSpritePart = swDrawSpritePart;
    swVtable.drawSpritePartColor = swDrawSpritePartColor;
    swVtable.drawSpritePos = swDrawSpritePos;
    swVtable.drawRectangle = swDrawRectangle;
    swVtable.drawRectangleColor = swDrawRectangleColor;
    swVtable.drawLine = swDrawLine;
    swVtable.drawLineColor = swDrawLineColor;
    swVtable.drawTriangle = swDrawTriangle;
    swVtable.drawText = swDrawText;
    swVtable.drawTextColor = swDrawTextColor;
    swVtable.drawTextUI = swDrawTextUI;
    swVtable.flush = swFlush;
    swVtable.clearScreen = swClearScreen;
    swVtable.createSpriteFromSurface = swCreateSpriteFromSurface;
    swVtable.deleteSprite = swDeleteSprite;
    swVtable.gpuGetBlendFactors = swGpuGetBlendFactors;
    swVtable.gpuGetBlendMode = swGpuGetBlendMode;
    swVtable.gpuSetBlendMode = swGpuSetBlendMode;
    swVtable.gpuSetBlendModeExt = swGpuSetBlendModeExt;
    swVtable.gpuSetBlendEnable = swGpuSetBlendEnable;
    swVtable.gpuSetAlphaTestEnable = swGpuSetAlphaTestEnable;
    swVtable.gpuGetAlphaTestEnable = swGpuGetAlphaTestEnable;
    swVtable.gpuSetAlphaTestRef = swGpuSetAlphaTestRef;
    swVtable.gpuSetColorWriteEnable = swGpuSetColorWriteEnable;
    swVtable.gpuGetColorWriteEnable = swGpuGetColorWriteEnable;
    swVtable.gpuGetBlendEnable = swGpuGetBlendEnable;
    swVtable.gpuSetTexFilter = swGpuSetTexFilter;
    swVtable.gpuSetFog = swGpuSetFog;
    swVtable.drawTile = nullptr;        /* nullptr lets generic Renderer_drawTile resolve TPAG and call drawSpritePart */
    swVtable.drawSpriteTiled = swDrawSpriteTiled;
    swVtable.drawTiledPart = nullptr;   /* nullptr lets generic nine-slice fallback call drawSpritePart */
    swVtable.createSurface = swCreateSurface;
    swVtable.surfaceExists = swSurfaceExists;
    swVtable.setRenderTarget = swSetRenderTarget;
    swVtable.ensureApplicationSurface = swEnsureApplicationSurface;
    swVtable.getSurfaceWidth = swGetSurfaceWidth;
    swVtable.getSurfaceHeight = swGetSurfaceHeight;
    swVtable.drawSurface = swDrawSurface;
    swVtable.drawSurfaceColor = swDrawSurfaceColor;
    swVtable.drawSurfaceTiled = swDrawSurfaceTiled;
    swVtable.surfaceResize = swSurfaceResize;
    swVtable.surfaceFree = swSurfaceFree;
    swVtable.surfaceCopy = swSurfaceCopy;
    swVtable.surfaceGetPixels = swSurfaceGetPixels;
    swVtable.surfaceSetPixels = swSurfaceSetPixels;
    swVtable.surfaceUploadPixels = swSurfaceUploadPixels;
    swVtable.primitiveBegin = swPrimitiveBegin;
    swVtable.primitiveBeginTexture = swPrimitiveBeginTexture;
    swVtable.primitiveEnd = swPrimitiveEnd;
    swVtable.drawVertex = swDrawVertex;
    swVtable.drawVertexBuffer = swDrawVertexBuffer;
    swVtable.gpuSetShader = swGpuSetShader;
    swVtable.gpuResetShader = swGpuResetShader;
    swVtable.shaderGetUniform = swShaderGetUniform;
    swVtable.shaderGetSamplerIndex = swShaderGetSamplerIndex;
    swVtable.shaderSetUniformF = swShaderSetUniformF;
    swVtable.shaderSetUniformFArray = swShaderSetUniformFArray;
    swVtable.shaderSetUniformI = swShaderSetUniformI;
    swVtable.spriteGetTexture = swSpriteGetTexture;
    swVtable.surfaceGetTexture = swSurfaceGetTexture;
    swVtable.textureGetTexelWidth = swTextureGetTexelWidth;
    swVtable.textureGetTexelHeight = swTextureGetTexelHeight;
    swVtable.textureGetUVs = swTextureGetUVs;
    swVtable.textureSetStage = swTextureSetStage;
    swVtable.shaderIsCompiled = swShaderIsCompiled;
    swVtable.shadersSupported = swShadersSupported;
    swVtable.setMatrix = swSetMatrix;

    return (Renderer*)sw;
}