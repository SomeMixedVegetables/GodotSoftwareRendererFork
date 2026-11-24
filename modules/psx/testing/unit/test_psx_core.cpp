// Standalone unit tests for psx_core (GPU, GTE, trig, ordering table, demo).
// No Godot dependency: built and run by `psx_test.ps1 unit` with MSVC.
//
//   test_psx_core.exe                 run all tests; exit code 0 = all passed
//   test_psx_core.exe --dump <dir>    also save demo frames as PNG for review
//
// Each failure prints file:line and the expression.

#define _CRT_SECURE_NO_WARNINGS // std::fopen in the PNG writer

#include "../../psx_core/psx_frame.h"
#include "../../psx_core/psx_gpu.h"
#include "../../psx_core/psx_gte.h"
#include "../../psx_core/psx_ot.h"
#include "../../psx_core/psx_trig.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace psx;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(expr)                                                         \
	do {                                                                    \
		g_checks++;                                                         \
		if (!(expr)) {                                                      \
			g_failures++;                                                   \
			std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
		}                                                                   \
	} while (0)

#define CHECK_EQ(a, b)                                                                                    \
	do {                                                                                                  \
		g_checks++;                                                                                       \
		const long long va_ = (long long)(a), vb_ = (long long)(b);                                       \
		if (va_ != vb_) {                                                                                 \
			g_failures++;                                                                                 \
			std::printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
		}                                                                                                 \
	} while (0)

// --- helpers ---

static void send(GPU &g, std::initializer_list<uint32_t> words) {
	for (uint32_t w : words) {
		g.write_gp0(w);
	}
}

static uint32_t col(uint8_t r, uint8_t g, uint8_t b) {
	return rgb24(r, g, b);
}

static int count_nonzero(const GPU &g, int x0, int y0, int x1, int y1) {
	int n = 0;
	for (int y = y0; y < y1; y++) {
		for (int x = x0; x < x1; x++) {
			n += g.vram_get(x, y) != 0;
		}
	}
	return n;
}

static void flat_tri(GPU &g, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c, bool semi = false) {
	const GP0Encoder &e = g.get_encoder();
	send(g, { ((semi ? 0x22u : 0x20u) << 24) | c, e.vertex(x0, y0), e.vertex(x1, y1), e.vertex(x2, y2) });
}

static void upload(GPU &g, int x, int y, int w, int h, const std::vector<uint16_t> &data) {
	const GP0Encoder &e = g.get_encoder();
	send(g, { 0xA0000000u, e.xy(x, y), e.xy(w, h) });
	for (size_t i = 0; i < data.size(); i += 2) {
		g.write_gp0(data[i] | ((uint32_t)(i + 1 < data.size() ? data[i + 1] : 0) << 16));
	}
}

static const int DITHER[4][4] = { { -4, 0, -3, 1 }, { 2, -2, 3, -1 }, { -3, 1, -4, 0 }, { 3, -1, 2, -2 } };

static int dithered(int c, int x, int y) {
	int v = c + DITHER[y & 3][x & 3];
	v = v < 0 ? 0 : v;
	v >>= 3;
	return v > 31 ? 31 : v;
}

// --- trig ---

static void test_trig() {
	CHECK_EQ(rsin(0), 0);
	CHECK_EQ(rsin(1024), 4096);
	CHECK_EQ(rsin(2048), 0);
	CHECK_EQ(rsin(3072), -4096);
	CHECK_EQ(rcos(0), 4096);
	CHECK_EQ(rcos(2048), -4096);
	CHECK_EQ(rsin(512), 2896); // sin 45 = 0.70711
	bool periodic = true, odd = true;
	for (int a = 0; a < 4096; a++) {
		periodic = periodic && rsin(a) == rsin(a + 4096) && rsin(a) == rsin(a - 4096);
		odd = odd && rsin(-a) == -rsin(a);
	}
	CHECK(periodic);
	CHECK(odd);
}

// --- GTE ---

static void test_unr_divide() {
	bool overflow = false;
	CHECK_EQ(GTE::unr_divide(256, 512, overflow), 0x8000);
	CHECK(!overflow);
	CHECK_EQ(GTE::unr_divide(256, 128, overflow), 0x1FFFF); // SZ = H/2: overflow
	CHECK(overflow);
	GTE::unr_divide(256, 129, overflow);
	CHECK(!overflow);
	GTE::unr_divide(256, 0, overflow);
	CHECK(overflow);
	// Hardware UNR differs from the ideal quotient only in the low bits.
	int max_diff = 0;
	const uint16_t hs[] = { 1, 100, 256, 300, 1000, 4096, 0x7FFF, 0xFFFF };
	for (uint16_t hv : hs) {
		for (uint32_t s = 1; s <= 0xFFFF; s++) {
			const uint32_t q = GTE::unr_divide(hv, (uint16_t)s, overflow);
			if (overflow) {
				continue;
			}
			const int d = (int)q - (int)GTE::ideal_divide(hv, (uint16_t)s);
			max_diff = (d < 0 ? -d : d) > max_diff ? (d < 0 ? -d : d) : max_diff;
		}
	}
	std::printf("  info: max |UNR - ideal| = %d\n", max_diff);
	CHECK(max_diff <= 2);
}

