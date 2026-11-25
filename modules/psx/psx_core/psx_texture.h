// Texture conversion and VRAM placement (design doc, section 11).
//
// convert_texture turns an RGBA8 image into what the GPU samples: a
// power-of-two texture of at most 256 x 256 texels, 4-bit or 8-bit indices
// into a CLUT of 15-bit colours (or direct 15-bit colour), packed into
// halfwords the way the texture page stores them. Like the mesh converter it
// knows nothing about Godot and serves both runtime and import-time paths.
//
// Transparency follows the hardware: texel 0000h is never drawn, and bit 15
// (STP) marks a texel that blends when its polygon is semi-transparent.
// Opaque black is stored as 8000h (as PS1 artists did), or as the darkest
// grey when the texture also has semi-transparent texels, since 8000h would
// blend in a semi-transparent polygon. clut_semi is the same palette with
// STP on every visible colour, for polygons that are 50% transparent as a
// whole.
//
// VramAllocator hands out space in one 1024 x 512 texture region (VRAM's,
// or a texture bank's). Every
// texture gets a power-of-two box aligned to its own size, so it never
// crosses a texture page and its texture window (repeat) is always legal.
// CLUTs are packed into 256 x 8 blocks taken from the same space.

#pragma once

#include "psx_gpu.h"

#include <cstdint>
#include <vector>

namespace psx {

enum TextureDepthChoice : uint8_t {
	DEPTH_AUTO = 0, // 4-bit if 16 colours are enough, else 8-bit
	DEPTH_FORCE_4BIT = 1,
	DEPTH_FORCE_8BIT = 2,
	DEPTH_FORCE_15BIT = 3,
};

enum TextureAlphaMode : uint8_t {
	ALPHA_IGNORE = 0, // Opaque material: texture alpha is not used
	ALPHA_CUT = 1, // Alpha scissor: below alpha_cut is transparent
	ALPHA_BLEND = 2, // Blended: <= 0.25 transparent, < 0.75 semi, else opaque
};

struct TextureOptions {
	int32_t max_size = 256; // Largest side in texels: a power of two, 8..256
	uint8_t depth = DEPTH_AUTO;
	// Palette size limit (2..256); 0 = as many as the depth allows.
	int32_t max_colors = 0;
	uint8_t alpha_mode = ALPHA_IGNORE;
	float alpha_cut = 0.5f;
	// A fixed palette (PSXPalette) instead of one made for the image: its
	// visible colours as 15-bit keys, at most palette_entries - 1 of them.
	// CLUT entry 0 stays transparent and entry i + 1 is palette[i], so the
	// CLUT is the same for every texture made with the palette and they
	// share it in VRAM. The texture is 4-bit for 16 entries and 8-bit for
	// 256 (depth and max_colors are not used); every texel takes its nearest
	// colour, and half-transparent texels become opaque (the palette's
	// colours carry no STP).
	const uint16_t *palette = nullptr;
	int32_t palette_size = 0;
	int32_t palette_entries = 16;
	// Ordered (4x4 Bayer) dither where colours are lost, locked to the
	// texel grid: on the 8- to 5-bit step of 15-bit textures, and on the
	// choice of palette entry of 4- and 8-bit ones, by about the distance
	// between neighbouring palette colours (at most eight 5-bit steps). A palette that holds every
	// colour of the image is not dithered.
	bool dither = false;
};

struct TextureData {
	int32_t width = 0; // Texels, power of two, 8..256
	int32_t height = 0;
	uint8_t depth = TEXTURE_4BIT;
	std::vector<uint16_t> clut; // 16 or 256 entries; empty for 15-bit
	std::vector<uint16_t> clut_semi; // clut with STP on every visible entry
	std::vector<uint16_t> clut_white; // Every visible entry white with STP (fog pass of cut-outs)
	std::vector<uint16_t> texels; // hw_width() * height halfwords, row-major
	bool has_transparent = false; // Some texels are 0000h
	bool has_semi = false; // Some texels have STP set
	uint8_t mean[3] = { 128, 128, 128 }; // Average visible colour, sRGB 0..255
	int32_t colors = 0; // Palette entries in use
	// Made with a fixed palette: index i + 1 is the palette's colour i, so
	// another palette of the same size can be swapped in (apply_palette).
	bool shared_palette = false;

