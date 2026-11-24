// Standalone unit tests for psx_core. No Godot dependency: built and run by
// modules/psx/testing/psx_test.ps1 unit (MSVC, a few seconds).
//
// Exit code 0 = all passed. Each failure prints file:line and the expression.

#include "../../psx_core/psx_raster.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

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

#define CHECK_EQ(a, b)                                                                            \
	do {                                                                                          \
		g_checks++;                                                                               \
		const long long va_ = (long long)(a), vb_ = (long long)(b);                               \
		if (va_ != vb_) {                                                                         \
			g_failures++;                                                                         \
			std::printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
		}                                                                                         \
	} while (0)

struct TestFB {
	std::vector<uint32_t> pixels;
	psx::Framebuffer fb;
	TestFB(int w, int h) :
			pixels((size_t)w * h, 0) {
		fb.width = w;
		fb.height = h;
		fb.pixels = pixels.data();
	}
	uint32_t at(int x, int y) const { return pixels[(size_t)y * fb.width + x]; }
	int count_nonzero() const {
		int n = 0;
		for (uint32_t p : pixels) {
			n += p != 0;
		}
		return n;
	}
};

static psx::Vertex V(int x, int y, uint8_t r = 255, uint8_t g = 255, uint8_t b = 255) {
	psx::Vertex v;
	v.x = x;
	v.y = y;
	v.r = r;
	v.g = g;
	v.b = b;
	return v;
}

// --- tests ---

static void test_pack_rgba() {
	CHECK_EQ(psx::pack_rgba(0x11, 0x22, 0x33), 0xFF332211u);
	uint32_t p = psx::pack_rgba(1, 2, 3);
	const uint8_t *bytes = (const uint8_t *)&p;
	CHECK(bytes[0] == 1 && bytes[1] == 2 && bytes[2] == 3 && bytes[3] == 255);
}

static void test_clear() {
	TestFB t(8, 4);
	psx::clear(t.fb, 0xDEADBEEF);
	bool all = true;
	for (uint32_t p : t.pixels) {
		all = all && p == 0xDEADBEEF;
	}
	CHECK(all);
}

static void test_degenerate_draws_nothing() {
	TestFB t(16, 16);
	psx::draw_triangle_gouraud(t.fb, V(2, 2), V(8, 8), V(14, 14)); // collinear
	psx::draw_triangle_gouraud(t.fb, V(5, 5), V(5, 5), V(5, 5)); // point
	CHECK_EQ(t.count_nonzero(), 0);
}

static void test_winding_independent() {
	TestFB a(32, 32), b(32, 32);
	psx::draw_triangle_gouraud(a.fb, V(3, 2), V(28, 9), V(10, 29));
	psx::draw_triangle_gouraud(b.fb, V(3, 2), V(10, 29), V(28, 9));
	CHECK(a.count_nonzero() > 0);
	CHECK(a.pixels == b.pixels);
}

// Two triangles sharing an edge must cover every pixel of the quad exactly
// once: no gaps, no double-draw. Checked for several shared-edge directions.
static void check_shared_edge(psx::Vertex p0, psx::Vertex p1, psx::Vertex p2, psx::Vertex p3) {
	// Quad p0 p1 p2 p3, split along p0-p2.
	TestFB a(40, 40), b(40, 40);
	psx::draw_triangle_gouraud(a.fb, p0, p1, p2);
	psx::draw_triangle_gouraud(b.fb, p0, p2, p3);
	int overlap = 0;
	for (size_t i = 0; i < a.pixels.size(); i++) {
		overlap += (a.pixels[i] != 0 && b.pixels[i] != 0);
	}
	CHECK_EQ(overlap, 0);

	// No gap: the same quad split along the other diagonal covers the same
	// pixel set, since both splits tile the same region.
	TestFB c(40, 40), d(40, 40);
	psx::draw_triangle_gouraud(c.fb, p0, p1, p3);
	psx::draw_triangle_gouraud(d.fb, p1, p2, p3);
	int union_ab = 0, union_cd = 0, mismatch = 0;
	for (size_t i = 0; i < a.pixels.size(); i++) {
		const bool ab = a.pixels[i] || b.pixels[i];
		const bool cd = c.pixels[i] || d.pixels[i];
		union_ab += ab;
		union_cd += cd;
		mismatch += ab != cd;
	}
	CHECK(union_ab > 0);
	CHECK_EQ(mismatch, 0);
}