static GTE basic_gte() {
	GTE g;
	g.rotation = Matrix::identity();
	g.translation = { 0, 0, 0 };
	g.h = 256;
	g.ofx = 160 << 16;
	g.ofy = 120 << 16;
	return g;
}

static void test_rtps() {
	GTE g = basic_gte();
	g.v[0] = { 100, 50, 512 };
	g.rtps();
	CHECK_EQ(g.sx[2], 210);
	CHECK_EQ(g.sy[2], 145);
	CHECK_EQ(g.sz[3], 512);
	CHECK_EQ(g.ir[1], 100);
	CHECK_EQ(g.ir[3], 512);
	CHECK_EQ(g.flag, 0);

	// Translation and rotation: 90 degrees about Y maps +Z to +X.
	g.rotation = rot_matrix(0, 1024, 0);
	g.translation = { 0, 0, 1000 };
	g.v[0] = { 0, 0, 200 };
	g.rtps();
	CHECK_EQ(g.ir[1], 200);
	CHECK_EQ(g.sz[3], 1000);

	// Screen X saturates at 1023 (scale 1) and sets SX and the error bit.
	g = basic_gte();
	g.v[0] = { 30000, 0, 300 };
	g.rtps();
	CHECK_EQ(g.sx[2], 1023);
	CHECK(g.flag & FLAG_SX_SAT);
	CHECK(g.flag & FLAG_ERROR);
	// Scale 2 doubles the range.
	g.scale = 2;
	g.rtps();
	CHECK_EQ(g.sx[2], 2047);

	// Behind the camera: SZ clamps to 0, divide overflows.
	g = basic_gte();
	g.v[0] = { 0, 0, -50 };
	g.rtps();
	CHECK_EQ(g.sz[3], 0);
	CHECK(g.flag & FLAG_SZ_SAT);
	CHECK(g.flag & FLAG_DIVIDE);

	// Nearer than H/2 overflows the divide but is not saturated.
	g = basic_gte();
	g.v[0] = { 10, 10, 100 };
	g.rtps();
	CHECK(g.flag & FLAG_DIVIDE);
	CHECK(!(g.flag & FLAG_SZ_SAT));

	// RTPT pushes three vertices through the FIFOs.
	g = basic_gte();
	g.v[0] = { 0, 0, 256 };
	g.v[1] = { 256, 0, 256 };
	g.v[2] = { 0, 256, 512 };
	g.rtpt();
	CHECK_EQ(g.sx[0], 160);
	CHECK_EQ(g.sx[1], 416);
	CHECK_EQ(g.sy[2], 248);
	CHECK_EQ(g.sz[1], 256);
	CHECK_EQ(g.sz[3], 512);

	// Snap grid (non-hardware option).
	g = basic_gte();
	g.xy_snap = 4;
	g.v[0] = { 100, 50, 512 };
	g.rtps();
	CHECK_EQ(g.sx[2], 208);
	CHECK_EQ(g.sy[2], 144);
}

static void test_nclip_avsz() {
	GTE g;
	g.sx[0] = 0;
	g.sy[0] = 0;
	g.sx[1] = 10;
	g.sy[1] = 0;
	g.sx[2] = 0;
	g.sy[2] = 10; // TL, TR, BL on screen: positive.
	g.nclip();
	CHECK_EQ(g.mac[0], 100);
	std::swap(g.sx[1], g.sx[2]);
	std::swap(g.sy[1], g.sy[2]);
	g.nclip();
	CHECK_EQ(g.mac[0], -100);
	g.sx[2] = 20;
	g.sy[2] = 0; // Collinear.
	g.sx[1] = 10;
	g.sy[1] = 0;
	g.nclip();
	CHECK_EQ(g.mac[0], 0);

	g.sz[0] = 100;
	g.sz[1] = 300;
	g.sz[2] = 600;
	g.sz[3] = 900;
	g.avsz3();
	CHECK_EQ(g.otz, (1365 * 1800) >> 12);
	g.avsz4();
	CHECK_EQ(g.otz, 475);
}

// --- GPU: coverage ---

static void test_half_open_coverage() {
	GPU g;
	flat_tri(g, 0, 0, 10, 0, 0, 10, col(255, 255, 255));
	CHECK(g.vram_get(0, 0) != 0);
	bool touched_edge = false;
	for (int i = 0; i <= 10; i++) {
		touched_edge = touched_edge || g.vram_get(10, i) || g.vram_get(i, 10);
	}
	CHECK(!touched_edge);

	// A 10x10 quad covers exactly 100 pixels.
	GPU q;
	const GP0Encoder &e = q.get_encoder();
	send(q, { 0x28000000u | col(255, 255, 255), e.vertex(4, 4), e.vertex(14, 4), e.vertex(4, 14), e.vertex(14, 14) });
	CHECK_EQ(count_nonzero(q, 0, 0, 32, 32), 100);
}