	int32_t hw_width() const { return depth == TEXTURE_4BIT ? width / 4 : (depth == TEXTURE_8BIT ? width / 2 : width); }
	int32_t clut_entries() const { return depth == TEXTURE_4BIT ? 16 : (depth == TEXTURE_8BIT ? 256 : 0); }
};

// Returns false (r_texture empty) for an empty image.
bool convert_texture(const uint8_t *p_rgba, int32_t p_width, int32_t p_height, const TextureOptions &p_options, TextureData &r_texture);

// An image a palette is made from: RGBA8, p_width x p_height.
struct PaletteSource {
	const uint8_t *rgba = nullptr;
	int32_t width = 0;
	int32_t height = 0;
};

// A palette of up to p_colors 15-bit keys (sorted) for all the sources
// together: the median cut over every pixel with alpha of at least 128.
// Returns false (r_palette empty) when there are none.
bool make_palette(const PaletteSource *p_sources, int32_t p_count, int32_t p_colors, std::vector<uint16_t> &r_palette);

// A palette swap: replaces r_texture's CLUTs with p_palette's colours, as
// convert_texture would build them. False (r_texture unchanged) when the
// texture was not made with a fixed palette or p_count colours do not fit
// its CLUT. Indices past p_count become transparent.
bool apply_palette(const uint16_t *p_palette, int32_t p_count, TextureData &r_texture);

// A texture converted ahead of time (the PSX Texture importer) for one way
// a material can use its alpha.
struct BakedTexture {
	uint8_t alpha_mode = ALPHA_IGNORE;
	float alpha_cut = 0.5f; // ALPHA_CUT only
	TextureData data;
};

// Appends p_textures to r_bytes in a little-endian format of its own
// ("PSXT", version). read_baked_textures returns false (r_textures empty)
// for anything it does not recognise or that is cut short.
void write_baked_textures(const std::vector<BakedTexture> &p_textures, std::vector<uint8_t> &r_bytes);
bool read_baked_textures(const uint8_t *p_bytes, size_t p_size, std::vector<BakedTexture> &r_textures);

// What the GPU would show for each texel, as RGBA8 (width x height): 15-bit
// colour widened to 8 bits; alpha 0 for transparent texels, 128 for STP
// ones when p_semi_alpha, else 255. For previews and tests.
void decode_texture(const TextureData &p_texture, bool p_semi_alpha, std::vector<uint8_t> &r_rgba);

// CPU->VRAM copy (GP0 A0h) of a p_width x p_height rectangle of halfwords,
// into texture bank p_bank (0 = VRAM).
void upload_rect(GPU &p_gpu, int32_t p_x, int32_t p_y, int32_t p_width, int32_t p_height, const uint16_t *p_data, uint16_t p_bank = 0);

class VramAllocator {
public:
	// Allocation grid: 2 halfwords x 8 rows per cell over the texture region.
	static constexpr int32_t CELL_W = 2;
	static constexpr int32_t CELL_H = 8;
	static constexpr int32_t GRID_W = VRAM_BASE_WIDTH / CELL_W;
	static constexpr int32_t GRID_H = VRAM_BASE_HEIGHT / CELL_H;

	VramAllocator() { reset(); }

	void reset();
	// Marks a rectangle (any VRAM coordinates; clipped to the region) as used.
	void reserve(int32_t p_x, int32_t p_y, int32_t p_width, int32_t p_height);
	// A box of at least p_hw_width halfwords x p_height rows, rounded up to
	// powers of two and aligned to them. False when there is no room.
	bool alloc(int32_t p_hw_width, int32_t p_height, int32_t &r_x, int32_t &r_y);
	void free(int32_t p_x, int32_t p_y, int32_t p_hw_width, int32_t p_height);
	// A CLUT of 16 or 256 entries.
	bool alloc_clut(int32_t p_entries, int32_t &r_x, int32_t &r_y);
	void free_clut(int32_t p_x, int32_t p_y, int32_t p_entries);

	uint32_t free_cells() const;
	static int32_t box_width(int32_t p_hw_width);
	static int32_t box_height(int32_t p_height);

private:
	std::vector<uint8_t> cells;
	struct ClutBlock {
		int32_t x = 0, y = 0;
		uint16_t rows[CELL_H] = {}; // One bit per 16-entry slot
	};
	std::vector<ClutBlock> clut_blocks;

	bool _fits(int32_t p_cx, int32_t p_cy, int32_t p_cw, int32_t p_ch) const;
	void _mark(int32_t p_cx, int32_t p_cy, int32_t p_cw, int32_t p_ch, uint8_t p_value);
};

} // namespace psx
