// Standalone unit tests for psx_core (GPU, GTE, trig, ordering table, demo).
// No Godot dependency: built and run by `psx_test.ps1 unit` with MSVC.
//
//   test_psx_core.exe                 run all tests; exit code 0 = all passed
//   test_psx_core.exe --dump <dir>    also save demo frames as PNG for review
//
// Each failure prints file:line and the expression.

#define _CRT_SECURE_NO_WARNINGS // std::fopen in the PNG writer

#include "../../psx_core/psx_convert.h"
#include "../../psx_core/psx_frame.h"
#include "../../psx_core/psx_gpu.h"
#include "../../psx_core/psx_gte.h"
#include "../../psx_core/psx_ot.h"
#include "../../psx_core/psx_render.h"
#include "../../psx_core/psx_texture.h"
#include "../../psx_core/psx_trig.h"

#include <cmath>
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

static GTE lit_gte() {
	GTE g;
	g.light_dirs.m[0][2] = 4096; // Light 0 shines along +Z (toward normals facing +Z).
	g.light_colors.m[0][0] = g.light_colors.m[1][0] = g.light_colors.m[2][0] = 4096; // White.
	return g;
}

static void test_gte_lighting() {
	// NCS: full light saturates at 255, half light is 128.
	GTE g = lit_gte();
	g.v[0] = { 0, 0, 4096 };
	g.ncs();
	CHECK_EQ(g.rgb[2].r, 255);
	CHECK(g.flag & FLAG_COLOR_R_SAT);
	CHECK(!(g.flag & FLAG_ERROR)); // Colour saturation is not an error bit.
	g.v[0] = { 0, 0, 2048 };
	g.ncs();
	CHECK_EQ(g.rgb[2].g, 128);

	// Facing away: lm clamps the level to 0, leaving the ambient (BK = 0.25).
	g.ambient = { 1024, 1024, 1024 };
	g.v[0] = { 0, 0, -4096 };
	g.ncs();
	CHECK_EQ(g.rgb[2].b, 64);
	CHECK(g.flag & FLAG_IR1_SAT);

	// NCCS: half light times the vertex colour.
	g = lit_gte();
	g.rgbc = { 200, 100, 50, 0x30 };
	g.v[0] = { 0, 0, 2048 };
	g.nccs();
	CHECK_EQ(g.rgb[2].r, 100);
	CHECK_EQ(g.rgb[2].g, 50);
	CHECK_EQ(g.rgb[2].b, 25);
	CHECK_EQ(g.rgb[2].code, 0x30);

	// NCDS: IR0 = 0 keeps the lit colour, 1.0 gives FC, 0.5 lands between.
	g.far_color = { 30 * 16, 60 * 16, 90 * 16 };
	g.ir[0] = 0;
	g.ncds();
	CHECK_EQ(g.rgb[2].r, 100);
	g.ir[0] = 4096;
	g.ncds();
	CHECK_EQ(g.rgb[2].r, 30);
	CHECK_EQ(g.rgb[2].g, 60);
	CHECK_EQ(g.rgb[2].b, 90);
	g.ir[0] = 2048;
	g.ncds();
	CHECK_EQ(g.rgb[2].r, 65);

	// NCCT lights three normals into the FIFO in order.
	g.v[0] = { 0, 0, 4096 };
	g.v[1] = { 0, 0, 2048 };
	g.v[2] = { 0, 0, 0 };
	g.ncct();
	CHECK_EQ(g.rgb[0].r, 200);
	CHECK_EQ(g.rgb[1].r, 100);
	CHECK_EQ(g.rgb[2].r, 0);

	// DPCS with IR0 = 0 passes RGBC through.
	g.ir[0] = 0;
	g.dpcs();
	CHECK_EQ(g.rgb[2].r, 200);
	CHECK_EQ(g.rgb[2].b, 50);
}

// --- Mesh conversion and drawing ---

static void test_convert_mesh() {
	const float pos[] = { -100, -100, 0, 100, -100, 0, -100, 100, 0, 100, 100, 0 };
	const float nrm[] = { 0, 0, -2, 0, 0, -2, 0, 0, -2, 0, 0, -2 };
	const float colors[] = { 0.5f, 0.5f, 0.5f, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 1 };
	const int32_t idx[] = { 0, 1, 2, 1, 3, 2, 0, 1, 9 }; // The last triangle is invalid.
	MeshInput in;
	in.positions = pos;
	in.normals = nrm;
	in.colors = colors;
	in.vertex_count = 4;
	in.indices = idx;
	in.index_count = 9;

	ConvertOptions opt;
	opt.scale = 512.0f; // 100 * 512 = 51200: needs one bit of shift.
	opt.albedo[0] = 1.0f;
	opt.albedo[1] = 0.5f;
	opt.albedo[2] = 0.0f;
	MeshData m;
	CHECK(convert_mesh(in, opt, m));
	CHECK_EQ(m.triangle_count(), 2);
	CHECK_EQ(m.shift, 1);
	CHECK_EQ(m.positions[3].x, 25600);
	CHECK_EQ(m.positions[0].y, -25600);
	CHECK(!m.clamped);
	CHECK_EQ(m.normals[0].z, -4096); // Normalised.
	CHECK_EQ(m.colors[0], 255); // Albedo only (vertex colours off).
	CHECK_EQ(m.colors[1], 128);
	CHECK_EQ(m.colors[2], 0);
	CHECK(m.radius >= 36203 && m.radius <= 36205); // 25600 * sqrt(2)

	// Linear vertex colour 0.5 times white albedo is sRGB ~188.
	opt.albedo[1] = opt.albedo[2] = 1.0f;
	opt.use_vertex_colors = true;
	CHECK(convert_mesh(in, opt, m));
	CHECK_EQ(m.colors[0], 188);
	CHECK_EQ(m.colors[3 * 2 + 2], 0);
	opt.vertex_colors_srgb = true; // sRGB vertex colour passes straight through.
	CHECK(convert_mesh(in, opt, m));
	CHECK_EQ(m.colors[0], 128);

	MeshInput empty;
	CHECK(!convert_mesh(empty, opt, m));
}