// Every pixel of a triangle fan is written exactly once: drawn with additive
// blending of red 1, any pixel drawn twice would read 2.
static void test_fill_rule_fan() {
	GPU g;
	send(g, { 0xE1000000u | (BLEND_ADD << 5) });
	const int cx = 64, cy = 64, n = 13;
	int px[n + 1], py[n + 1];
	for (int i = 0; i <= n; i++) {
		const int a = i * 4096 / n + 37;
		px[i] = cx + (rcos(a) * 50) / 4096;
		py[i] = cy + (rsin(a) * 47) / 4096;
	}
	for (int i = 0; i < n; i++) {
		flat_tri(g, cx, cy, px[i], py[i], px[i + 1], py[i + 1], col(8, 0, 0), true);
	}
	int ones = 0, more = 0;
	for (int y = 0; y < 128; y++) {
		for (int x = 0; x < 128; x++) {
			const int r = g.vram_get(x, y) & 31;
			ones += r == 1;
			more += r > 1;
		}
	}
	CHECK(ones > 6000);
	CHECK_EQ(more, 0);
}

static void test_size_limits() {
	GPU g;
	flat_tri(g, 0, 0, 1023, 5, 0, 10, col(255, 0, 0));
	CHECK_EQ(g.stats.triangles, 1);
	flat_tri(g, -1, 20, 1023, 25, 0, 30, col(255, 0, 0)); // 1024 wide
	CHECK_EQ(g.stats.rejected_triangles, 1);
	flat_tri(g, 0, 0, 10, 511, 20, 0, col(255, 0, 0));
	CHECK_EQ(g.stats.triangles, 2);
	flat_tri(g, 0, 0, 10, 512, 20, 0, col(255, 0, 0)); // 512 tall
	CHECK_EQ(g.stats.rejected_triangles, 2);
	flat_tri(g, 0, 0, 10, 0, 20, 0, col(255, 0, 0)); // zero height
	CHECK_EQ(g.stats.rejected_triangles, 3);

	// Scale 2: limits double, vertices beyond 1023 are representable.
	GPU s(2);
	CHECK_EQ(s.get_vram_width(), 2048);
	CHECK_EQ(s.get_vram_height(), 1024);
	flat_tri(s, 10, 10, 1510, 20, 10, 700, col(255, 0, 0));
	CHECK_EQ(s.stats.triangles, 1);
	CHECK(s.vram_get(1400, 20) != 0);
	CHECK(s.vram_get(20, 650) != 0);
}

static void test_drawing_area_and_offset() {
	GPU g;
	const GP0Encoder &e = g.get_encoder();
	send(g, { e.draw_area_top_left(10, 10), e.draw_area_bottom_right(20, 20) });
	flat_tri(g, 0, 0, 100, 0, 0, 100, col(255, 255, 255));
	CHECK_EQ(count_nonzero(g, 0, 0, 128, 128), 121); // 11 x 11, inclusive
	CHECK(g.vram_get(10, 10) != 0 && g.vram_get(20, 20) != 0);

	GPU o;
	const GP0Encoder &eo = o.get_encoder();
	send(o, { eo.draw_offset(100, 200) });
	flat_tri(o, 0, 0, 10, 0, 0, 10, col(255, 255, 255));
	CHECK(o.vram_get(100, 200) != 0);
	CHECK(o.vram_get(0, 0) == 0);
}

// --- GPU: pixel pipeline ---

static void test_dither() {
	GPU g;
	const GP0Encoder &e = g.get_encoder();
	send(g, { 0xE1000000u | (1u << 9) }); // dither on
	// Gouraud with equal vertex colours: every pixel is the dithered colour.
	const uint32_t c = col(100, 37, 201);
	send(g, { 0x38000000u | c, e.vertex(0, 0), c, e.vertex(32, 0), c, e.vertex(0, 32), c, e.vertex(32, 32) });
	bool ok = true;
	for (int y = 0; y < 32; y++) {
		for (int x = 0; x < 32; x++) {
			const uint16_t p = g.vram_get(x, y);
			ok = ok && (p & 31) == dithered(100, x, y) && ((p >> 5) & 31) == dithered(37, x, y) && ((p >> 10) & 31) == dithered(201, x, y);
		}
	}
	CHECK(ok);
	// Flat untextured polygons are never dithered.
	GPU f;
	send(f, { 0xE1000000u | (1u << 9) });
	flat_tri(f, 0, 0, 32, 0, 0, 32, col(100, 100, 100));
	CHECK_EQ(f.vram_get(1, 1), rgb15(12, 12, 12));
	CHECK_EQ(f.vram_get(3, 2), rgb15(12, 12, 12));
	// Dither off: plain truncation.
	GPU n;
	send(n, { 0x38000000u | c, n.get_encoder().vertex(0, 0), c, n.get_encoder().vertex(32, 0), c, n.get_encoder().vertex(0, 32), c, n.get_encoder().vertex(32, 32) });
	CHECK_EQ(n.vram_get(5, 5), rgb15(100 >> 3, 37 >> 3, 201 >> 3));
}

static void test_gouraud_interpolation() {
	GPU g;
	const GP0Encoder &e = g.get_encoder();
	// Horizontal ramp 0 -> 248 over 32 pixels: red increases left to right.
	send(g, { 0x38000000u | col(0, 0, 0), e.vertex(0, 0), col(248, 0, 0), e.vertex(32, 0), col(0, 0, 0), e.vertex(0, 4), col(248, 0, 0), e.vertex(32, 4) });
	bool monotonic = true;
	for (int x = 1; x < 32; x++) {
		monotonic = monotonic && (g.vram_get(x, 1) & 31) >= (g.vram_get(x - 1, 1) & 31);
	}
	CHECK(monotonic);
	CHECK_EQ(g.vram_get(0, 1) & 31, 0);
	CHECK((g.vram_get(31, 1) & 31) >= 29);
}

