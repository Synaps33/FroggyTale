/*
 * sw_renderer.h - the SOFTWARE GraphicsAPI backend.
 *
 * The SF2000/GB300 have no GPU at all (confirmed in mcpe64-gb300/README.md: "3D is
 * rendered by a software rasterizer: this hardware has no GPU"), so this implements
 * Butterscotch's RendererVtable directly on a CPU blitter rather than wrapping GL.
 *
 * Upstream already has the `SOFTWARE` slot reserved in enum GraphicsAPI and the
 * #ifdef ENABLE_SW_RENDERER hooks in loop.c / cli/args.c, but no implementation ever
 * landed: there is no sw_renderer.c in the tree.  This fills that slot, which is why
 * loop.c needs no changes at all.
 *
 * Output is 16-bit RGB565, the panel's native format.  The frontend's
 * retro_video_refresh_cb passes a buffer straight to the OSD blit unless the core asks
 * for XRGB8888, so we deliberately do not ask - see SET_PIXEL_FORMAT in
 * sf2000_multicore/src/libretro_frontend/core_api.c.
 */

#ifndef _BS_SW_RENDERER_H_
#define _BS_SW_RENDERER_H_

#include <stdbool.h>
#include <stdint.h>

#include "renderer.h"

/* Called from src/loop.c via the existing ENABLE_SW_RENDERER hooks. */
Renderer* SWRenderer_create(void);
void      SWRenderer_clearFrameBuffer(Renderer* renderer, uint32_t color);

/* ------------------------------------------------------------------ config -- */
/* Retro_load_game() learns the ROM directory from info->path; the atlas has to live
 * next to it, and SWRenderer_create() runs later inside loop(), so stash it here. */
void      SwRenderer_setAssetDir(const char* dir);

typedef struct {
    int32_t width;          /* render target size, <= 320x240                */
    int32_t height;
    int32_t cacheBytes;     /* atlas block cache budget                       */
    bool    bilinear;       /* filter when upscaling a block                  */
    bool    debugOverlay;
} SwRendererConfig;

void SwRenderer_setConfig(const SwRendererConfig* cfg);

/* Called by platformSwapBuffers() once per frame, from loop(). */
void SwRenderer_present(void);

#endif /* _BS_SW_RENDERER_H_ */