static void test_draw_mesh() {
	// A 200-unit square facing the camera (Y down, Z forward), wound clockwise on screen.
	const float pos[] = { -100, -100, 0, 100, -100, 0, -100, 100, 0, 100, 100, 0 };
	const float nrm[] = { 0, 0, -1, 0, 0, -1, 0, 0, -1, 0, 0, -1 };
	const int32_t front[] = { 0, 1, 2, 1, 3, 2 };
	const int32_t back[] = { 0, 2, 1, 1, 2, 3 };
	MeshInput in;
	in.positions = pos;
	in.normals = nrm;
	in.vertex_count = 4;
	in.indices = front;
	in.index_count = 6;
	ConvertOptions opt;
	opt.scale = 1.0f;
	opt.albedo[0] = 0.0f;
	opt.albedo[1] = 1.0f;
	opt.albedo[2] = 0.0f;
	MeshData m;
	CHECK(convert_mesh(in, opt, m));

	auto run = [&](const MeshData &p_mesh, int32_t p_z, const DrawParams &p_params, DrawStats &r_stats) {
		GPU g;
		GTE gte = basic_gte();
		gte.translation = { 0, 0, p_z };
		gte.ambient = { 4096, 4096, 4096 }; // Ambient 1.0: the baked colour.
		PacketArena arena(1u << 12);
		PacketBuilder pb(arena, g.get_encoder());
		OrderingTable ot(arena, 256);
		ot.clear();
		ot.add(255, pb.environment(0, 0, 319, 239, 0, 0, GP0Encoder::texpage(0, 0, 0, 0, true)));
		draw_mesh(gte, pb, ot, p_mesh, p_params, r_stats);
		ot.draw(g);
		return g.vram_get(160, 120);
	};

	DrawParams params;
	DrawStats st;
	CHECK_EQ(run(m, 1000, params, st), rgb15(0, 31, 0));
	CHECK_EQ(st.packets, 2);
	CHECK_EQ(st.culled_backface, 0);

	// Facing away: culled with CULL_BACK, drawn with CULL_NONE or CULL_FRONT.
	in.indices = back;
	MeshData mb;
	CHECK(convert_mesh(in, opt, mb));
	DrawStats sb;
	CHECK_EQ(run(mb, 1000, params, sb), 0);
	CHECK_EQ(sb.culled_backface, 2);
	params.cull = CULL_FRONT;
	DrawStats sf;
	CHECK_EQ(run(mb, 1000, params, sf), rgb15(0, 31, 0));
	CHECK_EQ(sf.packets, 2);

	// Too near (SZ < H/2): dropped.
	params.cull = CULL_BACK;
	DrawStats sn;
	run(m, 100, params, sn);
	CHECK_EQ(sn.culled_near, 2);
	CHECK_EQ(sn.near_clips, 0); // Wholly nearer: nothing to cut.

	// A floor running from behind the camera to far ahead crosses the near
	// plane: split into pieces, the ones in front drawn, down to the bottom
	// of the screen (Z = 160 at y = 200).
	{
		const float floor_pos[] = { -100, 50, -300, 100, 50, -300, -100, 50, 1700, 100, 50, 1700 };
		const float floor_nrm[] = { 0, -1, 0, 0, -1, 0, 0, -1, 0, 0, -1, 0 };
		MeshInput fin = in;
		fin.positions = floor_pos;
		fin.normals = floor_nrm;
		fin.indices = front;
		MeshData mf;
		CHECK(convert_mesh(fin, opt, mf));
		DrawParams pf;
		pf.cull = CULL_NONE;
		DrawStats sfl;
		CHECK_EQ(run(mf, 0, pf, sfl), 0); // Screen centre: the horizon, above the floor
		CHECK(sfl.near_clips > 0);
		CHECK(sfl.packets > 0);
		GPU g;
		GTE gte = basic_gte();
		gte.ambient = { 4096, 4096, 4096 };
		PacketArena arena(1u << 14);
		PacketBuilder pb(arena, g.get_encoder());
		OrderingTable ot(arena, 256);
		ot.clear();
		ot.add(255, pb.environment(0, 0, 319, 239, 0, 0, GP0Encoder::texpage(0, 0, 0, 0, false)));
		DrawStats s2;
		draw_mesh(gte, pb, ot, mf, pf, s2);
		ot.draw(g);
		CHECK_EQ(g.vram_get(160, 200), rgb15(0, 31, 0));
		CHECK_EQ(g.vram_get(160, 140), rgb15(0, 31, 0));
	}
	// Beyond the far plane: dropped.
	params.far_z = 500;
	DrawStats sfar;
	run(m, 1000, params, sfar);
	CHECK_EQ(sfar.culled_far, 2);
	params.far_z = 0xFFFF;

	// Lit by one light at half strength: half the colour.
	DrawStats sl;
	{
		GPU g;
		GTE gte = basic_gte();
		gte.translation = { 0, 0, 1000 };
		gte.light_dirs.m[0][2] = -4096; // Toward the camera, along the normals.
		gte.light_colors.m[0][0] = gte.light_colors.m[1][0] = gte.light_colors.m[2][0] = 2048;
		PacketArena arena(1u << 12);
		PacketBuilder pb(arena, g.get_encoder());
		OrderingTable ot(arena, 256);
		ot.clear();
		ot.add(255, pb.environment(0, 0, 319, 239, 0, 0, GP0Encoder::texpage(0, 0, 0, 0, false)));
		draw_mesh(gte, pb, ot, m, params, sl);
		ot.draw(g);
		CHECK_EQ(g.vram_get(160, 120), rgb15(0, 15, 0)); // 255 * 0.5 = 127 -> 15
	}

	// Semi-transparent additive over a blue background.
	DrawStats ss;
	{
		GPU g;
		GTE gte = basic_gte();
		gte.translation = { 0, 0, 1000 };
		gte.ambient = { 4096, 4096, 4096 };
		PacketArena arena(1u << 12);
		PacketBuilder pb(arena, g.get_encoder());
		OrderingTable ot(arena, 256);
		ot.clear();
		ot.add(255, pb.fill(0, 0, 320, 240, 0, 0, 255));
		ot.add(255, pb.environment(0, 0, 319, 239, 0, 0, GP0Encoder::texpage(0, 0, 0, 0, false)));
		DrawParams p;
		p.semi = true;
		p.blend = 1; // B + F
		draw_mesh(gte, pb, ot, m, p, ss);
		ot.draw(g);
		CHECK_EQ(g.vram_get(160, 120), rgb15(0, 31, 31));
		CHECK_EQ(ss.packets, 4); // Two triangles, each with its draw mode.
	}
}

