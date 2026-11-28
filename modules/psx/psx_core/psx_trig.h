// Fixed-point trigonometry on the PlayStation angle format (4096 = 360 degrees),
// returning 1.3.12 values (4096 = 1.0). Table-based, no floating point.

#pragma once

#include <cstdint>

namespace psx {

constexpr int32_t ONE = 4096; // 1.0 in 1.3.12
constexpr int32_t ANGLE_FULL = 4096; // 360 degrees

int32_t rsin(int32_t p_angle);
int32_t rcos(int32_t p_angle);

} // namespace psx
