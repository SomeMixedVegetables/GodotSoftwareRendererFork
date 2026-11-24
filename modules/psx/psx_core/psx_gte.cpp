// Software GTE implementation, written from the psx-spx GTE documentation.

#include "psx_gte.h"

#include "psx_trig.h"

namespace psx {

Matrix Matrix::identity() {
	Matrix m;
	m.m[0][0] = m.m[1][1] = m.m[2][2] = 4096;
	return m;
}

// The UNR divide's 257-entry seed table: max(0, (40000h / (i + 100h) + 1) / 2 - 101h).
struct UNRTable {
	uint8_t entries[257];
	UNRTable() {
		for (int i = 0; i < 257; i++) {
			const int v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
			entries[i] = (uint8_t)(v < 0 ? 0 : v);
		}
	}
};

static const uint8_t *unr_table() {
	static const UNRTable table; // Thread-safe one-time initialisation.
	return table.entries;
}

static int count_leading_zeros16(uint16_t p_value) {
	int n = 0;
	for (uint32_t bit = 0x8000; bit != 0 && !(p_value & bit); bit >>= 1) {
		n++;
	}
	return n;
}

uint32_t GTE::unr_divide(uint16_t p_h, uint16_t p_sz, bool &r_overflow) {
	if ((uint32_t)p_h >= (uint32_t)p_sz * 2) {
		r_overflow = true;
		return 0x1FFFF;
	}
	r_overflow = false;
	const int z = count_leading_zeros16(p_sz);
	const uint64_t n = (uint64_t)p_h << z;
	uint32_t d = (uint32_t)p_sz << z;
	const uint32_t u = (uint32_t)unr_table()[(d - 0x7FC0) >> 7] + 0x101;
	d = (uint32_t)((0x2000080 - (uint64_t)d * u) >> 8);
	d = (uint32_t)((0x0000080 + (uint64_t)d * u) >> 8);
	const uint64_t q = (n * d + 0x8000) >> 16;
	return q > 0x1FFFF ? 0x1FFFF : (uint32_t)q;
}

uint32_t GTE::ideal_divide(uint16_t p_h, uint16_t p_sz) {
	if (p_sz == 0) {
		return 0x1FFFF;
	}
	const uint64_t q = (((uint64_t)p_h * 0x20000 / p_sz) + 1) / 2;
	return q > 0x1FFFF ? 0x1FFFF : (uint32_t)q;
}

// MAC1-3 are 44-bit signed: flag overflow, then wrap to 44 bits.
int64_t GTE::_check_mac(int p_index, int64_t p_value) {
	if (p_value > INT64_C(0x7FFFFFFFFFF)) {
		flag |= 1u << (31 - p_index);
	} else if (p_value < -INT64_C(0x80000000000)) {
		flag |= 1u << (28 - p_index);
	}
	return (int64_t)((uint64_t)p_value << 20) >> 20;
}

int32_t GTE::_check_mac0(int64_t p_value) {
	if (p_value > INT64_C(0x7FFFFFFF)) {
		flag |= FLAG_MAC0_POS;
	} else if (p_value < -INT64_C(0x80000000)) {
		flag |= FLAG_MAC0_NEG;
	}
	return (int32_t)(uint32_t)(uint64_t)p_value;
}

int16_t GTE::_saturate_ir(int p_index, int64_t p_value, bool p_lm, bool p_set_flag) {
	const int64_t lo = p_lm ? 0 : -0x8000;
	if (p_value < lo || p_value > 0x7FFF) {
		if (p_set_flag) {
			flag |= 1u << (25 - p_index);
		}
		return (int16_t)(p_value < lo ? lo : 0x7FFF);
	}
	return (int16_t)p_value;
}

void GTE::_end() {
	if (flag & 0x7F87E000u) {
		flag |= FLAG_ERROR;
	}
}

void GTE::_rtp(int p_vertex, bool p_sf, bool p_lm, bool p_last) {
	const SVector &vv = v[p_vertex];
	const int shift = p_sf ? 12 : 0;
	const int64_t tr[3] = { translation.x, translation.y, translation.z };
	int64_t raw3 = 0;
	for (int i = 0; i < 3; i++) {
		const int16_t *row = rotation.m[i];
		int64_t acc = _check_mac(i + 1, tr[i] * 4096 + (int64_t)row[0] * vv.x);
		acc = _check_mac(i + 1, acc + (int64_t)row[1] * vv.y);
		acc = _check_mac(i + 1, acc + (int64_t)row[2] * vv.z);
		mac[i + 1] = (int32_t)(acc >> shift);
		if (i < 2) {
			ir[i + 1] = _saturate_ir(i + 1, mac[i + 1], p_lm);
		} else {
			raw3 = acc;
			// IR3 is saturated from MAC3, but its flag tests MAC3 >> 12
			// as if lm = 0 (hardware quirk).
			ir[3] = _saturate_ir(3, mac[3], p_lm, false);
			const int64_t t = acc >> 12;
			if (t < -0x8000 || t > 0x7FFF) {
				flag |= FLAG_IR3_SAT;
			}
		}
	}

	// Screen Z FIFO.
	int64_t z = raw3 >> 12;
	if (z < 0 || z > 0xFFFF) {
		flag |= FLAG_SZ_SAT;
		z = z < 0 ? 0 : 0xFFFF;
	}
	sz[0] = sz[1];
	sz[1] = sz[2];
	sz[2] = sz[3];
	sz[3] = (uint16_t)z;

	bool overflow = false;
	const int64_t n = unr_divide(h, sz[3], overflow);
	if (overflow) {
		flag |= FLAG_DIVIDE;
	}

	const int32_t lo = -0x400 * scale, hi = 0x400 * scale - 1;
	auto screen = [&](int64_t p_offset, int16_t p_ir, uint32_t p_flag) -> int16_t {
		const int64_t m0 = n * p_ir + p_offset;
		_check_mac0(m0);
		int64_t s = m0 >> 16;
		if (s < lo || s > hi) {
			flag |= p_flag;
			s = s < lo ? lo : hi;
		}
		int32_t r = (int32_t)s;
		if (xy_snap > 1) {
			r = floor_div(r, xy_snap) * xy_snap;
		}
		return (int16_t)r;
	};
	const int16_t new_sx = screen(ofx, ir[1], FLAG_SX_SAT);
	const int16_t new_sy = screen(ofy, ir[2], FLAG_SY_SAT);
	sx[0] = sx[1];
	sx[1] = sx[2];
	sx[2] = new_sx;
	sy[0] = sy[1];
	sy[1] = sy[2];
	sy[2] = new_sy;

	if (p_last) {
		const int64_t m0 = n * dqa + dqb;
		mac[0] = _check_mac0(m0);
		int64_t i0 = m0 >> 12;
		if (i0 < 0 || i0 > 0x1000) {
			flag |= FLAG_IR0_SAT;
			i0 = i0 < 0 ? 0 : 0x1000;
		}
		ir[0] = (int16_t)i0;
	}
}

void GTE::rtps(bool p_sf, bool p_lm) {
	_begin();
	_rtp(0, p_sf, p_lm, true);
	_end();
}

void GTE::rtpt(bool p_sf, bool p_lm) {
	_begin();
	_rtp(0, p_sf, p_lm, false);
	_rtp(1, p_sf, p_lm, false);
	_rtp(2, p_sf, p_lm, true);
	_end();
}

void GTE::nclip() {
	_begin();
	const int64_t m0 = (int64_t)sx[0] * sy[1] + (int64_t)sx[1] * sy[2] + (int64_t)sx[2] * sy[0] -
			(int64_t)sx[0] * sy[2] - (int64_t)sx[1] * sy[0] - (int64_t)sx[2] * sy[1];
	mac[0] = _check_mac0(m0);
	_end();
}

static uint16_t saturate_otz(int64_t p_value, uint32_t &r_flag) {
	const int64_t z = p_value >> 12;
	if (z < 0 || z > 0xFFFF) {
		r_flag |= FLAG_SZ_SAT;
		return (uint16_t)(z < 0 ? 0 : 0xFFFF);
	}
	return (uint16_t)z;
}

void GTE::avsz3() {
	_begin();
	const int64_t m0 = (int64_t)zsf3 * ((int64_t)sz[1] + sz[2] + sz[3]);
	mac[0] = _check_mac0(m0);
	otz = saturate_otz(m0, flag);
	_end();
}

void GTE::avsz4() {
	_begin();
	const int64_t m0 = (int64_t)zsf4 * ((int64_t)sz[0] + sz[1] + sz[2] + sz[3]);
	mac[0] = _check_mac0(m0);
	otz = saturate_otz(m0, flag);
	_end();
}

// --- CPU-side helpers ---

Matrix mul_matrix(const Matrix &p_a, const Matrix &p_b) {
	Matrix r;
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			const int64_t s = (int64_t)p_a.m[i][0] * p_b.m[0][j] + (int64_t)p_a.m[i][1] * p_b.m[1][j] + (int64_t)p_a.m[i][2] * p_b.m[2][j];
			r.m[i][j] = (int16_t)clamp_value<int64_t>(s >> 12, -0x8000, 0x7FFF);
		}
	}
	return r;
}