static void test_draw_mesh_features() {
	// Two quads' worth of triangles at different alphas: 1.0, 0.5, 0.1.
	const float pos[] = { -100, -100, 0, 100, -100, 0, -100, 100, 0, 100, 100, 0 };
	const float nrm[] = { 0, 0, -1, 0, 0, -1, 0, 0, -1, 0, 0, -1 };
	const float half_alpha[] = { 1, 1, 1, 0.5f, 1, 1, 1, 0.5f, 1, 1, 1, 0.5f, 1, 1, 1, 0.5f };
	const int32_t idx[] = { 0, 1, 2, 1, 3, 2 };
	MeshInput in;
	in.positions = pos;
	in.normals = nrm;
	in.colors = half_alpha;
	in.vertex_count = 4;
	in.indices = idx;
	in.index_count = 6;
	ConvertOptions opt;
	opt.scale = 1.0f;
	opt.use_vertex_colors = true;
	opt.use_alpha = true;
	MeshData m;
	CHECK(convert_mesh(in, opt, m));
	CHECK_EQ(m.triangle_alpha.size(), 2);
	CHECK_EQ(m.triangle_alpha[0], TRI_HALF);
	opt.albedo[3] = 0.2f; // 0.2 * 0.5: hidden.
	MeshData hidden;
	CHECK(convert_mesh(in, opt, hidden));
	CHECK_EQ(hidden.triangle_alpha[1], TRI_HIDDEN);
	opt.albedo[3] = 1.0f;
	in.colors = nullptr; // Opaque everywhere: no classes kept.
	opt.use_vertex_colors = false;
	MeshData opaque;
	CHECK(convert_mesh(in, opt, opaque));
	CHECK(opaque.triangle_alpha.empty());

	auto draw = [&](const MeshData &p_mesh, const DrawParams &p_params, GTE &p_gte, DrawStats &r_stats) {
		GPU g;
		PacketArena arena(1u << 12);
		PacketBuilder pb(arena, g.get_encoder());
		OrderingTable ot(arena, 256);
		ot.clear();
		ot.add(255, pb.fill(0, 0, 320, 240, 0, 0, 0));
		ot.add(255, pb.environment(0, 0, 319, 239, 0, 0, GP0Encoder::texpage(0, 0, 0, 0, false)));
		draw_mesh(p_gte, pb, ot, p_mesh, p_params, r_stats);
		ot.draw(g);
		return g.vram_get(160, 120);
	};
	GTE gte = basic_gte();
	gte.translation = { 0, 0, 1000 };
	gte.ambient = { 2048, 2048, 2048 }; // Light level 0.5

	// Alpha classes: hidden triangles are dropped, half ones blended.
	DrawParams p;
	p.semi = true;
	DrawStats sh;
	draw(hidden, p, gte, sh);
	CHECK_EQ(sh.culled_alpha, 2);
	DrawStats sm;
	draw(m, p, gte, sm);
	CHECK_EQ(sm.packets, 4); // Both triangles 50%, each with a draw mode.

	// PS1 lighting: white (255) * 0.5 = 127 -> 15. Godot match: linear 0.5
	// is sRGB 188 -> 23.
	DrawParams lp;
	DrawStats s1;
	CHECK_EQ(draw(opaque, lp, gte, s1), rgb15(15, 15, 15));
	lp.lighting = LIGHTING_LINEAR;
	DrawStats s2;
	CHECK_EQ(draw(opaque, lp, gte, s2), rgb15(23, 23, 23));

	// Emission adds after lighting (PS1 mode: 127 + 64 = 191 -> 23).
	DrawParams ep;
	ep.emission[0] = ep.emission[1] = ep.emission[2] = 64;
	DrawStats s3;
	CHECK_EQ(draw(opaque, ep, gte, s3), rgb15(23, 23, 23));

	// Fog: DQA/DQB giving IR0 = 1.0 everywhere turns everything FC.
	DrawParams fp;
	fp.fog = true;
	gte.far_color = { 200 * 16, 0, 0 };
	gte.dqa = 0;
	gte.dqb = 0x1000000; // 1.0 in 1.7.24
	DrawStats s4;
	CHECK_EQ(draw(opaque, fp, gte, s4), rgb15(25, 0, 0));
}

