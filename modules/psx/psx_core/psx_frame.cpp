#include "psx_frame.h"

#include <vector>

namespace psx {

// --- FrameLayout ---

FrameLayout FrameLayout::make(const GPU &p_gpu, int32_t p_width, int32_t p_height) {
	FrameLayout l;
	l.width = p_width;
	l.height = p_height;
	const int32_t vw = p_gpu.get_vram_width(), vh = p_gpu.get_vram_height();
	if (p_width <= 0 || p_height <= 0) {
		return l;
	}
	auto align16 = [](int32_t v) { return (v + 15) & ~15; };
	// Drawing offsets are 11-bit (scale 1) or 12-bit (scale > 1) signed.
	const int32_t max_origin = p_gpu.get_scale() == 1 ? 1023 : 2047;
	auto place = [&](int32_t x0, int32_t y0, int32_t x1, int32_t y1) -> bool {
		const int32_t w = x1 - x0, h = y1 - y0;
		if (p_width <= w && 2 * p_height <= h && y0 + p_height <= max_origin) {
			l.buffer_count = 2;
			l.buffer_x[0] = l.buffer_x[1] = x0;
			l.buffer_y[0] = y0;
			l.buffer_y[1] = y0 + p_height;
			return true;
		}
		if (2 * align16(p_width) <= w && p_height <= h && x0 + align16(p_width) <= max_origin) {
			l.buffer_count = 2;
			l.buffer_x[0] = x0;
			l.buffer_x[1] = x0 + align16(p_width);
			l.buffer_y[0] = l.buffer_y[1] = y0;
			return true;
		}
		return false;
	};
	auto place_single = [&](int32_t x0, int32_t y0, int32_t x1, int32_t y1) -> bool {
		if (p_width <= x1 - x0 && p_height <= y1 - y0) {
			l.buffer_count = 1;
			l.buffer_x[0] = l.buffer_x[1] = x0;
			l.buffer_y[0] = l.buffer_y[1] = y0;
			return true;
		}
		return false;
	};
	if (p_gpu.get_scale() == 1) {
		// As on hardware: buffers at the left, textures from X = 640.
		if (place(0, 0, 640, vh) || place_single(0, 0, 640, vh)) {
			return l;
		}
		place_single(0, 0, vw, vh);
		return l;
	}
	// Outside the 1024 x 512 texture region: the strip to its right, then the
	// strip below it.
	if (place(VRAM_BASE_WIDTH, 0, vw, vh) || place(0, VRAM_BASE_HEIGHT, vw, vh)) {
		return l;
	}
	if (place_single(VRAM_BASE_WIDTH, 0, vw, vh) || place_single(0, VRAM_BASE_HEIGHT, vw, vh)) {
		return l;
	}
	// Last resort: one buffer against the bottom of VRAM, covering the lower
	// rows of the texture region.
	if (place_single(0, vh - p_height, vw, vh) && vh - p_height <= max_origin) {
		l.overlaps_texture_region = true;
	} else {
		l.buffer_count = 0;
	}
	return l;
}

// --- DemoScene ---

static void upload(GPU &p_gpu, int32_t p_x, int32_t p_y, int32_t p_w, int32_t p_h, const std::vector<uint16_t> &p_data) {
	const GP0Encoder &enc = p_gpu.get_encoder();
	p_gpu.write_gp0(0xA0000000u);
	p_gpu.write_gp0(enc.xy(p_x, p_y));
	p_gpu.write_gp0(enc.xy(p_w, p_h));
	for (size_t i = 0; i < p_data.size(); i += 2) {
		const uint32_t lo = p_data[i];
		const uint32_t hi = i + 1 < p_data.size() ? p_data[i + 1] : 0;
		p_gpu.write_gp0(lo | (hi << 16));
	}
}

void DemoScene::upload_textures(GPU &p_gpu) {
	// 4-bit: two interleaved colour ramps in an 8x8 checker.
	std::vector<uint16_t> tex4(16 * 64, 0);
	for (int y = 0; y < 64; y++) {
		for (int x = 0; x < 64; x++) {
			const uint32_t index = (((x >> 3) ^ (y >> 3)) & 1) ? 8 + ((x + y) >> 4) % 8 : ((x >> 2) & 7);
			tex4[y * 16 + x / 4] |= (uint16_t)(index << ((x & 3) * 4));
		}
	}
	upload(p_gpu, TEX4_X, TEX4_Y, 16, 64, tex4);
	std::vector<uint16_t> clut4(16);
	for (int i = 0; i < 16; i++) {
		clut4[i] = i < 8 ? rgb15(8 + i * 3, 4 + i * 2, 2) : rgb15(2, 6 + (i - 8) * 3, 12 + (i - 8) * 2);
	}
	upload(p_gpu, CLUT4_X, CLUT4_Y, 16, 1, clut4);

	// 8-bit: diagonal bands through a 256-colour palette.
	std::vector<uint16_t> tex8(32 * 64, 0);
	for (int y = 0; y < 64; y++) {
		for (int x = 0; x < 64; x++) {
			const uint32_t index = (uint32_t)(x * 4 + y * 2 + ((x * y) >> 5)) & 0xFF;
			tex8[y * 32 + x / 2] |= (uint16_t)(index << ((x & 1) * 8));
		}
	}
	upload(p_gpu, TEX8_X, TEX8_Y, 32, 64, tex8);
	std::vector<uint16_t> clut8(256);
	for (int i = 0; i < 256; i++) {
		const uint16_t c = rgb15((uint32_t)i >> 3, (uint32_t)(i * 3 >> 3) & 31, 31 - ((uint32_t)i >> 3));
		clut8[i] = c ? c : 1;
	}
	upload(p_gpu, CLUT8_X, CLUT8_Y, 256, 1, clut8);

	// 15-bit: a ring with a transparent hole (0000h), an opaque-black rim
	// (8000h) and a checker of semi-transparent texels (bit 15).
	std::vector<uint16_t> tex15(64 * 64, 0);
	for (int y = 0; y < 64; y++) {
		for (int x = 0; x < 64; x++) {
			const int dx = x - 32, dy = y - 32, d2 = dx * dx + dy * dy;
			uint16_t t;
			if (d2 < 100) {
				t = 0x0000;
			} else if (d2 > 30 * 30) {
				t = 0x8000;
			} else {
				t = rgb15((uint32_t)x >> 1, (uint32_t)y >> 1, 20);
				if (((x >> 3) + (y >> 3)) & 1) {
					t |= 0x8000;
				}
			}
			tex15[y * 64 + x] = t;
		}
	}
	upload(p_gpu, TEX15_X, TEX15_Y, 64, 64, tex15);
}

namespace {

struct Face {
	int8_t corner[4][3]; // TL, TR, BL, BR seen from outside, as -1 / +1
};

// Corners in Z order (TL, TR, BL, BR) as seen from outside each face, with
// camera-space X right, Y down, Z forward.
const Face CUBE_FACES[6] = {
	{ { { -1, -1, -1 }, { +1, -1, -1 }, { -1, +1, -1 }, { +1, +1, -1 } } }, // -Z
	{ { { +1, -1, +1 }, { -1, -1, +1 }, { +1, +1, +1 }, { -1, +1, +1 } } }, // +Z
	{ { { -1, -1, +1 }, { -1, -1, -1 }, { -1, +1, +1 }, { -1, +1, -1 } } }, // -X
	{ { { +1, -1, -1 }, { +1, -1, +1 }, { +1, +1, -1 }, { +1, +1, +1 } } }, // +X
	{ { { -1, -1, +1 }, { +1, -1, +1 }, { -1, -1, -1 }, { +1, -1, -1 } } }, // -Y
	{ { { -1, +1, -1 }, { +1, +1, -1 }, { -1, +1, +1 }, { +1, +1, +1 } } }, // +Y
};

} // namespace

DemoStats DemoScene::render(GPU &p_gpu, const FrameLayout &p_layout, int p_buffer, uint32_t p_frame) {
	DemoStats out;
	if (p_layout.buffer_count == 0) {
		return out;
	}
	const int buffer = p_layout.buffer_count == 2 ? (p_buffer & 1) : 0;
	const int32_t bx = p_layout.buffer_x[buffer], by = p_layout.buffer_y[buffer];
	const int32_t w = p_layout.width, h = p_layout.height;

	arena.clear();
	PacketBuilder pb(arena, p_gpu.get_encoder());
	constexpr uint32_t OT_SIZE = 1024;
	OrderingTable ot(arena, OT_SIZE);
	ot.clear();

	const uint16_t page4 = GP0Encoder::texpage(TEX4_X, TEX4_Y, TEXTURE_4BIT, BLEND_HALF);
	const uint16_t page8 = GP0Encoder::texpage(TEX8_X, TEX8_Y, TEXTURE_8BIT, BLEND_HALF);
	const uint16_t page15 = GP0Encoder::texpage(TEX15_X, TEX15_Y, TEXTURE_15BIT, BLEND_HALF);
	const uint16_t page15_add = GP0Encoder::texpage(TEX15_X, TEX15_Y, TEXTURE_15BIT, BLEND_ADD);
	const uint16_t clut4 = GP0Encoder::clut(CLUT4_X, CLUT4_Y);
	const uint16_t clut8 = GP0Encoder::clut(CLUT8_X, CLUT8_Y);

	// Far end: drawing environment for this buffer, then the clear. Within a
	// bucket the newest packet is drawn first, so the environment goes last.
	ot.add(OT_SIZE - 1, pb.fill(bx, by, w, h, 24, 24, 56));
	ot.add(OT_SIZE - 1, pb.environment(bx, by, bx + w - 1, by + h - 1, bx, by, GP0Encoder::texpage(TEX4_X, TEX4_Y, TEXTURE_4BIT, BLEND_HALF, true)));

	// Geometry: one rotation/translation per object, vertices through the GTE.
	GTE gte;
	gte.scale = p_gpu.get_scale();
	gte.h = (uint16_t)(w * 4 / 5); // Same field of view at any width: 256 at 320.
	gte.ofx = (w / 2) * 65536;
	gte.ofy = (h / 2) * 65536;
	gte.rotation = rot_matrix((int32_t)(p_frame * 12), (int32_t)(p_frame * 20), (int32_t)(p_frame * 6));
	gte.translation = { 0, 0, 1100 };
	constexpr int16_t S = 300;

	for (int f = 0; f < 6; f++) {
		const Face &face = CUBE_FACES[f];
		SVector c[4];
		for (int i = 0; i < 4; i++) {
			c[i].x = (int16_t)(face.corner[i][0] * S);
			c[i].y = (int16_t)(face.corner[i][1] * S);
			c[i].z = (int16_t)(face.corner[i][2] * S);
		}
		out.quads_submitted++;
		gte.v[0] = c[0];
		gte.v[1] = c[1];
		gte.v[2] = c[2];
		gte.rtpt();
		uint32_t flags = gte.flag;
		gte.nclip();
		const int32_t area0 = gte.mac[0];
		int32_t sx[4], sy[4];
		for (int i = 0; i < 3; i++) {
			sx[i] = gte.sx[i];
			sy[i] = gte.sy[i];
		}
		gte.v[0] = c[3];
		gte.rtps();
		flags |= gte.flag;
		sx[3] = gte.sx[2];
		sy[3] = gte.sy[2];
		gte.nclip(); // FIFO now holds TR, BL, BR: the second triangle, opposite winding.
		const int32_t area1 = -gte.mac[0];
		if (flags & FLAG_DIVIDE) {
			out.quads_culled++;
			continue; // Too near: dropped whole, PS1 style.
		}
		if (area0 <= 0 && area1 <= 0) {
			out.quads_culled++;
			continue; // Both triangles face away.
		}
		gte.avsz4();
		const uint32_t otz = gte.otz >> 1;
		if (otz < 2) {
			out.quads_culled++;
			continue;
		}

		PacketBuilder::Vert v[4];
		for (int i = 0; i < 4; i++) {
			v[i].x = sx[i];
			v[i].y = sy[i];
			v[i].u = (uint8_t)((i & 1) ? 63 : 0);
			v[i].v = (uint8_t)((i & 2) ? 63 : 0);
		}
		PacketBuilder::Flags fl;
		uint16_t clut = 0, page = page4;
		switch (f) {
			case 0: // 4-bit, flat modulated at 1.0.
				fl.textured = true;
				clut = clut4;
				page = page4;
				break;
			case 1: { // 8-bit, Gouraud-modulated.
				fl.textured = fl.shaded = true;
				clut = clut8;
				page = page8;
				const uint8_t cols[4][3] = { { 255, 128, 128 }, { 128, 255, 128 }, { 128, 128, 255 }, { 64, 64, 64 } };
				for (int i = 0; i < 4; i++) {
					v[i].r = cols[i][0];
					v[i].g = cols[i][1];
					v[i].b = cols[i][2];
				}
			} break;
			case 2: // 15-bit raw: cut-out hole, no modulation.
				fl.textured = fl.raw = true;
				page = page15;
				break;
			case 3: { // 4-bit, overbright Gouraud (above 80h brightens).
				fl.textured = fl.shaded = true;
				clut = clut4;
				page = page4;
				const uint8_t cols[4] = { 255, 128, 200, 60 };
				for (int i = 0; i < 4; i++) {
					v[i].r = v[i].g = v[i].b = cols[i];
				}
			} break;
			case 4: { // Untextured Gouraud: the dither pattern.
				fl.shaded = true;
				const uint8_t cols[4][3] = { { 255, 32, 32 }, { 32, 255, 32 }, { 32, 32, 255 }, { 255, 255, 32 } };
				for (int i = 0; i < 4; i++) {
					v[i].r = cols[i][0];
					v[i].g = cols[i][1];
					v[i].b = cols[i][2];
				}
			} break;
			default: // 15-bit modulated, semi-transparent additive.
				fl.textured = fl.semi = true;
				page = page15_add;
				break;
		}
		ot.add(otz, pb.polygon(v, 4, fl, clut, page));
	}

	// Front (bucket 0): HUD-style 2D on top. Drawn in this order: draw mode
	// with subtractive blend, a semi-transparent tile, a line frame, a sprite.
	const int32_t s = p_gpu.get_scale();
	ot.add(0, pb.rectangle(8 * s, 8 * s, 16, 16, 128, 128, 128, true, true, false, 0, 0, clut4));
	PacketBuilder::Vert l0, l1;
	l0.x = 4;
	l0.y = h - 5;
	l0.r = 255;
	l0.g = 255;
	l0.b = 0;
	l1.x = w - 5;
	l1.y = h - 5;
	l1.r = 0;
	l1.g = 255;
	l1.b = 255;
	ot.add(0, pb.line(l0, l1, true, false));
	PacketBuilder::Vert l2 = l1, l3 = l0;
	l2.y = 4;
	l3.y = 4;
	ot.add(0, pb.line(l1, l2, true, false));
	ot.add(0, pb.line(l3, l0, true, false));
	ot.add(0, pb.rectangle(w - 48 * s, 8 * s, 40 * s, 24 * s, 96, 96, 96, false, false, true));
	const uint32_t subtract_mode = p_gpu.get_encoder().draw_mode(GP0Encoder::texpage(TEX4_X, TEX4_Y, TEXTURE_4BIT, BLEND_SUBTRACT, true));
	ot.add(0, pb.raw_words(&subtract_mode, 1));

	out.packets_drawn = ot.draw(p_gpu);
	p_gpu.display.x = bx;
	p_gpu.display.y = by;
	p_gpu.display.width = w;
	p_gpu.display.height = h;
	return out;
}

} // namespace psx