static void test_fill_rule_shared_edges() {
	check_shared_edge(V(5, 5), V(30, 5), V(30, 30), V(5, 30)); // axis-aligned square
	check_shared_edge(V(20, 2), V(37, 20), V(20, 37), V(2, 20)); // diamond
	check_shared_edge(V(3, 4), V(33, 9), V(29, 35), V(6, 27)); // irregular
}

static void test_axis_aligned_coverage() {
	// A 10x10 square split in two covers exactly 100 pixels: half-open
	// [x0, x1) x [y0, y1) like a GPU, not 11x11.
	TestFB t(32, 32);
	psx::draw_triangle_gouraud(t.fb, V(4, 4), V(14, 4), V(14, 14));
	psx::draw_triangle_gouraud(t.fb, V(4, 4), V(14, 14), V(4, 14));
	CHECK_EQ(t.count_nonzero(), 100);
}

static void test_gouraud_vertex_and_flat() {
	// Flat colour stays flat.
	TestFB t(32, 32);
	psx::draw_triangle_gouraud(t.fb, V(0, 0, 10, 20, 30), V(31, 0, 10, 20, 30), V(0, 31, 10, 20, 30));
	bool flat = true;
	for (uint32_t p : t.pixels) {
		flat = flat && (p == 0 || p == psx::pack_rgba(10, 20, 30));
	}
	CHECK(flat);
	// At a drawn vertex the colour equals that vertex's colour.
	TestFB g(32, 32);
	psx::draw_triangle_gouraud(g.fb, V(0, 0, 255, 0, 0), V(31, 0, 0, 255, 0), V(0, 31, 0, 0, 255));
	CHECK_EQ(g.at(0, 0), psx::pack_rgba(255, 0, 0));
}

static void test_clipping_offscreen() {
	TestFB t(16, 16);
	psx::draw_triangle_gouraud(t.fb, V(-100, -100), V(-50, -100), V(-100, -50)); // fully off
	CHECK_EQ(t.count_nonzero(), 0);
	// Hypotenuse x + y = 80 lies well outside the 16x16 target: full cover, clipped.
	psx::draw_triangle_gouraud(t.fb, V(-20, -20), V(100, -20), V(-20, 100));
	CHECK_EQ(t.count_nonzero(), 16 * 16);
}

static void test_deterministic() {
	TestFB a(64, 48), b(64, 48);
	for (int i = 0; i < 2; i++) {
		TestFB &t = i == 0 ? a : b;
		psx::clear(t.fb, psx::pack_rgba(1, 2, 3));
		psx::draw_triangle_gouraud(t.fb, V(1, 1, 200, 10, 10), V(60, 5, 10, 200, 10), V(20, 45, 10, 10, 200));
	}
	CHECK(a.pixels == b.pixels);
}

int main() {
	struct Test {
		const char *name;
		void (*fn)();
	};
	const Test tests[] = {
		{ "pack_rgba", test_pack_rgba },
		{ "clear", test_clear },
		{ "degenerate_draws_nothing", test_degenerate_draws_nothing },
		{ "winding_independent", test_winding_independent },
		{ "fill_rule_shared_edges", test_fill_rule_shared_edges },
		{ "axis_aligned_coverage", test_axis_aligned_coverage },
		{ "gouraud_vertex_and_flat", test_gouraud_vertex_and_flat },
		{ "clipping_offscreen", test_clipping_offscreen },
		{ "deterministic", test_deterministic },
	};
	int failed_tests = 0;
	for (const Test &t : tests) {
		const int before = g_failures;
		t.fn();
		const bool ok = g_failures == before;
		failed_tests += !ok;
		std::printf("%s %s\n", ok ? "PASS" : "FAIL", t.name);
	}
	const int total = (int)(sizeof(tests) / sizeof(tests[0]));
	std::printf("PSX_UNIT tests=%d failed=%d checks=%d check_failures=%d\n", total, failed_tests, g_checks, g_failures);
	return failed_tests == 0 ? 0 : 1;
}