static void test_subdivide_and_point_lights() {
	const float pos[] = { -100, -100, 0, 100, -100, 0, -100, 100, 0, 100, 100, 0 };
	const float nrm[] = { 0, 0, -1, 0, 0, -1, 0, 0, -1, 0, 0, -1 };
	const int32_t idx[] = { 0, 1, 2, 1, 3, 2 };
	MeshInput in;
	in.positions = pos;
	in.normals = nrm;
	in.vertex_count = 4;
	in.indices = idx;
	in.index_count = 6;
	ConvertOptions opt;
	opt.scale = 1.0f;
	opt.max_edge = 60.0f;
	MeshData m;
	CHECK(convert_mesh(in, opt, m));
	CHECK(m.triangle_count() > 8);
	// Every edge within the limit, every triangle still facing the same way,
	// and every edge shared by at most two triangles in opposite directions
	// (no T-junction cracks: a split edge is split for both neighbours).
	bool short_edges = true, same_winding = true, conforming = true;
	std::vector<std::pair<uint32_t, uint32_t>> edges;
	for (uint32_t t = 0; t < m.triangle_count(); t++) {
		const SVector *v[3] = { &m.positions[m.indices[t * 3]], &m.positions[m.indices[t * 3 + 1]], &m.positions[m.indices[t * 3 + 2]] };
		for (int e = 0; e < 3; e++) {
			const SVector &a = *v[e], &b = *v[(e + 1) % 3];
			const double dx = a.x - b.x, dy = a.y - b.y;
			short_edges = short_edges && dx * dx + dy * dy <= 60.0 * 60.0 + 1.0;
			edges.push_back({ m.indices[t * 3 + e], m.indices[t * 3 + (e + 1) % 3] });
		}
		const long long cross = (long long)(v[1]->x - v[0]->x) * (v[2]->y - v[0]->y) - (long long)(v[1]->y - v[0]->y) * (v[2]->x - v[0]->x);
		same_winding = same_winding && cross > 0;
	}
	for (const auto &e : edges) {
		int same = 0;
		for (const auto &o : edges) {
			same += (o.first == e.first && o.second == e.second);
		}
		conforming = conforming && same == 1;
	}
	// Boundary edges appear once; interior ones once each way. A T-junction
	// would leave a long interior edge unmatched: check total boundary length.
	double boundary = 0.0;
	for (const auto &e : edges) {
		bool twin = false;
		for (const auto &o : edges) {
			twin = twin || (o.first == e.second && o.second == e.first);
		}
		if (!twin) {
			const SVector &a = m.positions[e.first], &b = m.positions[e.second];
			boundary += std::sqrt((double)(a.x - b.x) * (a.x - b.x) + (double)(a.y - b.y) * (a.y - b.y));
		}
	}
	CHECK(short_edges);
	CHECK(same_winding);
	CHECK(conforming);
	CHECK(boundary > 799.0 && boundary < 801.0); // Only the square's outline.

	// Point light 100 units in front of the quad's centre, range 1 m at
	// 100 units per metre: the centre is lit, a corner far away is not.
	GPU g;
	GTE gte = basic_gte();
	gte.translation = { 0, 0, 1000 };
	PacketArena arena(1u << 14);
	PacketBuilder pb(arena, g.get_encoder());
	OrderingTable ot(arena, 256);
	ot.clear();
	ot.add(255, pb.fill(0, 0, 320, 240, 0, 0, 0));
	ot.add(255, pb.environment(0, 0, 319, 239, 0, 0, GP0Encoder::texpage(0, 0, 0, 0, false)));
	PointLight pl;
	pl.position[2] = -50.0f;
	pl.metres_per_unit = 0.01f;
	pl.range = 1.0f;
	pl.decay = 0.0f;
	DrawParams p;
	p.point_lights = &pl;
	p.point_light_count = 1;
	DrawStats st;
	draw_mesh(gte, pb, ot, m, p, st);
	ot.draw(g);
	CHECK((g.vram_get(160, 120) & 31) >= 20);
	CHECK((g.vram_get(160 - 24, 120 - 24) & 31) == 0); // Corner: out of range.
}

// --- GPU: coverage ---

static std::vector<uint8_t> rgba_image(int w, int h, uint32_t (*p_fn)(int, int)) {
	std::vector<uint8_t> img((size_t)w * h * 4);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const uint32_t c = p_fn(x, y); // 0xAABBGGRR
			for (int k = 0; k < 4; k++) {
				img[((size_t)y * w + x) * 4 + k] = (uint8_t)(c >> (k * 8));
			}
		}
	}
	return img;
}

static uint16_t texel_index(const TextureData &t, int x, int y) {
	const int per = t.depth == TEXTURE_4BIT ? 4 : (t.depth == TEXTURE_8BIT ? 2 : 1);
	const int bits = 16 / per;
	const uint16_t hw = t.texels[(size_t)y * t.hw_width() + x / per];
	return per == 1 ? hw : (uint16_t)((hw >> ((x % per) * bits)) & ((1 << bits) - 1));
}