static void test_blend_modes() {
	bool ok = true;
	for (int mode = 0; mode < 4; mode++) {
		GPU g;
		const GP0Encoder &e = g.get_encoder();
		send(g, { 0xE1000000u | ((uint32_t)mode << 5) });
		for (int b = 0; b < 32; b++) {
			for (int f = 0; f < 32; f++) {
				g.vram_set(f, b, rgb15((uint32_t)b, 0, 0));
				// 1x1 semi-transparent tile: rectangles are never dithered.
				send(g, { 0x6A000000u | col((uint8_t)(f << 3), 0, 0), e.vertex(f, b) });
				int expect;
				switch (mode) {
					case 0:
						expect = (b + f) >> 1;
						break;
					case 1:
						expect = b + f > 31 ? 31 : b + f;
						break;
					case 2:
						expect = b - f < 0 ? 0 : b - f;
						break;
					default:
						expect = b + (f >> 2) > 31 ? 31 : b + (f >> 2);
						break;
				}
				const uint16_t p = g.vram_get(f, b);
				ok = ok && (p & 31) == expect && (p & 0x8000) == 0;
			}
		}
	}
	CHECK(ok);
}

static void setup_15bit_texture(GPU &g, const std::vector<uint16_t> &texels, int w, int h) {
	upload(g, 512, 0, w, h, texels);
	g.write_gp0(0xE1000000u | GP0Encoder::texpage(512, 0, TEXTURE_15BIT, BLEND_ADD));
}

static void test_modulation_and_raw() {
	GPU g;
	std::vector<uint16_t> row(32);
	for (int i = 0; i < 32; i++) {
		row[i] = (uint16_t)(rgb15((uint32_t)i, (uint32_t)i, (uint32_t)(31 - i)) | 0x8000);
	}
	setup_15bit_texture(g, row, 32, 1);
	const GP0Encoder &e = g.get_encoder();
	// Modulated sprite at 80h: output equals the texel.
	send(g, { 0x64000000u | col(128, 128, 128), e.vertex(0, 10), e.texcoord(0, 0), e.xy(32, 1) });
	bool neutral = true;
	for (int i = 0; i < 32; i++) {
		neutral = neutral && g.vram_get(i, 10) == row[i];
	}
	CHECK(neutral);
	// Colour FFh brightens up to about 2x, saturating at 31.
	send(g, { 0x64000000u | col(255, 255, 255), e.vertex(0, 11), e.texcoord(0, 0), e.xy(32, 1) });
	bool bright = true;
	for (int i = 0; i < 32; i++) {
		int v = (i * 255) >> 4 >> 3;
		v = v > 31 ? 31 : v;
		bright = bright && (g.vram_get(i, 11) & 31) == v;
	}
	CHECK(bright);
	// Colour 0: black, but bit 15 from the texel survives.
	send(g, { 0x64000000u | col(0, 0, 0), e.vertex(0, 12), e.texcoord(0, 0), e.xy(32, 1) });
	CHECK_EQ(g.vram_get(5, 12), 0x8000);
	// Raw: the colour is ignored.
	send(g, { 0x65000000u | col(0, 0, 0), e.vertex(0, 13), e.texcoord(0, 0), e.xy(32, 1) });
	CHECK_EQ(g.vram_get(7, 13), row[7]);
}

static void test_transparency_rules() {
	GPU g;
	const std::vector<uint16_t> texels = { 0x0000, 0x8000, rgb15(10, 10, 10), (uint16_t)(rgb15(10, 10, 10) | 0x8000) };
	setup_15bit_texture(g, texels, 4, 1); // blend mode ADD
	const GP0Encoder &e = g.get_encoder();
	const uint16_t bg = rgb15(5, 5, 5);
	for (int x = 0; x < 4; x++) {
		g.vram_set(x, 20, bg);
		g.vram_set(x, 21, bg);
	}
	// Opaque raw sprite.
	send(g, { 0x65000000u, e.vertex(0, 20), e.texcoord(0, 0), e.xy(4, 1) });
	CHECK_EQ(g.vram_get(0, 20), bg); // 0000h: transparent
	CHECK_EQ(g.vram_get(1, 20), 0x8000); // 8000h: opaque black
	CHECK_EQ(g.vram_get(2, 20), rgb15(10, 10, 10));
	CHECK_EQ(g.vram_get(3, 20), rgb15(10, 10, 10) | 0x8000);
	// Semi-transparent raw sprite: only texels with bit 15 blend.
	send(g, { 0x67000000u, e.vertex(0, 21), e.texcoord(0, 0), e.xy(4, 1) });
	CHECK_EQ(g.vram_get(0, 21), bg);
	CHECK_EQ(g.vram_get(1, 21), bg | 0x8000); // bg + black
	CHECK_EQ(g.vram_get(2, 21), rgb15(10, 10, 10)); // not flagged: opaque
	CHECK_EQ(g.vram_get(3, 21), rgb15(15, 15, 15) | 0x8000);
}

