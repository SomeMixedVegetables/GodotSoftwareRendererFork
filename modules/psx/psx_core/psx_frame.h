// Frame layout and the built-in test scene (a spinning textured cube).
//
// FrameLayout places the display buffers in VRAM, PS1 style: double buffering
// is two rectangles, drawing into one while the other is shown, with no copy.
// At scale 1 the buffers sit at the left of VRAM (as on hardware) and
// textures go to their right; at scale > 1 they go outside the 1024 x 512
// texture region.
//
// DemoScene exercises the whole core in one deterministic frame: CPU->VRAM
// texture uploads, 4/8/15-bit textures and CLUTs, Gouraud and dithering,
// overbright modulation, 0000h cut-outs, semi-transparency, the GTE
// (RTPT/RTPS, NCLIP, AVSZ4), the ordering table, sprites and lines.

#pragma once

#include "psx_gpu.h"
#include "psx_gte.h"
#include "psx_ot.h"

#include <cstdint>

namespace psx {

struct FrameLayout {
	int32_t width = 0;
	int32_t height = 0;
	int buffer_count = 0; // 2 = double-buffered, 1 = single, 0 = does not fit
	int32_t buffer_x[2] = { 0, 0 };
	int32_t buffer_y[2] = { 0, 0 };
	// True when the only place left was the lower part of the texture region
	// (scale > 1, frame too big for the space around it).
	bool overlaps_texture_region = false;

	static FrameLayout make(const GPU &p_gpu, int32_t p_width, int32_t p_height);
};

struct DemoStats {
	uint32_t quads_submitted = 0;
	uint32_t quads_culled = 0;
	uint32_t packets_drawn = 0;
};

class DemoScene {
public:
	// Uploads the test textures and CLUTs into the texture region.
	void upload_textures(GPU &p_gpu);
	// Builds and draws frame p_frame into buffer p_buffer of p_layout, then
	// points the display at it.
	DemoStats render(GPU &p_gpu, const FrameLayout &p_layout, int p_buffer, uint32_t p_frame);

	// Rotation speed in 1/256 units: 256 = normal, 64 = quarter speed.
	uint32_t speed = 256;

	// Texture placement (all inside the 1024 x 512 texture region).
	static constexpr int32_t TEX4_X = 640, TEX4_Y = 0; // 64x64 texels, 16 halfwords wide
	static constexpr int32_t TEX8_X = 704, TEX8_Y = 0; // 64x64 texels, 32 halfwords wide
	static constexpr int32_t TEX15_X = 768, TEX15_Y = 0; // 64x64 texels
	static constexpr int32_t CLUT4_X = 640, CLUT4_Y = 64;
	static constexpr int32_t CLUT8_X = 640, CLUT8_Y = 65;
	// Rows of the texture region the demo uses (textures and CLUTs).
	static constexpr int32_t USED_ROWS = 66;

private:
	PacketArena arena{ 1u << 16 };
};

} // namespace psx
