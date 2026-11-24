// Engine layer: draws converted meshes through the GTE into an ordering
// table, the way a PS1 game's object loop did. Per triangle: RTPT, near/far
// rejection, NCLIP culling, AVSZ3 for the OT bucket, then per vertex
// lighting (NCCS, or NCS with linear colour maths), emission and depth-cue
// fog (DPCS), and one F3/G3 packet.
//
// The caller loads the GTE first: rotation (already multiplied by
// 2^mesh.shift), translation, H, OFX/OFY and, for lit meshes, LLM/LCM/BK.

#pragma once

#include "psx_convert.h"
#include "psx_gte.h"
#include "psx_ot.h"

#include <cstdint>

namespace psx {

enum CullMode : uint8_t {
	CULL_BACK,
	CULL_FRONT,
	CULL_NONE,
};

enum LightingMode : uint8_t {
	// PS1: NCCS, the display (sRGB) colour times the light level. Darker
	// midtones than Godot, as on the hardware.
	LIGHTING_PS1,
	// Godot match: the GTE computes the light level (NCS); it multiplies the
	// linear colour on the CPU, so lit surfaces match Godot's brightness.
	LIGHTING_LINEAR,
};

// An omni or spot light evaluated per vertex on the CPU (the GTE's three
// light slots hold directional lights only). Positions and axes are in the
// mesh's stored object space.
struct PointLight {
	float position[3] = { 0, 0, 0 }; // Stored mesh units
	float metres_per_unit = 1.0f; // Size of one stored unit
	float range = 1.0f; // Metres
	float decay = 1.0f; // Godot's attenuation exponent
	float color[3] = { 1, 1, 1 }; // Colour * energy (1.0 = full)
	float spot_axis[3] = { 0, 0, -1 }; // Unit vector the light points along
	float spot_cos = -2.0f; // Cos of the cone half-angle; -2 = omni
};

static constexpr int MAX_POINT_LIGHTS = 4;

struct DrawParams {
	CullMode cull = CULL_BACK;
	bool lit = true; // Lighting on; otherwise the baked colour as-is
	LightingMode lighting = LIGHTING_PS1;
	// Added after lighting, sRGB 0..255 (material emission).
	uint8_t emission[3] = { 0, 0, 0 };
	const PointLight *point_lights = nullptr;
	int point_light_count = 0;
	// Depth cue toward GTE FC, per vertex, using DQA/DQB (DPCS).
	bool fog = false;
	// Transparent material. With BLEND_HALF the mesh's per-triangle alpha
	// classes decide (opaque, 50% or hidden); other modes blend every triangle.
	bool semi = false;
	uint8_t blend = 0; // BlendMode for semi-transparent meshes
	bool dither = true;
	// OTZ = AVSZ3 result >> ot_shift. Triangles nearer than OTZ 1 still land in bucket 0.
	int32_t ot_shift = 4;
	// Triangles whose every vertex is beyond this SZ are dropped (far plane).
	int32_t far_z = 0xFFFF;
	// Orthographic: RT's X/Y rows already map to screen pixels (MVMVA, no
	// divide); OFX/OFY are added on the CPU and SZ is the view depth (MAC3).
	// Vertices nearer than near_z are dropped (the perspective path uses H/2).
	bool ortho = false;
	int32_t near_z = 0;
	// Visible screen rectangle (relative to the drawing offset).
	int32_t screen_width = 320;
	int32_t screen_height = 240;
};

struct DrawStats {
	uint32_t triangles_in = 0;
	uint32_t culled_near = 0;
	uint32_t culled_far = 0;
	uint32_t culled_backface = 0;
	uint32_t culled_offscreen = 0;
	uint32_t culled_alpha = 0; // Alpha below 0.25
	uint32_t packets = 0;
	uint32_t arena_full = 0;

	void add(const DrawStats &p_other);
};

void draw_mesh(GTE &p_gte, PacketBuilder &p_builder, OrderingTable &p_ot, const MeshData &p_mesh, const DrawParams &p_params, DrawStats &r_stats);

} // namespace psx
