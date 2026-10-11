#!/usr/bin/env python3
"""
pack_assets.py - build-time asset packer for the Butterscotch GB300V2/SF2000 port.

Reads a GameMaker 1.4 data.win (a.k.a. game.unx) and produces the atlas the console
runtime needs so that it never has to decode a PNG:

    ATLAS.BIN        32-byte header + one 32-byte record per TPAG (6550 for Undertale)
    ATLAS.RGBA4444   2 bytes/pixel, R,G,B,A = 4:4:4:4, one block per unique source rect

Why a per-TPAG atlas at all
---------------------------
The 26 TXTR pages decode to ~180 MB of RGBA.  That cannot happen on a device with
58 MiB of heap, no mmap and no GPU - decoding a single 2048x2048 PNG through
stb_image in soft-float MIPS32 is a multi-second stall.

But the geometry is extremely lopsided: 90.7% of the 6550 TPAGs have a bounding box
<= 160 px and together they account for only 9.6 of the 35.5 MPix of total source
area.  So every *unique source rect* is cropped out of its page and stored on its
own.  Nothing ever decodes a whole page.

The block is the FULL source rect, not just the content box
-----------------------------------------------------------
This mirrors glDrawSprite()/glDrawSpritePartColor() in src/gl/gl_renderer.c exactly:

  drawSprite      UV  = (sourceX .. sourceX+sourceWidth) on the page
                 dest = (targetX-originX .. +targetWidth)
  drawSpritePart  UV  = (sourceX+srcOffX .. +srcW) on the page
                 dest = (x .. x + srcW*xscale)

So the renderer's source space is source-rect space with (0,0) at the source rect's
top-left, and the content box sits at (targetX, targetY) inside it.  Cropping to the
content box instead would silently break draw_sprite_part and would make our UV
math diverge from upstream's.  Keep the padding.

Downscale cap
-------------
Rects larger than --atlas-cap px on their long side are box-filtered down to that
size.  Anything wider than the 320x240 panel is resampled by the panel anyway, so
this is visually free.  Rebuild with --atlas-cap 512 (conservative) or 0 (off).

Format
------
RGBA4444 rather than RGB565: same 2 bytes/pixel but keeps the alpha channel that
Undertale leans on constantly.  The blit converts to the panel's RGB565 with an
ordered dither, once per pixel, inside the inner loop.

No mip chain: capping at 320 px already bounds the worst-case block at 320*320*2 =
205 kB, so a byte-budgeted LRU (12 MB by default) holds ~60 of them.

Usage:
    pack_assets.py --datawin assets/game.unx --out build [--atlas-cap 320]
"""

import argparse
import io
import os
import struct
import sys
import time

from PIL import Image, ImageChops

ATLAS_MAGIC = b'ATLS'
ATLAS_VERSION = 1
HEADER_SIZE = 32
RECORD_SIZE = 32

# Record layout (little-endian, 32 bytes):
#   u16 srcW, srcH          original source rect size on the page
#   u16 contentW, contentH  TPAG targetWidth/targetHeight
#   u16 contentX, contentY  TPAG targetX/targetY (content origin inside the rect)
#   u16 storedW, storedH    block size after --atlas-cap
#   u32 pixelOffset         byte offset into ATLAS.RGBA4444 (0 = empty block)
#   u16 bboxW, bboxH        TPAG boundingWidth/boundingHeight
#   u16 pageId              original TXTR page
#   u16 flags
#   u32 reserved
RECORD_FMT = '<8HIHHHHI'
assert struct.calcsize(RECORD_FMT) == RECORD_SIZE, struct.calcsize(RECORD_FMT)

EMPTY_RECORD = struct.pack(RECORD_FMT, *([0] * 14))


# --------------------------------------------------------------- data.win ----

def read_chunk_table(data):
    """Yield (chunk_id, payload_offset, payload_length) for a GMS FORM/WAD file."""
    if data[:4] != b'FORM':
        raise ValueError('not a FORM/WAD file: %r' % data[:4])
    end = struct.unpack_from('<I', data, 4)[0] + 8
    pos = 8
    while pos + 8 <= end:
        cid, clen = struct.unpack_from('<4sI', data, pos)
        if not all(32 <= c < 127 for c in cid):
            break
        yield cid.decode('ascii'), pos + 8, clen
        pos = (pos + 8 + clen + 3) & ~3


def read_pointer_table(data, offset):
    count = struct.unpack_from('<I', data, offset)[0]
    return count, list(struct.unpack_from('<%dI' % count, data, offset + 4))


