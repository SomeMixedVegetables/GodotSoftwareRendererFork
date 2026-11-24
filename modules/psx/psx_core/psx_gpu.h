// GPU model: a command-driven 2D rasterizer that owns one array of 16-bit VRAM.
//
// Framebuffers, textures and palettes all live in VRAM and every primitive
// reads and writes it directly. Commands arrive as GP0 words, exactly as DMA
// fed the real GPU, so an ordering table (psx_ot.h) or a recorded command
// stream can drive it. See the design doc, sections 8-14.
//
// Scaled VRAM. With scale N, VRAM is (1024 * N) x (512 * N) and the vertex
// range, primitive size limits, drawing area and fill/copy fields grow by N.
// N = 1 is the real hardware, bit for bit. Texture pages and CLUTs are
// addressed with the hardware's GP0 fields at every N, so they always live in
// the top-left 1024 x 512 of VRAM (the "texture region"); framebuffers can go
// anywhere. Where a GP0 field is too narrow for N > 1 (vertices, drawing area,
// drawing offset, fill/copy rectangles) a wider encoding is used; GP0Encoder
// writes the right one for the GPU's scale.

#pragma once

#include "psx_fixed.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace psx {

constexpr int32_t VRAM_BASE_WIDTH = 1024;
constexpr int32_t VRAM_BASE_HEIGHT = 512;
constexpr int MAX_VRAM_SCALE = 4;

enum BlendMode : uint8_t {
	BLEND_HALF = 0, // B/2 + F/2
	BLEND_ADD = 1, // B + F
	BLEND_SUBTRACT = 2, // B - F
	BLEND_ADD_QUARTER = 3, // B + F/4
};

enum TextureDepth : uint8_t {
	TEXTURE_4BIT = 0,
	TEXTURE_8BIT = 1,
	TEXTURE_15BIT = 2,
};

// Which VRAM rectangle is shown, and how.
struct DisplayArea {
	int32_t x = 0;
	int32_t y = 0;
	int32_t width = 320;
	int32_t height = 240;
	bool rgb24 = false; // Display-only 24-bit mode, for images and video.
};

struct GPUStats {
	uint32_t triangles = 0; // Drawn (each quad counts as two).
	uint32_t rejected_triangles = 0; // Size limit, zero area or zero height.
	uint32_t lines = 0;
	uint32_t rectangles = 0;
	uint32_t fills = 0;
	uint32_t commands = 0;
	void reset() { *this = GPUStats(); }
};

// The drawing state ("draw mode") set by GP0 E1h-E6h and by textured polygons.
struct DrawState {
	// Texpage (E1h), decoded.
	int32_t page_x = 0; // VRAM X of the texture page, in halfwords.
	int32_t page_y = 0;
	uint8_t blend = BLEND_HALF;
	uint8_t depth = TEXTURE_4BIT;
	bool dither = false;
	bool draw_to_display = false;
	bool flip_x = false; // Rectangles only.
	bool flip_y = false;
	// Texture window (E2h), as AND / OR masks on 8-bit texture coordinates.
	uint8_t window_and_x = 0xFF;
	uint8_t window_and_y = 0xFF;
	uint8_t window_or_x = 0;
	uint8_t window_or_y = 0;
	// Drawing area (E3h/E4h), inclusive.
	int32_t area_left = 0;
	int32_t area_top = 0;
	int32_t area_right = 0;
	int32_t area_bottom = 0;
	// Drawing offset (E5h).
	int32_t offset_x = 0;
	int32_t offset_y = 0;
	// Mask settings (E6h).
	bool set_mask = false;
	bool check_mask = false;
	// Interlaced field rendering: when on, rows whose LSB equals
	// active_field_lsb are skipped (they are on screen).
	bool interlaced = false;
	uint8_t active_field_lsb = 0;
};

// Builds GP0 words for a GPU of a given scale.
struct GP0Encoder {
	int scale = 1;

	explicit GP0Encoder(int p_scale = 1) :
			scale(p_scale) {}

	// Vertex XY. N = 1: signed 11-bit fields; N > 1: signed 16-bit halves.
	uint32_t vertex(int32_t p_x, int32_t p_y) const;
	// Absolute VRAM position or size (fill, copies, rectangles).
	uint32_t xy(int32_t p_x, int32_t p_y) const;
	uint32_t texcoord(uint8_t p_u, uint8_t p_v, uint16_t p_high = 0) const {
		return (uint32_t)p_u | ((uint32_t)p_v << 8) | ((uint32_t)p_high << 16);
	}
	// CLUT halfword: X in 16-halfword steps, Y 0..511.
	static uint16_t clut(int32_t p_x, int32_t p_y) {
		return (uint16_t)(((p_x >> 4) & 0x3F) | ((p_y & 0x1FF) << 6));
	}
	// Texpage halfword (also the E1h parameter): page X in 64-halfword steps,
	// page Y 0 or 256.
	static uint16_t texpage(int32_t p_page_x, int32_t p_page_y, uint8_t p_depth, uint8_t p_blend, bool p_dither = false, bool p_draw_to_display = false, bool p_flip_x = false, bool p_flip_y = false) {
		return (uint16_t)(((p_page_x >> 6) & 0xF) | (((p_page_y >> 8) & 1) << 4) | ((p_blend & 3) << 5) | ((p_depth & 3) << 7) |
				(p_dither ? 1 << 9 : 0) | (p_draw_to_display ? 1 << 10 : 0) | (p_flip_x ? 1 << 12 : 0) | (p_flip_y ? 1 << 13 : 0));
	}
	uint32_t draw_mode(uint16_t p_texpage) const { return 0xE1000000u | p_texpage; }
	// Texture window in 8-texel units (5-bit mask and offset per axis).
	uint32_t texture_window(uint8_t p_mask_x, uint8_t p_mask_y, uint8_t p_offset_x, uint8_t p_offset_y) const {
		return 0xE2000000u | (p_mask_x & 0x1Fu) | ((p_mask_y & 0x1Fu) << 5) | ((p_offset_x & 0x1Fu) << 10) | ((p_offset_y & 0x1Fu) << 15);
	}
	uint32_t draw_area_top_left(int32_t p_x, int32_t p_y) const;
	uint32_t draw_area_bottom_right(int32_t p_x, int32_t p_y) const;
	uint32_t draw_offset(int32_t p_x, int32_t p_y) const;
	uint32_t mask_bits(bool p_set_mask, bool p_check_mask) const {
		return 0xE6000000u | (p_set_mask ? 1u : 0u) | (p_check_mask ? 2u : 0u);
	}
};

