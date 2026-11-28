// Mesh conversion: float geometry in, GTE-ready fixed-point geometry out.
//
// One converter serves both paths (design doc, section 15): the Godot
// renderer calls it at runtime for meshes it meets in a scene, and the
// importer will call it at import time to bake .psxmesh resources. It knows
// nothing about Godot; callers pass plain arrays.
//
// Positions become int16 "GTE units": p * scale, rounded. When a mesh is too
// large for int16 it is stored shifted right by `shift` bits (0..2), and the
// caller multiplies the rotation matrix by 2^shift to compensate. Normals
// become 1.3.12 unit vectors. Colours are baked per vertex as the final
// display colour (material albedo times vertex colour, sRGB 0..255), which is
// what NCCS multiplies by light, so 255 at light 1.0 is the albedo itself.

#pragma once

#include "psx_gte.h"

#include <cstdint>
#include <vector>

namespace psx {

// What a mesh's indices describe.
enum MeshPrimitive : uint8_t {
	MESH_TRIANGLES = 0, // Three indices per triangle
	MESH_LINES = 1, // Two per line (drawn with the GPU's line command)
	MESH_POINTS = 2, // One per point (drawn as a small tile)
};

struct MeshInput {
	MeshPrimitive primitive = MESH_TRIANGLES;
	const float *positions = nullptr; // xyz, vertex_count entries
	const float *normals = nullptr; // xyz, optional
	const float *colors = nullptr; // rgba, optional
	const float *uvs = nullptr; // uv, optional (used with a texture)
	int32_t vertex_count = 0;
	const int32_t *indices = nullptr; // optional: none means 0, 1, 2, ...
	int32_t index_count = 0;
};

struct ConvertOptions {
	// GTE units per input unit (world scale times the instance's scale).
	float scale = 512.0f;
	// Material colour, sRGB 0..1 (Godot's albedo as authored).
	float albedo[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	bool use_vertex_colors = false;
	// Vertex colours are already sRGB (otherwise they are linear, as glTF stores them).
	bool vertex_colors_srgb = false;
	// The material is alpha-blended: classify triangles by their alpha
	// (albedo alpha times vertex alpha when vertex colours are used).
	bool use_alpha = false;
	// Subdivide until no edge is longer than this, in output (GTE) units;
	// 0 = off. Big polygons sort badly in the ordering table, cross the near
	// plane whole and warp affine textures: PS1 games split them the same way.
	float max_edge = 0.0f;
	// Normals are multiplied by this before normalising: the inverse of an
	// instance's non-uniform scale (relative to its largest axis), so lights
	// given in its rotation-only frame shade it as Godot does.
	float normal_scale[3] = { 1.0f, 1.0f, 1.0f };
	// Stop subdividing once a surface has this many triangles (a pass can
	// still multiply them by up to 4). Keeps one huge polygon from turning
	// into millions of triangles.
	uint32_t max_triangles = 1u << 14;
	// Texture size in texels (power of two, 8..256, from convert_texture);
	// 0 = untextured. UVs become texel coordinates per triangle corner.
	int32_t texture_width = 0;
	int32_t texture_height = 0;
	// Repeat (Godot's texture_repeat): each triangle is moved to the tile it
	// starts in; one spanning several tiles uses the texture window, and one
	// spanning more than 256 texels is split along tile edges. Otherwise UVs
	// are clamped to the texture.
	bool texture_repeat = true;
	// uv' = uv * uv_scale + uv_offset (StandardMaterial3D's uv1_scale/offset).
	float uv_scale[2] = { 1.0f, 1.0f };
	float uv_offset[2] = { 0.0f, 0.0f };
	// Repeating textures: keep a texel coordinate equal to the texture size
	// (its far edge) and use the texture window, instead of pulling it in to
	// size - 1, so every frame of a sprite sheet spans the same number of
	// texels (particle animation frames). Not repeating: pulled in as usual.
	bool keep_far_edge = false;
};

// How a triangle of an alpha-blended mesh is drawn. The PS1 has one alpha
// level, 50%, so alpha is rounded to opaque, half or nothing.
enum TriangleAlpha : uint8_t {
	TRI_OPAQUE = 0, // alpha >= 0.75
	TRI_HALF = 1, // 0.25 < alpha < 0.75
	TRI_HIDDEN = 2, // alpha <= 0.25
};

struct MeshData {
	std::vector<SVector> positions; // >> shift
	std::vector<SVector> normals; // 1.3.12, empty if the input had none
	std::vector<uint8_t> colors; // rgb, one triple per vertex
	MeshPrimitive primitive = MESH_TRIANGLES;
	// Three per triangle, two per line or one per point. Lines and points
	// are never textured or subdivided and have no alpha classes.
	std::vector<uint32_t> indices;
	// Textured meshes: u, v per triangle corner (six per triangle), in texels
	// relative to the texture's top-left corner.
	std::vector<uint8_t> tri_uvs;
	// One flag per triangle: 1 = its UVs run past the texture's edge and it
	// needs the texture window; empty = no triangle does.
	std::vector<uint8_t> tri_window;
	bool textured = false;
	// One TriangleAlpha per triangle when the material blends; empty = all opaque.
	std::vector<uint8_t> triangle_alpha;
	int32_t shift = 0;
	bool clamped = false; // Some positions did not fit even at the largest shift.
	// Bounding sphere in stored units (before the shift is undone).
	SVector center;
	int32_t radius = 0;
	float alpha = 1.0f; // Material alpha (for choosing a blend mode)

	uint32_t triangle_count() const { return (uint32_t)(indices.size() / 3); }
};

static constexpr int32_t CONVERT_MAX_SHIFT = 2;

// Fills r_mesh. Returns false (and leaves r_mesh empty) for empty or invalid input.
bool convert_mesh(const MeshInput &p_input, const ConvertOptions &p_options, MeshData &r_mesh);

// A vertex conversion added (UV tile cuts, subdivision): a mix of two
// earlier vertices, a + (b - a) * t, or their midpoint when t < 0.
struct VertexLerp {
	int32_t a = 0, b = 0;
	float t = -1.0f;
};

// A mesh whose positions and normals change every frame (skinning, blend
// shapes) but nothing else: converted once from its rest pose, then posed
// with apply_pose, which only re-quantizes positions and normals. Colours,
// UVs, alpha classes and the triangles (subdivision included, decided by the
// rest pose) are kept.
struct PoseTemplate {
	MeshData mesh;
	int32_t source_vertices = 0;
	std::vector<VertexLerp> lerps; // Vertex source_vertices + i of mesh
};

// Fills r_template from the rest pose. Returns false (template empty) as
// convert_mesh does.
bool convert_pose_template(const MeshInput &p_input, const ConvertOptions &p_options, PoseTemplate &r_template);

// r_mesh = the template with positions and normals from p_positions and
// p_normals (source_vertices each, posed; p_normals may be null when the
// template has none). The same result as convert_mesh on the posed input
// when subdivision would pick the same triangles. r_scratch is reused.
void apply_pose(const PoseTemplate &p_template, const float *p_positions, const float *p_normals, const ConvertOptions &p_options, MeshData &r_mesh, std::vector<float> &r_scratch);

// sRGB <-> linear, 0..1.
float srgb_to_linear(float p_value);
float linear_to_srgb(float p_value);

} // namespace psx