static void test_clut_textures() {
	GPU g;
	const GP0Encoder &e = g.get_encoder();
	// 4-bit: texels 0..15 in a row, CLUT entry i = colour i.
	std::vector<uint16_t> tex4 = { 0x3210, 0x7654, 0xBA98, 0xFEDC };
	upload(g, 640, 0, 4, 1, tex4);
	std::vector<uint16_t> clut(256);
	for (int i = 0; i < 256; i++) {
		clut[i] = rgb15((uint32_t)i & 31, (uint32_t)(i >> 5), 1);
	}
	upload(g, 640, 100, 256, 1, clut);
	g.write_gp0(0xE1000000u | GP0Encoder::texpage(640, 0, TEXTURE_4BIT, BLEND_HALF));
	send(g, { 0x65000000u, e.vertex(0, 0), e.texcoord(0, 0, GP0Encoder::clut(640, 100)), e.xy(16, 1) });
	bool ok4 = true;
	for (int i = 0; i < 16; i++) {
		ok4 = ok4 && g.vram_get(i, 0) == clut[i];
	}
	CHECK(ok4);
	// 8-bit: two texels per halfword.
	std::vector<uint16_t> tex8 = { (uint16_t)(200 | (37 << 8)), (uint16_t)(255 | (1 << 8)) };
	upload(g, 704, 0, 2, 1, tex8);
	g.write_gp0(0xE1000000u | GP0Encoder::texpage(704, 0, TEXTURE_8BIT, BLEND_HALF));
	send(g, { 0x65000000u, e.vertex(0, 1), e.texcoord(0, 0, GP0Encoder::clut(640, 100)), e.xy(4, 1) });
	CHECK_EQ(g.vram_get(0, 1), clut[200]);
	CHECK_EQ(g.vram_get(1, 1), clut[37]);
	CHECK_EQ(g.vram_get(2, 1), clut[255]);
	CHECK_EQ(g.vram_get(3, 1), clut[1]);
	// Palette swap: same texture, another CLUT row.
	std::vector<uint16_t> clut2(16, rgb15(31, 0, 31));
	upload(g, 640, 101, 16, 1, clut2);
	g.write_gp0(0xE1000000u | GP0Encoder::texpage(640, 0, TEXTURE_4BIT, BLEND_HALF));
	send(g, { 0x65000000u, e.vertex(0, 2), e.texcoord(0, 0, GP0Encoder::clut(640, 101)), e.xy(16, 1) });
	CHECK_EQ(g.vram_get(9, 2), rgb15(31, 0, 31));
}

static void test_texture_window_and_flip() {
	GPU g;
	const GP0Encoder &e = g.get_encoder();
	std::vector<uint16_t> row(32);
	for (int i = 0; i < 32; i++) {
		row[i] = (uint16_t)(i + 1);
	}
	setup_15bit_texture(g, row, 32, 1);
	// Window: mask 1Fh replaces U bits 3-7 with the offset, so U repeats
	// within 8 texels; offset 2 selects texels 16-23.
	send(g, { e.texture_window(0x1F, 0, 2, 0) });
	send(g, { 0x65000000u, e.vertex(0, 0), e.texcoord(0, 0), e.xy(24, 1) });
	bool ok = true;
	for (int x = 0; x < 24; x++) {
		ok = ok && g.vram_get(x, 0) == row[16 + (x & 7)];
	}
	CHECK(ok);
	send(g, { e.texture_window(0, 0, 0, 0) });
	// X flip on rectangles: coordinates count down from the origin texel.
	g.write_gp0(0xE1000000u | GP0Encoder::texpage(512, 0, TEXTURE_15BIT, BLEND_HALF, false, false, true, false));
	send(g, { 0x65000000u, e.vertex(0, 1), e.texcoord(10, 0), e.xy(4, 1) });
	CHECK_EQ(g.vram_get(0, 1), row[10]);
	CHECK_EQ(g.vram_get(3, 1), row[7]);
}

static void test_textured_triangle_affine() {
	GPU g;
	std::vector<uint16_t> tex(64 * 64);
	for (int y = 0; y < 64; y++) {
		for (int x = 0; x < 64; x++) {
			tex[y * 64 + x] = (uint16_t)(rgb15((uint32_t)x >> 1, (uint32_t)y >> 1, 0) | 1);
		}
	}
	setup_15bit_texture(g, tex, 64, 64);
	const GP0Encoder &e = g.get_encoder();
	// 1:1 mapped quad: each pixel shows the texel at its own position.
	send(g, { 0x2D000000u, e.vertex(0, 0), e.texcoord(0, 0, 0), e.vertex(64, 0), e.texcoord(64, 0, GP0Encoder::texpage(512, 0, TEXTURE_15BIT, BLEND_HALF)),
					 e.vertex(0, 64), e.texcoord(0, 64), e.vertex(64, 64), e.texcoord(64, 64) });
	bool ok = true;
	for (int y = 0; y < 64; y++) {
		for (int x = 0; x < 64; x++) {
			ok = ok && g.vram_get(x, y) == tex[y * 64 + x];
		}
	}
	CHECK(ok);
}