class GPU {
public:
	explicit GPU(int p_scale = 1);

	int get_scale() const { return scale; }
	int32_t get_vram_width() const { return vram_width; }
	int32_t get_vram_height() const { return vram_height; }
	const GP0Encoder &get_encoder() const { return encoder; }

	uint16_t *get_vram() { return vram.data(); }
	const uint16_t *get_vram() const { return vram.data(); }
	uint16_t vram_get(int32_t p_x, int32_t p_y) const { return vram[(size_t)p_y * vram_width + p_x]; }
	void vram_set(int32_t p_x, int32_t p_y, uint16_t p_value) { vram[(size_t)p_y * vram_width + p_x] = p_value; }

	// Clears VRAM and all state.
	void reset();

	// GP0: one word at a time or a block. Multi-word commands and transfers
	// are buffered until complete.
	void write_gp0(uint32_t p_word);
	void write_gp0(const uint32_t *p_words, size_t p_count);

	// Halfwords produced by VRAM->CPU copies (GP0 C0h), oldest first.
	std::vector<uint16_t> &get_readback() { return readback; }

	const DrawState &get_state() const { return state; }

	DisplayArea display;
	GPUStats stats;

	// Converts the displayed rectangle to host RGBA8 (display.width *
	// display.height pixels, row-major).
	void scanout_rgba8(uint32_t *r_pixels) const;

private:
	struct Vertex {
		int32_t x, y;
		uint8_t r, g, b, u, v;
	};

	struct Texture {
		int32_t page_x, page_y;
		int32_t clut_x, clut_y;
		uint8_t depth;
	};

	int scale = 1;
	int32_t vram_width = VRAM_BASE_WIDTH;
	int32_t vram_height = VRAM_BASE_HEIGHT;
	int32_t max_width = 1024; // Primitives must span less than this.
	int32_t max_height = 512;
	GP0Encoder encoder;
	std::vector<uint16_t> vram;
	std::vector<uint16_t> readback;
	DrawState state;

	// Command assembly.
	uint32_t cmd[16] = {};
	int cmd_len = 0;
	int cmd_need = 0;
	// Polyline continuation.
	bool polyline_active = false;
	bool polyline_shaded = false;
	bool polyline_semi = false;
	bool polyline_want_color = false;
	uint32_t polyline_next_color = 0;
	Vertex polyline_prev = {};
	// CPU->VRAM transfer.
	uint32_t transfer_remaining = 0; // Halfwords.
	int32_t transfer_x = 0, transfer_y = 0, transfer_w = 0, transfer_h = 0;
	int32_t transfer_col = 0, transfer_row = 0;

	// 4x4 ordered dither folded into a lookup: [y & 3][x & 3][value] -> 5-bit.
	uint8_t dither_lut[4][4][512];

	int _command_length(uint32_t p_word) const;
	void _execute();
	void _execute_polygon();
	void _execute_line();
	void _execute_rectangle();
	void _execute_fill();
	void _execute_copy();
	void _set_texpage(uint16_t p_bits, bool p_from_polygon);

	int32_t _vertex_x(uint32_t p_word) const;
	int32_t _vertex_y(uint32_t p_word) const;
	int32_t _wrap_coord(int32_t p_value) const;
	void _transfer_halfword(uint16_t p_value);

	void _draw_triangle(const Vertex &p_v0, const Vertex &p_v1, const Vertex &p_v2, bool p_shaded, bool p_textured, bool p_raw, bool p_semi, const Texture &p_tex);
	template <bool SHADED, bool TEXTURED, bool RAW, bool SEMI>
	void _rasterize_triangle(const Vertex *p_v0, const Vertex *p_v1, const Vertex *p_v2, const Texture &p_tex);
	void _draw_line(const Vertex &p_a, const Vertex &p_b, bool p_shaded, bool p_semi);

	template <bool TEXTURED, bool RAW, bool SEMI>
	void _shade_pixel(int32_t p_x, int32_t p_y, uint8_t p_r, uint8_t p_g, uint8_t p_b, uint8_t p_u, uint8_t p_v, bool p_dither, const Texture &p_tex);
	uint16_t _fetch_texel(uint8_t p_u, uint8_t p_v, const Texture &p_tex) const;
	uint16_t _blend(uint16_t p_back, uint16_t p_front) const;
	bool _skip_row(int32_t p_y) const { return state.interlaced && (uint32_t)(p_y & 1) == state.active_field_lsb; }
};

} // namespace psx
