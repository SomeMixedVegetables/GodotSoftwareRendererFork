// Engine layer: draws converted meshes through the GTE into an ordering
// table, the way a PS1 game's object loop did. Per triangle: RTPT, near/far
// rejection, NCLIP culling, AVSZ3 for the OT bucket, then per vertex
// lighting (NCCS, or NCS with linear colour maths), emission and depth-cue
// fog (DPCS), and one F3/G3 packet, or FT3/GT3 for textured meshes.
//
// The caller loads the GTE first: rotation (already multiplied by
// 2^mesh.shift), translation, H, OFX/OFY and, for lit meshes, LLM/LCM/BK.

#pragma once

#include "psx_convert.h"
#include "psx_gte.h"
#include "psx_ot.h"
#include "psx_texture.h"

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
// mesh's stored units, in the frame where vertex positions times
// DrawParams::point_scale are in proportion (object space for uniform scale).
struct PointLight {
	float position[3] = { 0, 0, 0 }; // Stored mesh units
	float metres_per_unit = 1.0f; // Size of one stored unit
	float range = 1.0f; // Metres
	float decay = 1.0f; // Godot's attenuation exponent
	float color[3] = { 1, 1, 1 }; // Colour * energy (1.0 = full)
	float spot_axis[3] = { 0, 0, -1 }; // Unit vector the light points along
	float spot_cos = -2.0f; // Cos of the cone half-angle; -2 = omni
	// Godot's 1 / spot_attenuation: how the light fades toward the cone's
	// edge (1 = evenly, larger = harder edge, smaller = softer).
	float spot_edge_exponent = 1.0f;
};

static constexpr int MAX_POINT_LIGHTS = 4;

// How many times draw_mesh may split a triangle that is too big for the GPU
// or the screen coordinate range (each split makes four pieces).
static constexpr int NEAR_SPLIT_DEPTH = 4;

// Where a textured mesh's texture sits in VRAM (from VramAllocator), as the
// polygon fields that address it.
struct DrawTexture {
	// GP0Encoder::texpage halfword: page, depth, and the mesh's blend mode and dither.
	uint16_t texpage = 0;
	uint16_t clut = 0; // GP0Encoder::clut halfword
	uint16_t clut_semi = 0; // The same palette with STP on every colour
	bool has_clut_semi = false;
	// Every visible colour white: the additive fog pass keeps cut-out holes.
	uint16_t clut_white = 0;
	bool has_clut_white = false;
	uint8_t u_offset = 0; // The texture's top-left corner inside its page
	uint8_t v_offset = 0;
	// Texture bank (GP0 E7h) the texture and its CLUTs are in; 0 = VRAM.
	uint16_t bank = 0;
	// GP0 E2h word that repeats this texture, for triangles flagged in tri_window.
	uint32_t window_word = 0xE2000000u;
	bool has_semi_texels = false; // Some texels have STP set
};

// The DrawTexture for a texture placed at VRAM (p_x, p_y) by VramAllocator,
// with its CLUTs at p_clut_x/y, p_semi_x/y and p_white_x/y (x < 0: none).
DrawTexture make_draw_texture(const TextureData &p_texture, int32_t p_x, int32_t p_y, int32_t p_clut_x, int32_t p_clut_y, int32_t p_semi_x, int32_t p_semi_y, int32_t p_white_x, int32_t p_white_y, uint8_t p_blend, bool p_dither, uint16_t p_bank = 0);

struct DrawParams {
	CullMode cull = CULL_BACK;
	bool lit = true; // Lighting on; otherwise the baked colour as-is
	LightingMode lighting = LIGHTING_PS1;
	// Multiplies the mesh's baked colours before lighting, sRGB 0..255
	// (255 = unchanged): a MultiMesh instance's colour.
	uint8_t tint[3] = { 255, 255, 255 };
	// Added after lighting, sRGB 0..255 (material emission).
	uint8_t emission[3] = { 0, 0, 0 };
	const PointLight *point_lights = nullptr;
	int point_light_count = 0;
	// Vertex positions are scaled per axis by this before point lighting:
	// a non-uniformly scaled instance's scale relative to its largest axis,
	// with lights given in its rotation-only frame.
	float point_scale[3] = { 1.0f, 1.0f, 1.0f };
	// Depth cue toward GTE FC, per vertex, using DQA/DQB (DPCS). Textured
	// triangles are darkened by the fog amount instead and get a second,
	// additive pass of FC times the fog amount: texel * c * (1 - f) + FC * f.
	bool fog = false;
	// Transparent material. With BLEND_HALF the mesh's per-triangle alpha
	// classes decide (opaque, 50% or hidden); other modes blend every triangle.
	bool semi = false;
	uint8_t blend = 0; // BlendMode for semi-transparent meshes
	// With BLEND_HALF: blend every triangle, ignoring the alpha classes.
	bool semi_whole = false;
	// The GPU's 4x4 ordered dither for this mesh (shaded and textured
	// polygons; flat untextured ones never dither). Dither is draw-mode
	// state (GP0 E1h; a polygon's texpage does not carry it): frame_dither
	// is what the frame's drawing environment set. When the two differ,
	// each triangle is wrapped in E1h commands that switch it and back.
	bool dither = true;
	bool frame_dither = true;
	// Textured meshes (MeshData::textured) are drawn with this texture; null
	// draws them untextured. The vertex colour then modulates the texel, with
	// 128 = 1.0, so lights brighter than 1.0 can overbright up to 2x.
	const DrawTexture *texture = nullptr;
	// OTZ = AVSZ3 result >> ot_shift. Triangles nearer than OTZ 1 still land in bucket 0.
	int32_t ot_shift = 4;
	// Added to every OTZ after the shift (clamped to the table): negative
	// sorts the mesh nearer, so it draws over coplanar meshes (a sort bias,
	// as PS1 games used for decals and outlines).
	int32_t ot_bias = 0;
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
	uint32_t culled_near = 0; // Triangles and pieces dropped at the near plane
	uint32_t near_clips = 0; // Triangles cut at the near plane
	uint32_t near_splits = 0; // Pieces split for being too big for the GPU or the screen range
	uint32_t culled_far = 0;
	uint32_t culled_backface = 0;
	uint32_t culled_offscreen = 0;
	uint32_t culled_alpha = 0; // Alpha below 0.25
	uint32_t packets = 0;
	uint32_t textured = 0; // Textured triangles drawn
	uint32_t windowed = 0; // Of those, drawn with the texture window
	uint32_t fog_passes = 0; // Additive fog polygons over textured ones
	uint32_t banked = 0; // Textured triangles in texture banks above 0
	uint32_t arena_full = 0;

	void add(const DrawStats &p_other);
};

void draw_mesh(GTE &p_gte, PacketBuilder &p_builder, OrderingTable &p_ot, const MeshData &p_mesh, const DrawParams &p_params, DrawStats &r_stats);

} // namespace psx
