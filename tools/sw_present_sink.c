/*
 * sw_present_sink.c - host-only present sink for BACKEND=sw builds.
 *
 * The software renderer ends a frame by calling sw_renderer_present_sink() (see the
 * !PLATFORM_SF2000 branch of SwRenderer_present).  On the real console that symbol is
 * sf2000_present_frame(); on a desktop it is this file, which writes the RGB565
 * framebuffer to a PNG so it can be diffed against the GL reference frames in
 * docs/golden/.
 *
 * Not part of the SF2000 build: the core Makefile only globs src/sw_renderer/*.c.
 */

/* stb_image_write is already compiled into the binary (src/cli/stb_impl.c defines
 * STB_IMAGE_WRITE_IMPLEMENTATION), so only the declaration is needed here. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int stbi_write_png(char const* filename, int w, int h, int comp,
                          const void* data, int stride_bytes);

static int   g_shotIndex = 0;
static int   g_maxShots  = 0;
static char  g_prefix[512] = "sw";
static unsigned g_pitch = 320;

/* Controlled by env vars so loop.c needs no changes:
 *   SW_SHOT_PREFIX  output filename prefix (default "sw")
 *   SW_SHOT_MAX     stop capturing after N frames (default 0 = unlimited)
 *   SW_SHOT_FIRST   first frame index to capture (default 0)
 */
__attribute__((constructor)) static void sw_sink_init(void) {
    const char* p = getenv("SW_SHOT_PREFIX");
    if (p && p[0]) snprintf(g_prefix, sizeof(g_prefix), "%s", p);
    const char* m = getenv("SW_SHOT_MAX");
    if (m && m[0]) g_maxShots = atoi(m);
}

void sw_sink_set_pitch(unsigned pitch) { g_pitch = pitch; }

void sw_renderer_present_sink(const void* pixels, unsigned w, unsigned h, unsigned pitch) {
    if (pixels == NULL || w == 0 || h == 0) return;
    if (g_maxShots > 0 && g_shotIndex >= g_maxShots) return;
    g_pitch = pitch ? pitch : w;

    /* RGB565 -> RGB8.  Replicate the high bits so full-scale white stays white;
     * stb_image_write wants 3 bytes per pixel. */
    const uint16_t* src = (const uint16_t*)pixels;
    unsigned char* rgb = (unsigned char*)malloc((size_t)w * h * 3u);
    if (rgb == NULL) return;

    for (unsigned y = 0; y < h; y++) {
        const uint16_t* row = src + (size_t)y * g_pitch;
        unsigned char* d = rgb + (size_t)y * w * 3u;
        for (unsigned x = 0; x < w; x++) {
            uint16_t c = row[x];
            uint8_t r5 = (uint8_t)((c >> 11) & 0x1F);
            uint8_t g6 = (uint8_t)((c >> 5) & 0x3F);
            uint8_t b5 = (uint8_t)(c & 0x1F);
            d[x * 3 + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));
            d[x * 3 + 1] = (uint8_t)((g6 << 2) | (g6 >> 4));
            d[x * 3 + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
        }
    }

    char path[600];
    snprintf(path, sizeof(path), "%s_%04d.png", g_prefix, g_shotIndex);
    stbi_write_png(path, (int)w, (int)h, 3, rgb, (int)(w * 3u));
    free(rgb);
    g_shotIndex++;
}