static void test_mask_bit() {
	GPU g;
	const GP0Encoder &e = g.get_encoder();
	send(g, { e.mask_bits(true, false) });
	flat_tri(g, 0, 0, 16, 0, 0, 16, col(255, 0, 0));
	CHECK_EQ(g.vram_get(1, 1), rgb15(31, 0, 0) | 0x8000);
	send(g, { e.mask_bits(false, true) });
	send(g, { 0x60000000u | col(0, 255, 0), e.vertex(0, 0), e.xy(16, 16) });
	CHECK_EQ(g.vram_get(1, 1), rgb15(31, 0, 0) | 0x8000); // protected
	CHECK_EQ(g.vram_get(15, 15), rgb15(0, 31, 0)); // outside the triangle
	// Fill ignores the mask.
	send(g, { 0x02000000u | col(0, 0, 255), e.xy(0, 0), e.xy(16, 16) });
	CHECK_EQ(g.vram_get(1, 1), rgb15(0, 0, 31));
}

static void test_fill_and_copies() {
	GPU g;
	const GP0Encoder &e = g.get_encoder();
	send(g, { e.draw_area_top_left(0, 0), e.draw_area_bottom_right(0, 0) });
	// Fill: ignores the drawing area; X is rounded down to 16, width up to 16.
	send(g, { 0x02000000u | col(255, 255, 255), e.xy(37, 3), e.xy(5, 2) });
	CHECK_EQ(g.vram_get(32, 3), rgb15(31, 31, 31));
	CHECK_EQ(g.vram_get(47, 4), rgb15(31, 31, 31));
	CHECK_EQ(g.vram_get(48, 3), 0);
	CHECK_EQ(g.vram_get(32, 5), 0);
	// VRAM -> VRAM copy.
	send(g, { 0x80000000u, e.xy(32, 3), e.xy(100, 100), e.xy(4, 2) });
	CHECK_EQ(g.vram_get(103, 101), rgb15(31, 31, 31));
	CHECK_EQ(g.vram_get(104, 101), 0);
	// CPU -> VRAM, then VRAM -> CPU.
	upload(g, 200, 50, 3, 1, { 1, 2, 3 });
	CHECK_EQ(g.vram_get(202, 50), 3);
	send(g, { 0xC0000000u, e.xy(200, 50), e.xy(3, 1) });
	CHECK_EQ(g.get_readback().size(), 3);
	CHECK_EQ(g.get_readback()[1], 2);
}

static void test_lines() {
	GPU g;
	const GP0Encoder &e = g.get_encoder();
	send(g, { 0x40000000u | col(255, 255, 255), e.vertex(0, 0), e.vertex(5, 0) });
	CHECK_EQ(count_nonzero(g, 0, 0, 16, 16), 6); // both endpoints
	// Vertical, drawn upward.
	send(g, { 0x40000000u | col(255, 255, 255), e.vertex(10, 10), e.vertex(10, 3) });
	CHECK_EQ(count_nonzero(g, 10, 0, 11, 16), 8);
	// Diagonal: exactly one pixel per step.
	GPU d;
	send(d, { 0x40000000u | col(255, 255, 255), d.get_encoder().vertex(0, 0), d.get_encoder().vertex(7, 7) });
	CHECK_EQ(count_nonzero(d, 0, 0, 16, 16), 8);
	CHECK(d.vram_get(7, 7) != 0);
	// Polyline ending at the 5xxx5xxx terminator.
	GPU p;
	const GP0Encoder &ep = p.get_encoder();
	send(p, { 0x48000000u | col(255, 255, 255), ep.vertex(0, 0), ep.vertex(4, 0), ep.vertex(4, 4), 0x55555555u });
	CHECK(p.vram_get(4, 4) != 0);
	CHECK(p.vram_get(2, 0) != 0);
	// The next command after the terminator decodes normally.
	send(p, { 0x60000000u | col(255, 0, 0), ep.vertex(20, 20), ep.xy(2, 2) });
	CHECK_EQ(p.vram_get(21, 21), rgb15(31, 0, 0));
}

static void test_textured_polygon_sets_draw_mode() {
	GPU g;
	const GP0Encoder &e = g.get_encoder();
	// A textured polygon's texpage sets the blend mode later untextured
	// semi-transparent primitives use (degenerate poly, nothing drawn).
	const uint16_t page = GP0Encoder::texpage(512, 0, TEXTURE_15BIT, BLEND_SUBTRACT);
	send(g, { 0x24000000u, e.vertex(-10, -10), e.texcoord(0, 0, 0), e.vertex(-10, -10), e.texcoord(0, 0, page), e.vertex(-10, -10), e.texcoord(0, 0) });
	CHECK_EQ(g.get_state().blend, BLEND_SUBTRACT);
	g.vram_set(5, 5, rgb15(20, 20, 20));
	send(g, { 0x6A000000u | col(80, 80, 80), e.vertex(5, 5) });
	CHECK_EQ(g.vram_get(5, 5), rgb15(10, 10, 10));
}

// --- ordering table ---

