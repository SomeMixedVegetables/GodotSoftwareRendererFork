// Software GTE: the geometry co-processor as a register file plus fixed
// micro-ops, following the hardware formulas in psx-spx (GTE chapter) and the
// design doc, sections 2-5. Game code loads the rotation/translation once per
// object and streams vertices through RTPS/RTPT, as PS1 code did via libgte.
//
// Number formats: rotation matrix 1.3.12 (4096 = 1.0), translation 1.31.0,
// OFX/OFY 1.15.16, H 0.16.0, DQA 1.7.8, DQB 1.7.24, ZSF3/ZSF4 1.3.12.
//
// Scaled VRAM: with scale N the screen XY saturation range is
// -1024 * N .. 1024 * N - 1. N = 1 is the hardware.

#pragma once

#include "psx_fixed.h"

#include <cstdint>

namespace psx {

struct Matrix {
	int16_t m[3][3] = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };
	static Matrix identity();
};

struct SVector {
	int16_t x = 0, y = 0, z = 0;
};

struct LVector {
	int32_t x = 0, y = 0, z = 0;
};

// FLAG register bits.
enum GTEFlag : uint32_t {
	FLAG_MAC1_POS = 1u << 30,
	FLAG_MAC2_POS = 1u << 29,
	FLAG_MAC3_POS = 1u << 28,
	FLAG_MAC1_NEG = 1u << 27,
	FLAG_MAC2_NEG = 1u << 26,
	FLAG_MAC3_NEG = 1u << 25,
	FLAG_IR1_SAT = 1u << 24,
	FLAG_IR2_SAT = 1u << 23,
	FLAG_IR3_SAT = 1u << 22,
	FLAG_COLOR_R_SAT = 1u << 21,
	FLAG_COLOR_G_SAT = 1u << 20,
	FLAG_COLOR_B_SAT = 1u << 19,
	FLAG_SZ_SAT = 1u << 18, // SZ3 or OTZ
	FLAG_DIVIDE = 1u << 17, // H/SZ overflow: vertex nearer than H/2
	FLAG_MAC0_POS = 1u << 16,
	FLAG_MAC0_NEG = 1u << 15,
	FLAG_SX_SAT = 1u << 14,
	FLAG_SY_SAT = 1u << 13,
	FLAG_IR0_SAT = 1u << 12,
	FLAG_ERROR = 1u << 31, // OR of bits 30-23 and 18-13
};

class GTE {
public:
	// --- Control registers ---
	Matrix rotation; // RT
	LVector translation; // TRX, TRY, TRZ
	int32_t ofx = 0; // Screen offset, 16.16
	int32_t ofy = 0;
	uint16_t h = 256; // Projection distance
	int16_t dqa = 0; // Depth cue slope
	int32_t dqb = 0; // Depth cue intercept
	int16_t zsf3 = 4096 / 3; // AVSZ3 scale (here: true average)
	int16_t zsf4 = 4096 / 4;
	// Lighting: up to three directional lights.
	Matrix light_dirs; // LLM: one light direction per row (object space), 1.3.12
	Matrix light_colors; // LCM: one light colour per column, 1.3.12 (4096 = 1.0)
	LVector ambient; // BK: 1.19.12 (4096 = 1.0)
	LVector far_color; // FC: 1.27.4 (16 = colour value 1)

	// --- Data registers ---
	SVector v[3]; // V0, V1, V2
	int32_t mac[4] = { 0, 0, 0, 0 }; // MAC0 .. MAC3
	int16_t ir[4] = { 0, 0, 0, 0 }; // IR0 .. IR3
	uint16_t sz[4] = { 0, 0, 0, 0 }; // SZ0 .. SZ3 FIFO
	int16_t sx[3] = { 0, 0, 0 }; // SXY0 .. SXY2 FIFO
	int16_t sy[3] = { 0, 0, 0 };
	uint16_t otz = 0;
	uint32_t flag = 0;
	struct Color {
		uint8_t r = 0, g = 0, b = 0, code = 0;
	};
	Color rgbc; // Input colour for NCCS/NCDS (vertex/material colour)
	Color rgb[3]; // Colour FIFO RGB0 .. RGB2

