// GPU model implementation. Algorithms follow the hardware behaviour documented
// in psx-spx (GPU chapter) and the design doc, sections 8-14; written for this
// project, not copied from any emulator.

#include "psx_gpu.h"

#include <cstring>
#include <utility>

namespace psx {

// --- GP0Encoder ---

uint32_t GP0Encoder::vertex(int32_t p_x, int32_t p_y) const {
	if (scale == 1) {
		return ((uint32_t)p_x & 0x7FFu) | (((uint32_t)p_y & 0x7FFu) << 16);
	}
	return ((uint32_t)p_x & 0xFFFFu) | (((uint32_t)p_y & 0xFFFFu) << 16);
}

uint32_t GP0Encoder::xy(int32_t p_x, int32_t p_y) const {
	return ((uint32_t)p_x & 0xFFFFu) | (((uint32_t)p_y & 0xFFFFu) << 16);
}

uint32_t GP0Encoder::draw_area_top_left(int32_t p_x, int32_t p_y) const {
	if (scale == 1) {
		return 0xE3000000u | ((uint32_t)p_x & 0x3FFu) | (((uint32_t)p_y & 0x3FFu) << 10);
	}
	return 0xE3000000u | ((uint32_t)p_x & 0xFFFu) | (((uint32_t)p_y & 0xFFFu) << 12);
}

uint32_t GP0Encoder::draw_area_bottom_right(int32_t p_x, int32_t p_y) const {
	if (scale == 1) {
		return 0xE4000000u | ((uint32_t)p_x & 0x3FFu) | (((uint32_t)p_y & 0x3FFu) << 10);
	}
	return 0xE4000000u | ((uint32_t)p_x & 0xFFFu) | (((uint32_t)p_y & 0xFFFu) << 12);
}

uint32_t GP0Encoder::draw_offset(int32_t p_x, int32_t p_y) const {
	if (scale == 1) {
		return 0xE5000000u | ((uint32_t)p_x & 0x7FFu) | (((uint32_t)p_y & 0x7FFu) << 11);
	}
	return 0xE5000000u | ((uint32_t)p_x & 0xFFFu) | (((uint32_t)p_y & 0xFFFu) << 12);
}

// --- GPU ---

// The 4x4 ordered dither offsets, indexed [y & 3][x & 3] (psx-spx).
static const int8_t DITHER_MATRIX[4][4] = {
	{ -4, +0, -3, +1 },
	{ +2, -2, +3, -1 },
	{ -3, +1, -4, +0 },
	{ +3, -1, +2, -2 },
};

// With dithering off, the lookup uses this entry, whose offset is 0.
static constexpr int NO_DITHER_Y = 2;
static constexpr int NO_DITHER_X = 3;

GPU::GPU(int p_scale) {
	scale = clamp_value(p_scale, 1, MAX_VRAM_SCALE);
	vram_width = VRAM_BASE_WIDTH * scale;
	vram_height = VRAM_BASE_HEIGHT * scale;
	max_width = 1024 * scale;
	max_height = 512 * scale;
	encoder = GP0Encoder(scale);
	for (int y = 0; y < 4; y++) {
		for (int x = 0; x < 4; x++) {
			for (int i = 0; i < 512; i++) {
				const int v = max_value(i + DITHER_MATRIX[y][x], 0) >> 3;
				dither_lut[y][x][i] = (uint8_t)min_value(v, 31);
			}
		}
	}
	reset();
}

void GPU::reset() {
	vram.assign((size_t)vram_width * vram_height, 0);
	banks.clear();
	readback.clear();
	state = DrawState();
	state.area_right = vram_width - 1;
	state.area_bottom = vram_height - 1;
	cmd_len = 0;
	cmd_need = 0;
	polyline_active = false;
	transfer_remaining = 0;
	stats.reset();
}

int32_t GPU::_wrap_coord(int32_t p_value) const {
	// Wraps into [-1024 * N, 1024 * N): the 11-bit sign extension at N = 1.
	const int32_t range = 2048 * scale;
	int32_t v = (p_value + 1024 * scale) % range;
	if (v < 0) {
		v += range;
	}
	return v - 1024 * scale;
}

int32_t GPU::_vertex_x(uint32_t p_word) const {
	return scale == 1 ? sign_extend(p_word, 11) : sign_extend(p_word & 0xFFFFu, 16);
}

int32_t GPU::_vertex_y(uint32_t p_word) const {
	return scale == 1 ? sign_extend(p_word >> 16, 11) : sign_extend(p_word >> 16, 16);
}

int GPU::_command_length(uint32_t p_word) const {
	const uint32_t op = p_word >> 24;
	switch (op >> 5) {
		case 0:
			return op == 0x02 ? 3 : 1;
		case 1: { // Polygon.
			const bool shaded = op & 0x10, quad = op & 0x08, textured = op & 0x04;
			const int n = quad ? 4 : 3;
			return 1 + n * (textured ? 2 : 1) + (shaded ? n - 1 : 0);
		}
		case 2: // Line; polylines continue past this.
			return (op & 0x10) ? 4 : 3;
		case 3: { // Rectangle.
			const bool textured = op & 0x04;
			const bool variable = ((op >> 3) & 3) == 0;
			return 2 + (textured ? 1 : 0) + (variable ? 1 : 0);
		}
		case 4:
			return 4; // VRAM -> VRAM.
		case 5:
		case 6:
			return 3; // CPU -> VRAM header, VRAM -> CPU.
		default:
			return 1; // Environment commands.
	}
}

void GPU::write_gp0(const uint32_t *p_words, size_t p_count) {
	for (size_t i = 0; i < p_count; i++) {
		write_gp0(p_words[i]);
	}
}

void GPU::write_gp0(uint32_t p_word) {
	if (transfer_remaining > 0) {
		_transfer_halfword((uint16_t)(p_word & 0xFFFFu));
		if (transfer_remaining > 0) {
			_transfer_halfword((uint16_t)(p_word >> 16));
		}
		return;
	}

	if (polyline_active) {
		const bool vertex_start = !polyline_shaded || polyline_want_color;
		if (vertex_start && (p_word & 0xF000F000u) == 0x50005000u) {
			polyline_active = false;
			return;
		}
		if (polyline_shaded && polyline_want_color) {
			polyline_next_color = p_word;
			polyline_want_color = false;
			return;
		}
		Vertex next = polyline_prev;
		next.x = _wrap_coord(state.offset_x + _vertex_x(p_word));
		next.y = _wrap_coord(state.offset_y + _vertex_y(p_word));
		if (polyline_shaded) {
			next.r = (uint8_t)polyline_next_color;
			next.g = (uint8_t)(polyline_next_color >> 8);
			next.b = (uint8_t)(polyline_next_color >> 16);
		}
		_draw_line(polyline_prev, next, polyline_shaded, polyline_semi);
		polyline_prev = next;
		polyline_want_color = polyline_shaded;
		return;
	}

	cmd[cmd_len++] = p_word;
	if (cmd_len == 1) {
		cmd_need = _command_length(p_word);
	}
	if (cmd_len >= cmd_need) {
		_execute();
		cmd_len = 0;
	}
}

void GPU::_execute() {
	stats.commands++;
	const uint32_t op = cmd[0] >> 24;
	switch (op >> 5) {
		case 0:
			if (op == 0x02) {
				_execute_fill();
			}
			return; // NOP, cache clear, IRQ: nothing to draw.
		case 1:
			_execute_polygon();
			return;
		case 2:
			_execute_line();
			return;
		case 3:
			_execute_rectangle();
			return;
		case 4:
		case 5:
		case 6:
			_execute_copy();
			return;
		default:
			break;
	}
	const uint32_t w = cmd[0];
	switch (op) {
		case 0xE1:
			_set_texpage((uint16_t)(w & 0x3FFFu), false);
			break;
		case 0xE2: {
			const uint32_t mask_x = w & 0x1F, mask_y = (w >> 5) & 0x1F;
			const uint32_t off_x = (w >> 10) & 0x1F, off_y = (w >> 15) & 0x1F;
			state.window_and_x = (uint8_t)~(mask_x * 8);
			state.window_and_y = (uint8_t)~(mask_y * 8);
			state.window_or_x = (uint8_t)((off_x & mask_x) * 8);
			state.window_or_y = (uint8_t)((off_y & mask_y) * 8);
		} break;
		case 0xE3:
		case 0xE4: {
			int32_t x, y;
			if (scale == 1) {
				x = (int32_t)(w & 0x3FF);
				y = (int32_t)((w >> 10) & 0x1FF);
			} else {
				x = (int32_t)(w & 0xFFF);
				y = (int32_t)((w >> 12) & 0xFFF);
			}
			x = min_value(x, vram_width - 1);
			y = min_value(y, vram_height - 1);
			if (op == 0xE3) {
				state.area_left = x;
				state.area_top = y;
			} else {
				state.area_right = x;
				state.area_bottom = y;
			}
		} break;
		case 0xE5:
			if (scale == 1) {
				state.offset_x = sign_extend(w, 11);
				state.offset_y = sign_extend(w >> 11, 11);
			} else {
				state.offset_x = sign_extend(w, 12);
				state.offset_y = sign_extend(w >> 12, 12);
			}
			break;
		case 0xE6:
			state.set_mask = (w & 1) != 0;
			state.check_mask = (w & 2) != 0;
			break;
		case 0xE7: {
			const uint32_t bank = w & 0xFFFF;
			state.texture_bank = (uint16_t)(bank > MAX_TEXTURE_BANKS ? 0 : bank);
			if (state.texture_bank > banks.size()) {
				banks.resize(state.texture_bank);
				for (std::vector<uint16_t> &b : banks) {
					if (b.empty()) {
						b.assign((size_t)VRAM_BASE_WIDTH * VRAM_BASE_HEIGHT, 0);
					}
				}
			}
		} break;
		default:
			break;
	}
}

void GPU::_set_texpage(uint16_t p_bits, bool p_from_polygon) {
	state.page_x = (p_bits & 0xF) * 64;
	state.page_y = ((p_bits >> 4) & 1) * 256;
	state.blend = (uint8_t)((p_bits >> 5) & 3);
	const uint8_t depth = (uint8_t)((p_bits >> 7) & 3);
	state.depth = depth == 3 ? TEXTURE_15BIT : depth; // 3 is reserved: 15-bit.
	if (!p_from_polygon) {
		// A polygon's texpage only carries bits 0-8 (and 11); dither,
		// draw-to-display and the rectangle flips come from E1h alone.
		state.dither = (p_bits >> 9) & 1;
		state.draw_to_display = (p_bits >> 10) & 1;
		state.flip_x = (p_bits >> 12) & 1;
		state.flip_y = (p_bits >> 13) & 1;
	}
}

void GPU::_execute_polygon() {
	const uint32_t op = cmd[0] >> 24;
	const bool shaded = op & 0x10, quad = op & 0x08, textured = op & 0x04, semi = op & 0x02, raw = op & 0x01;
	const int n = quad ? 4 : 3;
	Vertex v[4];
	uint16_t clut = 0, texpage = 0;
	int idx = 1;
	uint32_t color = cmd[0];
	for (int i = 0; i < n; i++) {
		if (shaded && i > 0) {
			color = cmd[idx++];
		}
		v[i].r = (uint8_t)color;
		v[i].g = (uint8_t)(color >> 8);
		v[i].b = (uint8_t)(color >> 16);
		const uint32_t xy = cmd[idx++];
		v[i].x = _wrap_coord(state.offset_x + _vertex_x(xy));
		v[i].y = _wrap_coord(state.offset_y + _vertex_y(xy));
		v[i].u = v[i].v = 0;
		if (textured) {
			const uint32_t t = cmd[idx++];
			v[i].u = (uint8_t)t;
			v[i].v = (uint8_t)(t >> 8);
			if (i == 0) {
				clut = (uint16_t)(t >> 16);
			} else if (i == 1) {
				texpage = (uint16_t)(t >> 16);
			}
		}
	}
	Texture tex = {};
	if (textured) {
		// A textured polygon writes its texpage into the global draw mode,
		// which later untextured semi-transparent primitives then use.
		_set_texpage(texpage, true);
		tex = _texture(clut);
	}
	_draw_triangle(v[0], v[1], v[2], shaded, textured, raw, semi, tex);
	if (quad) {
		_draw_triangle(v[1], v[2], v[3], shaded, textured, raw, semi, tex);
	}
}

void GPU::_draw_triangle(const Vertex &p_v0, const Vertex &p_v1, const Vertex &p_v2, bool p_shaded, bool p_textured, bool p_raw, bool p_semi, const Texture &p_tex) {
	const int32_t min_x = min_value(p_v0.x, min_value(p_v1.x, p_v2.x));
	const int32_t max_x = max_value(p_v0.x, max_value(p_v1.x, p_v2.x));
	const int32_t min_y = min_value(p_v0.y, min_value(p_v1.y, p_v2.y));
	const int32_t max_y = max_value(p_v0.y, max_value(p_v1.y, p_v2.y));
	if (max_x - min_x >= max_width || max_y - min_y >= max_height) {
		stats.rejected_triangles++;
		return; // The GPU silently refuses oversized primitives.
	}
	using Fn = void (GPU::*)(const Vertex *, const Vertex *, const Vertex *, const Texture &);
	static const Fn table[16] = {
		&GPU::_rasterize_triangle<false, false, false, false>,
		&GPU::_rasterize_triangle<false, false, false, true>,
		&GPU::_rasterize_triangle<false, false, true, false>,
		&GPU::_rasterize_triangle<false, false, true, true>,
		&GPU::_rasterize_triangle<false, true, false, false>,
		&GPU::_rasterize_triangle<false, true, false, true>,
		&GPU::_rasterize_triangle<false, true, true, false>,
		&GPU::_rasterize_triangle<false, true, true, true>,
		&GPU::_rasterize_triangle<true, false, false, false>,
		&GPU::_rasterize_triangle<true, false, false, true>,
		&GPU::_rasterize_triangle<true, false, true, false>,
		&GPU::_rasterize_triangle<true, false, true, true>,
		&GPU::_rasterize_triangle<true, true, false, false>,
		&GPU::_rasterize_triangle<true, true, false, true>,
		&GPU::_rasterize_triangle<true, true, true, false>,
		&GPU::_rasterize_triangle<true, true, true, true>,
	};
	const bool raw = p_textured && p_raw; // Raw only means something when textured.
	const int index = (p_shaded ? 8 : 0) | (p_textured ? 4 : 0) | (raw ? 2 : 0) | (p_semi ? 1 : 0);
	(this->*table[index])(&p_v0, &p_v1, &p_v2, p_tex);
}

// Triangle rasterization: integer vertices, scanline spans stepped in 32.32
// fixed point, half-open in X and Y, attributes from per-triangle plane
// equations with 24 fraction bits (design doc sections 9 and 10).
template <bool SHADED, bool TEXTURED, bool RAW, bool SEMI>
void GPU::_rasterize_triangle(const Vertex *p_v0, const Vertex *p_v1, const Vertex *p_v2, const Texture &p_tex) {
	const Vertex *v0 = p_v0, *v1 = p_v1, *v2 = p_v2;

	// Which input vertex is leftmost, as one bit per vertex; ties go to v1
	// over v0, v2 over v1, and v0 over v2. The bits follow the Y sort.
	uint32_t left_bits;
	if (v1->x <= v0->x) {
		left_bits = (v2->x <= v1->x) ? 4u : 2u;
	} else {
		left_bits = (v2->x < v0->x) ? 4u : 1u;
	}
	auto swap_bits = [](uint32_t p_bits, uint32_t p_a, uint32_t p_b) {
		const uint32_t rest = p_bits & ~(p_a | p_b);
		return rest | ((p_bits & p_a) ? p_b : 0u) | ((p_bits & p_b) ? p_a : 0u);
	};
	if (v2->y < v1->y) {
		std::swap(v1, v2);
		left_bits = swap_bits(left_bits, 2, 4);
	}
	if (v1->y < v0->y) {
		std::swap(v0, v1);
		left_bits = swap_bits(left_bits, 1, 2);
	}
	if (v2->y < v1->y) {
		std::swap(v1, v2);
		left_bits = swap_bits(left_bits, 2, 4);
	}
	const int core = (int)(left_bits >> 1); // Sorted index 0, 1 or 2.

	if (v0->y == v2->y) {
		stats.rejected_triangles++;
		return;
	}
	const int32_t d1x = v1->x - v0->x, d1y = v1->y - v0->y;
	const int32_t d2x = v2->x - v1->x, d2y = v2->y - v1->y;
	const int64_t det = (int64_t)d1x * d2y - (int64_t)d2x * d1y;
	if (det == 0) {
		stats.rejected_triangles++;
		return;
	}
	stats.triangles++;

	// Attribute planes. Each gradient is (numerator * 4096 / det) truncated
	// toward zero, then shifted to 24 fraction bits, in wrapping u32.
	auto gradient = [det](int64_t p_numerator) -> uint32_t {
		return (uint32_t)(p_numerator * 4096 / det) << 12;
	};
	auto plane = [&](int32_t a0, int32_t a1, int32_t a2, uint32_t &r_dx, uint32_t &r_dy) {
		const int32_t da1 = a1 - a0, da2 = a2 - a1;
		r_dx = gradient((int64_t)da1 * d2y - (int64_t)da2 * d1y);
		r_dy = gradient((int64_t)d1x * da2 - (int64_t)d2x * da1);
	};
	uint32_t dr_dx = 0, dr_dy = 0, dg_dx = 0, dg_dy = 0, db_dx = 0, db_dy = 0;
	uint32_t du_dx = 0, du_dy = 0, dv_dx = 0, dv_dy = 0;
	if constexpr (SHADED) {
		plane(v0->r, v1->r, v2->r, dr_dx, dr_dy);
		plane(v0->g, v1->g, v2->g, dg_dx, dg_dy);
		plane(v0->b, v1->b, v2->b, db_dx, db_dy);
	}
	if constexpr (TEXTURED) {
		plane(v0->u, v1->u, v2->u, du_dx, du_dy);
		plane(v0->v, v1->v, v2->v, dv_dx, dv_dy);
	}
	// Values at the core (leftmost) vertex, plus half an attribute unit.
	const Vertex *sorted[3] = { v0, v1, v2 };
	const Vertex &c = *sorted[core];
	auto init = [](uint8_t a) -> uint32_t { return (((uint32_t)a << 12) + (1u << 11)) << 12; };
	const uint32_t r0 = init(c.r), g0 = init(c.g), b0 = init(c.b), u0 = init(c.u), w0 = init(c.v);

	const bool dither = state.dither && (SHADED || (TEXTURED && !RAW));
	const int32_t left = state.area_left, right = state.area_right;
	const int32_t top = state.area_top, bottom = state.area_bottom;

	auto span = [&](int32_t y, int32_t x_start, int32_t x_bound) {
		int32_t x = x_start;
		int32_t width = x_bound - x_start;
		if (x < left) {
			width -= left - x;
			x = left;
		}
		if (x + width > right + 1) {
			width = right + 1 - x;
		}
		if (width <= 0) {
			return;
		}
		const uint32_t ox = (uint32_t)(x - c.x), oy = (uint32_t)(y - c.y);
		uint32_t r = r0 + dr_dx * ox + dr_dy * oy;
		uint32_t g = g0 + dg_dx * ox + dg_dy * oy;
		uint32_t b = b0 + db_dx * ox + db_dy * oy;
		uint32_t u = u0 + du_dx * ox + du_dy * oy;
		uint32_t v = w0 + dv_dx * ox + dv_dy * oy;
		for (; width > 0; width--, x++) {
			_shade_pixel<TEXTURED, RAW, SEMI>(x, y, (uint8_t)(r >> 24), (uint8_t)(g >> 24), (uint8_t)(b >> 24), (uint8_t)(u >> 24), (uint8_t)(v >> 24), dither, p_tex);
			r += dr_dx;
			g += dg_dx;
			b += db_dx;
			u += du_dx;
			v += dv_dx;
		}
	};

	// Edges in 32.32. The start bias (1 - 2^-21) and the step rounded away
	// from zero together act as the fill rule.
	auto make_fp = [](int32_t x) -> int64_t {
		return (int64_t)x * (INT64_C(1) << 32) + ((INT64_C(1) << 32) - (1 << 11));
	};
	auto make_step = [](int32_t dx, int32_t dy) -> int64_t {
		int64_t n = (int64_t)dx * (INT64_C(1) << 32);
		if (dx < 0) {
			n -= dy - 1;
		} else if (dx > 0) {
			n += dy - 1;
		}
		return n / dy;
	};
	const int64_t base_coord = make_fp(v0->x);
	const int64_t base_step = make_step(v2->x - v0->x, v2->y - v0->y);
	int64_t bound_upper = 0, bound_lower = 0;
	bool right_facing;
	if (v1->y == v0->y) {
		right_facing = v1->x > v0->x;
	} else {
		bound_upper = make_step(d1x, d1y);
		right_facing = bound_upper > base_step;
	}
	if (v2->y != v1->y) {
		bound_lower = make_step(d2x, d2y);
	}
	const int rfi = right_facing ? 1 : 0, ofi = 1 - rfi;

	// The two halves, split at v1. Halves are walked away from the core
	// vertex: bottom-up when the leftmost vertex is below the half's top.
	struct Half {
		int32_t start_y, end_y;
		bool upward;
		int64_t x[2], step[2];
	};
	Half halves[2];
	const int vo = core != 0 ? 1 : 0;
	const int vp = core == 2 ? 3 : 0;
	const Vertex *vs[4] = { v0, v1, v2, nullptr };
	Half &upper = halves[vo];
	upper.start_y = vs[0 ^ vo]->y;
	upper.end_y = vs[1 ^ vo]->y;
	upper.upward = vo != 0;
	upper.x[rfi] = make_fp(vs[0 ^ vo]->x);
	upper.step[rfi] = bound_upper;
	upper.x[ofi] = base_coord + (int64_t)(vs[vo]->y - v0->y) * base_step;
	upper.step[ofi] = base_step;
	Half &lower = halves[vo ^ 1];
	lower.start_y = vs[1 ^ vp]->y;
	lower.end_y = vs[2 ^ vp]->y;
	lower.upward = vp != 0;
	lower.x[rfi] = make_fp(vs[1 ^ vp]->x);
	lower.step[rfi] = bound_lower;
	lower.x[ofi] = base_coord + (int64_t)(vs[1 ^ vp]->y - v0->y) * base_step;
	lower.step[ofi] = base_step;

	for (int i = 0; i < 2; i++) {
		const Half &h = halves[i];
		int64_t xl = h.x[0], xr = h.x[1];
		int32_t y = h.start_y;
		if (!h.upward) {
			while (y < h.end_y) {
				if (y > bottom) {
					break;
				}
				if (y >= top && !_skip_row(y)) {
					span(y, (int32_t)(xl >> 32), (int32_t)(xr >> 32));
				}
				y++;
				xl += h.step[0];
				xr += h.step[1];
			}
		} else {
			while (y > h.end_y) {
				y--;
				xl -= h.step[0];
				xr -= h.step[1];
				if (y < top) {
					break;
				}
				if (y <= bottom && !_skip_row(y)) {
					span(y, (int32_t)(xl >> 32), (int32_t)(xr >> 32));
				}
			}
		}
	}
}

GPU::Texture GPU::_texture(uint16_t p_clut) const {
	Texture tex = {};
	tex.page_x = state.page_x;
	tex.page_y = state.page_y;
	tex.depth = state.depth;
	tex.clut_x = (p_clut & 0x3F) * 16;
	tex.clut_y = (p_clut >> 6) & 0x1FF;
	if (state.texture_bank > 0 && state.texture_bank <= banks.size()) {
		tex.base = banks[state.texture_bank - 1].data();
		tex.stride = VRAM_BASE_WIDTH;
	} else {
		tex.base = vram.data();
		tex.stride = vram_width;
	}
	return tex;
}

uint16_t GPU::_fetch_texel(uint8_t p_u, uint8_t p_v, const Texture &p_tex) const {
	const uint32_t u = (p_u & state.window_and_x) | state.window_or_x;
	const uint32_t v = (p_v & state.window_and_y) | state.window_or_y;
	// Textures and CLUTs live in the 1024x512 texture region (of VRAM or of
	// the selected texture bank) and wrap there.
	const uint16_t *mem = p_tex.base;
	const size_t row = (size_t)((p_tex.page_y + (int32_t)v) & 511) * p_tex.stride;
	const size_t clut_row = (size_t)p_tex.clut_y * p_tex.stride;
	switch (p_tex.depth) {
		case TEXTURE_4BIT: {
			const uint16_t word = mem[row + ((p_tex.page_x + (int32_t)(u >> 2)) & 1023)];
			const uint32_t index = (word >> ((u & 3) * 4)) & 0xF;
			return mem[clut_row + ((p_tex.clut_x + index) & 1023)];
		}
		case TEXTURE_8BIT: {
			const uint16_t word = mem[row + ((p_tex.page_x + (int32_t)(u >> 1)) & 1023)];
			const uint32_t index = (word >> ((u & 1) * 8)) & 0xFF;
			return mem[clut_row + ((p_tex.clut_x + index) & 1023)];
		}
		default:
			return mem[row + ((p_tex.page_x + (int32_t)u) & 1023)];
	}
}

uint16_t GPU::bank_get(uint32_t p_bank, int32_t p_x, int32_t p_y) const {
	if (p_bank == 0) {
		return vram_get(p_x, p_y);
	}
	if (p_bank > banks.size()) {
		return 0;
	}
	return banks[p_bank - 1][(size_t)(p_y & 511) * VRAM_BASE_WIDTH + (p_x & 1023)];
}

uint16_t *GPU::_bank_memory(int32_t &r_stride, int32_t &r_width, int32_t &r_height) {
	if (state.texture_bank > 0 && state.texture_bank <= banks.size()) {
		r_stride = r_width = VRAM_BASE_WIDTH;
		r_height = VRAM_BASE_HEIGHT;
		return banks[state.texture_bank - 1].data();
	}
	r_stride = r_width = vram_width;
	r_height = vram_height;
	return vram.data();
}

uint16_t GPU::_blend(uint16_t p_back, uint16_t p_front) const {
	const int32_t br = p_back & 31, bg = (p_back >> 5) & 31, bb = (p_back >> 10) & 31;
	const int32_t fr = p_front & 31, fg = (p_front >> 5) & 31, fb = (p_front >> 10) & 31;
	int32_t r, g, b;
	switch (state.blend) {
		case BLEND_HALF:
			r = (br + fr) >> 1;
			g = (bg + fg) >> 1;
			b = (bb + fb) >> 1;
			break;
		case BLEND_ADD:
			r = min_value(br + fr, 31);
			g = min_value(bg + fg, 31);
			b = min_value(bb + fb, 31);
			break;
		case BLEND_SUBTRACT:
			r = max_value(br - fr, 0);
			g = max_value(bg - fg, 0);
			b = max_value(bb - fb, 0);
			break;
		default: // BLEND_ADD_QUARTER
			r = min_value(br + (fr >> 2), 31);
			g = min_value(bg + (fg >> 2), 31);
			b = min_value(bb + (fb >> 2), 31);
			break;
	}
	return rgb15((uint32_t)r, (uint32_t)g, (uint32_t)b);
}

// Per-pixel pipeline: fetch, modulate (0x80 = 1.0), dither + quantise to
// 5 bits, mask test, blend, write (design doc section 12).
template <bool TEXTURED, bool RAW, bool SEMI>
inline void GPU::_shade_pixel(int32_t p_x, int32_t p_y, uint8_t p_r, uint8_t p_g, uint8_t p_b, uint8_t p_u, uint8_t p_v, bool p_dither, const Texture &p_tex) {
	const uint8_t *lut = p_dither ? dither_lut[p_y & 3][p_x & 3] : dither_lut[NO_DITHER_Y][NO_DITHER_X];
	uint16_t color;
	uint16_t texel = 0;
	if constexpr (TEXTURED) {
		texel = _fetch_texel(p_u, p_v, p_tex);
		if (texel == 0) {
			return; // 0000h is always transparent.
		}
		if constexpr (RAW) {
			color = texel;
		} else {
			color = (uint16_t)(rgb15(lut[((texel & 31u) * p_r) >> 4], lut[(((texel >> 5) & 31u) * p_g) >> 4], lut[(((texel >> 10) & 31u) * p_b) >> 4]) | (texel & 0x8000u));
		}
	} else {
		color = rgb15(lut[p_r], lut[p_g], lut[p_b]);
	}
	uint16_t &dst = vram[(size_t)p_y * vram_width + p_x];
	if (state.check_mask && (dst & 0x8000u)) {
		return;
	}
	if constexpr (SEMI) {
		// Untextured always blends; textured only where texel bit 15 is set.
		const bool blends = !TEXTURED || (texel & 0x8000u) != 0;
		if (blends) {
			color = (uint16_t)(_blend(dst, color) | (TEXTURED ? 0x8000u : 0u));
		}
	}
	dst = (uint16_t)(color | (state.set_mask ? 0x8000u : 0u));
}

void GPU::_execute_line() {
	const uint32_t op = cmd[0] >> 24;
	const bool shaded = op & 0x10, poly = op & 0x08, semi = op & 0x02;
	Vertex a = {}, b = {};
	a.r = (uint8_t)cmd[0];
	a.g = (uint8_t)(cmd[0] >> 8);
	a.b = (uint8_t)(cmd[0] >> 16);
	a.x = _wrap_coord(state.offset_x + _vertex_x(cmd[1]));
	a.y = _wrap_coord(state.offset_y + _vertex_y(cmd[1]));
	b = a;
	uint32_t xy = cmd[2];
	if (shaded) {
		b.r = (uint8_t)cmd[2];
		b.g = (uint8_t)(cmd[2] >> 8);
		b.b = (uint8_t)(cmd[2] >> 16);
		xy = cmd[3];
	}
	b.x = _wrap_coord(state.offset_x + _vertex_x(xy));
	b.y = _wrap_coord(state.offset_y + _vertex_y(xy));
	_draw_line(a, b, shaded, semi);
	if (poly) {
		polyline_active = true;
		polyline_shaded = shaded;
		polyline_semi = semi;
		polyline_want_color = shaded;
		polyline_prev = b;
	}
}

// Lines: a DDA over k = max(|dx|, |dy|) steps, both endpoints drawn, walked
// left to right; X/Y in 32.32, colour in 12 fraction bits (section 13).
void GPU::_draw_line(const Vertex &p_a, const Vertex &p_b, bool p_shaded, bool p_semi) {
	Vertex p0 = p_a, p1 = p_b;
	const int32_t adx = p1.x > p0.x ? p1.x - p0.x : p0.x - p1.x;
	const int32_t ady = p1.y > p0.y ? p1.y - p0.y : p0.y - p1.y;
	if (adx >= max_width || ady >= max_height) {
		return;
	}
	const int32_t k = max_value(adx, ady);
	if (p0.x >= p1.x && k > 0) {
		std::swap(p0, p1);
	}
	auto div_xy = [](int32_t d, int32_t steps) -> int64_t {
		int64_t n = (int64_t)d * (INT64_C(1) << 32);
		if (d < 0) {
			n -= steps - 1;
		} else if (d > 0) {
			n += steps - 1;
		}
		return n / steps;
	};
	int64_t dx_dk = 0, dy_dk = 0;
	int32_t dr_dk = 0, dg_dk = 0, db_dk = 0;
	if (k > 0) {
		dx_dk = div_xy(p1.x - p0.x, k);
		dy_dk = div_xy(p1.y - p0.y, k);
		if (p_shaded) {
			dr_dk = ((int32_t)p1.r - p0.r) * 4096 / k;
			dg_dk = ((int32_t)p1.g - p0.g) * 4096 / k;
			db_dk = ((int32_t)p1.b - p0.b) * 4096 / k;
		}
	}
	int64_t x = (int64_t)p0.x * (INT64_C(1) << 32) + (INT64_C(1) << 31) - 1024;
	int64_t y = (int64_t)p0.y * (INT64_C(1) << 32) + (INT64_C(1) << 31) - (dy_dk < 0 ? 1024 : 0);
	int32_t r = ((int32_t)p0.r << 12) + (1 << 11);
	int32_t g = ((int32_t)p0.g << 12) + (1 << 11);
	int32_t b = ((int32_t)p0.b << 12) + (1 << 11);
	const Texture no_tex = {};
	const bool dither = state.dither;
	stats.lines++;
	for (int32_t i = 0; i <= k; i++) {
		const int32_t px = (int32_t)(x >> 32), py = (int32_t)(y >> 32);
		if (px >= state.area_left && px <= state.area_right && py >= state.area_top && py <= state.area_bottom && !_skip_row(py)) {
			const uint8_t cr = (uint8_t)(r >> 12), cg = (uint8_t)(g >> 12), cb = (uint8_t)(b >> 12);
			if (p_semi) {
				_shade_pixel<false, false, true>(px, py, cr, cg, cb, 0, 0, dither, no_tex);
			} else {
				_shade_pixel<false, false, false>(px, py, cr, cg, cb, 0, 0, dither, no_tex);
			}
		}
		x += dx_dk;
		y += dy_dk;
		r += dr_dk;
		g += dg_dk;
		b += db_dk;
	}
}

// Rectangles (sprites and tiles): axis-aligned, one texel per pixel, no
// Gouraud and never dithered (section 13).
void GPU::_execute_rectangle() {
	const uint32_t op = cmd[0] >> 24;
	const bool textured = op & 0x04, semi = op & 0x02, raw = op & 0x01;
	const uint32_t size_code = (op >> 3) & 3;
	int idx = 1;
	const uint32_t xy = cmd[idx++];
	uint8_t u0 = 0, v0 = 0;
	Texture tex = {};
	if (textured) {
		const uint32_t t = cmd[idx++];
		u0 = (uint8_t)t;
		v0 = (uint8_t)(t >> 8);
		const uint16_t clut = (uint16_t)(t >> 16);
		tex = _texture(clut);
	}
	int32_t w, h;
	switch (size_code) {
		case 1:
			w = h = 1;
			break;
		case 2:
			w = h = 8;
			break;
		case 3:
			w = h = 16;
			break;
		default: {
			const uint32_t s = cmd[idx++];
			if (scale == 1) {
				w = (int32_t)(s & 0x3FF);
				h = (int32_t)((s >> 16) & 0x1FF);
			} else {
				w = min_value((int32_t)(s & 0xFFFF), max_width - 1);
				h = min_value((int32_t)(s >> 16), max_height - 1);
			}
		} break;
	}
	const int32_t x0 = _wrap_coord(state.offset_x + _vertex_x(xy));
	const int32_t y0 = _wrap_coord(state.offset_y + _vertex_y(xy));
	const uint8_t r = (uint8_t)cmd[0], g = (uint8_t)(cmd[0] >> 8), b = (uint8_t)(cmd[0] >> 16);
	stats.rectangles++;

	const int32_t row_begin = max_value(y0, state.area_top), row_end = min_value(y0 + h - 1, state.area_bottom);
	const int32_t col_begin = max_value(x0, state.area_left), col_end = min_value(x0 + w - 1, state.area_right);
	for (int32_t y = row_begin; y <= row_end; y++) {
		if (_skip_row(y)) {
			continue;
		}
		const int32_t dy = y - y0;
		const uint8_t v = (uint8_t)(state.flip_y ? v0 - dy : v0 + dy);
		for (int32_t x = col_begin; x <= col_end; x++) {
			const int32_t dx = x - x0;
			const uint8_t u = (uint8_t)(state.flip_x ? u0 - dx : u0 + dx);
			if (!textured) {
				if (semi) {
					_shade_pixel<false, false, true>(x, y, r, g, b, 0, 0, false, tex);
				} else {
					_shade_pixel<false, false, false>(x, y, r, g, b, 0, 0, false, tex);
				}
			} else if (raw) {
				if (semi) {
					_shade_pixel<true, true, true>(x, y, r, g, b, u, v, false, tex);
				} else {
					_shade_pixel<true, true, false>(x, y, r, g, b, u, v, false, tex);
				}
			} else {
				if (semi) {
					_shade_pixel<true, false, true>(x, y, r, g, b, u, v, false, tex);
				} else {
					_shade_pixel<true, false, false>(x, y, r, g, b, u, v, false, tex);
				}
			}
		}
	}
}

// Fill (GP0 02h): absolute VRAM coordinates, X in 16-pixel steps, ignores the
// drawing area and the mask settings, wraps at VRAM edges.
void GPU::_execute_fill() {
	const uint32_t c = cmd[0];
	const uint16_t color = rgb15((c & 0xFF) >> 3, ((c >> 8) & 0xFF) >> 3, ((c >> 16) & 0xFF) >> 3);
	int32_t x, y, w, h;
	if (scale == 1) {
		x = (int32_t)(cmd[1] & 0x3F0);
		y = (int32_t)((cmd[1] >> 16) & 0x1FF);
		w = (int32_t)(((cmd[2] & 0x3FF) + 0xF) & ~0xFu);
		h = (int32_t)((cmd[2] >> 16) & 0x1FF);
	} else {
		x = (int32_t)((cmd[1] & 0xFFFF) % (uint32_t)vram_width) & ~0xF;
		y = (int32_t)((cmd[1] >> 16) % (uint32_t)vram_height);
		w = (min_value((int32_t)(cmd[2] & 0xFFFF), vram_width) + 0xF) & ~0xF;
		h = min_value((int32_t)(cmd[2] >> 16), vram_height);
	}
	stats.fills++;
	for (int32_t row = 0; row < h; row++) {
		uint16_t *line = &vram[(size_t)((y + row) % vram_height) * vram_width];
		for (int32_t col = 0; col < w; col++) {
			line[(x + col) % vram_width] = color;
		}
	}
}

void GPU::_execute_copy() {
	const uint32_t op = cmd[0] >> 24;
	auto pos = [this](uint32_t p_word, int32_t &r_x, int32_t &r_y) {
		if (scale == 1) {
			r_x = (int32_t)(p_word & 0x3FF);
			r_y = (int32_t)((p_word >> 16) & 0x1FF);
		} else {
			r_x = (int32_t)((p_word & 0xFFFF) % (uint32_t)vram_width);
			r_y = (int32_t)((p_word >> 16) % (uint32_t)vram_height);
		}
	};
	auto size = [this](uint32_t p_word, int32_t &r_w, int32_t &r_h) {
		// 0 means the full width/height.
		const int32_t w = (int32_t)(p_word & 0xFFFF), h = (int32_t)(p_word >> 16);
		r_w = ((w - 1 + vram_width) % vram_width) + 1;
		r_h = ((h - 1 + vram_height) % vram_height) + 1;
		if (scale == 1) {
			r_w = (int32_t)(((p_word & 0xFFFF) - 1) & 0x3FF) + 1;
			r_h = (int32_t)((((p_word >> 16) & 0xFFFF) - 1) & 0x1FF) + 1;
		}
	};
	const uint16_t mask_or = state.set_mask ? 0x8000 : 0;
	if ((op >> 5) == 4) { // VRAM -> VRAM.
		int32_t sx, sy, dx, dy, w, h;
		pos(cmd[1], sx, sy);
		pos(cmd[2], dx, dy);
		size(cmd[3], w, h);
		for (int32_t row = 0; row < h; row++) {
			for (int32_t col = 0; col < w; col++) {
				const uint16_t src = vram[(size_t)((sy + row) % vram_height) * vram_width + (sx + col) % vram_width];
				uint16_t &dst = vram[(size_t)((dy + row) % vram_height) * vram_width + (dx + col) % vram_width];
				if (state.check_mask && (dst & 0x8000)) {
					continue;
				}
				dst = (uint16_t)(src | mask_or);
			}
		}
	} else if ((op >> 5) == 5) { // CPU -> VRAM: data words follow.
		pos(cmd[1], transfer_x, transfer_y);
		size(cmd[2], transfer_w, transfer_h);
		transfer_col = transfer_row = 0;
		transfer_remaining = (uint32_t)transfer_w * (uint32_t)transfer_h;
	} else { // VRAM -> CPU (from the selected texture bank).
		int32_t sx, sy, w, h;
		pos(cmd[1], sx, sy);
		size(cmd[2], w, h);
		int32_t stride, bw, bh;
		const uint16_t *mem = _bank_memory(stride, bw, bh);
		for (int32_t row = 0; row < h; row++) {
			for (int32_t col = 0; col < w; col++) {
				readback.push_back(mem[(size_t)((sy + row) % bh) * stride + (sx + col) % bw]);
			}
		}
	}
}

void GPU::_transfer_halfword(uint16_t p_value) {
	// Into the selected texture bank (VRAM itself for bank 0).
	int32_t stride, bw, bh;
	uint16_t *mem = _bank_memory(stride, bw, bh);
	uint16_t &dst = mem[(size_t)((transfer_y + transfer_row) % bh) * stride + (transfer_x + transfer_col) % bw];
	if (!(state.check_mask && (dst & 0x8000))) {
		dst = (uint16_t)(p_value | (state.set_mask ? 0x8000 : 0));
	}
	transfer_remaining--;
	if (++transfer_col == transfer_w) {
		transfer_col = 0;
		transfer_row++;
	}
}

void GPU::scanout_rgba8(uint32_t *r_pixels) const {
	for (int32_t y = 0; y < display.height; y++) {
		const uint16_t *line = &vram[(size_t)((display.y + y) % vram_height) * vram_width];
		uint32_t *out = r_pixels + (size_t)y * display.width;
		if (!display.rgb24) {
			for (int32_t x = 0; x < display.width; x++) {
				out[x] = rgba8_from_rgb15(line[(display.x + x) % vram_width]);
			}
		} else {
			// 24-bit: three bytes per pixel packed across halfwords.
			for (int32_t x = 0; x < display.width; x++) {
				uint32_t bytes[3];
				for (int i = 0; i < 3; i++) {
					const int32_t byte_index = x * 3 + i;
					const uint16_t hw = line[(display.x + byte_index / 2) % vram_width];
					bytes[i] = (byte_index & 1) ? (hw >> 8) : (hw & 0xFF);
				}
				out[x] = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | 0xFF000000u;
			}
		}
	}
}

} // namespace psx