static void test_ordering_table() {
	GPU g;
	PacketArena arena(4096);
	PacketBuilder pb(arena, g.get_encoder());
	OrderingTable ot(arena, 16);
	ot.clear();
	// Same bucket: the older packet is drawn last (on top).
	ot.add(5, pb.rectangle(0, 0, 4, 4, 255, 0, 0)); // A (older)
	ot.add(5, pb.rectangle(0, 0, 4, 4, 0, 255, 0)); // B (newer, behind)
	// Different buckets: lower OTZ is nearer and drawn later.
	ot.add(3, pb.rectangle(10, 0, 4, 4, 0, 0, 255)); // near
	ot.add(10, pb.rectangle(10, 0, 4, 4, 255, 255, 0)); // far
	// Out-of-range OTZ clamps to the far end.
	ot.add(999, pb.rectangle(20, 0, 4, 4, 255, 255, 255));
	const uint32_t drawn = ot.draw(g);
	CHECK_EQ(drawn, 5);
	CHECK_EQ(g.vram_get(1, 1), rgb15(31, 0, 0));
	CHECK_EQ(g.vram_get(11, 1), rgb15(0, 0, 31));
	CHECK_EQ(g.vram_get(21, 1), rgb15(31, 31, 31));

	// Nested sub-OT linked into one bucket sorts inside itself.
	GPU g2;
	PacketArena a2(4096);
	PacketBuilder p2(a2, g2.get_encoder());
	OrderingTable parent(a2, 8), child(a2, 4);
	parent.clear();
	child.clear();
	child.add(0, p2.rectangle(0, 0, 2, 2, 255, 0, 0)); // child near
	child.add(3, p2.rectangle(0, 0, 2, 2, 0, 255, 0)); // child far
	parent.add(6, p2.rectangle(0, 0, 2, 2, 0, 0, 255)); // behind the child
	parent.add(2, p2.rectangle(4, 0, 2, 2, 255, 255, 255)); // in front
	parent.add_table(4, child);
	CHECK_EQ(parent.draw(g2), 4);
	CHECK_EQ(g2.vram_get(0, 0), rgb15(31, 0, 0));
	CHECK_EQ(g2.vram_get(4, 0), rgb15(31, 31, 31));
}

// --- frame layout and demo ---

static void test_frame_layout() {
	GPU g1;
	FrameLayout l = FrameLayout::make(g1, 320, 240);
	CHECK_EQ(l.buffer_count, 2);
	CHECK_EQ(l.buffer_y[1], 240);
	l = FrameLayout::make(g1, 640, 480);
	CHECK_EQ(l.buffer_count, 1);
	GPU g2(2);
	l = FrameLayout::make(g2, 640, 480);
	CHECK_EQ(l.buffer_count, 2);
	CHECK_EQ(l.buffer_x[0], 1024);
	CHECK(!l.overlaps_texture_region);
	l = FrameLayout::make(g2, 1280, 960);
	CHECK_EQ(l.buffer_count, 1);
	GPU g3(3);
	l = FrameLayout::make(g3, 1280, 720);
	CHECK_EQ(l.buffer_count, 2);
}

static std::vector<uint16_t> render_demo(int p_scale, int p_w, int p_h, uint32_t p_frame, DemoStats *r_stats = nullptr) {
	GPU g(p_scale);
	DemoScene scene;
	scene.upload_textures(g);
	const FrameLayout layout = FrameLayout::make(g, p_w, p_h);
	const DemoStats s = scene.render(g, layout, (int)(p_frame & 1), p_frame);
	if (r_stats) {
		*r_stats = s;
	}
	std::vector<uint16_t> out((size_t)p_w * p_h);
	for (int y = 0; y < p_h; y++) {
		for (int x = 0; x < p_w; x++) {
			out[(size_t)y * p_w + x] = g.vram_get(g.display.x + x, g.display.y + y);
		}
	}
	return out;
}

static void test_demo() {
	DemoStats s;
	const std::vector<uint16_t> a = render_demo(1, 320, 240, 30, &s);
	const std::vector<uint16_t> b = render_demo(1, 320, 240, 30);
	CHECK(a == b); // deterministic
	CHECK_EQ(s.quads_submitted, 6);
	CHECK(s.quads_culled >= 2 && s.quads_culled <= 4);
	// The cube covers a good part of the centre.
	int lit = 0;
	const uint16_t bg = rgb15(24 >> 3, 24 >> 3, 56 >> 3);
	for (int y = 60; y < 180; y++) {
		for (int x = 100; x < 220; x++) {
			lit += (a[(size_t)y * 320 + x] & 0x7FFF) != bg;
		}
	}
	CHECK(lit > 5000);
	// Scaled VRAM renders too, and differs from a mere upscale.
	const std::vector<uint16_t> big = render_demo(2, 640, 480, 30);
	int big_lit = 0;
	for (uint16_t p : big) {
		big_lit += (p & 0x7FFF) != bg;
	}
	CHECK(big_lit > 4 * 5000);
}

// --- PNG writer for --dump (stored deflate blocks, no compression) ---

static uint32_t crc32(const uint8_t *p, size_t n, uint32_t c = 0xFFFFFFFFu) {
	for (size_t i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++) {
			c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
		}
	}
	return c;
}

