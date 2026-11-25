// A second GTE, written separately from psx_core/psx_gte.cpp for the unit
// tests to check it against (conformance level L2, design doc section 18).
// It follows the command descriptions in psx-spx (GTE chapter) step by step,
// in the order emulators run them, with its own register file, its own UNR
// table and its own 44-bit arithmetic, so that a slip in one is unlikely to
// be repeated in the other. Not used by the renderer.

#pragma once

#include <cstdint>

namespace gte_ref {

// The published UNR seed table, first and last entries (psx-spx, "GTE
// Division Inaccuracy"); the rest follow the formula.
static const uint8_t UNR_FIRST[16] = { 0xFF, 0xFD, 0xFB, 0xF9, 0xF7, 0xF5, 0xF3, 0xF1, 0xEF, 0xEE, 0xEC, 0xEA, 0xE8, 0xE6, 0xE4, 0xE3 };
static const uint8_t UNR_LAST[4] = { 0x01, 0x00, 0x00, 0x00 }; // Entries 253 .. 256

inline uint8_t unr_entry(int p_index) {
	const int v = ((0x40000 / (p_index + 0x100)) + 1) / 2 - 0x101;
	return (uint8_t)(v > 0 ? v : 0);
}

// H / SZ3 as the hardware computes it; r_flag gets bit 17 on overflow.
inline uint32_t divide(uint32_t p_h, uint32_t p_sz3, uint32_t &r_flag) {
	if (!(p_h < p_sz3 * 2)) {
		r_flag |= 1u << 17;
		return 0x1FFFF;
	}
	int z = 0;
	while (((p_sz3 << z) & 0x8000) == 0) {
		z++;
	}
	const int64_t n = (int64_t)p_h << z;
	int64_t d = (int64_t)p_sz3 << z;
	const int64_t u = unr_entry((int)((d - 0x7FC0) >> 7)) + 0x101;
	d = (0x2000080 - d * u) >> 8;
	d = (0x0000080 + d * u) >> 8;
	const int64_t q = (n * d + 0x8000) >> 16;
	return (uint32_t)(q < 0x1FFFF ? q : 0x1FFFF);
}

struct Regs {
	// Control
	int16_t rt[3][3] = {};
	int32_t tr[3] = {};
	int16_t llm[3][3] = {};
	int16_t lcm[3][3] = {};
	int32_t bk[3] = {};
	int32_t fc[3] = {};
	int32_t ofx = 0, ofy = 0;
	uint16_t h = 0;
	int16_t dqa = 0;
	int32_t dqb = 0;
	int16_t zsf3 = 0, zsf4 = 0;
	// Data
	int16_t v[3][3] = {};
	uint8_t rgbc[4] = {};
	int32_t mac[4] = {};
	int16_t ir[4] = {};
	uint16_t sz[4] = {};
	int16_t sx[3] = {}, sy[3] = {};
	uint8_t rgb[3][4] = {};
	uint16_t otz = 0;
	uint32_t flag = 0;
};

class Machine {
public:
	Regs r;

	void rtps(bool p_sf, bool p_lm) {
		start();
		rtp(0, p_sf, p_lm, true);
		finish();
	}
	void rtpt(bool p_sf, bool p_lm) {
		start();
		rtp(0, p_sf, p_lm, false);
		rtp(1, p_sf, p_lm, false);
		rtp(2, p_sf, p_lm, true);
		finish();
	}
	void nclip() {
		start();
		const int64_t s = (int64_t)r.sx[0] * r.sy[1] + (int64_t)r.sx[1] * r.sy[2] + (int64_t)r.sx[2] * r.sy[0] - (int64_t)r.sx[0] * r.sy[2] - (int64_t)r.sx[1] * r.sy[0] - (int64_t)r.sx[2] * r.sy[1];
		set_mac0(s);
		finish();
	}
	void avsz3() {
		start();
		const int64_t s = (int64_t)r.zsf3 * ((int64_t)r.sz[1] + r.sz[2] + r.sz[3]);
		set_mac0(s);
		set_otz(s >> 12);
		finish();
	}
	void avsz4() {
		start();
		const int64_t s = (int64_t)r.zsf4 * ((int64_t)r.sz[0] + r.sz[1] + r.sz[2] + r.sz[3]);
		set_mac0(s);
		set_otz(s >> 12);
		finish();
	}
	// p_kind: 0 NCS, 1 NCCS, 2 NCDS.
	void nc(int p_kind, bool p_triple, bool p_sf, bool p_lm) {
		start();
		for (int i = 0; i < (p_triple ? 3 : 1); i++) {
			nc_vertex(i, p_kind, p_sf, p_lm);
		}
		finish();
	}
	void dpcs(bool p_sf, bool p_lm) {
		start();
		const int64_t in[3] = { (int64_t)r.rgbc[0] << 16, (int64_t)r.rgbc[1] << 16, (int64_t)r.rgbc[2] << 16 };
		interpolate(in, p_sf ? 12 : 0, p_lm);
		push_rgb();
		finish();
	}

private:
	void start() { r.flag = 0; }
	void finish() {
		// Bit 31 summarises bits 30..23 and 18..13 (not the colour or IR0 bits).
		const uint32_t summary = (0xFFu << 23) | (0x3Fu << 13);
		if (r.flag & summary & ~(1u << 31)) {
			r.flag |= 1u << 31;
		}
	}