LVector apply_matrix(const Matrix &p_m, const LVector &p_v) {
	LVector r;
	int32_t *out[3] = { &r.x, &r.y, &r.z };
	for (int i = 0; i < 3; i++) {
		const int64_t s = (int64_t)p_m.m[i][0] * p_v.x + (int64_t)p_m.m[i][1] * p_v.y + (int64_t)p_m.m[i][2] * p_v.z;
		*out[i] = (int32_t)(s >> 12);
	}
	return r;
}

Matrix transpose_matrix(const Matrix &p_m) {
	Matrix r;
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			r.m[i][j] = p_m.m[j][i];
		}
	}
	return r;
}

Matrix rot_matrix(int32_t p_x, int32_t p_y, int32_t p_z) {
	const int16_t sx = (int16_t)rsin(p_x), cx = (int16_t)rcos(p_x);
	const int16_t sy = (int16_t)rsin(p_y), cy = (int16_t)rcos(p_y);
	const int16_t sz = (int16_t)rsin(p_z), cz = (int16_t)rcos(p_z);
	Matrix rx = Matrix::identity(), ry = Matrix::identity(), rz = Matrix::identity();
	rx.m[1][1] = cx;
	rx.m[1][2] = (int16_t)-sx;
	rx.m[2][1] = sx;
	rx.m[2][2] = cx;
	ry.m[0][0] = cy;
	ry.m[0][2] = sy;
	ry.m[2][0] = (int16_t)-sy;
	ry.m[2][2] = cy;
	rz.m[0][0] = cz;
	rz.m[0][1] = (int16_t)-sz;
	rz.m[1][0] = sz;
	rz.m[1][1] = cz;
	return mul_matrix(rz, mul_matrix(ry, rx));
}

} // namespace psx