	// --- Options (not hardware) ---
	int scale = 1; // Scaled VRAM factor: widens the SX/SY range.
	int xy_snap = 1; // Snap SX/SY down to multiples of this (chunky jitter at high res).

	// Rotate, translate and perspective-project V0 (RTPS) or V0-V2 (RTPT).
	// p_sf: shift results right by 12 (almost always on). p_lm: clamp IR to 0..7FFFh.
	void rtps(bool p_sf = true, bool p_lm = false);
	void rtpt(bool p_sf = true, bool p_lm = false);
	// Signed 2D area of SXY0-2 into MAC0 (positive = one winding, 0 = degenerate).
	void nclip();
	// Average Z of SZ1-3 (AVSZ3) or SZ0-3 (AVSZ4), scaled by ZSF3/ZSF4, into OTZ.
	void avsz3();
	void avsz4();
	// Normal-colour lighting on normal V0 (or V0-V2 for the T variants), all
	// pushing results into the colour FIFO (design doc section 6):
	//   NCS:  ambient + lights, as a colour (1.0 = 255)
	//   NCCS: ... multiplied by RGBC (the vertex/material colour)
	//   NCDS: ... then blended toward FC by IR0 (depth cue / fog)
	// p_lm defaults on: negative light (facing away) clamps to 0.
	void ncs(bool p_sf = true, bool p_lm = true);
	void nccs(bool p_sf = true, bool p_lm = true);
	void ncds(bool p_sf = true, bool p_lm = true);
	void nct(bool p_sf = true, bool p_lm = true);
	void ncct(bool p_sf = true, bool p_lm = true);
	void ncdt(bool p_sf = true, bool p_lm = true);
	// MVMVA with RT, V[p_vertex] and TR: rotate and translate without the
	// perspective divide (MAC1-3 / IR1-3). Used for orthographic views.
	void mvmva_rt(int p_vertex, bool p_sf = true, bool p_lm = false);
	// Depth cue RGBC toward FC by IR0 (no lighting).
	void dpcs(bool p_sf = true, bool p_lm = false);

	// Unsigned Newton-Raphson reciprocal: returns H/SZ in 1.16 (0..1FFFFh)
	// exactly as the hardware does; sets r_overflow when SZ <= H/2.
	static uint32_t unr_divide(uint16_t p_h, uint16_t p_sz, bool &r_overflow);
	// The ideal ((H * 20000h / SZ) + 1) / 2, for reference.
	static uint32_t ideal_divide(uint16_t p_h, uint16_t p_sz);

private:
	void _begin() { flag = 0; }
	void _end();
	int64_t _check_mac(int p_index, int64_t p_value);
	int32_t _check_mac0(int64_t p_value);
	int16_t _saturate_ir(int p_index, int64_t p_value, bool p_lm, bool p_set_flag = true);
	void _rtp(int p_vertex, bool p_sf, bool p_lm, bool p_last);
	enum NCMode {
		NC_PLAIN,
		NC_COLOR,
		NC_DEPTH,
	};
	void _nc(int p_vertex, NCMode p_mode, bool p_sf, bool p_lm);
	void _color_tail(int64_t p_mac[3], NCMode p_mode, bool p_sf, bool p_lm);
	void _push_color(const int64_t p_mac[3]);
};

// --- CPU-side matrix helpers (what libgte did on the CPU, not the GTE) ---

// Rotation matrix from angles in 4096-per-turn units, applied X, then Y, then Z
// (R = Rz * Ry * Rx).
Matrix rot_matrix(int32_t p_x, int32_t p_y, int32_t p_z);
// a * b, 1.3.12 entries, each sum of products shifted right by 12 once.
Matrix mul_matrix(const Matrix &p_a, const Matrix &p_b);
// m * v >> 12, for 32-bit vectors (translations).
LVector apply_matrix(const Matrix &p_m, const LVector &p_v);
Matrix transpose_matrix(const Matrix &p_m);

} // namespace psx