	// A MAC1..3 intermediate: flags 44-bit overflow and wraps to 44 bits.
	int64_t a44(int p_index, int64_t p_value) {
		const int64_t limit = (int64_t)1 << 43;
		if (p_value >= limit) {
			r.flag |= 1u << (31 - p_index);
		} else if (p_value < -limit) {
			r.flag |= 1u << (28 - p_index);
		}
		const int64_t span = limit * 2;
		int64_t w = (p_value + limit) % span;
		if (w < 0) {
			w += span;
		}
		return w - limit;
	}

	int16_t clamp_ir(int p_index, int32_t p_value, bool p_lm) {
		const int32_t lo = p_lm ? 0 : -0x8000;
		if (p_value < lo) {
			r.flag |= 1u << (25 - p_index);
			return (int16_t)lo;
		}
		if (p_value > 0x7FFF) {
			r.flag |= 1u << (25 - p_index);
			return 0x7FFF;
		}
		return (int16_t)p_value;
	}

	// MACi = value SAR shift (as a 32-bit register), IRi = MACi saturated.
	void set_mac_ir(int p_index, int64_t p_value, int p_shift, bool p_lm) {
		const int64_t checked = a44(p_index, p_value);
		r.mac[p_index] = (int32_t)(uint32_t)(uint64_t)(checked >> p_shift);
		r.ir[p_index] = clamp_ir(p_index, r.mac[p_index], p_lm);
	}

	void set_mac0(int64_t p_value) {
		if (p_value > INT64_C(0x7FFFFFFF)) {
			r.flag |= 1u << 16;
		} else if (p_value < -INT64_C(0x80000000)) {
			r.flag |= 1u << 15;
		}
		r.mac[0] = (int32_t)(uint32_t)(uint64_t)p_value;
	}

	void set_otz(int64_t p_value) {
		if (p_value < 0) {
			r.flag |= 1u << 18;
			r.otz = 0;
		} else if (p_value > 0xFFFF) {
			r.flag |= 1u << 18;
			r.otz = 0xFFFF;
		} else {
			r.otz = (uint16_t)p_value;
		}
	}

	// MAC0 checked, then SAR 16 and saturated to -400h..3FFh (bit 14 or 13).
	int16_t screen(int64_t p_mac0, int p_bit) {
		if (p_mac0 > INT64_C(0x7FFFFFFF)) {
			r.flag |= 1u << 16;
		} else if (p_mac0 < -INT64_C(0x80000000)) {
			r.flag |= 1u << 15;
		}
		const int32_t s = (int32_t)(p_mac0 >> 16);
		if (s < -0x400) {
			r.flag |= 1u << p_bit;
			return -0x400;
		}
		if (s > 0x3FF) {
			r.flag |= 1u << p_bit;
			return 0x3FF;
		}
		return (int16_t)s;
	}