static void test_convert_texture() {
	// Four colours plus cut-out texels: 4-bit, index 0 transparent.
	const std::vector<uint8_t> quad = rgba_image(16, 16, [](int x, int y) -> uint32_t {
		if (x == 0 && y == 0) {
			return 0x00FFFFFFu; // Alpha 0
		}
		const uint32_t colors[4] = { 0xFF000000u, 0xFF0000FFu, 0xFF00FF00u, 0xFFFFFFFFu };
		return colors[(x / 4 + y / 4) & 3];
	});
	TextureOptions opt;
	opt.alpha_mode = ALPHA_CUT;
	TextureData t;
	CHECK(convert_texture(quad.data(), 16, 16, opt, t));
	CHECK_EQ(t.width, 16);
	CHECK_EQ(t.depth, TEXTURE_4BIT);
	CHECK(t.has_transparent);
	CHECK(!t.has_semi);
	CHECK_EQ(t.colors, 5);
	CHECK_EQ(texel_index(t, 0, 0), 0);
	CHECK_EQ(t.clut[0], 0);
	CHECK_EQ(t.clut[texel_index(t, 1, 0)], 0x8000); // Opaque black
	CHECK_EQ(t.clut[texel_index(t, 4, 0)], 0x001F); // Red
	CHECK_EQ(t.clut[texel_index(t, 12, 0)], 0x7FFF); // White
	CHECK_EQ(t.clut_semi[texel_index(t, 12, 0)], 0xFFFF);
	CHECK_EQ(t.clut_semi[0], 0); // Transparent stays transparent

	// Opaque materials ignore alpha.
	opt.alpha_mode = ALPHA_IGNORE;
	CHECK(convert_texture(quad.data(), 16, 16, opt, t));
	CHECK(!t.has_transparent);
	CHECK_EQ(t.colors, 4);

	// A gradient with 1024 colours: 8-bit, quantised; or 4-bit with a
	// 16-colour limit. Sizes become powers of two, at most max_size.
	const std::vector<uint8_t> grad = rgba_image(48, 20, [](int x, int y) -> uint32_t {
		return 0xFF000000u | (uint32_t)(x * 5) | ((uint32_t)(y * 12) << 8) | ((uint32_t)((x * y) & 255) << 16);
	});
	CHECK(convert_texture(grad.data(), 48, 20, opt, t));
	CHECK_EQ(t.width, 64);
	CHECK_EQ(t.height, 32);
	CHECK_EQ(t.depth, TEXTURE_8BIT);
	CHECK(t.colors > 200 && t.colors <= 256);
	CHECK_EQ((int)t.texels.size(), 32 * 32);
	opt.max_colors = 16;
	CHECK(convert_texture(grad.data(), 48, 20, opt, t));
	CHECK_EQ(t.depth, TEXTURE_4BIT);
	CHECK(t.colors <= 16);
	opt.max_colors = 0;
	opt.max_size = 32;
	CHECK(convert_texture(grad.data(), 48, 20, opt, t));
	CHECK_EQ(t.width, 32);
	CHECK_EQ(t.height, 32);
	opt.max_size = 256;

	// Blended: half alpha sets STP; opaque black then avoids 8000h.
	const std::vector<uint8_t> glass = rgba_image(8, 8, [](int x, int) -> uint32_t {
		return x < 4 ? 0x80FF0000u : 0xFF000000u;
	});
	opt.alpha_mode = ALPHA_BLEND;
	CHECK(convert_texture(glass.data(), 8, 8, opt, t));
	CHECK(t.has_semi);
	CHECK_EQ(t.clut[texel_index(t, 0, 0)], 0xFC00); // Blue, STP
	CHECK_EQ(t.clut[texel_index(t, 7, 0)], 0x0421);

	// Direct 15-bit.
	opt.depth = DEPTH_FORCE_15BIT;
	opt.alpha_mode = ALPHA_IGNORE;
	CHECK(convert_texture(quad.data(), 16, 16, opt, t));
	CHECK_EQ(t.depth, TEXTURE_15BIT);
	CHECK(t.clut.empty());
	CHECK_EQ(texel_index(t, 4, 0), 0x001F);
	CHECK_EQ(texel_index(t, 1, 0), 0x8000);

	CHECK(!convert_texture(nullptr, 0, 0, opt, t));
}

