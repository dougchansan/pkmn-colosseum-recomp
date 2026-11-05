// colosseum-standalone's HD text: Colosseum's text drawn from a host font at
// output resolution, as ModernGekko does (VideoCommon/ColosseumNativeText in
// its Dolphin fork).
//
// The game copies each glyph from its bitmap font into a 512x512 I4 scratch
// texture and draws a quad from that slot. The recomp module reports every
// copy as BP writes 0x6A-0x6E in the GX stream (GXRuntime native_sdk.c,
// dolrecomp_native_msg_glyph_blit); Aurora hands each report to the
// rasterizer registered here and keeps an output-resolution stand-in for the
// scratch texture built from what it returns. The game's layout is unchanged;
// only the pixels inside each slot come from the font. The module also asks
// the host (external_pointer(0xFFFFE000, 0)) where to put a slot so the host
// font's taller ascenders fit: native_text_place_slot.
#pragma once

#include <cstdint>

#include "gxruntime/aurora_backend.h"

// On or off at any time (the HD text setting); off, the game's own glyphs show.
void native_text_set_enabled(bool enabled);
// Whether the font loaded (tried once, on first use).
bool native_text_available();

// For the module's slot query: see ColosseumNativeText::PlaceSlot.
int native_text_place_slot(uint32_t code, uint32_t font, int32_t height, int32_t* y_offset);

// Aurora's text rasterizer (aurora_native_set_text_rasterizer). `ram` is MEM1,
// for the game's own bitmaps of the symbols the font does not draw.
void native_text_set_guest_ram(const uint8_t* ram, uint32_t size);
bool native_text_rasterize(const u32 words[5], u32 scale, AuroraTextGlyph* out);
