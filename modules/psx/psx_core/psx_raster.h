// PSX core: integer rasterizer. No Godot dependency, so it can be unit-tested
// on its own and ported line for line to GLSL for the compute path.
//
// Spike version: flat clear plus one Gouraud-shaded triangle, integer-only
// inner loop. Screen coordinates are whole pixels (PS1 style, no subpixel).

#pragma once

#include <cstdint>

namespace psx {

// 32-bit pixels laid out R, G, B, A in memory (RGBA8 on little-endian).
struct Framebuffer {
	int32_t width = 0;
	int32_t height = 0;
	uint32_t *pixels = nullptr;
};

struct Vertex {
	int32_t x = 0;
	int32_t y = 0;
	uint8_t r = 0;
	uint8_t g = 0;
	uint8_t b = 0;
};

inline uint32_t pack_rgba(uint32_t p_r, uint32_t p_g, uint32_t p_b) {
	return p_r | (p_g << 8) | (p_b << 16) | (0xFFu << 24);
}

inline void clear(Framebuffer &p_fb, uint32_t p_color) {
	const int32_t count = p_fb.width * p_fb.height;
	for (int32_t i = 0; i < count; i++) {
		p_fb.pixels[i] = p_color;
	}
}

// Twice the signed area of the triangle (a, b, p).
inline int32_t edge(const Vertex &p_a, const Vertex &p_b, int32_t p_x, int32_t p_y) {
	return (p_b.x - p_a.x) * (p_y - p_a.y) - (p_b.y - p_a.y) * (p_x - p_a.x);
}

// Fill rule for pixels exactly on an edge. A shared edge runs a->b in one
// triangle and b->a in its neighbour, and this test gives opposite answers for
// the two, so each such pixel is drawn by exactly one triangle.
// TODO: confirm the orientation matches PS1 GPU behaviour (top-left).
inline bool is_top_left(const Vertex &p_a, const Vertex &p_b) {
	const int32_t dx = p_b.x - p_a.x;
	const int32_t dy = p_b.y - p_a.y;
	return (dy == 0 && dx > 0) || dy < 0;
}

inline int32_t min3(int32_t p_a, int32_t p_b, int32_t p_c) {
	const int32_t m = p_a < p_b ? p_a : p_b;
	return m < p_c ? m : p_c;
}

inline int32_t max3(int32_t p_a, int32_t p_b, int32_t p_c) {
	const int32_t m = p_a > p_b ? p_a : p_b;
	return m > p_c ? m : p_c;
}

// Coordinate range: products in edge() stay within 32 bits while |x|, |y| <
// 2^15. Colour interpolation multiplies an edge value (< 2 * width * height)
// by 255, which fits 32 bits up to about 4M pixels per triangle.
inline void draw_triangle_gouraud(Framebuffer &p_fb, Vertex p_v0, Vertex p_v1, Vertex p_v2) {
	int32_t area = edge(p_v0, p_v1, p_v2.x, p_v2.y);
	if (area == 0) {
		return;
	}
	if (area < 0) {
		// Normalise to one winding so the inside test is always "all >= 0".
		const Vertex t = p_v1;
		p_v1 = p_v2;
		p_v2 = t;
		area = -area;
	}

	int32_t min_x = min3(p_v0.x, p_v1.x, p_v2.x);
	int32_t min_y = min3(p_v0.y, p_v1.y, p_v2.y);
	int32_t max_x = max3(p_v0.x, p_v1.x, p_v2.x);
	int32_t max_y = max3(p_v0.y, p_v1.y, p_v2.y);
	min_x = min_x < 0 ? 0 : min_x;
	min_y = min_y < 0 ? 0 : min_y;
	max_x = max_x > p_fb.width - 1 ? p_fb.width - 1 : max_x;
	max_y = max_y > p_fb.height - 1 ? p_fb.height - 1 : max_y;

	// Bias makes non-top-left edges exclusive.
	const int32_t bias0 = is_top_left(p_v1, p_v2) ? 0 : -1;
	const int32_t bias1 = is_top_left(p_v2, p_v0) ? 0 : -1;
	const int32_t bias2 = is_top_left(p_v0, p_v1) ? 0 : -1;

	for (int32_t y = min_y; y <= max_y; y++) {
		uint32_t *row = p_fb.pixels + y * p_fb.width;
		for (int32_t x = min_x; x <= max_x; x++) {
			const int32_t w0 = edge(p_v1, p_v2, x, y);
			const int32_t w1 = edge(p_v2, p_v0, x, y);
			const int32_t w2 = edge(p_v0, p_v1, x, y);
			if (w0 + bias0 < 0 || w1 + bias1 < 0 || w2 + bias2 < 0) {
				continue;
			}
			const uint32_t r = (uint32_t)(w0 * p_v0.r + w1 * p_v1.r + w2 * p_v2.r) / (uint32_t)area;
			const uint32_t g = (uint32_t)(w0 * p_v0.g + w1 * p_v1.g + w2 * p_v2.g) / (uint32_t)area;
			const uint32_t b = (uint32_t)(w0 * p_v0.b + w1 * p_v1.b + w2 * p_v2.b) / (uint32_t)area;
			row[x] = pack_rgba(r, g, b);
		}
	}
}

} // namespace psx