def parse_txtr(data, offset, length):
    """GMS 1.4 TXTR entries are two u32 (scaled, blobOffset); sizes come from the
    gap to the next entry's blob."""
    count, ptrs = read_pointer_table(data, offset)
    out = []
    for i, p in enumerate(ptrs):
        blob = struct.unpack_from('<II', data, p)[1]
        if i + 1 < count:
            nxt = struct.unpack_from('<II', data, ptrs[i + 1])[1]
            size = nxt - blob
        else:
            size = offset + length - blob
        out.append((blob, max(0, size)))
    return out


def parse_tpag(data, offset):
    """GMS 1.4.1804 TPAG entries are 11x u16.  Older WADs add separation+bpp
    (13x u16); detect that by checking whether field 10 looks like a page id."""
    count, ptrs = read_pointer_table(data, offset)
    stride = 22
    if len(ptrs) >= 2:
        stride = ptrs[1] - ptrs[0]
    if stride not in (22, 26):
        # Fall back on the most common gap.
        gaps = sorted(ptrs[i + 1] - ptrs[i] for i in range(len(ptrs) - 1))
        stride = gaps[len(gaps) // 2]
    base = 10 if stride == 22 else 12
    ents = []
    for p in ptrs:
        v = struct.unpack_from('<%dH' % (stride // 2), data, p)
        ents.append({
            'sourceX': v[0], 'sourceY': v[1],
            'sourceWidth': v[2], 'sourceHeight': v[3],
            'targetX': v[4], 'targetY': v[5],
            'targetWidth': v[6], 'targetHeight': v[7],
            'boundingWidth': v[8], 'boundingHeight': v[9],
            'pageId': v[base],
        })
    return ents, stride


# ------------------------------------------------------------------ packing ---

# 8-bit LUTs so every per-pixel operation stays inside PIL's C loops.
_LUT_HI = list((v & 0xF0) for v in range(256))
_LUT_LO = list((v >> 4) for v in range(256))


def rgba_to_rgba4444(img):
    """RGBA8 PIL image -> R,G,B,A packed as 4:4:4:4 bytes (interleaved rb, ga per pixel).

    Uses only PIL C loops; no numpy required.  ImageChops.lighter is broken in the
    packaged Pillow (returns image1), so use add() -- with the nibbles already
    separated there is no carry and add() is exactly a bitwise OR.
    Output per pixel:
      byte 0 (rb): (R & 0xF0) | (B >> 4)
      byte 1 (ga): (G & 0xF0) | (A >> 4)
    """
    r, g, b, a = img.split()
    rb = ImageChops.add(r.point(_LUT_HI), b.point(_LUT_LO))
    ga = ImageChops.add(g.point(_LUT_HI), a.point(_LUT_LO))
    return Image.merge('LA', (rb, ga)).tobytes()


def fit_cap(w, h, cap):
    if cap <= 0:
        return w, h
    m = max(w, h)
    if m <= cap:
        return w, h
    s = float(cap) / float(m)
    return max(1, int(round(w * s))), max(1, int(round(h * s)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--datawin', default='assets/game.unx')
    ap.add_argument('--out', default='build')
    ap.add_argument('--atlas-cap', type=int, default=320,
                    help='box-filter source rects larger than this many px (0 = off)')
    ap.add_argument('--report', default=None)
    args = ap.parse_args()

    t0 = time.time()
    os.makedirs(args.out, exist_ok=True)

    print('reading %s ...' % args.datawin, flush=True)
    with open(args.datawin, 'rb') as f:
        data = f.read()
    print('  %.1f MB' % (len(data) / 1048576.0), flush=True)

    chunks = {c: (o, l) for c, o, l in read_chunk_table(data)}
    for want in ('TXTR', 'TPAG'):
        if want not in chunks:
            print('FATAL: no %s chunk - wrong file?' % want)
            return 1
    print('  %d chunks: %s' % (len(chunks), ' '.join(sorted(chunks))), flush=True)

    txtr_off, txtr_len = chunks['TXTR']
    pages = parse_txtr(data, txtr_off, txtr_len)
    tpag_off, _ = chunks['TPAG']
    tpag, stride = parse_tpag(data, tpag_off)
    print('TXTR %d pages | TPAG %d items (stride %d)'
          % (len(pages), len(tpag), stride), flush=True)

    # ---- unique source rects, first occurrence wins --------------------------
    unique = {}
    order = []
    for t in tpag:
        if t['sourceWidth'] <= 0 or t['sourceHeight'] <= 0:
            continue
        key = (t['pageId'], t['sourceX'], t['sourceY'],
               t['sourceWidth'], t['sourceHeight'])
        if key not in unique:
            unique[key] = None
            order.append(key)
    print('unique source rects: %d (dedup from %d TPAGs)\n'
          % (len(order), len(tpag)), flush=True)

    atlas_path = os.path.join(args.out, 'ATLAS.BIN')
    pix_path = os.path.join(args.out, 'ATLAS.RGBA4444')

    n_scaled = n_clipped = n_dropped = 0
    pixel_bytes = 0

    # Group by page so each PNG is decoded exactly once and can then be freed.
    by_page = {}
    for key in order:
        by_page.setdefault(key[0], []).append(key)

    with open(pix_path, 'wb') as pixf:
        done = 0
        for page_id in sorted(by_page):
            blob_off, blob_size = pages[page_id]
            raw = data[blob_off:blob_off + blob_size]
            if raw[:8] != b'\x89PNG\r\n\x1a\n':
                print('FATAL: TXTR page %d is not a PNG' % page_id)
                return 1
            page = Image.open(io.BytesIO(raw)).convert('RGBA')
            del raw

            for key in by_page[page_id]:
                _, sx, sy, ow, oh = key
                sw, sh = ow, oh

                # Clamp the crop to the page; a few TPAGs point past the edge.
                if sx >= page.width or sy >= page.height:
                    n_dropped += 1
                    continue
                if sx + sw > page.width:
                    sw = page.width - sx
                    n_clipped += 1
                if sy + sh > page.height:
                    sh = page.height - sy
                    n_clipped += 1
                if sw <= 0 or sh <= 0:
                    n_dropped += 1
                    continue

                crop = page.crop((sx, sy, sx + sw, sy + sh))
                nw, nh = fit_cap(sw, sh, args.atlas_cap)
                if (nw, nh) != (sw, sh):
                    n_scaled += 1
                    crop = crop.resize((nw, nh), Image.BOX)

                payload = rgba_to_rgba4444(crop)
                # Key stays the ORIGINAL rect so the index can find it even when
                # the crop had to be clamped to the page.
                unique[key] = (pixel_bytes, ow, oh, nw, nh)
                pixf.write(payload)
                pixel_bytes += len(payload)

                done += 1
                if done % 1000 == 0:
                    print('  %d/%d rects  %.1f MB'
                          % (done, len(order), pixel_bytes / 1048576.0), flush=True)
            del page

    # ---- index --------------------------------------------------------------
    with open(atlas_path, 'wb') as af:
        af.write(struct.pack('<4sIIIII', ATLAS_MAGIC, ATLAS_VERSION,
                             len(tpag), len(pages), args.atlas_cap, len(order)))
        af.write(b'\x00' * (HEADER_SIZE - 24))
        n_empty = 0
        for t in tpag:
            rec = unique.get((t['pageId'], t['sourceX'], t['sourceY'],
                              t['sourceWidth'], t['sourceHeight']))
            if rec is None:
                af.write(EMPTY_RECORD)
                n_empty += 1
                continue
            off, sw, sh, nw, nh = rec
            af.write(struct.pack(
                RECORD_FMT,
                sw, sh,
                max(1, t['targetWidth']), max(1, t['targetHeight']),
                t['targetX'], t['targetY'],
                nw, nh,
                off,
                max(1, t['boundingWidth']), max(1, t['boundingHeight']),
                t['pageId'], 0, 0))

    print('\n--- result ---')
    print('ATLAS.BIN        %7.2f MB   %d records (%d empty)'
          % (os.path.getsize(atlas_path) / 1048576.0, len(tpag), n_empty))
    print('ATLAS.RGBA4444   %7.2f MB   cap=%d  downscaled=%d  clipped=%d  dropped=%d'
          % (pixel_bytes / 1048576.0, args.atlas_cap, n_scaled, n_clipped, n_dropped))
    big = pixel_bytes
    print('cache @12 MB holds %d worst-case 320x320 blocks' % (12 * 1048576 // 205056))
    print('elapsed %.1f s' % (time.time() - t0))

    if args.report:
        with open(args.report, 'w') as f:
            f.write('pages=%d\ntpag=%d\nstride=%d\nunique=%d\ncap=%d\n'
                    'downscaled=%d\nempty=%d\npixels=%d\n'
                    % (len(pages), len(tpag), stride, len(order), args.atlas_cap,
                       n_scaled, n_empty, pixel_bytes))
    return 0


if __name__ == '__main__':
    sys.exit(main())