	void rtp(int p_v, bool p_sf, bool p_lm, bool p_last) {
		const int shift = p_sf ? 12 : 0;
		const int16_t *vv = r.v[p_v];
		int64_t sum[3];
		for (int i = 0; i < 3; i++) {
			int64_t s = a44(i + 1, ((int64_t)r.tr[i] << 12) + (int64_t)r.rt[i][0] * vv[0]);
			s = a44(i + 1, s + (int64_t)r.rt[i][1] * vv[1]);
			s = a44(i + 1, s + (int64_t)r.rt[i][2] * vv[2]);
			sum[i] = s;
		}
		set_mac_ir(1, sum[0], shift, p_lm);
		set_mac_ir(2, sum[1], shift, p_lm);
		// IR3: saturated from MAC3 like the others, but its flag is decided
		// by sum SAR 12, whatever sf and lm are (psx-spx, RTPS notes).
		r.mac[3] = (int32_t)(uint32_t)(uint64_t)(sum[2] >> shift);
		{
			const uint32_t keep = r.flag;
			r.ir[3] = clamp_ir(3, r.mac[3], p_lm);
			r.flag = keep;
			const int64_t t = sum[2] >> 12;
			if (t < -0x8000 || t > 0x7FFF) {
				r.flag |= 1u << 22;
			}
		}
		// SZ3 = sum SAR 12, saturated to 0..FFFFh.
		const int32_t z = (int32_t)(sum[2] >> 12);
		r.sz[0] = r.sz[1];
		r.sz[1] = r.sz[2];
		r.sz[2] = r.sz[3];
		if (z < 0) {
			r.flag |= 1u << 18;
			r.sz[3] = 0;
		} else if (z > 0xFFFF) {
			r.flag |= 1u << 18;
			r.sz[3] = 0xFFFF;
		} else {
			r.sz[3] = (uint16_t)z;
		}
		const int64_t n = divide(r.h, r.sz[3], r.flag);
		const int16_t x = screen(n * r.ir[1] + r.ofx, 14);
		const int16_t y = screen(n * r.ir[2] + r.ofy, 13);
		r.sx[0] = r.sx[1];
		r.sx[1] = r.sx[2];
		r.sx[2] = x;
		r.sy[0] = r.sy[1];
		r.sy[1] = r.sy[2];
		r.sy[2] = y;
		if (p_last) {
			const int64_t d = n * r.dqa + r.dqb;
			set_mac0(d);
			const int64_t i0 = d >> 12;
			if (i0 < 0) {
				r.flag |= 1u << 12;
				r.ir[0] = 0;
			} else if (i0 > 0x1000) {
				r.flag |= 1u << 12;
				r.ir[0] = 0x1000;
			} else {
				r.ir[0] = (int16_t)i0;
			}
		}
	}

	// Colour FIFO <- MAC1..3 / 16, saturated to 0..FFh (bits 21..19).
	void push_rgb() {
		for (int k = 0; k < 4; k++) {
			r.rgb[0][k] = r.rgb[1][k];
			r.rgb[1][k] = r.rgb[2][k];
		}
		for (int i = 0; i < 3; i++) {
			const int32_t c = r.mac[i + 1] >> 4;
			if (c < 0) {
				r.flag |= 1u << (21 - i);
				r.rgb[2][i] = 0;
			} else if (c > 0xFF) {
				r.flag |= 1u << (21 - i);
				r.rgb[2][i] = 0xFF;
			} else {
				r.rgb[2][i] = (uint8_t)c;
			}
		}
		r.rgb[2][3] = r.rgbc[3];
	}

	// MAC = in + (FC - in) * IR0, the depth cue step of NCDS and DPCS.
	void interpolate(const int64_t p_in[3], int p_shift, bool p_lm) {
		for (int i = 0; i < 3; i++) {
			set_mac_ir(i + 1, ((int64_t)r.fc[i] << 12) - p_in[i], p_shift, false);
		}
		for (int i = 0; i < 3; i++) {
			set_mac_ir(i + 1, (int64_t)r.ir[i + 1] * r.ir[0] + p_in[i], p_shift, p_lm);
		}
	}

	void nc_vertex(int p_v, int p_kind, bool p_sf, bool p_lm) {
		const int shift = p_sf ? 12 : 0;
		const int16_t *n = r.v[p_v];
		for (int i = 0; i < 3; i++) {
			int64_t s = a44(i + 1, (int64_t)r.llm[i][0] * n[0]);
			s = a44(i + 1, s + (int64_t)r.llm[i][1] * n[1]);
			s = a44(i + 1, s + (int64_t)r.llm[i][2] * n[2]);
			set_mac_ir(i + 1, s, shift, p_lm);
		}
		const int64_t level[3] = { r.ir[1], r.ir[2], r.ir[3] };
		for (int i = 0; i < 3; i++) {
			int64_t s = a44(i + 1, ((int64_t)r.bk[i] << 12) + r.lcm[i][0] * level[0]);
			s = a44(i + 1, s + r.lcm[i][1] * level[1]);
			s = a44(i + 1, s + r.lcm[i][2] * level[2]);
			set_mac_ir(i + 1, s, shift, p_lm);
		}
		if (p_kind != 0) {
			int64_t in[3];
			for (int i = 0; i < 3; i++) {
				in[i] = ((int64_t)r.rgbc[i] << 4) * r.ir[i + 1];
			}
			if (p_kind == 2) {
				interpolate(in, shift, p_lm);
			} else {
				for (int i = 0; i < 3; i++) {
					set_mac_ir(i + 1, in[i], shift, p_lm);
				}
			}
		}
		push_rgb();
	}
};

} // namespace gte_ref
