// PSXTexture: a texture converted for the PS1 core at import time (the
// "PSX Texture" importer, Import As in the Import dock).
//
// It is an ordinary ImageTexture, so Godot, the editor and the other
// renderers draw it as usual (its image is the original or a PS1 preview).
// It also carries the converted texels and palettes, one set per way a
// material can use alpha. Every loaded PSXTexture registers its RID, so the
// scene bridge finds the baked data from the material's texture RID instead
// of reading the image back from the GPU and converting it.

#pragma once

#include "psx_core/psx_texture.h"
#include "psx_palette.h"

#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "scene/resources/image_texture.h"

class PSXTexture : public ImageTexture {
	GDCLASS(PSXTexture, ImageTexture);

	PackedByteArray baked_bytes;
	RID registered;

	void _register();
	void _unregister();

protected:
	static void _bind_methods();

public:
	// The converted textures in psx::write_baked_textures' format. Setting
	// it (re)registers this texture's RID.
	void set_baked_data(const PackedByteArray &p_bytes);
	PackedByteArray get_baked_data() const;

	// Converts p_image for the PS1 core: what the PSX Texture importer does,
	// also callable from scripts. p_max_size 0, p_depth -1 and p_max_colors
	// -1 and p_dither -1 use the rendering/psx/textures/* project settings
	// (p_dither 0 off, 1 ordered). With p_preview
	// the texture's own image becomes the PS1 result (what Godot shows).
	// With p_palette the texture is made with that palette (its depth and
	// max_colors are not used), so it shares the palette's CLUT and a
	// material can swap in another palette of the same size.
	static Ref<PSXTexture> bake(const Ref<Image> &p_image, int p_max_size = 0, int p_depth = -1, int p_max_colors = -1, float p_alpha_cut = 0.5f, bool p_preview = true, const Ref<PSXPalette> &p_palette = Ref<PSXPalette>(), int p_dither = -1);

	// A summary for the inspector, e.g. "64x64 4-bit, 12 colours".
	String get_baked_summary() const;

	// Thread-safe lookup for the renderer: the baked texture for p_texture
	// made for p_alpha_mode (and, for ALPHA_CUT, a cut within 1/512 of
	// p_alpha_cut). r_version changes whenever the texture is re-imported.
	static bool find_baked(RID p_texture, uint8_t p_alpha_mode, float p_alpha_cut, psx::TextureData &r_data, uint32_t &r_version);
	// Cheap check before find_baked: r_version of the matching baked
	// texture, or 0 when there is none.
	static uint32_t get_baked_version(RID p_texture, uint8_t p_alpha_mode, float p_alpha_cut);
	// The CLUT entries (16 or 256) of the palette the matching baked
	// texture was made with, or 0 when it has its own palette or none.
	static int32_t get_baked_palette_entries(RID p_texture, uint8_t p_alpha_mode, float p_alpha_cut);

	~PSXTexture();
};