static void test_baked_textures() {
	// Round trip: every variant comes back exactly as converted.
	const std::vector<uint8_t> img = rgba_image(20, 12, [](int x, int y) -> uint32_t {
		const uint32_t a = x < 3 ? 0x00u : (x < 6 ? 0x80u : 0xFFu);
		return (a << 24) | (uint32_t)(x * 12) | ((uint32_t)(y * 20) << 8) | ((uint32_t)((x ^ y) * 9 & 255) << 16);
	});
	std::vector<BakedTexture> baked(4);
	const uint8_t modes[4] = { ALPHA_IGNORE, ALPHA_CUT, ALPHA_BLEND, ALPHA_IGNORE };
	for (int i = 0; i < 4; i++) {
		TextureOptions opt;
		opt.alpha_mode = modes[i];
		opt.alpha_cut = 0.4f;
		opt.depth = i == 3 ? DEPTH_FORCE_15BIT : DEPTH_AUTO;
		baked[i].alpha_mode = modes[i];
		baked[i].alpha_cut = 0.4f;
		CHECK(convert_texture(img.data(), 20, 12, opt, baked[i].data));
	}
	std::vector<uint8_t> bytes;
	write_baked_textures(baked, bytes);
	std::vector<BakedTexture> back;
	CHECK(read_baked_textures(bytes.data(), bytes.size(), back));
	CHECK_EQ((int)back.size(), 4);
	for (int i = 0; i < 4 && i < (int)back.size(); i++) {
		const TextureData &a = baked[i].data, &b = back[i].data;
		CHECK_EQ(back[i].alpha_mode, baked[i].alpha_mode);
		CHECK(back[i].alpha_cut == 0.4f);
		CHECK_EQ(b.width, a.width);
		CHECK_EQ(b.height, a.height);
		CHECK_EQ(b.depth, a.depth);
		CHECK(b.has_transparent == a.has_transparent);
		CHECK(b.has_semi == a.has_semi);
		CHECK_EQ(b.colors, a.colors);
		CHECK(b.mean[0] == a.mean[0] && b.mean[1] == a.mean[1] && b.mean[2] == a.mean[2]);
		CHECK(b.clut == a.clut);
		CHECK(b.clut_semi == a.clut_semi);
		CHECK(b.clut_white == a.clut_white);
		CHECK(b.texels == a.texels);
	}

	// Cut short, wrong magic or a bad size: refused, nothing returned.
	CHECK(!read_baked_textures(bytes.data(), bytes.size() - 1, back));
	CHECK(back.empty());
	std::vector<uint8_t> bad = bytes;
	bad[0] ^= 1;
	CHECK(!read_baked_textures(bad.data(), bad.size(), back));
	bad = bytes;
	bad[17] = 7; // First texture's width: 7 is not a power of two
	CHECK(!read_baked_textures(bad.data(), bad.size(), back));
	CHECK(!read_baked_textures(nullptr, 0, back));

	// Decoding: transparent texels have alpha 0, STP texels 128 when asked.
	std::vector<uint8_t> rgba;
	const TextureData &blend = baked[2].data;
	decode_texture(blend, true, rgba);
	CHECK_EQ((int)rgba.size(), blend.width * blend.height * 4);
	CHECK_EQ(rgba[3], 0); // x = 0: alpha 0
	const int semi_x = 5 * blend.width / 20; // Source x = 5: half alpha
	CHECK_EQ(rgba[semi_x * 4 + 3], 128);
	CHECK_EQ(rgba[(blend.width - 1) * 4 + 3], 255);
	// 15-bit decodes the texel itself: 5 bits widened to 8.
	const TextureData &direct = baked[3].data;
	decode_texture(direct, false, rgba);
	const uint16_t c = direct.texels[direct.width - 1];
	CHECK_EQ(rgba[(direct.width - 1) * 4 + 0], (uint8_t)(((c & 31) << 3) | ((c & 31) >> 2)));
	CHECK_EQ(rgba[(direct.width - 1) * 4 + 3], 255);
}

static void test_vram_allocator() {
	VramAllocator a;
	a.reserve(0, 0, 320, 480); // Display buffers
	struct Box {
		int32_t x, y, w, h;
	};
	std::vector<Box> boxes;
	const int32_t sizes[][2] = { { 128, 256 }, { 64, 64 }, { 4, 16 }, { 2, 8 }, { 16, 32 }, { 128, 256 } };
	bool aligned = true, outside = true;
	for (const auto &s : sizes) {
		Box b = { 0, 0, s[0], s[1] };
		CHECK(a.alloc(s[0], s[1], b.x, b.y));
		aligned = aligned && b.x % s[0] == 0 && b.y % s[1] == 0;
		outside = outside && (b.x >= 320 || b.y >= 480);
		boxes.push_back(b);
	}
	CHECK(aligned);
	CHECK(outside);
	bool overlap = false;
	for (size_t i = 0; i < boxes.size(); i++) {
		for (size_t k = i + 1; k < boxes.size(); k++) {
			const Box &p = boxes[i], &q = boxes[k];
			overlap = overlap || (p.x < q.x + q.w && q.x < p.x + p.w && p.y < q.y + q.h && q.y < p.y + p.h);
		}
	}
	CHECK(!overlap);
	// Fill up with 8-bit 256 x 256 boxes, then free one and reuse it.
	int32_t x = 0, y = 0, last_x = -1, last_y = -1, count = 0;
	while (a.alloc(128, 256, x, y)) {
		last_x = x;
		last_y = y;
		count++;
	}
	CHECK(count >= 1);
	a.free(last_x, last_y, 128, 256);
	CHECK(a.alloc(128, 256, x, y));
	CHECK_EQ(x, last_x);
	CHECK_EQ(y, last_y);
	CHECK(!a.alloc(512, 8, x, y)); // Wider than a texture can be

	// CLUTs: X in 16-halfword steps; a block is freed when empty.
	VramAllocator c;
	const uint32_t before = c.free_cells();
	int32_t cx[3], cy[3];
	CHECK(c.alloc_clut(16, cx[0], cy[0]));
	CHECK(c.alloc_clut(256, cx[1], cy[1]));
	CHECK(c.alloc_clut(16, cx[2], cy[2]));
	CHECK(cx[0] % 16 == 0 && cx[1] % 16 == 0 && cx[2] % 16 == 0);
	CHECK(!(cx[0] == cx[2] && cy[0] == cy[2]));
	CHECK(cy[1] != cy[0]); // A 256-entry CLUT takes a whole row of its block.
	for (int i = 0; i < 3; i++) {
		c.free_clut(cx[i], cy[i], i == 1 ? 256 : 16);
	}
	CHECK_EQ(c.free_cells(), before);
}

