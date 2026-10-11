/*
 * atlas.h - reader for the build-time texture pack (see atlas.c).
 *
 * Block files are produced by tools/pack_assets.py from the data.win's TXTR pages and
 * TPAG table.  Layout of ATLAS.BIN:
 *
 *   header, 32 bytes
 *     char   magic[4]   "ATLS"
 *     u32    version    1
 *     u32    recordCount
 *     u32    pageCount
 *     u32    cap        longest-edge downscale cap that was applied (0 = none)
 *     u32    uniqueBlocks
 *     u8     pad[8]
 *
 *   recordCount records of 32 bytes, one per TPAG, in TPAG order
 *     u16 srcW, srcH          original source rect size on the page
 *     u16 contentW, contentH  TPAG targetWidth/targetHeight
 *     u16 contentX, contentY  TPAG targetX/targetY inside the rect
 *     u16 storedW, storedH    block size after the cap
 *     u32 offset              byte offset into ATLAS.RGBA4444 (0 = empty)
 *     u16 bboxW, bboxH        TPAG boundingWidth/boundingHeight
 *     u16 pageId              original TXTR page
 *     u16 flags
 *     u32 reserved
 *
 * ATLAS.RGBA4444 holds the pixels: interleaved byte pairs per pixel:
 *     byte 0: rb = (R & 0xF0) | (B >> 4)   (R in high nibble, B in low)
 *     byte 1: ga = (G & 0xF0) | (A >> 4)   (G in high nibble, A in low)
 */

#ifndef _BS_SW_ATLAS_H_
#define _BS_SW_ATLAS_H_

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint16_t srcWidth, srcHeight;
    uint16_t contentW, contentH;
    uint16_t contentX, contentY;
    uint16_t storedW, storedH;
    uint32_t offset;
    uint16_t bboxW, bboxH;
    uint16_t pageId;
    uint16_t flags;
} SwAtlasRecord;

typedef struct SwAtlasBlock {
    struct SwAtlasBlock* prev;
    struct SwAtlasBlock* next;
    int32_t  tpagIndex;
    uint16_t w, h;
    uint16_t* pixels;      /* w*h RGBA4444: (R<<12) | (G<<8) | (B<<4) | A */
    uint32_t bytes;
    bool     pinned;      /* set while a frame is blitting this block */
    bool     hasAlpha;
} SwAtlasBlock;

typedef struct {
    SwAtlasRecord* records;
    uint32_t count;
    FILE*    pixels;

    SwAtlasBlock* lruHead;
    uint32_t cacheBytes;
    uint32_t cacheBudget;
    uint32_t blockCount;
    uint32_t cacheHits;
    uint32_t cacheMisses;
} SwAtlas;

bool                  SwAtlas_open(SwAtlas* atlas, const char* dir);
void                  SwAtlas_close(SwAtlas* atlas);
const SwAtlasRecord*  SwAtlas_record(const SwAtlas* atlas, int32_t tpagIndex);

SwAtlasBlock*         SwAtlas_block(SwAtlas* atlas, int32_t tpagIndex);
void                  SwAtlasCache_release(SwAtlas* atlas, SwAtlasBlock* b);
void                  SwAtlasCache_clear(SwAtlas* atlas);

#endif /* _BS_SW_ATLAS_H_ */
