// Integer helpers shared by every psx_core module.
//
// psx_core has no Godot dependency and no floating point in the frame path:
// the same inputs give the same output bits on every compiler and platform.
// Right shifts of signed values are arithmetic (C++20 guarantees it, and every
// supported C++17 compiler already does it); nothing relies on signed overflow.

#pragma once

#include <cstdint>

namespace psx {

template <class T>
constexpr T clamp_value(T p_value, T p_min, T p_max) {
	return p_value < p_min ? p_min : (p_value > p_max ? p_max : p_value);
}

constexpr int32_t min_value(int32_t p_a, int32_t p_b) {
	return p_a < p_b ? p_a : p_b;
}

constexpr int32_t max_value(int32_t p_a, int32_t p_b) {
	return p_a > p_b ? p_a : p_b;
}

// Sign-extend the low p_bits bits of p_value.
constexpr int32_t sign_extend(uint32_t p_value, int p_bits) {
	const uint32_t m = 1u << (p_bits - 1);
	const uint32_t v = p_value & ((1u << p_bits) - 1u);
	return (int32_t)(v ^ m) - (int32_t)m;
}

// Floor division for a positive divisor.
constexpr int32_t floor_div(int32_t p_a, int32_t p_b) {
	return p_a >= 0 ? p_a / p_b : -((-p_a + p_b - 1) / p_b);
}

// 15-bit framebuffer pixel: bits 0-4 red, 5-9 green, 10-14 blue, 15 mask.
constexpr uint16_t rgb15(uint32_t p_r5, uint32_t p_g5, uint32_t p_b5) {
	return (uint16_t)((p_r5 & 31u) | ((p_g5 & 31u) << 5) | ((p_b5 & 31u) << 10));
}

// 24-bit colour word as used by GPU commands: bits 0-7 R, 8-15 G, 16-23 B.
constexpr uint32_t rgb24(uint32_t p_r, uint32_t p_g, uint32_t p_b) {
	return (p_r & 0xFFu) | ((p_g & 0xFFu) << 8) | ((p_b & 0xFFu) << 16);
}

// Host RGBA8 (bytes R, G, B, A in memory on little-endian) from a 15-bit pixel,
// with exact bit expansion c5 -> (c5 << 3) | (c5 >> 2).
constexpr uint32_t rgba8_from_rgb15(uint16_t p_pixel) {
	const uint32_t r = p_pixel & 31u, g = (p_pixel >> 5) & 31u, b = (p_pixel >> 10) & 31u;
	return ((r << 3) | (r >> 2)) | (((g << 3) | (g >> 2)) << 8) | (((b << 3) | (b >> 2)) << 16) | 0xFF000000u;
}

} // namespace psx