static void test_textured_mesh() {
	// A quad with UVs 0..1, then 0..3 (repeat), on a 32 x 32 texture.
	const float pos[] = { -400, -400, 0, 400, -400, 0, -400, 400, 0, 400, 400, 0 };
	const float nrm[] = { 0, 0, -1, 0, 0, -1, 0, 0, -1, 0, 0, -1 };
	const float uv1[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
	const float uv3[] = { 0, 0, 3, 0, 0, 3, 3, 3 };
	const int32_t idx[] = { 0, 1, 2, 1, 3, 2 };
	MeshInput in;
	in.positions = pos;
	in.normals = nrm;
	in.uvs = uv1;
	in.vertex_count = 4;
	in.indices = idx;
	in.index_count = 6;
	ConvertOptions opt;
	opt.scale = 1.0f;
	opt.texture_width = 32;
	opt.texture_height = 32;
	MeshData m;
	CHECK(convert_mesh(in, opt, m));
	CHECK(m.textured);
	CHECK_EQ(m.triangle_count(), 2);
	CHECK(m.tri_window.empty());
	CHECK_EQ(m.tri_uvs[2], 31); // Corner 1 of triangle 0: u = 1.0 -> 31
	CHECK_EQ(m.tri_uvs[5], 31); // Corner 2: v = 1.0 -> 31

	in.uvs = uv3;
	MeshData rep;
	CHECK(convert_mesh(in, opt, rep));
	CHECK_EQ(rep.triangle_count(), 2);
	CHECK_EQ(rep.tri_window.size(), 2);
	CHECK_EQ(rep.tri_uvs[2], 96); // 3 tiles of 32, through the window

	// Not repeating: clamped.
	opt.texture_repeat = false;
	MeshData clamp;
	CHECK(convert_mesh(in, opt, clamp));
	CHECK(clamp.tri_window.empty());
	CHECK_EQ(clamp.tri_uvs[2], 31);
	opt.texture_repeat = true;

	// A 256 texture repeated twice cannot use 8-bit UVs: split at the tile
	// edge, every piece inside one tile, nothing windowed.
	const float uv2[] = { 0, 0, 2, 0, 0, 2, 2, 2 };
	in.uvs = uv2;
	opt.texture_width = 256;
	opt.texture_height = 256;
	MeshData split;
	CHECK(convert_mesh(in, opt, split));
	CHECK(split.triangle_count() > 4);
	CHECK(split.tri_window.empty());
	// UV offsets (uv1_offset) move whole tiles: same UVs as without.
	opt.uv_offset[0] = 5.0f;
	MeshData moved;
	CHECK(convert_mesh(in, opt, moved));
	CHECK(moved.tri_uvs == split.tri_uvs);
	opt.uv_offset[0] = 0.0f;

	// Draw: an 8 x 8 checker of white and red, 4-bit, at (640, 0), lit at
	// 0.5 (PS1 mode): white 128 * 0.5 = 64 modulation -> 31 * 64 / 128 = 15.
	const std::vector<uint8_t> checker = rgba_image(8, 8, [](int x, int y) -> uint32_t {
		return ((x + y) & 1) ? 0xFF0000FFu : 0xFFFFFFFFu;
	});
	TextureOptions topt;
	TextureData tex;
	CHECK(convert_texture(checker.data(), 8, 8, topt, tex));
	in.uvs = uv1;
	opt.texture_width = tex.width;
	opt.texture_height = tex.height;
	opt.max_edge = 0.0f;
	MeshData quad;
	CHECK(convert_mesh(in, opt, quad));

	GPU g;
	VramAllocator alloc;
	alloc.reserve(0, 0, 320, 240);
	int32_t tx, ty, clx, cly;
	CHECK(alloc.alloc(tex.hw_width(), tex.height, tx, ty));
	CHECK(alloc.alloc_clut(16, clx, cly));
	upload_rect(g, tx, ty, tex.hw_width(), tex.height, tex.texels.data());
	upload_rect(g, clx, cly, 16, 1, tex.clut.data());
	const DrawTexture dt = make_draw_texture(tex, tx, ty, clx, cly, -1, 0, -1, 0, BLEND_HALF, false);

	auto draw = [&](const MeshData &p_mesh, DrawStats &r_stats, const DrawTexture *p_tex = nullptr, bool p_fog = false, bool p_dither = false) {
		PacketArena arena(1u << 12);
		PacketBuilder pb(arena, g.get_encoder());
		OrderingTable ot(arena, 256);
		ot.clear();
		GTE gte = basic_gte();
		gte.translation = { 0, 0, 1000 };
		gte.ambient = { 2048, 2048, 2048 };
		DrawParams p;
		p.dither = p_dither;
		p.frame_dither = false; // The environment below turns it off
		p.texture = p_tex ? p_tex : &dt;
		if (p_fog) {
			// Full fog everywhere (IR0 = DQB = 1.0), toward (0, 128, 255).
			p.fog = true;
			gte.dqa = 0;
			gte.dqb = 0x1000000;
			gte.far_color = { 0, 128 * 16, 255 * 16 };
		}
		ot.add(255, pb.fill(0, 0, 320, 240, 0, 0, 0));
		ot.add(255, pb.environment(0, 0, 319, 239, 0, 0, GP0Encoder::texpage(0, 0, 0, 0, false)));
		draw_mesh(gte, pb, ot, p_mesh, p, r_stats);
		ot.draw(g);
	};
	DrawStats s;
	draw(quad, s);
	CHECK_EQ(s.textured, 2);
	CHECK_EQ(s.windowed, 0);
	// 8 texels over ~205 pixels: about 25 pixels per texel.
	const uint16_t a = g.vram_get(60, 20), b = g.vram_get(60 + 26, 20);
	CHECK(a == 15 || a == (15 | 15 << 5 | 15 << 10));
	CHECK(b == 15 || b == (15 | 15 << 5 | 15 << 10));
	CHECK(a != b);

	// Repeated 3 times: the window keeps sampling inside the texture, so
	// every pixel is still white or red at half light.
	in.uvs = uv3;
	MeshData quad3;
	CHECK(convert_mesh(in, opt, quad3));
	CHECK(!quad3.tri_window.empty());
	DrawStats s3;
	draw(quad3, s3);
	CHECK_EQ(s3.windowed, 2);
	bool only_texture = true;
	int changes = 0;
	for (int x = 70; x < 250; x++) {
		const uint16_t v = g.vram_get(x, 120);
		only_texture = only_texture && (v == 15 || v == (15 | 15 << 5 | 15 << 10));
		changes += x > 70 && v != g.vram_get(x - 1, 120);
	}
	CHECK(only_texture);
	CHECK(changes >= 18); // 24 texel edges across the quad, a few off-screen
	CHECK_EQ(g.get_state().window_and_x, 0xFF); // Window reset after each triangle

	// Texture bank 2 (an extension): the same texture drawn from there.
	CHECK_EQ(g.get_texture_bank_count(), 1);
	upload_rect(g, 0, 0, tex.hw_width(), tex.height, tex.texels.data(), 2);
	upload_rect(g, 0, 8, 16, 1, tex.clut.data(), 2);
	CHECK_EQ(g.get_texture_bank_count(), 3);
	CHECK_EQ(g.bank_get(2, 1, 8), tex.clut[1]);
	CHECK_EQ(g.get_state().texture_bank, 0);
	const DrawTexture banked = make_draw_texture(tex, 0, 0, 0, 8, -1, 0, -1, 0, BLEND_HALF, false, 2);
	DrawStats sb;
	draw(quad, sb, &banked);
	CHECK_EQ(sb.banked, 2);
	CHECK_EQ(g.vram_get(60, 20), a);
	CHECK_EQ(g.vram_get(60 + 26, 20), b);
	CHECK_EQ(g.get_state().texture_bank, 0); // Back to VRAM after each triangle

	// Dither is draw-mode state: a mesh whose dither differs from the
	// frame's (a PSXMaterial3D override) switches it around each triangle
	// and restores it. A mid-grey texel at half light (about 64 of 255)
	// sits on a 5-bit step, so dithered pixels vary over 4 x 4.
	const std::vector<uint8_t> grey = rgba_image(8, 8, [](int, int) -> uint32_t { return 0xFF808080u; });
	TextureData gtex;
	CHECK(convert_texture(grey.data(), 8, 8, topt, gtex));
	int32_t gx, gy, gcx, gcy;
	CHECK(alloc.alloc(gtex.hw_width(), gtex.height, gx, gy));
	CHECK(alloc.alloc_clut(16, gcx, gcy));
	upload_rect(g, gx, gy, gtex.hw_width(), gtex.height, gtex.texels.data());
	upload_rect(g, gcx, gcy, 16, 1, gtex.clut.data());
	const DrawTexture dt_grey = make_draw_texture(gtex, gx, gy, gcx, gcy, -1, 0, -1, 0, BLEND_HALF, false);
	auto uniform_block = [&]() {
		const uint16_t first = g.vram_get(60, 20);
		bool same = true;
		for (int y = 20; y < 24; y++) {
			for (int x = 60; x < 64; x++) {
				same = same && g.vram_get(x, y) == first;
			}
		}
		return same;
	};
	DrawStats sd;
	draw(quad, sd, &dt_grey, false, false);
	CHECK(uniform_block());
	draw(quad, sd, &dt_grey, false, true);
	CHECK(!uniform_block());
	CHECK(!g.get_state().dither); // The frame's, restored
	// The texture's own texpage bit does not dither (polygons ignore it).
	const DrawTexture dt_dither = make_draw_texture(gtex, gx, gy, gcx, gcy, -1, 0, -1, 0, BLEND_HALF, true);
	draw(quad, sd, &dt_dither, false, false);
	CHECK(uniform_block());

	// Textured fog: the texel fades to black and the fog colour is added,
	// so full fog is the fog colour: (0, 128, 255) -> (0, 16, 31).
	DrawStats sf;
	draw(quad, sf, nullptr, true);
	CHECK_EQ(sf.fog_passes, 2);
	CHECK_EQ(g.vram_get(60, 20), (uint16_t)(16 << 5 | 31 << 10));
	CHECK_EQ(g.vram_get(60 + 26, 20), (uint16_t)(16 << 5 | 31 << 10));
}

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
		{ "gte_lighting", test_gte_lighting },
		{ "convert_mesh", test_convert_mesh },
		{ "draw_mesh", test_draw_mesh },
		{ "draw_mesh_features", test_draw_mesh_features },
		{ "subdivide_and_point_lights", test_subdivide_and_point_lights },
		{ "convert_texture", test_convert_texture },
		{ "baked_textures", test_baked_textures },
		{ "vram_allocator", test_vram_allocator },
		{ "textured_mesh", test_textured_mesh },
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