static bool write_png(const std::string &path, int w, int h, const std::vector<uint16_t> &pix) {
	std::vector<uint8_t> raw;
	for (int y = 0; y < h; y++) {
		raw.push_back(0);
		for (int x = 0; x < w; x++) {
			const uint32_t c = rgba8_from_rgb15(pix[(size_t)y * w + x]);
			raw.push_back((uint8_t)c);
			raw.push_back((uint8_t)(c >> 8));
			raw.push_back((uint8_t)(c >> 16));
		}
	}
	std::vector<uint8_t> z = { 0x78, 0x01 };
	uint32_t a = 1, b = 0;
	for (uint8_t v : raw) {
		a = (a + v) % 65521;
		b = (b + a) % 65521;
	}
	for (size_t i = 0; i < raw.size(); i += 65535) {
		const size_t n = raw.size() - i < 65535 ? raw.size() - i : 65535;
		z.push_back(i + n == raw.size() ? 1 : 0);
		z.push_back((uint8_t)n);
		z.push_back((uint8_t)(n >> 8));
		z.push_back((uint8_t)~n);
		z.push_back((uint8_t)(~n >> 8));
		z.insert(z.end(), raw.begin() + i, raw.begin() + i + n);
	}
	const uint32_t adler = (b << 16) | a;
	for (int s = 24; s >= 0; s -= 8) {
		z.push_back((uint8_t)(adler >> s));
	}
	FILE *f = std::fopen(path.c_str(), "wb");
	if (!f) {
		return false;
	}
	auto be32 = [&](uint32_t v) {
		const uint8_t q[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
		std::fwrite(q, 1, 4, f);
	};
	auto chunk = [&](const char *type, const std::vector<uint8_t> &data) {
		be32((uint32_t)data.size());
		std::fwrite(type, 1, 4, f);
		if (!data.empty()) {
			std::fwrite(data.data(), 1, data.size(), f);
		}
		uint32_t c = crc32((const uint8_t *)type, 4);
		c = crc32(data.data(), data.size(), c);
		be32(c ^ 0xFFFFFFFFu);
	};
	const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	std::fwrite(sig, 1, 8, f);
	std::vector<uint8_t> ihdr = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
		(uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0 };
	chunk("IHDR", ihdr);
	chunk("IDAT", z);
	chunk("IEND", {});
	std::fclose(f);
	return true;
}

static void dump_demo(const std::string &dir) {
	struct Shot {
		int scale, w, h;
		uint32_t frame;
	};
	const Shot shots[] = { { 1, 320, 240, 0 }, { 1, 320, 240, 30 }, { 1, 320, 240, 75 }, { 2, 640, 480, 30 } };
	for (const Shot &s : shots) {
		const std::vector<uint16_t> pix = render_demo(s.scale, s.w, s.h, s.frame);
		char name[128];
		std::snprintf(name, sizeof(name), "/demo_x%d_%dx%d_f%03u.png", s.scale, s.w, s.h, s.frame);
		const bool ok = write_png(dir + name, s.w, s.h, pix);
		std::printf("PSX_DUMP %s %s\n", ok ? "ok" : "fail", (dir + name).c_str());
	}
}

int main(int argc, char **argv) {
	struct Test {
		const char *name;
		void (*fn)();
	};
	const Test tests[] = {
		{ "trig", test_trig },
		{ "unr_divide", test_unr_divide },
		{ "rtps_rtpt", test_rtps },
		{ "nclip_avsz", test_nclip_avsz },
		{ "half_open_coverage", test_half_open_coverage },
		{ "fill_rule_fan", test_fill_rule_fan },
		{ "size_limits_and_scale", test_size_limits },
		{ "drawing_area_and_offset", test_drawing_area_and_offset },
		{ "dither", test_dither },
		{ "gouraud_interpolation", test_gouraud_interpolation },
		{ "blend_modes", test_blend_modes },
		{ "modulation_and_raw", test_modulation_and_raw },
		{ "transparency_rules", test_transparency_rules },
		{ "clut_textures", test_clut_textures },
		{ "texture_window_and_flip", test_texture_window_and_flip },
		{ "textured_triangle_affine", test_textured_triangle_affine },
		{ "mask_bit", test_mask_bit },
		{ "fill_and_copies", test_fill_and_copies },
		{ "lines", test_lines },
		{ "polygon_sets_draw_mode", test_textured_polygon_sets_draw_mode },
		{ "ordering_table", test_ordering_table },
		{ "frame_layout", test_frame_layout },
		{ "demo", test_demo },
	};
	int failed_tests = 0;
	for (const Test &t : tests) {
		const int before = g_failures;
		t.fn();
		const bool ok = g_failures == before;
		failed_tests += !ok;
		std::printf("%s %s\n", ok ? "PASS" : "FAIL", t.name);
	}
	if (argc >= 3 && std::strcmp(argv[1], "--dump") == 0) {
		dump_demo(argv[2]);
	}
	const int total = (int)(sizeof(tests) / sizeof(tests[0]));
	std::printf("PSX_UNIT tests=%d failed=%d checks=%d check_failures=%d\n", total, failed_tests, g_checks, g_failures);
	return failed_tests == 0 ? 0 : 1;
}
