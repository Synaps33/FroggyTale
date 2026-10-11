/*
 * STB implementation for the sf2000 platform.
 *
 * image_decoder.c calls stbi_load_from_memory(), but on this target we never feed it
 * a PNG: the textures come pre-cropped out of the pages and pre-converted to
 * RGBA4444 by tools/pack_assets.py (see ATLAS.RGBA4444).  The decoder is still
 * linked because runner.c and the debug font reference it, and Undertale's own
 * bitmap fonts are decoded through the same path.
 *
 * STBI_NO_STDIO: there is no stdio-based loading here - everything comes from memory.
 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#include "stb_image.h"
