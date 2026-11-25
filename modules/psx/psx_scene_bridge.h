// Bridge from Godot's 3D scene to the PSX core (design doc, sections 13-15).
//
// Each frame it reads what RendererSceneCull hands the scene renderer (the
// camera, the visible geometry instances, the visible lights and the
// environment) and turns it into GPU packets in an ordering table, the way a
// PS1 game's render loop would: per object, load the GTE with a rotation,
// translation and up to three lights, then stream the triangles through it.
//
// Mesh data comes from Godot's mesh storage once and is converted with the
// shared converter (psx_core/psx_convert), then cached per mesh, surface,
// scale bucket and material state. Skinned meshes are skinned on the CPU and
// converted every frame instead.
//
// Coordinates. Godot's camera space is X right, Y up, looking down -Z; the
// GTE's is X right, Y down, Z forward. Every object matrix is pre-multiplied
// by C = diag(1, -1, -1), a half turn about X, which keeps the winding.
// World units become GTE units through rendering/psx/world_scale (units per
// metre). Perspective: H comes from the projection, so the PSX view matches
// Godot's. Orthographic: the X/Y rows map straight to pixels (MVMVA path).

#pragma once

#ifdef RD_ENABLED

#include "psx_core/psx_convert.h"
#include "psx_core/psx_gte.h"
#include "psx_core/psx_ot.h"
#include "psx_core/psx_render.h"
#include "psx_core/psx_texture.h"
#include "psx_palette.h"
#include "psx_settings.h"
#include "psx_texture_resource.h"

#include "core/config/project_settings.h"
#include "core/io/image.h"
#include "core/variant/callable.h"
#include "core/os/os.h"
#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_geometry_instance.h"

#include <memory>
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/particles_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_scene_render.h"
#include "servers/rendering/rendering_server.h"

#include <cmath>

// What the PSX path takes from a Godot material.
// How a material turns its mesh toward the camera (BaseMaterial3D billboard_mode).
enum PSXBillboard : uint8_t {
	BILLBOARD_NONE = 0,
	BILLBOARD_ENABLED = 1, // Faces the camera
	BILLBOARD_FIXED_Y = 2, // Turns about Y only
	// Faces the camera, turned about the view axis by the instance's custom
	// data X (radians), as CPUParticles3D writes it (INSTANCE_CUSTOM.x).
	BILLBOARD_PARTICLES = 3,
};

struct PSXMaterialInfo {
	psx::CullMode cull = psx::CULL_BACK;
	bool lit = true;
	bool semi = false;
	uint8_t blend = psx::BLEND_HALF;
	bool vertex_colors = false;
	bool vertex_colors_srgb = false;
	// Godot's default material (no material set) is ALBEDO = vec3(0.6),
	// linear: about 0.8 in sRGB.
	Color albedo = Color(0.6, 0.6, 0.6, 1.0).linear_to_srgb();
	Color emission = Color(0, 0, 0); // Linear, energy applied
	float metallic = 0.0f;
	// Albedo texture (texture_albedo) and how it is sampled.
	RID texture;
	bool texture_repeat = true;
	Vector2 uv_scale = Vector2(1, 1);
	Vector2 uv_offset;
	uint8_t alpha_mode = psx::ALPHA_IGNORE;
	float alpha_cut = 0.5f;
	// Not baked into meshes (they change the transform or the sort only).
	uint8_t billboard = BILLBOARD_NONE;
	bool billboard_keep_scale = false;
	// Particles Anim (particles_anim_*): the texture is a sheet of h x v
	// frames, picked per instance by its custom data Z.
	bool anim = false;
	bool anim_loop = false;
	int32_t anim_h = 1;
	int32_t anim_v = 1;
	int32_t render_priority = 0; // Material render_priority: higher draws over lower
	// Point meshes: StandardMaterial3D's point_size when use_point_size is
	// on, in pixels; 1 otherwise. Drawn at the PS1 frame's resolution.
	float point_size = 1.0f;
	// PSXMaterial3D overrides (PSXMaterialOverrides::Dither; DEFAULT = the
	// resolved dither setting). no_subdivide is baked into meshes.
	uint8_t dither = 0;
	bool no_subdivide = false;
	// Blend every triangle at 50% (PSXMaterial3D psx_blend Half), instead
	// of by its alpha.
	bool semi_whole = false;
	// PSXMaterial3D psx_palette: the PSXPalette's object ID, 0 = none.
	uint64_t palette = 0;

	uint32_t hash() const { // Of what is baked into converted meshes.
		uint32_t h = hash_murmur3_one_32((uint32_t)cull);
		h = hash_murmur3_one_32((lit ? 1u : 0u) | (semi ? 2u : 0u) | (vertex_colors ? 4u : 0u) | (vertex_colors_srgb ? 8u : 0u) | ((uint32_t)blend << 4) | (texture_repeat ? 1u << 8 : 0u) | (no_subdivide ? 1u << 9 : 0u) | (anim ? 1u << 10 : 0u) | (semi_whole ? 1u << 11 : 0u), h);
		h = hash_murmur3_one_float(albedo.r, h);
		h = hash_murmur3_one_float(albedo.g, h);
		h = hash_murmur3_one_float(albedo.b, h);
		h = hash_murmur3_one_float(albedo.a, h);
		h = hash_murmur3_one_64(texture.get_id(), h);
		h = hash_murmur3_one_float(uv_scale.x, h);
		h = hash_murmur3_one_float(uv_scale.y, h);
		h = hash_murmur3_one_float(uv_offset.x, h);
		h = hash_murmur3_one_float(uv_offset.y, h);
		h = hash_murmur3_one_64(palette, h);
		return hash_fmix32(h);
	}
};

// The renderer's MaterialData type for its ShaderData type (both nested in
// the same scene-shader class); specialised where the renderers are known
// (psx_compositor_rd.cpp). MaterialData holds the render priority.
template <class ShaderDataT>
struct PSXMaterialDataOf;

// A GPU buffer read finished by RenderingDevice::buffer_get_data_async. The
// slot is shared, so a read delivered late writes to memory still owned.
struct PSXDownload {
	Vector<uint8_t> data;
	bool done = false;
};

class PSXDownloadCallable : public CallableCustom {
	std::shared_ptr<PSXDownload> slot;

	static bool _equal(const CallableCustom *p_a, const CallableCustom *p_b) { return p_a == p_b; }
	static bool _less(const CallableCustom *p_a, const CallableCustom *p_b) { return p_a < p_b; }

public:
	explicit PSXDownloadCallable(const std::shared_ptr<PSXDownload> &p_slot) :
			slot(p_slot) {}
	uint32_t hash() const override { return hash_murmur3_one_64((uint64_t)(uintptr_t)slot.get()); }
	String get_as_text() const override { return "PSXDownload"; }
	CompareEqualFunc get_compare_equal_func() const override { return _equal; }
	CompareLessFunc get_compare_less_func() const override { return _less; }
	bool is_valid() const override { return true; }
	ObjectID get_object() const override { return ObjectID(); }
	void call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, Callable::CallError &r_call_error) const override {
		r_call_error.error = Callable::CallError::CALL_OK;
		if (p_argcount > 0) {
			slot->data = *p_arguments[0];
			slot->done = true;
		}
	}
};

// What psx_read_material takes from a compiled shader (not its materials'
// parameters): read once per shader and code, since finding it means
// searching the generated code, which every surface drawn would repeat.
struct PSXShaderFacts {
	String code; // Holds the code's buffer, so its address names this version
	psx::CullMode cull = psx::CULL_BACK;
	bool lit = true;
	bool semi = false;
	uint8_t blend = psx::BLEND_HALF;
	bool vertex_colors = false;
	bool vertex_colors_srgb = false;
	bool emissive = false;
	bool alpha_cut = false;
	uint8_t billboard = 0;
	bool billboard_keep_scale = false;
	bool anim = false;
	bool point_size = false;
	bool texture_repeat_known = false;
	bool texture_repeat = false;
	StringName color_param = "albedo";
	StringName texture_param = "texture_albedo";
};

template <class ShaderDataT>
const PSXShaderFacts &psx_shader_facts(const ShaderDataT *p_sd) {
	static HashMap<const void *, PSXShaderFacts> cache; // Render thread only
	PSXShaderFacts *found = cache.getptr(p_sd);
	if (found && found->code.ptr() == p_sd->code.ptr()) {
		return *found;
	}
	if (!found && cache.size() >= 4096) {
		cache.clear(); // Shaders come and go in the editor; start again.
	}
	PSXShaderFacts facts;
	facts.code = p_sd->code;
	using SD = RendererRD::MaterialStorage::ShaderData;
	const ShaderDataT *sd = p_sd;
	const int cull = (int)sd->cull_mode;
	facts.cull = cull == RSE::CULL_MODE_DISABLED ? psx::CULL_NONE : (cull == RSE::CULL_MODE_FRONT ? psx::CULL_FRONT : psx::CULL_BACK);
	facts.lit = !sd->unshaded;
	const int blend = sd->blend_mode;
	if (blend == SD::BLEND_MODE_ADD) {
		facts.semi = true;
		facts.blend = psx::BLEND_ADD;
	} else if (blend == SD::BLEND_MODE_SUB) {
		facts.semi = true;
		facts.blend = psx::BLEND_SUBTRACT;
	} else if (blend == SD::BLEND_MODE_MUL || blend == SD::BLEND_MODE_PREMULTIPLIED_ALPHA || (sd->uses_alpha && !sd->uses_alpha_clip)) {
		facts.semi = true; // The PS1 has one alpha: 50%.
		facts.blend = psx::BLEND_HALF;
	}
	// StandardMaterial3D's generated code: see BaseMaterial3D::_update_shader().
	facts.vertex_colors = sd->code.contains("albedo_tex *= COLOR");
	facts.vertex_colors_srgb = sd->code.contains("COLOR.rgb = mix(");
	facts.emissive = sd->code.contains("EMISSION = ");
	facts.alpha_cut = sd->uses_alpha_clip;
	// Billboards are done in the vertex shader; the bridge turns the
	// instance to face the camera instead (_billboard).
	if (sd->code.contains("// Billboard Mode: Enabled")) {
		facts.billboard = BILLBOARD_ENABLED;
	} else if (sd->code.contains("// Billboard Mode: Particles")) {
		facts.billboard = BILLBOARD_PARTICLES;
	} else if (sd->code.contains("// Billboard Mode: Y-Billboard")) {
		facts.billboard = BILLBOARD_FIXED_Y;
	}
	facts.billboard_keep_scale = sd->code.contains("// Billboard Keep Scale: Enabled");
	facts.anim = sd->code.contains("particle_total_frames");
	facts.point_size = sd->code.contains("POINT_SIZE = point_size");
	// StandardMaterial3D's uniform names; a custom shader's first colour and
	// texture uniforms marked source_color stand in for them.
	using U = ShaderLanguage::ShaderNode::Uniform;
	if (!sd->uniforms.has(facts.color_param)) {
		int best = INT_MAX;
		for (const KeyValue<StringName, U> &kv : sd->uniforms) {
			const U &u = kv.value;
			if (u.scope == U::SCOPE_LOCAL && u.hint == U::HINT_SOURCE_COLOR && (u.type == ShaderLanguage::TYPE_VEC3 || u.type == ShaderLanguage::TYPE_VEC4) && u.prop_order < best) {
				best = u.prop_order;
				facts.color_param = kv.key;
			}
		}
	}
	if (!sd->uniforms.has(facts.texture_param)) {
		int best = INT_MAX;
		for (const KeyValue<StringName, U> &kv : sd->uniforms) {
			const U &u = kv.value;
			if (u.scope == U::SCOPE_LOCAL && u.use_color && u.type == ShaderLanguage::TYPE_SAMPLER2D && u.texture_order < best) { // source_color on a sampler sets use_color
				best = u.texture_order;
				facts.texture_param = kv.key;
			}
		}
	}
	// The albedo sampler's declaration says whether it repeats.
	const U *tex_uniform = sd->uniforms.getptr(facts.texture_param);
	if (tex_uniform) {
		facts.texture_repeat_known = true;
		facts.texture_repeat = tex_uniform->repeat != ShaderLanguage::REPEAT_DISABLE;
	}
	if (found) {
		*found = facts;
		return *found;
	}
	return cache.insert(p_sd, facts)->value;
}

// Reads cull mode, shading, blending and vertex-colour use from the compiled
// scene shader (ShaderDataT is the active renderer's ShaderData), and the
// albedo, emission and metallic uniforms of StandardMaterial3D/ORMMaterial3D.
template <class ShaderDataT>
PSXMaterialInfo psx_read_material(RID p_material) {
	PSXMaterialInfo info;
	if (!p_material.is_valid()) {
		return info;
	}
	// Parameter names, made once (a StringName from text is a table lookup).
	static const struct {
		const StringName uv1_scale = "uv1_scale";
		const StringName uv1_offset = "uv1_offset";
		const StringName alpha_scissor_threshold = "alpha_scissor_threshold";
		const StringName point_size = "point_size";
		const StringName emission = "emission";
		const StringName emission_energy = "emission_energy";
		const StringName particles_anim_h_frames = "particles_anim_h_frames";
		const StringName particles_anim_v_frames = "particles_anim_v_frames";
		const StringName particles_anim_loop = "particles_anim_loop";
		const StringName metallic = "metallic";
	} names;
	RendererRD::MaterialStorage *ms = RendererRD::MaterialStorage::get_singleton();
	if (!ms->material_get_data(p_material, RendererRD::MaterialStorage::SHADER_TYPE_3D)) {
		return info; // No spatial shader (yet): Godot's default material.
	}
	info.albedo = Color(1, 1, 1, 1); // Custom shaders without an albedo uniform.
	using MD = typename PSXMaterialDataOf<ShaderDataT>::Type;
	const MD *md = static_cast<const MD *>(ms->material_get_data(p_material, RendererRD::MaterialStorage::SHADER_TYPE_3D));
	info.render_priority = (int32_t)md->priority + RSE::MATERIAL_RENDER_PRIORITY_MIN; // Stored as 0..255
	const ShaderDataT *sd = static_cast<const ShaderDataT *>(ms->material_get_shader_data(p_material));
	bool emissive = false;
	bool point_size = false; // use_point_size
	// StandardMaterial3D's uniform names; a custom shader's first colour and
	// texture uniforms marked source_color stand in for them.
	static const StringName default_color_param = "albedo";
	static const StringName default_texture_param = "texture_albedo";
	StringName color_param = default_color_param;
	StringName texture_param = default_texture_param;
	if (sd) {
		const PSXShaderFacts &facts = psx_shader_facts<ShaderDataT>(sd);
		info.cull = facts.cull;
		info.lit = facts.lit;
		if (facts.semi) {
			info.semi = true;
			info.blend = facts.blend;
		}
		info.vertex_colors = facts.vertex_colors;
		info.vertex_colors_srgb = facts.vertex_colors_srgb;
		emissive = facts.emissive;
		if (facts.alpha_cut) {
			info.alpha_mode = psx::ALPHA_CUT;
		} else if (info.semi && info.blend == psx::BLEND_HALF) {
			info.alpha_mode = psx::ALPHA_BLEND;
		}
		info.billboard = facts.billboard;
		info.billboard_keep_scale = facts.billboard_keep_scale;
		info.anim = facts.anim;
		point_size = facts.point_size;
		color_param = facts.color_param;
		texture_param = facts.texture_param;
		if (facts.texture_repeat_known) {
			info.texture_repeat = facts.texture_repeat;
		}
	}
	// A parameter the material has not set takes the shader's default.
	auto param = [&](const StringName &p_name) -> Variant {
		const Variant v = ms->material_get_param(p_material, p_name);
		return (v.get_type() != Variant::NIL || !sd) ? v : sd->get_default_parameter(p_name);
	};
	const Variant texture = param(texture_param);
	if (texture.get_type() == Variant::RID) {
		info.texture = texture;
	}
	const Variant uv_scale = ms->material_get_param(p_material, names.uv1_scale);
	if (uv_scale.get_type() == Variant::VECTOR3) {
		const Vector3 s = uv_scale;
		info.uv_scale = Vector2(s.x, s.y);
	}
	const Variant uv_offset = ms->material_get_param(p_material, names.uv1_offset);
	if (uv_offset.get_type() == Variant::VECTOR3) {
		const Vector3 o = uv_offset;
		info.uv_offset = Vector2(o.x, o.y);
	}
	if (info.alpha_mode == psx::ALPHA_CUT) {
		const Variant cut = ms->material_get_param(p_material, names.alpha_scissor_threshold);
		if (cut.get_type() == Variant::FLOAT) {
			info.alpha_cut = CLAMP((float)cut, 0.0f, 1.0f);
		}
	}
	if (point_size) {
		const Variant ps = param(names.point_size);
		if (ps.get_type() == Variant::FLOAT || ps.get_type() == Variant::INT) {
			info.point_size = MAX((float)ps, 1.0f);
		}
	}
	const Variant albedo = param(color_param);
	if (albedo.get_type() == Variant::COLOR) {
		info.albedo = albedo;
	} else if (albedo.get_type() == Variant::VECTOR4) {
		const Vector4 c = albedo;
		info.albedo = Color(c.x, c.y, c.z, c.w);
	} else if (albedo.get_type() == Variant::VECTOR3) {
		const Vector3 c = albedo;
		info.albedo = Color(c.x, c.y, c.z, 1.0f);
	}
	if (emissive) {
		const Variant emission = ms->material_get_param(p_material, names.emission);
		const Variant energy = ms->material_get_param(p_material, names.emission_energy);
		if (emission.get_type() == Variant::COLOR) {
			const float e = energy.get_type() == Variant::FLOAT ? (float)energy : 1.0f;
			const Color c = Color(emission).srgb_to_linear() * e;
			info.emission = Color(c.r, c.g, c.b);
		}
	}
	if (info.anim) {
		const Variant h = param(names.particles_anim_h_frames);
		const Variant v = param(names.particles_anim_v_frames);
		const Variant loop = param(names.particles_anim_loop);
		info.anim_h = h.get_type() == Variant::INT ? CLAMP((int32_t)h, 1, 128) : 1;
		info.anim_v = v.get_type() == Variant::INT ? CLAMP((int32_t)v, 1, 128) : 1;
		info.anim_loop = loop.get_type() == Variant::BOOL && (bool)loop;
		// Every particle billboard has the animation code; one frame is
		// an ordinary texture.
		info.anim = info.anim_h * info.anim_v > 1;
	}
	const Variant metallic = ms->material_get_param(p_material, names.metallic);
	if (metallic.get_type() == Variant::FLOAT) {
		info.metallic = CLAMP((float)metallic, 0.0f, 1.0f);
	}
	return info;
}

// What a MultiMesh gives each instance besides its transform.
struct PSXInstanceData {
	float spin = 0.0f; // Custom data X: particle billboard rotation (radians)
	float anim = 0.0f; // Custom data Z: particle animation phase (0..1 = the whole sheet)
	// Instance colour; multiplies COLOR, so only materials that use vertex
	// colour as albedo show it (as in Godot).
	Color color = Color(1, 1, 1, 1);
	bool has_color = false;
};

struct PSXSceneSettings {
	float world_scale = 512.0f; // GTE units per metre
	Color default_ambient = Color(0.2, 0.2, 0.2);
	psx::LightingMode lighting = psx::LIGHTING_PS1;
	float subdivide_size = 1.0f; // Metres; 0 = off
	psx::TextureOptions texture; // max_size, depth, max_colors, dither (alpha set per material)
	// Texture banks (1024 x 512 each, bank 0 = VRAM); 0 = as many as needed.
	uint32_t texture_banks = 0;
	bool dither = true; // Unless a material says otherwise
};

struct PSXSceneStats {
	uint32_t instances = 0;
	uint32_t surfaces = 0;
	uint32_t conversions = 0;
	uint32_t skinned = 0; // Surfaces skinned or blend-shaped on the CPU this frame
	uint32_t particles = 0; // GPUParticles3D particles drawn (once per draw pass)
	uint64_t particles_read_usec = 0; // Reading GPUParticles3D instance data back
	uint32_t texture_uploads = 0; // Textures converted or re-uploaded this frame
	uint32_t texture_fallbacks = 0; // Surfaces drawn untextured: no VRAM left
	uint32_t textures_resident = 0;
	uint32_t textures_baked = 0; // Resident textures that came from a PSXTexture (not converted)
	uint32_t clut_slots = 0; // CLUTs in texture memory
	uint32_t shared_slots = 0; // Uses of a CLUT or texel box beyond its first (shared palettes, swaps)
	uint32_t vram_free_percent = 100; // Of all texture banks in use
	uint32_t texture_banks = 1;
	// Time spent this frame turning Godot data into PS1 data (print_stats:
	// PSX_CONVERT), and how many were done. Reads copy meshes and images
	// back from Godot's storage; conversions build the PS1 versions; skin is
	// the CPU skinning of every skinned surface drawn (redone each frame).
	uint64_t mesh_read_usec = 0;
	uint64_t mesh_convert_usec = 0;
	uint64_t skin_usec = 0;
	uint64_t texture_read_usec = 0;
	uint64_t texture_convert_usec = 0;
	uint32_t meshes_read = 0;
	uint32_t meshes_converted = 0;
	uint32_t textures_read = 0;
	uint32_t textures_converted = 0;
	// Where the frame's time went (print_stats: PSX_STATS): building the
	// ordering table from the scene (conversion and skinning included),
	// the GPU drawing it, and reading the picture out of VRAM.
	uint64_t build_usec = 0;
	uint64_t gpu_usec = 0;
	uint64_t scanout_usec = 0;
	psx::DrawStats draw;
};

// Adds the time from construction to destruction to a counter.
struct PSXScopedTimer {
	uint64_t &total;
	const uint64_t begin;
	explicit PSXScopedTimer(uint64_t &p_total) :
			total(p_total), begin(OS::get_singleton()->get_ticks_usec()) {}
	~PSXScopedTimer() { total += OS::get_singleton()->get_ticks_usec() - begin; }
};

template <class ShaderDataT>
class PSXSceneBridge {
public:
	static constexpr uint32_t OT_SIZE = 4096;
	static constexpr uint64_t CACHE_KEEP_FRAMES = 300;

	PSXSceneSettings settings;
	// The frame's stats while build() runs, for helpers that are not passed
	// them (mesh and image read-back).
	PSXSceneStats *active_stats = nullptr;
	PSXSceneStats unused_stats;

	// Layers 1-20, the ones a VisualInstance3D can be put on.
	static constexpr uint32_t USER_LAYERS = (1u << 20) - 1;

	// VRAM the textures must not use: the display buffers, and anything else
	// the caller has put in the texture region.
	void reserve_vram(int32_t p_x, int32_t p_y, int32_t p_width, int32_t p_height) {
		if (vram_banks.is_empty()) {
			vram_banks.resize(1);
		}
		vram_banks[0].reserve(p_x, p_y, p_width, p_height);
	}

	// Draws the scene into p_ot: geometry only. The caller adds the clear and
	// the drawing environment at the far end. Textures are uploaded to p_gpu's
	// VRAM as they are first needed, before the OT is drawn.
	void build(RendererSceneRender *p_scene_render, const RendererSceneRender::CameraData &p_camera, const PagedArray<RenderGeometryInstance *> &p_instances,
			const PagedArray<RID> &p_lights, RID p_environment, int32_t p_width, int32_t p_height, psx::GPU &p_gpu,
			psx::PacketBuilder &p_builder, psx::OrderingTable &p_ot, uint64_t p_frame, PSXSceneStats &r_stats) {
		r_stats = PSXSceneStats();
		active_stats = &r_stats;
		frame = p_frame;
		gpu = &p_gpu;
		const int vram_scale = p_gpu.get_scale();
		Frame f;
		f.builder = &p_builder;
		f.ot = &p_ot;
		f.stats = &r_stats;
		f.depth = depth_triangles;
		f.width = p_width;
		f.height = p_height;
		f.world = settings.world_scale;

		// Projection.
		const Projection &proj = p_camera.main_projection;
		const double p00 = proj.columns[0][0], p11 = proj.columns[1][1];
		f.gte.scale = vram_scale;
		f.gte.ofx = (p_width / 2) * 65536;
		f.gte.ofy = (p_height / 2) * 65536;
		f.ortho = p_camera.is_orthogonal;
		if (f.ortho) {
			// Pixels per GTE unit on X and Y; Z stays in GTE units.
			f.row_scale[0] = 0.5 * p_width * p00 / f.world;
			f.row_scale[1] = -0.5 * p_height * p11 / f.world;
			f.row_scale[2] = -1.0;
			f.near_z = (int32_t)CLAMP(std::floor(proj.get_z_near() * f.world), 0.0, 65535.0);
			f.gte.h = 256; // Unused by MVMVA; only the depth cue reads it.
		} else {
			// H from the vertical scale, the X row pre-scaled so the
			// horizontal field of view matches too.
			const double h_value = CLAMP(std::round(0.5 * p_height * p11), 1.0, 65535.0);
			f.gte.h = (uint16_t)h_value;
			f.row_scale[0] = (0.5 * p_width * p00) / h_value;
			f.row_scale[1] = -1.0;
			f.row_scale[2] = -1.0;
		}
		f.far_z = (int32_t)MIN(65535.0, std::ceil(proj.get_z_far() * f.world));
		while ((f.far_z >> f.ot_shift) >= (int32_t)OT_SIZE) {
			f.ot_shift++;
		}

		_gather_lights(p_lights);
		_read_sky(p_scene_render, p_environment, r_stats);
		const Color ambient = _ambient(p_scene_render, p_environment);
		f.gte.ambient = { _to_fixed(ambient.r), _to_fixed(ambient.g), _to_fixed(ambient.b) };
		f.fog = _setup_fog(p_scene_render, p_environment, proj, f);

		f.cam_inv = p_camera.main_transform.affine_inverse();
		f.cam_basis = p_camera.main_transform.basis.orthonormalized();
		RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
		_prefetch_meshes(mesh_storage, p_instances);
		_prefetch_textures(mesh_storage, p_instances);

		for (uint32_t i = 0; i < p_instances.size(); i++) {
			const RenderGeometryInstanceBase *inst = static_cast<const RenderGeometryInstanceBase *>(p_instances[i]);
			if (!inst->data) {
				continue;
			}
			// Editor helpers (grid, origin axes, gizmos, selection boxes) sit
			// only on layers 21-32, which nodes cannot use; they are not part
			// of the game's picture, and some are enormous.
			if ((inst->layer_mask & USER_LAYERS) == 0) {
				continue;
			}
			if (inst->data->base_type == RSE::INSTANCE_MESH) {
				_draw_instance(f, mesh_storage, inst, inst->data->base, inst->transform);
			} else if (inst->data->base_type == RSE::INSTANCE_MULTIMESH) {
				_draw_multimesh(f, mesh_storage, inst);
			} else if (inst->data->base_type == RSE::INSTANCE_PARTICLES) {
				_draw_particles(f, mesh_storage, inst);
			}
		}
		_draw_sky(f);
		fetched.clear(); // Fetched for a mesh not drawn after all
		fetched_images.clear(); // Or a texture
		_evict();
		r_stats.textures_resident = 0;
		r_stats.textures_baked = 0;
		for (const KeyValue<uint64_t, TextureEntry> &kv : textures) {
			r_stats.textures_resident += kv.value.placed ? 1 : 0;
			r_stats.textures_baked += (kv.value.placed && kv.value.baked) ? 1 : 0;
		}
		uint64_t free_cells = 0;
		for (const psx::VramAllocator &a : vram_banks) {
			free_cells += a.free_cells();
		}
		const uint64_t bank_cells = psx::VramAllocator::GRID_W * psx::VramAllocator::GRID_H;
		for (const BankSlots &b : bank_slots) {
			r_stats.clut_slots += b.cluts.size();
			for (const KeyValue<uint64_t, SharedSlot> &kv : b.cluts) {
				r_stats.shared_slots += kv.value.refs - 1;
			}
			for (const KeyValue<uint64_t, SharedSlot> &kv : b.boxes) {
				r_stats.shared_slots += kv.value.refs - 1;
			}
		}
		r_stats.texture_banks = MAX(1u, vram_banks.size());
		r_stats.vram_free_percent = vram_banks.is_empty() ? 100 : (uint32_t)(free_cells * 100 / (bank_cells * vram_banks.size()));
	}

	// The clear colour for this frame: the environment's background colour,
	// else the viewport's clear colour, then fogged as Godot fogs the sky.
	// With a sky, its mean colour: what an orthographic camera sees (the
	// dome is drawn over it for a perspective one).
	Color background(RendererSceneRender *p_scene_render, RID p_environment, const Color &p_clear) const {
		Color bg = p_clear;
		if (p_environment.is_valid() && p_scene_render->is_environment(p_environment) &&
				p_scene_render->environment_get_background(p_environment) == RSE::ENV_BG_COLOR) {
			bg = p_scene_render->environment_get_bg_color(p_environment);
		} else if (sky.kind != SKY_NONE) {
			bg = sky.mean;
		}
		if (fog_sky_affect > 0.0f) {
			bg = bg.lerp(fog_color, fog_sky_affect);
		}
		return bg;
	}

private:
	struct SourceSurface {
		// Triangles, lines or points; strips are read as lists.
		psx::MeshPrimitive primitive = psx::MESH_TRIANGLES;
		uint32_t vertex_count = 0;
		LocalVector<float> positions, normals, colors, uvs;
		LocalVector<int32_t> indices;
		// Skinning: bones and weights, `influences` per vertex (4 or 8).
		LocalVector<int32_t> bones;
		LocalVector<float> weights;
		int influences = 0;
		// Blend shapes: `shape_count` sets of positions (and of normals, when
		// the surface has normals), vertex_count * 3 floats each, as stored
		// (normalized or relative: see the mesh's blend shape mode).
		uint32_t shape_count = 0;
		LocalVector<float> shape_positions, shape_normals;
		Vector2 uv_min, uv_max; // UV bounds (when there are UVs)
	};

	struct MeshEntry {
		AABB aabb; // The mesh's current bounds (posed, for skinned meshes)
		RID mesh;
		uint32_t signature = 0; // _mesh_signature when the surfaces were read
		LocalVector<SourceSurface> surfaces;
		HashMap<uint64_t, psx::MeshData> converted;
		// Skinned or blend-shaped surfaces: converted once from the rest
		// pose, posed every frame (same keys as converted).
		HashMap<uint64_t, psx::PoseTemplate> pose_templates;
		uint64_t last_used = 0;
	};

	struct Light {
		Vector3 to_light; // Directional: unit vector toward the light (world)
		Vector3 position;
		Color color; // colour * energy
		bool directional = true;
		float range = 1.0f;
		float decay = 1.0f;
		Vector3 spot_axis; // Spot: unit vector the light points along
		float spot_cos = -2.0f; // Spot: cos of the cone half-angle (-2 = omni)
		float spot_edge_exponent = 1.0f; // Spot: 1 / spot_attenuation
	};

	// Per-frame state shared by every object.
	struct Frame {
		psx::GTE gte;
		psx::PacketBuilder *builder = nullptr;
		psx::OrderingTable *ot = nullptr;
		PSXSceneStats *stats = nullptr;
		std::vector<psx::DepthTriangle> *depth = nullptr; // depth_triangles for this frame
		Transform3D cam_inv;
		Basis cam_basis; // Camera rotation in world space (billboards)
		double row_scale[3] = { 1.0, -1.0, -1.0 }; // A * C
		double world = 512.0;
		bool ortho = false;
		bool fog = false;
		int32_t near_z = 0;
		int32_t far_z = 65535;
		int32_t ot_shift = 0;
		int32_t width = 320, height = 240;
		float light_colors[3][3] = {}; // This object's lights, [light][channel]
		// Omni/spot lights reaching this object, in world space (picked per
		// object, converted to mesh units per surface).
		const Light *near_lights[psx::MAX_POINT_LIGHTS] = {};
		int near_light_count = 0;
	};

	HashMap<RID, MeshEntry> meshes;
	LocalVector<Light> lights;
	LocalVector<Transform3D> bone_cache;
	LocalVector<float> skin_positions, skin_normals;
	Vector<uint8_t> particle_buffer; // GPUParticles3D instance data read back this frame
	struct BlendWeight {
		uint32_t shape = 0;
		float weight = 0.0f;
	};
	LocalVector<BlendWeight> blend_cache; // This instance's blend shape weights that are not zero
	psx::MeshData skin_mesh;
	std::vector<float> pose_scratch;
	Color fog_color;
	float fog_sky_affect = 0.0f;
	uint64_t frame = 0;

	// The environment's sky (ENV_BG_SKY), read by _read_sky each frame and
	// drawn by _draw_sky as a dome around the camera, the way PS1 games
	// drew their backdrops: ProceduralSkyMaterial (and an approximation of
	// PhysicalSkyMaterial) as Gouraud colours, PanoramaSkyMaterial as a
	// textured dome. Other sky shaders keep the clear colour.
	enum SkyKind : uint8_t {
		SKY_NONE,
		SKY_GRADIENT,
		SKY_PANORAMA,
	};
	struct SkyInfo {
		SkyKind kind = SKY_NONE;
		// Gradient: Godot's ProceduralSkyMaterial uniforms, linear, with
		// the material's and the environment's energy applied.
		Color top, horizon, ground_horizon, bottom;
		float inv_sky_curve = 4.0f;
		float inv_ground_curve = 30.0f;
		RID texture; // Panorama
		float energy = 1.0f; // Panorama: exposure times the environment's energy
		Basis orientation; // Sky space to world (Environment.sky_rotation)
		Color mean = Color(0, 0, 0); // sRGB, over the whole sphere: ambient light, orthographic cameras
	};
	SkyInfo sky;

public:
	// When set, build() records the opaque triangles of the scene's objects
	// (not the sky) here, for a depth image under the editor's helpers.
	std::vector<psx::DepthTriangle> *depth_triangles = nullptr;

private:
	uint32_t sky_mesh_key = 0;
	psx::MeshData sky_mesh;
	SourceSurface sky_source; // UV bounds for the panorama's texture lookup
	// Per-instance scratch for _draw_instance.
	LocalVector<PSXMaterialInfo> surface_mats;
	LocalVector<uint8_t> surface_groups;

	// A converted texture and where it lives in VRAM (or a texture bank),
	// shared by every use that converts to the same texels and palette. The
	// data is kept while the entry exists, so an evicted texture can be
	// uploaded again.
	struct TextureEntry {
		psx::TextureData data;
		bool needs_semi_clut = false;
		bool placed = false;
		bool baked = false; // From a PSXTexture's baked data
		// Keys of its texels and CLUTs in BankSlots.
		uint64_t texel_key = 0, clut_key = 0, semi_key = 0, white_key = 0;
		uint16_t bank = 0;
		int32_t x = 0, y = 0, clut_x = 0, clut_y = 0, semi_x = -1, semi_y = 0, white_x = -1, white_y = 0;
		psx::DrawTexture draw[4]; // Per blend mode
		uint64_t last_used = 0;
		uint64_t retry_frame = 0; // Next frame to try placing it again
	};
	HashMap<uint64_t, TextureEntry> textures; // By content

	// One material texture as one surface uses it: cropped to the box its
	// UVs cover (whole when they repeat), converted, and the UV transform
	// that maps the surface's UVs into the crop.
	struct TextureUse {
		bool valid = false; // False: no usable image
		uint64_t content = 0; // Key into textures
		Vector2 uv_scale = Vector2(1, 1);
		Vector2 uv_offset;
		uint64_t last_used = 0;
		uint64_t version = 0; // The source texture's content version it was converted from
	};
	HashMap<uint64_t, TextureUse> texture_uses;

	// Images read back from Godot's texture storage, as RGBA8.
	struct SourceImage {
		Ref<Image> image;
		uint64_t last_used = 0;
		uint64_t version = 0; // TextureStorage::texture_get_content_version when read
	};
	HashMap<RID, SourceImage> source_images;

	// Texture space: bank 0 is VRAM's texture region (shared with the display
	// buffers at scale 1); banks 1.. are added while settings.texture_banks
	// allows (0 = up to psx::MAX_TEXTURE_BANKS).
	LocalVector<psx::VramAllocator> vram_banks;

	// A CLUT or a box of texels in one bank, shared by every placed texture
	// whose halfwords are the same: textures made with one PSXPalette share
	// its CLUT, as PS1 games did, and palette swaps of a texture share its
	// texels.
	struct SharedSlot {
		int32_t x = 0, y = 0;
		uint32_t refs = 0;
	};
	struct BankSlots {
		HashMap<uint64_t, SharedSlot> cluts;
		HashMap<uint64_t, SharedSlot> boxes;
	};
	LocalVector<BankSlots> bank_slots; // One per vram_banks entry
	psx::GPU *gpu = nullptr;

	static int32_t _to_int32(double p_value) {
		return (int32_t)CLAMP(std::llround(p_value), (long long)INT32_MIN, (long long)INT32_MAX);
	}

	static int32_t _to_fixed(float p_value) { // 1.19.12
		return (int32_t)CLAMP(std::lround(p_value * 4096.0), 0L, 0x7FFFFL);
	}

	static uint8_t _to_byte(float p_value) {
		return (uint8_t)CLAMP(std::lround(p_value * 255.0f), 0L, 255L);
	}

	// One mesh instance (or one multimesh instance) at p_transform. Surfaces
	// whose material is a billboard are drawn with their own transform,
	// turned to the camera as Godot's billboard vertex code does.
	// p_data: the MultiMesh instance's colour and custom data.
	void _draw_instance(Frame &f, RendererRD::MeshStorage *p_storage, const RenderGeometryInstanceBase *p_inst, RID p_mesh, const Transform3D &p_transform, const PSXInstanceData &p_data = PSXInstanceData()) {
		MeshEntry *entry = _get_mesh(p_storage, p_mesh);
		if (!entry) {
			return;
		}
		f.stats->instances++;
		const uint32_t surface_count = entry->surfaces.size();
		surface_mats.resize(surface_count);
		surface_groups.resize(surface_count);
		uint32_t groups_used = 0;
		for (uint32_t surface = 0; surface < surface_count; surface++) {
			RID material = p_inst->data->material_override;
			if (!material.is_valid() && surface < (uint32_t)p_inst->data->surface_materials.size()) {
				material = p_inst->data->surface_materials[surface];
			}
			if (!material.is_valid()) {
				material = p_storage->mesh_surface_get_material(p_mesh, surface);
			}
			surface_mats[surface] = psx_read_material<ShaderDataT>(material);
			if (material.is_valid()) {
				const PSXMaterialOverrides o = PSXSettingsRegistry::get_material(material);
				surface_mats[surface].dither = o.dither;
				surface_mats[surface].no_subdivide = o.no_subdivide;
				surface_mats[surface].palette = o.palette;
				if (o.blend != PSXMaterialOverrides::BLEND_MATERIAL) {
					// The PS1 mode replaces Godot's transparency; a texture's
					// alpha still cuts holes.
					static const uint8_t modes[] = { psx::BLEND_HALF, psx::BLEND_HALF, psx::BLEND_ADD, psx::BLEND_SUBTRACT, psx::BLEND_ADD_QUARTER };
					PSXMaterialInfo &m = surface_mats[surface];
					m.semi = true;
					m.blend = modes[MIN(o.blend, (uint8_t)4)];
					m.semi_whole = o.blend == PSXMaterialOverrides::BLEND_HALF;
					if (m.alpha_mode == psx::ALPHA_BLEND) {
						m.alpha_mode = psx::ALPHA_CUT;
					}
				}
			}
			const uint8_t group = surface_mats[surface].billboard | (surface_mats[surface].billboard_keep_scale ? 4 : 0);
			surface_groups[surface] = group;
			groups_used |= 1u << group;
		}
		for (uint8_t group = 0; group < 8; group++) {
			if (groups_used & (1u << group)) {
				const Transform3D xform = (group & 3) ? _billboard(f, p_transform, group & 3, (group & 4) != 0, p_data.spin) : p_transform;
				_draw_surfaces(f, p_storage, p_inst, *entry, xform, group, p_data);
			}
		}
	}

	// p_transform with its rotation replaced by the camera's (keeping the
	// scale when asked), as BaseMaterial3D's billboard modes do.
	static Transform3D _billboard(const Frame &f, const Transform3D &p_transform, uint8_t p_mode, bool p_keep_scale, float p_spin) {
		const Vector3 cam_x = f.cam_basis.get_column(0);
		const Vector3 cam_z = f.cam_basis.get_column(2);
		Basis b = f.cam_basis;
		if (p_mode == BILLBOARD_FIXED_Y) {
			const Vector3 up(0, 1, 0);
			b = Basis(up.cross(cam_z).normalized(), up, cam_x.cross(up).normalized());
		} else if (p_mode == BILLBOARD_PARTICLES && p_spin != 0.0f) {
			// The shader's rotation matrix turns local X to (cos, -sin):
			// a turn of -spin about the view axis.
			b = b * Basis(Vector3(0, 0, 1), -p_spin);
		}
		if (p_keep_scale) {
			b.scale_local(Vector3(p_transform.basis.get_column(0).length(), p_transform.basis.get_column(1).length(), p_transform.basis.get_column(2).length()));
		}
		return Transform3D(b, p_transform.origin);
	}

	// The surfaces of p_entry in billboard group p_group (surface_groups).
	void _draw_surfaces(Frame &f, RendererRD::MeshStorage *p_storage, const RenderGeometryInstanceBase *p_inst, MeshEntry &r_entry, const Transform3D &p_transform, uint8_t p_group, const PSXInstanceData &p_data) {
		MeshEntry *entry = &r_entry;
		const Transform3D view = f.cam_inv * p_transform;
		const Basis &b = view.basis;
		const double s = MAX(MAX(b.get_column(0).length(), b.get_column(1).length()), b.get_column(2).length());
		if (s < 1e-6) {
			return;
		}
		const int32_t bucket = (int32_t)std::lround(std::log2(s) * 8.0);
		const double sb = std::exp2(bucket / 8.0);
		// Non-uniform scale: normals take the inverse scale (relative to the
		// largest axis, in 1/64 steps) at conversion; lights use the rotation.
		Vector3 normal_scale(1, 1, 1);
		const double axis_scale[3] = { b.get_column(0).length(), b.get_column(1).length(), b.get_column(2).length() };
		for (int a = 0; a < 3; a++) {
			const double r = MAX(1.0, std::round(axis_scale[a] / s * 64.0)) / 64.0;
			normal_scale[a] = (real_t)(1.0 / r);
		}

		f.gte.translation.x = _to_int32(f.row_scale[0] * f.world * view.origin.x);
		f.gte.translation.y = _to_int32(f.row_scale[1] * f.world * view.origin.y);
		f.gte.translation.z = _to_int32(f.row_scale[2] * f.world * view.origin.z);
		const double radius = entry->aabb.size.length() * 0.5 * s;
		_load_lights(f, p_transform, p_transform.xform(entry->aabb.get_center()), radius);
		// Point lights are placed in the instance's rotation-only frame (see
		// PointLight and DrawParams::point_scale).
		const Basis rotation_inv = p_transform.basis.orthonormalized().transposed();

		// Skinned: bone matrices for this frame (Godot's skinning transforms).
		const RID skeleton = p_inst->data->skeleton;
		bool skinned = false;
		if (skeleton.is_valid() && p_storage->owns_skeleton(skeleton)) {
			const int bone_count = p_storage->skeleton_get_bone_count(skeleton);
			bone_cache.resize(bone_count);
			for (int k = 0; k < bone_count; k++) {
				bone_cache[k] = p_storage->skeleton_bone_get_transform(skeleton, k);
			}
			skinned = bone_count > 0;
		}
		// Blend shapes: the weights set on this instance's mesh (not on the
		// draw passes of particles, which have their own meshes). Only the
		// weights that are not zero, as Godot's skinning shader does.
		blend_cache.clear();
		bool blend_normalized = false;
		if (r_entry.mesh == p_inst->data->base && p_inst->mesh_instance.is_valid()) {
			const LocalVector<float> *weights = p_storage->mesh_instance_get_blend_weights(p_inst->mesh_instance);
			if (weights) {
				for (uint32_t k = 0; k < weights->size(); k++) {
					if (Math::abs((*weights)[k]) > 0.0001f) {
						blend_cache.push_back(BlendWeight{ k, (*weights)[k] });
					}
				}
				blend_normalized = p_storage->mesh_get_blend_shape_mode(r_entry.mesh) == RSE::BLEND_SHAPE_MODE_NORMALIZED;
			}
		}

		for (uint32_t surface = 0; surface < entry->surfaces.size(); surface++) {
			const SourceSurface &src = entry->surfaces[surface];
			if (src.vertex_count == 0 || surface_groups[surface] != p_group) {
				continue;
			}
			PSXMaterialInfo mat = surface_mats[surface];
			// Particles Anim: the frame's part of the sheet (BaseMaterial3D's
			// vertex code), between the material's UV transform and the crop.
			Vector2 anim_scale(1, 1), anim_offset;
			if (mat.anim) {
				const int32_t total = mat.anim_h * mat.anim_v;
				int32_t frame_index = (int32_t)std::floor(p_data.anim * (float)total);
				frame_index = mat.anim_loop ? Math::posmod(frame_index, total) : CLAMP(frame_index, 0, total - 1);
				anim_scale = Vector2(1.0f / mat.anim_h, 1.0f / mat.anim_v);
				anim_offset = Vector2((float)(frame_index % mat.anim_h) / mat.anim_h, (float)(frame_index / mat.anim_h) / mat.anim_v);
			}
			TextureEntry *tex = _get_texture(mat, src, *f.stats, anim_scale, anim_offset);
			if (tex && !tex->placed) {
				// No room in VRAM: untextured, tinted by the texture's average colour.
				f.stats->texture_fallbacks++;
				const Color mean = Color(tex->data.mean[0] / 255.0f, tex->data.mean[1] / 255.0f, tex->data.mean[2] / 255.0f).srgb_to_linear();
				const Color lin = Color(mat.albedo.r, mat.albedo.g, mat.albedo.b).srgb_to_linear() * mean;
				mat.albedo = Color(lin.r, lin.g, lin.b).linear_to_srgb();
				mat.albedo.a = 1.0f;
				mat.texture = RID();
				tex = nullptr;
			} else if (!tex) {
				mat.texture = RID();
			}
			const psx::TextureData *tex_data = tex ? &tex->data : nullptr;

			const psx::MeshData *data = nullptr;
			const bool skin_surface = skinned && src.influences > 0;
			const bool shape_surface = !blend_cache.is_empty() && src.shape_count > 0;
			if (skin_surface || shape_surface) {
				data = _skin(*entry, surface, bucket, shape_surface, blend_normalized, skin_surface, mat, tex_data, normal_scale, sb, f);
			} else {
				data = _get_converted(*entry, surface, bucket, sb, mat, tex_data, normal_scale, *f.stats);
			}
			if (!data) {
				continue;
			}
			f.stats->surfaces++;

			const double k = (double)(1 << data->shift) / sb * 4096.0;
			for (int r = 0; r < 3; r++) {
				for (int c = 0; c < 3; c++) {
					f.gte.rotation.m[r][c] = (int16_t)CLAMP(std::lround(f.row_scale[r] * b.rows[r][c] * k), -32768L, 32767L);
				}
			}
			// Direct light scaled by the diffuse share of a metallic surface.
			const float diffuse = 1.0f - mat.metallic;
			for (int l = 0; l < 3; l++) {
				for (int ch = 0; ch < 3; ch++) {
					f.gte.light_colors.m[ch][l] = (int16_t)CLAMP(std::lround(f.light_colors[l][ch] * diffuse * 4096.0f), 0L, 32767L);
				}
			}

			// Point lights in this mesh's stored units.
			psx::PointLight point_lights[psx::MAX_POINT_LIGHTS];
			const double units_per_mesh_unit = settings.world_scale * sb / (double)(1 << data->shift);
			for (int l = 0; l < f.near_light_count; l++) {
				const Light &src_light = *f.near_lights[l];
				psx::PointLight &pl = point_lights[l];
				const Vector3 p = rotation_inv.xform(src_light.position - p_transform.origin) * (units_per_mesh_unit / s);
				pl.position[0] = (float)p.x;
				pl.position[1] = (float)p.y;
				pl.position[2] = (float)p.z;
				pl.metres_per_unit = (float)((double)(1 << data->shift) / settings.world_scale);
				pl.range = src_light.range;
				pl.decay = src_light.decay;
				pl.color[0] = src_light.color.r * diffuse;
				pl.color[1] = src_light.color.g * diffuse;
				pl.color[2] = src_light.color.b * diffuse;
				const Vector3 axis = rotation_inv.xform(src_light.spot_axis).normalized();
				pl.spot_axis[0] = (float)axis.x;
				pl.spot_axis[1] = (float)axis.y;
				pl.spot_axis[2] = (float)axis.z;
				pl.spot_cos = src_light.spot_cos;
				pl.spot_edge_exponent = src_light.spot_edge_exponent;
			}

			psx::DrawParams params;
			params.point_lights = point_lights;
			for (int a = 0; a < 3; a++) {
				params.point_scale[a] = (float)(1.0 / normal_scale[a]);
			}
			params.point_light_count = mat.lit ? f.near_light_count : 0;
			params.cull = mat.cull;
			params.lit = mat.lit;
			params.lighting = settings.lighting;
			params.frame_dither = settings.dither; // The frame's drawing environment (PSXSceneRenderRD)
			params.dither = mat.dither == PSXMaterialOverrides::DITHER_DEFAULT ? settings.dither : mat.dither == PSXMaterialOverrides::DITHER_ON;
			const Color em = Color(MIN(mat.emission.r, 1.0f), MIN(mat.emission.g, 1.0f), MIN(mat.emission.b, 1.0f)).linear_to_srgb();
			params.emission[0] = _to_byte(em.r);
			params.emission[1] = _to_byte(em.g);
			params.emission[2] = _to_byte(em.b);
			if (p_data.has_color && mat.vertex_colors) {
				// COLOR times the instance colour, as a tint on the baked
				// display colour: linear COLOR goes to sRGB first.
				for (int ch = 0; ch < 3; ch++) {
					const float c = CLAMP(p_data.color[ch], 0.0f, 1.0f);
					params.tint[ch] = _to_byte(mat.vertex_colors_srgb ? c : psx::linear_to_srgb(c));
				}
			}
			params.fog = f.fog;
			params.semi = mat.semi;
			params.semi_whole = mat.semi_whole;
			params.blend = mat.blend;
			params.texture = tex ? &tex->draw[mat.blend & 3] : nullptr;
			params.ortho = f.ortho;
			params.near_z = f.near_z;
			params.ot_shift = f.ot_shift;
			params.ot_bias = -CLAMP(mat.render_priority, -64, 64);
			params.far_z = f.far_z;
			params.screen_width = f.width;
			params.screen_height = f.height;
			params.point_size = MAX(1, (int)Math::round(mat.point_size)) * f.gte.scale;
			params.depth = f.depth;
			psx::draw_mesh(f.gte, *f.builder, *f.ot, *data, params, f.stats->draw);
		}
	}

	// MultiMesh: every visible instance is drawn as its own object. The
	// transforms come from the multimesh buffer, read once per frame.
	void _draw_multimesh(Frame &f, RendererRD::MeshStorage *p_storage, const RenderGeometryInstanceBase *p_inst) {
		const RID mm = p_inst->data->base;
		const RID mesh = p_storage->_multimesh_get_mesh(mm);
		if (!mesh.is_valid() || p_storage->multimesh_get_transform_format(mm) != RSE::MULTIMESH_TRANSFORM_3D) {
			return;
		}
		const int total = p_storage->_multimesh_get_instance_count(mm);
		int visible = p_storage->_multimesh_get_visible_instances(mm);
		visible = visible < 0 ? total : MIN(visible, total);
		if (visible <= 0) {
			return;
		}
		const Vector<float> buffer = p_storage->_multimesh_get_buffer(mm);
		if (buffer.size() < total * 12) {
			return;
		}
		const int stride = buffer.size() / total;
		// Per instance: 12 transform floats, then 4 colour floats when used,
		// then 4 custom data floats when used.
		const bool uses_colors = p_storage->multimesh_uses_colors(mm);
		const int color = uses_colors ? 12 : -1;
		const int custom = p_storage->multimesh_uses_custom_data(mm) ? 12 + (uses_colors ? 4 : 0) : -1;
		const float *r = buffer.ptr();
		for (int k = 0; k < visible; k++) {
			const float *d = r + k * stride;
			const Transform3D local(Basis(d[0], d[1], d[2], d[4], d[5], d[6], d[8], d[9], d[10]), Vector3(d[3], d[7], d[11]));
			PSXInstanceData data;
			if (color >= 0 && color + 4 <= stride) {
				data.color = Color(d[color], d[color + 1], d[color + 2], d[color + 3]);
				data.has_color = true;
			}
			if (custom >= 0 && custom + 4 <= stride) {
				data.spin = d[custom];
				data.anim = d[custom + 2];
			}
			_draw_instance(f, p_storage, p_inst, mesh, p_inst->transform * local, data);
		}
	}

	// GPUParticles3D: Godot simulates them on the GPU; their instance data
	// (what Godot draws them with) is read back every frame and each live
	// particle drawn with every draw pass's mesh, like a MultiMesh instance.
	// The read-back waits for the GPU. Trails are not drawn.
	void _draw_particles(Frame &f, RendererRD::MeshStorage *p_storage, const RenderGeometryInstanceBase *p_inst) {
		RendererRD::ParticlesStorage *ps = RendererRD::ParticlesStorage::get_singleton();
		const RID particles = p_inst->data->base;
		if (!ps || !ps->owns_particles(particles) || ps->particles_is_inactive(particles)) {
			return;
		}
		uint32_t amount = 0, trail_steps = 1;
		{
			PSXScopedTimer timer(f.stats->particles_read_usec);
			particle_buffer = ps->particles_get_instance_data(particles, amount, trail_steps);
		}
		if (trail_steps > 1) {
			WARN_PRINT_ONCE("PSX: GPUParticles3D trails are not drawn by the PS1 renderer.");
			return;
		}
		constexpr int STRIDE = 20; // 12 transform floats (rows), colour, custom data
		if (amount == 0 || particle_buffer.size() < (int)(amount * STRIDE * sizeof(float))) {
			return;
		}
		// Particles in local coordinates are placed by the node; the others
		// are already in world space.
		const Transform3D base = ps->particles_is_using_local_coords(particles) ? p_inst->transform : Transform3D();
		const float *r = reinterpret_cast<const float *>(particle_buffer.ptr());
		const int passes = ps->particles_get_draw_passes(particles);
		for (int pass = 0; pass < passes; pass++) {
			const RID mesh = ps->particles_get_draw_pass_mesh(particles, pass);
			if (!mesh.is_valid() || !p_storage->owns_mesh(mesh)) {
				continue;
			}
			for (uint32_t k = 0; k < amount; k++) {
				const float *d = r + k * STRIDE;
				// Dead particles have a zero basis and an infinite origin.
				if (!std::isfinite(d[3]) || !std::isfinite(d[7]) || !std::isfinite(d[11])) {
					continue;
				}
				const Basis basis(d[0], d[1], d[2], d[4], d[5], d[6], d[8], d[9], d[10]);
				if (basis.determinant() == 0.0f) {
					continue;
				}
				PSXInstanceData data;
				data.color = Color(d[12], d[13], d[14], d[15]);
				data.has_color = true;
				data.spin = d[16];
				data.anim = d[18];
				f.stats->particles++;
				_draw_instance(f, p_storage, p_inst, mesh, base * Transform3D(basis, Vector3(d[3], d[7], d[11])), data);
			}
		}
	}

	// Blend shapes (blend_cache's weights, applied as Godot's skinning shader
	// does) then linear-blend skinning on the CPU, then the surface's pose
	// template (converted once from the rest pose) posed with the result:
	// only positions and normals are converted every frame. Returns nullptr
	// if nothing to draw.
	const psx::MeshData *_skin(MeshEntry &p_entry, uint32_t p_surface, int32_t p_bucket, bool p_shapes, bool p_normalized, bool p_skin, const PSXMaterialInfo &p_mat, const psx::TextureData *p_texture, const Vector3 &p_normal_scale, double p_scale, Frame &f) {
		const SourceSurface &p_src = p_entry.surfaces[p_surface];
		const psx::ConvertOptions opt = _options(p_mat, p_texture, p_normal_scale, p_scale);
		const uint64_t key = _conversion_key(p_surface, p_bucket, p_mat, p_texture, p_normal_scale);
		psx::PoseTemplate *pose = p_entry.pose_templates.getptr(key);
		if (!pose) {
			pose = &p_entry.pose_templates.insert(key, psx::PoseTemplate())->value;
			PSXScopedTimer timer(f.stats->mesh_convert_usec);
			psx::convert_pose_template(_input(p_src), opt, *pose); // Empty on failure: remembered, not retried.
			f.stats->conversions++;
			f.stats->meshes_converted++;
		}
		if (pose->mesh.indices.empty()) {
			return nullptr;
		}
		PSXScopedTimer timer(f.stats->skin_usec);
		const uint32_t n = p_src.vertex_count;
		const int inf = p_src.influences;
		const bool has_normals = !p_src.normals.is_empty();
		skin_positions.resize(n * 3);
		skin_normals.resize(has_normals ? n * 3 : 0);
		const int bone_count = (int)bone_cache.size();
		const int influences = p_skin ? inf : 0;
		float shape_total = 0.0f;
		if (p_shapes) {
			for (const BlendWeight &bw : blend_cache) {
				if (bw.shape < p_src.shape_count) {
					shape_total += bw.weight;
				}
			}
		}
		for (uint32_t v = 0; v < n; v++) {
			Vector3 p(p_src.positions[v * 3], p_src.positions[v * 3 + 1], p_src.positions[v * 3 + 2]);
			Vector3 nrm = has_normals ? Vector3(p_src.normals[v * 3], p_src.normals[v * 3 + 1], p_src.normals[v * 3 + 2]) : Vector3();
			if (p_shapes) {
				Vector3 bp, bn;
				for (const BlendWeight &bw : blend_cache) {
					if (bw.shape >= p_src.shape_count) {
						continue;
					}
					const uint32_t o = (bw.shape * n + v) * 3;
					bp += Vector3(p_src.shape_positions[o], p_src.shape_positions[o + 1], p_src.shape_positions[o + 2]) * bw.weight;
					if (has_normals) {
						bn += Vector3(p_src.shape_normals[o], p_src.shape_normals[o + 1], p_src.shape_normals[o + 2]) * bw.weight;
					}
				}
				if (p_normalized) {
					p *= 1.0f - shape_total;
					nrm *= 1.0f - shape_total;
				}
				p += bp;
				nrm = (nrm + bn).normalized();
			}
			Vector3 sp, sn;
			float total = 0.0f;
			for (int j = 0; j < influences; j++) {
				const float w = p_src.weights[v * inf + j];
				const int bone = p_src.bones[v * inf + j];
				if (w <= 0.0f || bone < 0 || bone >= bone_count) {
					continue;
				}
				const Transform3D &t = bone_cache[bone];
				sp += t.xform(p) * w;
				sn += t.basis.xform(nrm) * w;
				total += w;
			}
			if (total <= 0.0f) {
				sp = p;
				sn = nrm;
			}
			skin_positions[v * 3] = (float)sp.x;
			skin_positions[v * 3 + 1] = (float)sp.y;
			skin_positions[v * 3 + 2] = (float)sp.z;
			if (has_normals) {
				skin_normals[v * 3] = (float)sn.x;
				skin_normals[v * 3 + 1] = (float)sn.y;
				skin_normals[v * 3 + 2] = (float)sn.z;
			}
		}
		psx::apply_pose(*pose, skin_positions.ptr(), has_normals ? skin_normals.ptr() : nullptr, opt, skin_mesh, pose_scratch);
		f.stats->skinned++;
		return skin_mesh.indices.empty() ? nullptr : &skin_mesh;
	}

	static psx::MeshInput _input(const SourceSurface &p_src) {
		psx::MeshInput in;
		in.positions = p_src.positions.ptr();
		in.normals = p_src.normals.is_empty() ? nullptr : p_src.normals.ptr();
		in.colors = p_src.colors.is_empty() ? nullptr : p_src.colors.ptr();
		in.uvs = p_src.uvs.is_empty() ? nullptr : p_src.uvs.ptr();
		in.vertex_count = (int32_t)p_src.vertex_count;
		in.indices = p_src.indices.is_empty() ? nullptr : p_src.indices.ptr();
		in.index_count = (int32_t)p_src.indices.size();
		in.primitive = p_src.primitive;
		return in;
	}

	psx::ConvertOptions _options(const PSXMaterialInfo &p_mat, const psx::TextureData *p_texture, const Vector3 &p_normal_scale, double p_scale) const {
		psx::ConvertOptions opt;
		opt.scale = (float)(settings.world_scale * p_scale);
		opt.albedo[0] = p_mat.albedo.r;
		opt.albedo[1] = p_mat.albedo.g;
		opt.albedo[2] = p_mat.albedo.b;
		opt.albedo[3] = p_mat.albedo.a;
		opt.use_vertex_colors = p_mat.vertex_colors;
		opt.vertex_colors_srgb = p_mat.vertex_colors_srgb;
		opt.use_alpha = p_mat.semi && p_mat.blend == psx::BLEND_HALF && !p_mat.semi_whole;
		opt.max_edge = p_mat.no_subdivide ? 0.0f : settings.subdivide_size * settings.world_scale;
		opt.normal_scale[0] = (float)p_normal_scale.x;
		opt.normal_scale[1] = (float)p_normal_scale.y;
		opt.normal_scale[2] = (float)p_normal_scale.z;
		if (p_texture) {
			opt.texture_width = p_texture->width;
			opt.texture_height = p_texture->height;
			opt.texture_repeat = p_mat.texture_repeat;
			opt.uv_scale[0] = p_mat.uv_scale.x;
			opt.uv_scale[1] = p_mat.uv_scale.y;
			opt.uv_offset[0] = p_mat.uv_offset.x;
			opt.uv_offset[1] = p_mat.uv_offset.y;
			opt.keep_far_edge = p_mat.anim;
		}
		return opt;
	}

	// What says a mesh's surfaces changed (added, replaced or cleared): each
	// surface's storage, bounds, vertex count and format. Not the mesh AABB:
	// Godot overwrites that with the posed bounds of a skinned mesh every
	// frame it is drawn, which made every animated mesh read back again.
	static uint32_t _mesh_signature(RendererRD::MeshStorage *p_storage, RID p_mesh, int p_surface_count) {
		uint32_t h = hash_murmur3_one_32((uint32_t)p_surface_count);
		for (int i = 0; i < p_surface_count; i++) {
			void *surface = p_storage->mesh_get_surface(p_mesh, (uint32_t)i); // The storage pointer, not the read-back overload
			h = hash_murmur3_one_64((uint64_t)(uintptr_t)surface, h);
			if (!surface) {
				continue;
			}
			const AABB b = p_storage->mesh_surface_get_aabb(surface);
			for (int a = 0; a < 3; a++) {
				h = hash_murmur3_one_float((float)b.position[a], h);
				h = hash_murmur3_one_float((float)b.size[a], h);
			}
			h = hash_murmur3_one_32(p_storage->mesh_surface_get_vertex_count(surface), h);
			h = hash_murmur3_one_64(p_storage->mesh_surface_get_format(surface), h);
		}
		return hash_fmix32(h);
	}

	// Whether p_mesh is new or its surfaces changed since they were read.
	bool _mesh_needs_read(RendererRD::MeshStorage *p_storage, RID p_mesh) {
		const int surface_count = p_storage->mesh_get_surface_count(p_mesh);
		const MeshEntry *entry = meshes.getptr(p_mesh);
		return !entry || entry->signature != _mesh_signature(p_storage, p_mesh, surface_count) || (int)entry->surfaces.size() != surface_count;
	}

	// Surface data read back for meshes about to be read (_get_mesh takes it).
	HashMap<RID, LocalVector<RenderingServerTypes::SurfaceData>> fetched;

	// Reads the surfaces of p_meshes back from the GPU into fetched. Every
	// buffer read waits for the GPU to finish all its work, a few
	// milliseconds, and a surface has several buffers (and LOD index buffers
	// the PS1 never uses), so the buffers are queued as asynchronous reads
	// and one synchronous read waits once for all of them. Batches stay
	// well inside the download staging space, beyond which RenderingDevice
	// would flush part way through a read. Meshes with blend shapes (whose
	// buffer is not exposed) and anything a batch could not take are read
	// the slow way.
	void _fetch_surfaces(RendererRD::MeshStorage *p_storage, const LocalVector<RID> &p_meshes) {
		RD *rd = RD::get_singleton();
		const RendererRD::MeshStorage *reader = p_storage; // The const mesh_get_surface reads back
		const uint64_t staging_mb = (uint64_t)MAX(1, (int)GLOBAL_GET("rendering/rendering_device/staging_buffer/max_size_mb"));
		const uint64_t budget = staging_mb * 1024 * 1024 / 4;
		struct Part {
			RID mesh;
			uint32_t surface = 0;
			int which = 0; // 0 vertex, 1 attribute, 2 skin, 3 index
			RID buffer;
			std::shared_ptr<PSXDownload> slot;
		};
		LocalVector<Part> parts;
		uint64_t batch_bytes = 0;
		RID trigger; // A vertex buffer of this batch (at least 12 bytes)
		auto finish = [&]() {
			if (parts.is_empty()) {
				return;
			}
			if (trigger.is_valid()) {
				rd->buffer_get_data(trigger, 0, 4); // The one wait: queued reads arrive in it.
			}
			for (Part &part : parts) {
				if (!part.slot->done) {
					part.slot->data = rd->buffer_get_data(part.buffer); // Not delivered: read it now.
				}
				RenderingServerTypes::SurfaceData &sd = fetched[part.mesh][part.surface];
				switch (part.which) {
					case 0:
						sd.vertex_data = part.slot->data;
						// As MeshStorage::mesh_get_surface: uncompressed normals
						// without tangents are stored with 4 bytes of padding.
						if (!(sd.format & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES) && (sd.format & RSE::ARRAY_FORMAT_NORMAL) && !(sd.format & RSE::ARRAY_FORMAT_TANGENT) && sd.vertex_data.size() >= 4) {
							sd.vertex_data.resize(sd.vertex_data.size() - sizeof(uint16_t) * 2);
						}
						break;
					case 1:
						sd.attribute_data = part.slot->data;
						break;
					case 2:
						sd.skin_data = part.slot->data;
						break;
					default:
						sd.index_data = part.slot->data;
						break;
				}
			}
			parts.clear();
			batch_bytes = 0;
			trigger = RID();
		};
		for (const RID &mesh : p_meshes) {
			const int count = p_storage->mesh_get_surface_count(mesh);
			LocalVector<RenderingServerTypes::SurfaceData> &out = fetched.insert(mesh, LocalVector<RenderingServerTypes::SurfaceData>())->value;
			out.resize(count);
			const bool shapes = p_storage->mesh_get_blend_shape_count(mesh) > 0;
			for (int i = 0; i < count; i++) {
				void *surface = p_storage->mesh_get_surface(mesh, (uint32_t)i);
				if (!surface) {
					continue;
				}
				RenderingServerTypes::SurfaceData &sd = out[i];
				sd.format = p_storage->mesh_surface_get_format(surface);
				sd.vertex_count = p_storage->mesh_surface_get_vertex_count(surface);
				sd.primitive = p_storage->mesh_surface_get_primitive(surface);
				sd.aabb = p_storage->mesh_surface_get_aabb(surface);
				sd.uv_scale = p_storage->mesh_surface_get_uv_scale(surface);
				const RID buffers[4] = {
					p_storage->mesh_surface_get_vertex_buffer_rd_rid(mesh, i),
					p_storage->mesh_surface_get_attribute_buffer_rd_rid(mesh, i),
					p_storage->mesh_surface_get_skin_buffer_rd_rid(mesh, i),
					p_storage->mesh_surface_get_index_buffer_rd_rid(mesh, i),
				};
				sd.index_count = buffers[3].is_valid() ? p_storage->mesh_surface_get_vertices_drawn_count(surface) : 0;
				// Generous: no vertex format takes 256 bytes.
				const uint64_t estimate = (uint64_t)sd.vertex_count * 256 + (uint64_t)sd.index_count * 4 + 64;
				if (shapes || estimate > budget || !buffers[0].is_valid()) {
					out[i] = reader->mesh_get_surface(mesh, i);
					continue;
				}
				if (batch_bytes + estimate > budget) {
					finish();
				}
				for (int w = 0; w < 4; w++) {
					if (!buffers[w].is_valid()) {
						continue;
					}
					Part part;
					part.mesh = mesh;
					part.surface = (uint32_t)i;
					part.which = w;
					part.buffer = buffers[w];
					part.slot = std::make_shared<PSXDownload>();
					if (rd->buffer_get_data_async(buffers[w], Callable(memnew(PSXDownloadCallable(part.slot)))) != OK) {
						part.slot->data = rd->buffer_get_data(buffers[w]);
						part.slot->done = true;
					}
					parts.push_back(part);
				}
				trigger = buffers[0];
				batch_bytes += estimate;
			}
		}
		finish();
	}

	// Reads, in one batch, every mesh the instances will draw that is new
	// or changed (particle draw passes are read when drawn).
	void _prefetch_meshes(RendererRD::MeshStorage *p_storage, const PagedArray<RenderGeometryInstance *> &p_instances) {
		LocalVector<RID> wanted;
		HashSet<RID> seen;
		for (uint32_t i = 0; i < p_instances.size(); i++) {
			const RenderGeometryInstanceBase *inst = static_cast<const RenderGeometryInstanceBase *>(p_instances[i]);
			if (!inst->data || (inst->layer_mask & USER_LAYERS) == 0) {
				continue;
			}
			RID mesh;
			if (inst->data->base_type == RSE::INSTANCE_MESH) {
				mesh = inst->data->base;
			} else if (inst->data->base_type == RSE::INSTANCE_MULTIMESH) {
				mesh = p_storage->_multimesh_get_mesh(inst->data->base);
			}
			if (mesh.is_valid() && p_storage->owns_mesh(mesh) && !seen.has(mesh)) {
				seen.insert(mesh);
				if (_mesh_needs_read(p_storage, mesh)) {
					wanted.push_back(mesh);
				}
			}
		}
		if (wanted.is_empty()) {
			return;
		}
		PSXSceneStats &st = active_stats ? *active_stats : unused_stats;
		PSXScopedTimer timer(st.mesh_read_usec);
		_fetch_surfaces(p_storage, wanted);
	}

	// Images read back for textures about to be converted (_source_image
	// takes them).
	HashMap<RID, Ref<Image>> fetched_images;

	// Reads back, in one batch, every texture the instances' materials use
	// that has not been read yet (or changed since): like a mesh buffer,
	// each texture read on its own waits for the GPU to finish all its
	// work. Textures baked at import and render targets are not read here;
	// anything this misses is read when converted.
	void _prefetch_textures(RendererRD::MeshStorage *p_storage, const PagedArray<RenderGeometryInstance *> &p_instances) {
		RendererRD::TextureStorage *ts = RendererRD::TextureStorage::get_singleton();
		LocalVector<RID> wanted;
		HashSet<RID> seen_materials, seen_textures;
		auto want = [&](RID p_material) {
			if (!p_material.is_valid() || seen_materials.has(p_material)) {
				return;
			}
			seen_materials.insert(p_material);
			const PSXMaterialInfo m = psx_read_material<ShaderDataT>(p_material);
			const RID tex = m.texture;
			if (!tex.is_valid() || seen_textures.has(tex)) {
				return;
			}
			seen_textures.insert(tex);
			if (!ts->owns_texture(tex) || ts->texture_is_render_target(tex) || PSXTexture::get_baked_version(tex, m.alpha_mode, m.alpha_cut) != 0) {
				return;
			}
			const SourceImage *found = source_images.getptr(tex);
			if (!found || found->version != ts->texture_get_content_version(tex)) {
				wanted.push_back(tex);
			}
		};
		for (uint32_t i = 0; i < p_instances.size(); i++) {
			const RenderGeometryInstanceBase *inst = static_cast<const RenderGeometryInstanceBase *>(p_instances[i]);
			if (!inst->data || (inst->layer_mask & USER_LAYERS) == 0) {
				continue;
			}
			RID mesh;
			if (inst->data->base_type == RSE::INSTANCE_MESH) {
				mesh = inst->data->base;
			} else if (inst->data->base_type == RSE::INSTANCE_MULTIMESH) {
				mesh = p_storage->_multimesh_get_mesh(inst->data->base);
			}
			if (!mesh.is_valid() || !p_storage->owns_mesh(mesh)) {
				continue;
			}
			// As _draw_instance picks each surface's material.
			if (inst->data->material_override.is_valid()) {
				want(inst->data->material_override);
				continue;
			}
			const int count = p_storage->mesh_get_surface_count(mesh);
			for (int surface = 0; surface < count; surface++) {
				RID material;
				if (surface < inst->data->surface_materials.size()) {
					material = inst->data->surface_materials[surface];
				}
				want(material.is_valid() ? material : p_storage->mesh_surface_get_material(mesh, surface));
			}
		}
		if (wanted.is_empty()) {
			return;
		}
		PSXSceneStats &st = active_stats ? *active_stats : unused_stats;
		PSXScopedTimer timer(st.texture_read_usec);
		RD *rd = RD::get_singleton();
		const uint64_t staging_mb = (uint64_t)MAX(1, (int)GLOBAL_GET("rendering/rendering_device/staging_buffer/max_size_mb"));
		const uint64_t budget = staging_mb * 1024 * 1024 / 4;
		struct Part {
			RID texture;
			RID source;
			std::shared_ptr<PSXDownload> slot;
		};
		LocalVector<Part> parts;
		uint64_t batch_bytes = 0;
		auto finish = [&]() {
			if (parts.is_empty()) {
				return;
			}
			// The one wait: the last texture read again directly; the queued
			// reads arrive in it.
			Part &last = parts[parts.size() - 1];
			last.slot->data = rd->texture_get_data(last.source, 0);
			last.slot->done = true;
			for (Part &part : parts) {
				if (!part.slot->done) {
					part.slot->data = rd->texture_get_data(part.source, 0); // Not delivered: read it now.
				}
				if (!part.slot->data.is_empty()) {
					fetched_images[part.texture] = ts->texture_2d_get_from_data(part.texture, part.slot->data);
				}
			}
			parts.clear();
			batch_bytes = 0;
		};
		for (const RID &tex : wanted) {
			const RID source = ts->texture_2d_get_read_source(tex);
			if (!source.is_valid() || !rd->texture_is_valid(source)) {
				continue; // Kept by the texture storage, or nothing to read
			}
			const RD::TextureFormat format = rd->texture_get_format(source);
			// Generous: 16 bytes a texel, and twice that for the mipmaps.
			const uint64_t estimate = (uint64_t)format.width * format.height * 32 + 256;
			if (estimate > budget) {
				continue;
			}
			if (batch_bytes + estimate > budget) {
				finish();
			}
			Part part;
			part.texture = tex;
			part.source = source;
			part.slot = std::make_shared<PSXDownload>();
			if (rd->texture_get_data_async(source, 0, Callable(memnew(PSXDownloadCallable(part.slot)))) != OK) {
				continue;
			}
			parts.push_back(part);
			batch_bytes += estimate;
		}
		finish();
	}

	MeshEntry *_get_mesh(RendererRD::MeshStorage *p_storage, RID p_mesh) {
		const AABB aabb = p_storage->mesh_get_aabb(p_mesh);
		const int surface_count = p_storage->mesh_get_surface_count(p_mesh);
		const uint32_t signature = _mesh_signature(p_storage, p_mesh, surface_count);
		MeshEntry *entry = meshes.getptr(p_mesh);
		if (entry && entry->signature == signature && (int)entry->surfaces.size() == surface_count) {
			entry->aabb = aabb;
			entry->last_used = frame;
			return entry;
		}
		// New or changed: read every surface back from the mesh storage
		// (already fetched with the others this frame, or now).
		PSXSceneStats &st = active_stats ? *active_stats : unused_stats;
		PSXScopedTimer timer(st.mesh_read_usec);
		st.meshes_read++;
		LocalVector<RenderingServerTypes::SurfaceData> *surface_data = fetched.getptr(p_mesh);
		if (!surface_data || (int)surface_data->size() != surface_count) {
			fetched.erase(p_mesh);
			LocalVector<RID> one;
			one.push_back(p_mesh);
			_fetch_surfaces(p_storage, one);
			surface_data = fetched.getptr(p_mesh);
		}
		if (!entry) {
			entry = &meshes.insert(p_mesh, MeshEntry())->value;
		}
		entry->aabb = aabb;
		entry->mesh = p_mesh;
		entry->signature = signature;
		entry->last_used = frame;
		entry->converted.clear();
		entry->pose_templates.clear();
		entry->surfaces.clear();
		entry->surfaces.resize(surface_count);
		for (int i = 0; i < surface_count; i++) {
			const RenderingServerTypes::SurfaceData &sd = (*surface_data)[i];
			if (sd.vertex_count == 0 || sd.primitive >= RSE::PRIMITIVE_MAX) {
				continue;
			}
			const Array arrays = RenderingServer::get_singleton()->mesh_create_arrays_from_surface_data(sd);
			if (arrays.size() != RSE::ARRAY_MAX) {
				continue;
			}
			SourceSurface &dst = entry->surfaces[i];
			const PackedVector3Array vertices = arrays[RSE::ARRAY_VERTEX];
			const PackedVector3Array normals = arrays[RSE::ARRAY_NORMAL];
			const PackedColorArray colors = arrays[RSE::ARRAY_COLOR];
			const PackedVector2Array uvs = arrays[RSE::ARRAY_TEX_UV];
			const PackedInt32Array indices = arrays[RSE::ARRAY_INDEX];
			const PackedInt32Array bones = arrays[RSE::ARRAY_BONES];
			const PackedFloat32Array weights = arrays[RSE::ARRAY_WEIGHTS];
			const int n = vertices.size();
			dst.vertex_count = n;
			dst.positions.resize(n * 3);
			for (int v = 0; v < n; v++) {
				dst.positions[v * 3 + 0] = (float)vertices[v].x;
				dst.positions[v * 3 + 1] = (float)vertices[v].y;
				dst.positions[v * 3 + 2] = (float)vertices[v].z;
			}
			if (normals.size() == n) {
				dst.normals.resize(n * 3);
				for (int v = 0; v < n; v++) {
					dst.normals[v * 3 + 0] = (float)normals[v].x;
					dst.normals[v * 3 + 1] = (float)normals[v].y;
					dst.normals[v * 3 + 2] = (float)normals[v].z;
				}
			}
			if (colors.size() == n) {
				dst.colors.resize(n * 4);
				for (int v = 0; v < n; v++) {
					dst.colors[v * 4 + 0] = colors[v].r;
					dst.colors[v * 4 + 1] = colors[v].g;
					dst.colors[v * 4 + 2] = colors[v].b;
					dst.colors[v * 4 + 3] = colors[v].a;
				}
			}
			if (uvs.size() == n && n > 0) {
				dst.uv_min = uvs[0];
				dst.uv_max = uvs[0];
				for (int v = 1; v < n; v++) {
					dst.uv_min = dst.uv_min.min(uvs[v]);
					dst.uv_max = dst.uv_max.max(uvs[v]);
				}
				dst.uvs.resize(n * 2);
				for (int v = 0; v < n; v++) {
					dst.uvs[v * 2 + 0] = (float)uvs[v].x;
					dst.uvs[v * 2 + 1] = (float)uvs[v].y;
				}
			}
			_read_indices(sd.primitive, indices, n, dst);
			if (n > 0 && bones.size() >= n * 4 && bones.size() == weights.size() && bones.size() % n == 0) {
				dst.influences = bones.size() / n;
				dst.bones.resize(bones.size());
				dst.weights.resize(weights.size());
				for (int k = 0; k < bones.size(); k++) {
					dst.bones[k] = bones[k];
					dst.weights[k] = weights[k];
				}
			}
			_read_blend_shapes(sd, dst);
		}
		fetched.erase(p_mesh);
		return entry;
	}

	// The surface's indices as a list: strips are unrolled (a triangle
	// strip's odd triangles flipped to keep the winding), and a strip with
	// no index array uses its vertices in order.
	static void _read_indices(RSE::PrimitiveType p_primitive, const PackedInt32Array &p_indices, int p_vertex_count, SourceSurface &r_dst) {
		r_dst.primitive = p_primitive == RSE::PRIMITIVE_POINTS ? psx::MESH_POINTS : ((p_primitive == RSE::PRIMITIVE_LINES || p_primitive == RSE::PRIMITIVE_LINE_STRIP) ? psx::MESH_LINES : psx::MESH_TRIANGLES);
		const bool strip = p_primitive == RSE::PRIMITIVE_LINE_STRIP || p_primitive == RSE::PRIMITIVE_TRIANGLE_STRIP;
		if (!strip) {
			r_dst.indices.resize(p_indices.size());
			for (int k = 0; k < p_indices.size(); k++) {
				r_dst.indices[k] = p_indices[k];
			}
			return;
		}
		const int count = p_indices.is_empty() ? p_vertex_count : p_indices.size();
		auto at = [&](int p_k) { return p_indices.is_empty() ? p_k : p_indices[p_k]; };
		r_dst.indices.clear();
		if (p_primitive == RSE::PRIMITIVE_LINE_STRIP) {
			for (int k = 0; k + 1 < count; k++) {
				r_dst.indices.push_back(at(k));
				r_dst.indices.push_back(at(k + 1));
			}
			return;
		}
		for (int k = 0; k + 2 < count; k++) {
			const bool odd = (k & 1) != 0;
			r_dst.indices.push_back(at(k));
			r_dst.indices.push_back(at(odd ? k + 2 : k + 1));
			r_dst.indices.push_back(at(odd ? k + 1 : k + 2));
		}
	}

	// Decodes a surface's blend shapes (stored uncompressed, positions and
	// normals only, one block per shape) into the source surface.
	static void _read_blend_shapes(const RenderingServerTypes::SurfaceData &p_sd, SourceSurface &r_dst) {
		const uint32_t n = r_dst.vertex_count;
		if (p_sd.blend_shape_data.is_empty() || n == 0 || p_sd.vertex_count != n) {
			return;
		}
		RenderingServer *rs = RenderingServer::get_singleton();
		const uint64_t bs_format = p_sd.format & RSE::ARRAY_FORMAT_BLEND_SHAPE_MASK;
		uint32_t offsets[RSE::ARRAY_MAX];
		uint32_t vertex_size = 0, normal_size = 0, attrib_size = 0, skin_size = 0;
		rs->mesh_surface_make_offsets_from_format(bs_format, n, 0, offsets, vertex_size, normal_size, attrib_size, skin_size);
		const int block = (int)((vertex_size + normal_size) * n);
		if (block <= 0 || p_sd.blend_shape_data.size() % block != 0) {
			return;
		}
		const uint32_t count = p_sd.blend_shape_data.size() / block;
		const bool has_normals = !r_dst.normals.is_empty();
		r_dst.shape_positions.resize(count * n * 3);
		r_dst.shape_normals.resize(has_normals ? count * n * 3 : 0);
		for (uint32_t i = 0; i < count; i++) {
			// Read like a surface of its own: the same decoding as the base.
			RenderingServerTypes::SurfaceData shape;
			shape.primitive = p_sd.primitive;
			shape.format = bs_format;
			shape.vertex_data = p_sd.blend_shape_data.slice(i * block, (i + 1) * block);
			shape.vertex_count = n;
			shape.aabb = p_sd.aabb;
			shape.uv_scale = p_sd.uv_scale;
			const Array arrays = rs->mesh_create_arrays_from_surface_data(shape);
			if (arrays.size() != RSE::ARRAY_MAX) {
				r_dst.shape_positions.clear();
				r_dst.shape_normals.clear();
				return;
			}
			const PackedVector3Array vertices = arrays[RSE::ARRAY_VERTEX];
			const PackedVector3Array normals = arrays[RSE::ARRAY_NORMAL];
			float *pos = r_dst.shape_positions.ptr() + i * n * 3;
			for (uint32_t v = 0; v < n; v++) {
				const Vector3 q = v < (uint32_t)vertices.size() ? vertices[v] : Vector3(r_dst.positions[v * 3], r_dst.positions[v * 3 + 1], r_dst.positions[v * 3 + 2]);
				pos[v * 3 + 0] = (float)q.x;
				pos[v * 3 + 1] = (float)q.y;
				pos[v * 3 + 2] = (float)q.z;
			}
			if (has_normals) {
				float *nrm = r_dst.shape_normals.ptr() + i * n * 3;
				for (uint32_t v = 0; v < n; v++) {
					const Vector3 q = v < (uint32_t)normals.size() ? normals[v] : Vector3(r_dst.normals[v * 3], r_dst.normals[v * 3 + 1], r_dst.normals[v * 3 + 2]);
					nrm[v * 3 + 0] = (float)q.x;
					nrm[v * 3 + 1] = (float)q.y;
					nrm[v * 3 + 2] = (float)q.z;
				}
			}
		}
		r_dst.shape_count = count;
	}

	// What a converted surface is cached by: the material, the settings and
	// scale baked into it, the texture size, the scale bucket and surface.
	uint64_t _conversion_key(uint32_t p_surface, int32_t p_bucket, const PSXMaterialInfo &p_mat, const psx::TextureData *p_texture, const Vector3 &p_normal_scale) const {
		uint32_t h = p_mat.hash();
		// Settings that change while running (PSXLiveSettings) re-convert.
		h = hash_murmur3_one_float(settings.world_scale, h);
		h = hash_murmur3_one_float(p_mat.no_subdivide ? 0.0f : settings.subdivide_size, h);
		for (int a = 0; a < 3; a++) {
			h = hash_murmur3_one_float((float)p_normal_scale[a], h);
		}
		if (p_texture) {
			h = hash_murmur3_one_32((uint32_t)(p_texture->width | p_texture->height << 16), h);
		}
		return ((uint64_t)hash_fmix32(h) << 32) | ((uint64_t)(uint16_t)p_bucket << 16) | (p_surface & 0xFFFF);
	}

	const psx::MeshData *_get_converted(MeshEntry &p_entry, uint32_t p_surface, int32_t p_bucket, double p_scale, const PSXMaterialInfo &p_mat, const psx::TextureData *p_texture, const Vector3 &p_normal_scale, PSXSceneStats &r_stats) {
		const uint64_t key = _conversion_key(p_surface, p_bucket, p_mat, p_texture, p_normal_scale);
		psx::MeshData *found = p_entry.converted.getptr(key);
		if (found) {
			return found->indices.empty() ? nullptr : found;
		}
		const psx::MeshInput in = _input(p_entry.surfaces[p_surface]);
		const psx::ConvertOptions opt = _options(p_mat, p_texture, p_normal_scale, p_scale);
		psx::MeshData &out = p_entry.converted.insert(key, psx::MeshData())->value;
		{
			PSXScopedTimer timer(r_stats.mesh_convert_usec);
			psx::convert_mesh(in, opt, out); // Empty on failure: remembered, not retried.
		}
		r_stats.conversions++;
		r_stats.meshes_converted++;
		if (out.clamped) {
			WARN_PRINT_ONCE("PSX: a mesh is too large for 16-bit GTE coordinates and was clamped; lower rendering/psx/world_scale or split the mesh.");
		}
		return out.indices.empty() ? nullptr : &out;
	}

	// The material's albedo texture as this surface uses it, converted (once)
	// and placed in texture memory. nullptr when it has none or its image
	// cannot be read; placed is false when texture memory is full even after
	// evicting textures not used this frame. Updates p_mat's UV transform for
	// the crop.
	// p_anim_scale/offset (particle animation frames) apply after the crop is
	// chosen, so every frame of a sheet shares one converted texture.
	TextureEntry *_get_texture(PSXMaterialInfo &p_mat, const SourceSurface &p_src, PSXSceneStats &r_stats, const Vector2 &p_anim_scale = Vector2(1, 1), const Vector2 &p_anim_offset = Vector2()) {
		RendererRD::TextureStorage *ts = RendererRD::TextureStorage::get_singleton();
		if (!p_mat.texture.is_valid() || !ts->owns_texture(p_mat.texture) || p_src.uvs.is_empty()) {
			return nullptr;
		}
		// The material's palette (PSXMaterial3D psx_palette), if it has one.
		PSXPaletteData pal;
		if (p_mat.palette == 0 || !PSXPalette::find(ObjectID(p_mat.palette), pal)) {
			pal = PSXPaletteData();
		}
		// Baked at import (PSXTexture): used whole, exactly as imported. With
		// a palette, only when it was baked with a palette of the same size:
		// the palette is swapped in. Otherwise it is converted to the palette.
		uint32_t baked_version = PSXTexture::get_baked_version(p_mat.texture, p_mat.alpha_mode, p_mat.alpha_cut);
		if (baked_version != 0 && pal.version != 0 && PSXTexture::get_baked_palette_entries(p_mat.texture, p_mat.alpha_mode, p_mat.alpha_cut) != pal.entries) {
			baked_version = 0;
		}
		TextureUse *use = nullptr;
		if (baked_version != 0) {
			uint32_t h = hash_murmur3_one_32(baked_version, hash_murmur3_one_32(0xBA4Eu | (p_mat.semi ? 0x10000u : 0u) | (uint32_t)p_mat.alpha_mode << 20));
			h = hash_murmur3_one_32(pal.version, h);
			const uint64_t id = p_mat.texture.get_id();
			const uint64_t key = ((uint64_t)hash_murmur3_one_64(id, h) << 32) | (id & 0xFFFFFFFFu);
			use = texture_uses.getptr(key);
			if (!use) {
				use = &texture_uses.insert(key, TextureUse())->value;
				psx::TextureData data;
				uint32_t version = 0;
				use->valid = PSXTexture::find_baked(p_mat.texture, p_mat.alpha_mode, p_mat.alpha_cut, data, version);
				if (use->valid && pal.version != 0) {
					use->valid = psx::apply_palette(pal.keys.data(), (int32_t)pal.keys.size(), data);
				}
				if (use->valid) {
					_store_texture(*use, std::move(data), p_mat.semi, true);
				}
			}
		} else {
			use = _converted_texture_use(ts, p_mat, p_src, pal, r_stats);
			if (!use) {
				return nullptr;
			}
		}
		use->last_used = frame;
		if (!use->valid) {
			return nullptr;
		}
		TextureEntry *e = textures.getptr(use->content);
		if (!e) {
			return nullptr;
		}
		e->last_used = frame;
		if (!e->placed && frame >= e->retry_frame) {
			_place_texture(*e, r_stats);
		}
		// Surface UVs -> crop UVs, after the material's own transform and
		// the animation frame.
		p_mat.uv_scale = p_mat.uv_scale * p_anim_scale;
		p_mat.uv_offset = p_mat.uv_offset * p_anim_scale + p_anim_offset;
		p_mat.uv_offset = p_mat.uv_offset * use->uv_scale + use->uv_offset;
		p_mat.uv_scale = p_mat.uv_scale * use->uv_scale;
		return e;
	}

	// Identical texels and palettes share one TextureEntry.
	void _store_texture(TextureUse &r_use, psx::TextureData &&p_data, bool p_semi, bool p_baked) {
		const psx::TextureData &data = p_data;
		uint32_t c = hash_murmur3_buffer(data.texels.data(), (int)(data.texels.size() * sizeof(uint16_t)));
		c = hash_murmur3_buffer(data.clut.data(), (int)(data.clut.size() * sizeof(uint16_t)), c);
		c = hash_murmur3_one_32((uint32_t)data.width | (uint32_t)data.height << 12 | (uint32_t)data.depth << 24 | (p_semi ? 1u << 28 : 0u), c);
		const uint32_t c2 = hash_murmur3_buffer(data.clut_semi.data(), (int)(data.clut_semi.size() * sizeof(uint16_t)), c ^ 0x9E3779B9u);
		r_use.content = ((uint64_t)c << 32) | c2;
		if (!textures.has(r_use.content)) {
			TextureEntry &e = textures.insert(r_use.content, TextureEntry())->value;
			e.data = std::move(p_data);
			e.needs_semi_clut = p_semi;
			e.baked = p_baked;
			const psx::TextureData &d = e.data;
			e.texel_key = _slot_key(d.texels, (uint32_t)d.hw_width() | (uint32_t)d.height << 16);
			e.clut_key = _slot_key(d.clut, 1);
			e.semi_key = _slot_key(d.clut_semi, 1);
			e.white_key = _slot_key(d.clut_white, 1);
		}
	}

	static uint64_t _slot_key(const std::vector<uint16_t> &p_halfwords, uint32_t p_shape) {
		const int bytes = (int)(p_halfwords.size() * sizeof(uint16_t));
		const uint32_t a = hash_murmur3_buffer(p_halfwords.data(), bytes, hash_murmur3_one_32(p_shape));
		const uint32_t b = hash_murmur3_buffer(p_halfwords.data(), bytes, hash_murmur3_one_32(p_shape ^ 0x5bd1e995u));
		return ((uint64_t)a << 32) | b;
	}

	// Runtime conversion: the image read back from the GPU, cropped to the
	// UVs the surface uses. nullptr when the image cannot be read.
	TextureUse *_converted_texture_use(RendererRD::TextureStorage *ts, const PSXMaterialInfo &p_mat, const SourceSurface &p_src, const PSXPaletteData &p_pal, PSXSceneStats &r_stats) {
		const Ref<Image> image = _source_image(ts, p_mat.texture);
		if (image.is_null()) {
			return nullptr;
		}
		// Crop: per axis, the power-of-two box of whole texels the UVs cover,
		// or the whole axis when they repeat or the box is not smaller.
		const int32_t size[2] = { image->get_width(), image->get_height() };
		int32_t start[2] = { 0, 0 }, extent[2] = { size[0], size[1] };
		for (int a = 0; a < 2; a++) {
			const double s0 = p_src.uv_min[a] * p_mat.uv_scale[a] + p_mat.uv_offset[a];
			const double s1 = p_src.uv_max[a] * p_mat.uv_scale[a] + p_mat.uv_offset[a];
			const double lo = MIN(s0, s1), hi = MAX(s0, s1);
			if (lo < -1e-4 || hi > 1.0 + 1e-4) {
				continue;
			}
			const int32_t first = CLAMP((int32_t)std::floor(lo * size[a]), 0, size[a] - 1);
			const int32_t used = MAX(1, (int32_t)std::ceil(hi * size[a]) - first);
			int32_t box = 8;
			while (box < used) {
				box <<= 1;
			}
			if (box >= size[a]) {
				continue;
			}
			start[a] = MIN(first, size[a] - box);
			extent[a] = box;
		}

		// The converted texels depend on how the material uses alpha.
		uint32_t h = hash_murmur3_one_32(p_mat.alpha_mode | (p_mat.semi ? 0x100u : 0u));
		h = hash_murmur3_one_float(p_mat.alpha_mode == psx::ALPHA_CUT ? p_mat.alpha_cut : 0.0f, h);
		h = hash_murmur3_one_32((uint32_t)start[0] | (uint32_t)start[1] << 16, h);
		h = hash_murmur3_one_32((uint32_t)extent[0] | (uint32_t)extent[1] << 16, h);
		h = hash_murmur3_one_32((uint32_t)settings.texture.max_size | (uint32_t)settings.texture.depth << 16 | (uint32_t)settings.texture.max_colors << 20 | (settings.texture.dither ? 1u << 30 : 0u), h);
		h = hash_murmur3_one_32(p_pal.version, h);
		const uint64_t id = p_mat.texture.get_id();
		const uint64_t key = ((uint64_t)hash_murmur3_one_64(id, h) << 32) | (id & 0xFFFFFFFFu);
		const uint64_t version = ts->texture_get_content_version(p_mat.texture);
		TextureUse *use = texture_uses.getptr(key);
		if (use && use->version != version) {
			// The texture changed since (ImageTexture.update or set_image, a
			// ViewportTexture's new frame): convert it again.
			_release_content(key, use->content);
			*use = TextureUse();
		} else if (!use) {
			use = &texture_uses.insert(key, TextureUse())->value;
		} else {
			return use;
		}
		{
			use->version = version;
			psx::TextureData data;
			{
				PSXScopedTimer timer(r_stats.texture_convert_usec);
				use->valid = _convert_texture(image, Rect2i(start[0], start[1], extent[0], extent[1]), p_mat, ts->texture_is_render_target(p_mat.texture), p_pal, data);
			}
			r_stats.conversions++;
			r_stats.textures_converted++;
			if (use->valid) {
				for (int a = 0; a < 2; a++) {
					use->uv_scale[a] = (real_t)size[a] / extent[a];
					use->uv_offset[a] = -(real_t)start[a] / extent[a];
				}
				_store_texture(*use, std::move(data), p_mat.semi, false);
			}
		}
		return use;
	}

	// Frees p_content's texture memory now unless another use (other than
	// p_key) still draws it.
	void _release_content(uint64_t p_key, uint64_t p_content) {
		for (const KeyValue<uint64_t, TextureUse> &kv : texture_uses) {
			if (kv.key != p_key && kv.value.content == p_content && kv.value.valid) {
				return;
			}
		}
		TextureEntry *e = textures.getptr(p_content);
		if (e) {
			_unplace_texture(*e);
			textures.erase(p_content);
		}
	}

	Ref<Image> _source_image(RendererRD::TextureStorage *p_storage, RID p_texture) {
		const uint64_t version = p_storage->texture_get_content_version(p_texture);
		SourceImage *found = source_images.getptr(p_texture);
		if (found && found->version == version) {
			found->last_used = frame;
			return found->image;
		}
		if (!found) {
			found = &source_images.insert(p_texture, SourceImage())->value;
		}
		SourceImage &entry = *found;
		PSXSceneStats &st = active_stats ? *active_stats : unused_stats;
		PSXScopedTimer timer(st.texture_read_usec);
		st.textures_read++;
		entry.image = Ref<Image>();
		entry.version = version;
		entry.last_used = frame;
		Ref<Image> source;
		if (const Ref<Image> *fetched_image = fetched_images.getptr(p_texture)) {
			source = *fetched_image; // Read with the others this frame
			fetched_images.erase(p_texture);
		} else {
			source = p_storage->texture_2d_get(p_texture);
		}
		if (source.is_null() || source->is_empty()) {
			return Ref<Image>();
		}
		Ref<Image> image;
		image.instantiate();
		image->copy_internals_from(source);
		if (image->is_compressed()) {
			image->decompress();
			if (image->is_compressed()) {
				WARN_PRINT_ONCE("PSX: a texture's compressed format cannot be decompressed; it is drawn untextured. Import it as Lossless.");
				return Ref<Image>();
			}
		}
		image->clear_mipmaps();
		image->convert(Image::FORMAT_RGBA8);
		entry.image = image;
		return image;
	}

	// p_dynamic: a render target, converted again every frame it changes:
	// 15-bit (no palette to build), as a PS1 game would copy a rendered
	// image from the frame buffer.
	bool _convert_texture(const Ref<Image> &p_image, const Rect2i &p_crop, const PSXMaterialInfo &p_mat, bool p_dynamic, const PSXPaletteData &p_pal, psx::TextureData &r_data) const {
		const Ref<Image> crop = (p_crop.position == Point2i() && p_crop.size == p_image->get_size()) ? p_image : p_image->get_region(p_crop);
		psx::TextureOptions opt = settings.texture;
		if (p_dynamic) {
			opt.depth = psx::DEPTH_FORCE_15BIT;
		} else if (p_pal.version != 0) {
			opt.palette = p_pal.keys.data();
			opt.palette_size = (int32_t)p_pal.keys.size();
			opt.palette_entries = p_pal.entries;
		}
		opt.alpha_mode = p_mat.alpha_mode;
		opt.alpha_cut = p_mat.alpha_cut;
		const Vector<uint8_t> bytes = crop->get_data();
		return psx::convert_texture(bytes.ptr(), crop->get_width(), crop->get_height(), opt, r_data);
	}

	// Finds room for a texture and its CLUTs (the palette; the all-STP one
	// for blended materials; the all-white one for the fog pass of
	// cut-outs) in one bank, adding banks while allowed, then evicting. A
	// bank already holding the same texels (a palette swap of a placed
	// texture) is tried first; identical CLUTs and texels share one slot.
	void _place_texture(TextureEntry &e, PSXSceneStats &r_stats) {
		const uint32_t max_banks = settings.texture_banks == 0 ? psx::MAX_TEXTURE_BANKS + 1 : MIN(settings.texture_banks, psx::MAX_TEXTURE_BANKS + 1);
		if (vram_banks.is_empty()) {
			vram_banks.resize(1);
		}
		for (;;) {
			for (uint32_t bank = 0; bank < bank_slots.size() && bank < vram_banks.size(); bank++) {
				if (bank_slots[bank].boxes.has(e.texel_key) && _place_in(e, bank, r_stats)) {
					return;
				}
			}
			for (uint32_t bank = 0; bank <= vram_banks.size(); bank++) {
				if (bank == vram_banks.size()) {
					if (bank >= max_banks) {
						break;
					}
					vram_banks.push_back(psx::VramAllocator()); // A new, empty bank.
				}
				if (_place_in(e, bank, r_stats)) {
					return;
				}
			}
			if (!_evict_one_texture()) {
				e.retry_frame = frame + 30; // Try again later, not every frame.
				WARN_PRINT_ONCE("PSX: texture memory is full; some surfaces are drawn untextured. Raise rendering/psx/textures/vram_banks, lower rendering/psx/textures/max_size or use fewer textures.");
				return;
			}
		}
	}

	bool _place_in(TextureEntry &e, uint32_t p_bank, PSXSceneStats &r_stats) {
		const psx::TextureData &d = e.data;
		const int32_t entries = d.clut_entries();
		if (bank_slots.size() < vram_banks.size()) {
			bank_slots.resize(vram_banks.size());
		}
		int32_t x = 0, y = 0, cx = 0, cy = 0, sx = -1, sy = 0, wx = -1, wy = 0;
		const bool box = _acquire_slot(p_bank, false, e.texel_key, d.hw_width(), d.height, d.texels.data(), x, y);
		const bool clut = box && (entries == 0 || _acquire_slot(p_bank, true, e.clut_key, entries, 1, d.clut.data(), cx, cy));
		const bool semi = clut && (entries == 0 || !e.needs_semi_clut || _acquire_slot(p_bank, true, e.semi_key, entries, 1, d.clut_semi.data(), sx, sy));
		const bool white = semi && (entries == 0 || !d.has_transparent || _acquire_slot(p_bank, true, e.white_key, entries, 1, d.clut_white.data(), wx, wy));
		if (!white) {
			if (semi && sx >= 0) {
				_release_slot(p_bank, true, e.semi_key, entries, 1);
			}
			if (clut && entries > 0) {
				_release_slot(p_bank, true, e.clut_key, entries, 1);
			}
			if (box) {
				_release_slot(p_bank, false, e.texel_key, d.hw_width(), d.height);
			}
			return false;
		}
		const uint16_t b = (uint16_t)p_bank;
		e.placed = true;
		e.bank = b;
		e.x = x;
		e.y = y;
		e.clut_x = cx;
		e.clut_y = cy;
		e.semi_x = sx;
		e.semi_y = sy;
		e.white_x = wx;
		e.white_y = wy;
		for (int mode = 0; mode < 4; mode++) {
			e.draw[mode] = psx::make_draw_texture(d, x, y, cx, cy, sx, sy, wx, wy, (uint8_t)mode, true, b);
		}
		r_stats.texture_uploads++;
		return true;
	}

	// The slot of p_data in bank p_bank: one already holding the same
	// halfwords, or new room, uploaded. p_width is the CLUT's entries for a
	// CLUT, halfwords for texels.
	bool _acquire_slot(uint32_t p_bank, bool p_clut, uint64_t p_key, int32_t p_width, int32_t p_height, const uint16_t *p_data, int32_t &r_x, int32_t &r_y) {
		HashMap<uint64_t, SharedSlot> &slots = p_clut ? bank_slots[p_bank].cluts : bank_slots[p_bank].boxes;
		SharedSlot *found = slots.getptr(p_key);
		if (found) {
			found->refs++;
			r_x = found->x;
			r_y = found->y;
			return true;
		}
		psx::VramAllocator &a = vram_banks[p_bank];
		if (!(p_clut ? a.alloc_clut(p_width, r_x, r_y) : a.alloc(p_width, p_height, r_x, r_y))) {
			return false;
		}
		psx::upload_rect(*gpu, r_x, r_y, p_width, p_height, p_data, (uint16_t)p_bank);
		SharedSlot &slot = slots.insert(p_key, SharedSlot())->value;
		slot.x = r_x;
		slot.y = r_y;
		slot.refs = 1;
		return true;
	}

	void _release_slot(uint32_t p_bank, bool p_clut, uint64_t p_key, int32_t p_width, int32_t p_height) {
		if (p_bank >= bank_slots.size() || p_bank >= vram_banks.size()) {
			return;
		}
		HashMap<uint64_t, SharedSlot> &slots = p_clut ? bank_slots[p_bank].cluts : bank_slots[p_bank].boxes;
		SharedSlot *found = slots.getptr(p_key);
		if (!found || --found->refs > 0) {
			return;
		}
		if (p_clut) {
			vram_banks[p_bank].free_clut(found->x, found->y, p_width);
		} else {
			vram_banks[p_bank].free(found->x, found->y, p_width, p_height);
		}
		slots.erase(p_key);
	}

	// Frees the least recently used texture that this frame has not drawn.
	bool _evict_one_texture() {
		TextureEntry *oldest = nullptr;
		for (KeyValue<uint64_t, TextureEntry> &kv : textures) {
			TextureEntry &t = kv.value;
			if (t.placed && t.last_used < frame && (!oldest || t.last_used < oldest->last_used)) {
				oldest = &t;
			}
		}
		if (!oldest) {
			return false;
		}
		_unplace_texture(*oldest);
		return true;
	}

	void _unplace_texture(TextureEntry &e) {
		if (!e.placed || e.bank >= vram_banks.size()) {
			e.placed = false;
			return;
		}
		const int32_t entries = e.data.clut_entries();
		_release_slot(e.bank, false, e.texel_key, e.data.hw_width(), e.data.height);
		if (entries > 0) {
			_release_slot(e.bank, true, e.clut_key, entries, 1);
			if (e.semi_x >= 0) {
				_release_slot(e.bank, true, e.semi_key, entries, 1);
			}
			if (e.white_x >= 0) {
				_release_slot(e.bank, true, e.white_key, entries, 1);
			}
		}
		e.placed = false;
		e.semi_x = -1;
		e.white_x = -1;
	}

	void _evict() {
		LocalVector<RID> stale;
		for (const KeyValue<RID, MeshEntry> &kv : meshes) {
			if (frame - kv.value.last_used > CACHE_KEEP_FRAMES) {
				stale.push_back(kv.key);
			}
		}
		for (const RID &rid : stale) {
			meshes.erase(rid);
		}
		LocalVector<uint64_t> stale_uses;
		for (const KeyValue<uint64_t, TextureUse> &kv : texture_uses) {
			if (frame - kv.value.last_used > CACHE_KEEP_FRAMES) {
				stale_uses.push_back(kv.key);
			}
		}
		for (const uint64_t key : stale_uses) {
			texture_uses.erase(key);
		}
		LocalVector<RID> stale_images;
		for (const KeyValue<RID, SourceImage> &kv : source_images) {
			if (frame - kv.value.last_used > CACHE_KEEP_FRAMES) {
				stale_images.push_back(kv.key);
			}
		}
		for (const RID &rid : stale_images) {
			source_images.erase(rid);
		}
		LocalVector<uint64_t> stale_textures;
		for (KeyValue<uint64_t, TextureEntry> &kv : textures) {
			if (frame - kv.value.last_used > CACHE_KEEP_FRAMES) {
				_unplace_texture(kv.value);
				stale_textures.push_back(kv.key);
			}
		}
		for (const uint64_t key : stale_textures) {
			textures.erase(key);
		}
	}

	// Reads the environment's sky into `sky` (SKY_NONE without one). The
	// panorama is looked up here, so its mean colour is known for the
	// ambient light of this frame.
	void _read_sky(RendererSceneRender *p_scene_render, RID p_environment, PSXSceneStats &r_stats) {
		sky = SkyInfo();
		if (!p_environment.is_valid() || !p_scene_render->is_environment(p_environment) || p_scene_render->environment_get_background(p_environment) != RSE::ENV_BG_SKY) {
			return;
		}
		const RID sky_rid = p_scene_render->environment_get_sky(p_environment);
		RendererSceneRenderRD *rd = RendererSceneRenderRD::get_singleton();
		if (!sky_rid.is_valid() || !rd) {
			return;
		}
		const RID material = rd->get_sky()->sky_get_material(sky_rid);
		if (!material.is_valid()) {
			return;
		}
		RendererRD::MaterialStorage *ms = RendererRD::MaterialStorage::get_singleton();
		auto color = [&](const char *p_name, bool &r_found) -> Color {
			const Variant v = ms->material_get_param(material, p_name);
			r_found = v.get_type() == Variant::COLOR;
			// source_color uniforms: authored in sRGB, used in linear.
			return r_found ? ((Color)v).srgb_to_linear() : Color();
		};
		auto number = [&](const char *p_name, float p_default) -> float {
			const Variant v = ms->material_get_param(material, p_name);
			return (v.get_type() == Variant::FLOAT || v.get_type() == Variant::INT) ? (float)v : p_default;
		};
		const float bg_energy = p_scene_render->environment_get_bg_energy_multiplier(p_environment);
		sky.orientation = p_scene_render->environment_get_sky_orientation(p_environment);
		sky.energy = number("exposure", 1.0f) * bg_energy;

		const Variant panorama = ms->material_get_param(material, "source_panorama");
		if (panorama.get_type() == Variant::RID && ((RID)panorama).is_valid()) {
			sky.kind = SKY_PANORAMA;
			sky.texture = panorama;
			PSXMaterialInfo mat;
			TextureEntry *tex = _sky_texture(mat, r_stats);
			if (!tex) {
				sky.kind = SKY_NONE; // No readable image: the clear colour.
				return;
			}
			const Color m = Color(tex->data.mean[0] / 255.0f, tex->data.mean[1] / 255.0f, tex->data.mean[2] / 255.0f).srgb_to_linear() * sky.energy;
			sky.mean = Color(MIN(m.r, 1.0f), MIN(m.g, 1.0f), MIN(m.b, 1.0f)).linear_to_srgb();
			return;
		}
		bool procedural = false, physical = false, found = false;
		sky.top = color("sky_top_color", procedural);
		if (procedural) {
			// ProceduralSkyMaterial (sun disks and the cover texture are not drawn).
			sky.horizon = color("sky_horizon_color", found);
			sky.bottom = color("ground_bottom_color", found);
			sky.ground_horizon = color("ground_horizon_color", found);
			sky.inv_sky_curve = number("inv_sky_curve", 4.0f);
			sky.inv_ground_curve = number("inv_ground_curve", 30.0f);
		} else {
			// PhysicalSkyMaterial, roughly: Rayleigh colour overhead, Mie
			// colour at the horizon, the ground colour below.
			sky.top = color("rayleigh_color", physical);
			if (!physical) {
				return; // Another sky shader: the clear colour.
			}
			sky.horizon = color("mie_color", found);
			sky.bottom = color("ground_color", found);
			sky.ground_horizon = sky.bottom;
		}
		sky.kind = SKY_GRADIENT;
		const float e = sky.energy;
		sky.top *= e;
		sky.horizon *= e;
		sky.bottom *= e;
		sky.ground_horizon *= e;
		// Mean over the sphere: uniform in Y is uniform in area.
		Color sum(0, 0, 0);
		const int samples = 64;
		for (int i = 0; i < samples; i++) {
			sum += _sky_color(-1.0f + (i + 0.5f) * 2.0f / samples);
		}
		const Color m = sum / (float)samples;
		sky.mean = Color(MIN(m.r, 1.0f), MIN(m.g, 1.0f), MIN(m.b, 1.0f)).linear_to_srgb();
		sky.mean.a = 1.0f;
	}

	// ProceduralSkyMaterial's colour at direction height p_y (-1..1), linear.
	Color _sky_color(float p_y) const {
		if (p_y >= 0.0f) {
			const float t = CLAMP(std::pow(1.0f - p_y, sky.inv_sky_curve), 0.0f, 1.0f);
			return sky.top.lerp(sky.horizon, t);
		}
		const float t = CLAMP(std::pow(1.0f + p_y, sky.inv_ground_curve), 0.0f, 1.0f);
		return sky.bottom.lerp(sky.ground_horizon, t);
	}

	// The panorama, converted and placed like a material texture (repeating,
	// so the whole image is used). r_mat gets the UV transform into it.
	TextureEntry *_sky_texture(PSXMaterialInfo &r_mat, PSXSceneStats &r_stats) {
		if (sky_source.uvs.is_empty()) {
			sky_source.uvs.resize(4);
			sky_source.uvs[0] = 0.0f;
			sky_source.uvs[1] = 0.0f;
			sky_source.uvs[2] = 1.0f;
			sky_source.uvs[3] = 1.0f;
			sky_source.uv_min = Vector2(0, 0);
			sky_source.uv_max = Vector2(1, 1);
		}
		r_mat = PSXMaterialInfo();
		r_mat.texture = sky.texture;
		r_mat.texture_repeat = true;
		return _get_texture(r_mat, sky_source, r_stats);
	}

	// The sky dome: rings of vertices around the camera, packed toward the
	// horizon where the gradient changes fastest, in the farthest ordering
	// table bucket. Drawn after every object, so it is added to that bucket
	// last and drawn first there: objects sorted into the same bucket still
	// cover it. Orthographic cameras see only the mean colour (background()).
	void _draw_sky(Frame &f) {
		if (sky.kind == SKY_NONE || f.ortho) {
			return;
		}
		PSXMaterialInfo mat;
		TextureEntry *tex = nullptr;
		if (sky.kind == SKY_PANORAMA) {
			tex = _sky_texture(mat, *f.stats);
			if (tex && !tex->placed) {
				f.stats->texture_fallbacks++;
				tex = nullptr; // No room in VRAM: the mean colour.
			}
		}
		const bool gradient = sky.kind == SKY_GRADIENT;
		const float fog_amount = f.fog ? fog_sky_affect : 0.0f;

		// The mesh changes only with the sky, the fog or the texture's place.
		uint32_t key = hash_murmur3_one_32((uint32_t)sky.kind | (tex ? 0x100u : 0u));
		if (gradient) {
			const Color cs[4] = { sky.top, sky.horizon, sky.ground_horizon, sky.bottom };
			for (const Color &c : cs) {
				key = hash_murmur3_one_float(c.r, hash_murmur3_one_float(c.g, hash_murmur3_one_float(c.b, key)));
			}
			key = hash_murmur3_one_float(sky.inv_sky_curve, hash_murmur3_one_float(sky.inv_ground_curve, key));
			key = hash_murmur3_one_float(fog_amount, key);
			key = hash_murmur3_one_float(fog_color.r, hash_murmur3_one_float(fog_color.g, hash_murmur3_one_float(fog_color.b, key)));
		}
		if (tex) {
			key = hash_murmur3_one_32((uint32_t)(tex->data.width | tex->data.height << 16), key);
			key = hash_murmur3_one_float(mat.uv_scale.x, hash_murmur3_one_float(mat.uv_scale.y, key));
			key = hash_murmur3_one_float(mat.uv_offset.x, hash_murmur3_one_float(mat.uv_offset.y, key));
		}
		key = hash_fmix32(key);
		if (key != sky_mesh_key || sky_mesh.indices.empty()) {
			sky_mesh_key = key;
			_build_sky_mesh(tex, mat, gradient, fog_amount);
		}
		if (sky_mesh.indices.empty()) {
			return;
		}

		// Rotation only: the dome moves with the camera.
		const Basis view = f.cam_inv.basis * sky.orientation;
		for (int r = 0; r < 3; r++) {
			for (int c = 0; c < 3; c++) {
				f.gte.rotation.m[r][c] = (int16_t)CLAMP(std::lround(f.row_scale[r] * view.rows[r][c] * 4096.0), -32768L, 32767L);
			}
		}
		f.gte.translation = { 0, 0, 0 };
		psx::DrawParams params;
		params.cull = psx::CULL_NONE;
		params.lit = false;
		params.lighting = settings.lighting;
		params.dither = settings.dither;
		params.frame_dither = settings.dither;
		params.ot_shift = f.ot_shift;
		params.ot_bias = (int32_t)OT_SIZE; // Clamped to the farthest bucket.
		params.far_z = 0xFFFF;
		params.screen_width = f.width;
		params.screen_height = f.height;
		if (tex) {
			params.texture = &tex->draw[psx::BLEND_HALF];
			// Fog over the panorama: the texel darkened by the fog amount,
			// then the fog colour times it added by a second, untextured pass.
			const uint8_t keep = _to_byte(1.0f - fog_amount);
			params.tint[0] = params.tint[1] = params.tint[2] = keep;
		} else if (!gradient) {
			// Panorama without room in VRAM: its mean colour.
			params.tint[0] = _to_byte(sky.mean.r);
			params.tint[1] = _to_byte(sky.mean.g);
			params.tint[2] = _to_byte(sky.mean.b);
		}
		psx::draw_mesh(f.gte, *f.builder, *f.ot, sky_mesh, params, f.stats->draw);
		if (tex && fog_amount > 0.0f) {
			psx::DrawParams add = params;
			add.texture = nullptr;
			add.semi = true;
			add.blend = psx::BLEND_ADD;
			add.tint[0] = _to_byte(MIN(fog_color.r, 1.0f) * fog_amount);
			add.tint[1] = _to_byte(MIN(fog_color.g, 1.0f) * fog_amount);
			add.tint[2] = _to_byte(MIN(fog_color.b, 1.0f) * fog_amount);
			psx::draw_mesh(f.gte, *f.builder, *f.ot, sky_mesh, add, f.stats->draw);
		}
	}

	void _build_sky_mesh(const TextureEntry *p_tex, const PSXMaterialInfo &p_mat, bool p_gradient, float p_fog) {
		// Elevations in degrees, top to bottom; the horizon twice (sky side,
		// ground side), since the two colours meet there.
		static const float rings[] = { 90, 65, 45, 30, 20, 12, 7, 3.5f, 1.5f, 0, 0, -1.5f, -3.5f, -7, -12, -20, -30, -45, -65, -90 };
		const int ring_count = (int)(sizeof(rings) / sizeof(rings[0]));
		const int horizon_split = 10; // Rings [0, 10) are sky, [10, 20) ground.
		const int segments = 24;
		const float radius = 16000.0f; // GTE units: well past the near plane, inside int16.
		LocalVector<float> positions, colors, uvs;
		LocalVector<int32_t> indices;
		for (int i = 0; i < ring_count; i++) {
			const float elev = Math::deg_to_rad(rings[i]);
			const float y = Math::sin(elev), across = Math::cos(elev);
			Color c(1, 1, 1);
			if (p_gradient) {
				// Ground-side horizon ring: the ground colour at the horizon.
				const Color lin = i >= horizon_split && rings[i] == 0.0f ? _sky_color(-1e-6f) : _sky_color(y);
				c = Color(MIN(lin.r, 1.0f), MIN(lin.g, 1.0f), MIN(lin.b, 1.0f)).linear_to_srgb();
				if (p_fog > 0.0f) {
					c = c.lerp(fog_color, p_fog);
				}
			}
			for (int j = 0; j <= segments; j++) {
				// Godot's panorama lookup: u = atan2(x, -z) / 2pi, v = acos(y) / pi.
				const float u = (float)j / segments;
				const float angle = u * (float)Math::TAU;
				positions.push_back(Math::sin(angle) * across * radius);
				positions.push_back(y * radius);
				positions.push_back(-Math::cos(angle) * across * radius);
				colors.push_back(c.r);
				colors.push_back(c.g);
				colors.push_back(c.b);
				colors.push_back(1.0f);
				uvs.push_back(u);
				uvs.push_back(Math::acos(CLAMP(y, -1.0f, 1.0f)) / (float)Math::PI);
			}
		}
		const int row = segments + 1;
		for (int i = 0; i + 1 < ring_count; i++) {
			if (i == horizon_split - 1) {
				continue; // Between the two horizon rings: nothing.
			}
			for (int j = 0; j < segments; j++) {
				const int a = i * row + j, b = a + 1, c = a + row, d = c + 1;
				if (i != 0) {
					indices.push_back(a);
					indices.push_back(b);
					indices.push_back(c);
				}
				if (i + 1 != ring_count - 1) {
					indices.push_back(b);
					indices.push_back(d);
					indices.push_back(c);
				}
			}
		}
		psx::MeshInput in;
		in.positions = positions.ptr();
		in.colors = colors.ptr();
		in.uvs = p_tex ? uvs.ptr() : nullptr;
		in.vertex_count = (int32_t)(positions.size() / 3);
		in.indices = indices.ptr();
		in.index_count = (int32_t)indices.size();
		psx::ConvertOptions opt;
		opt.scale = 1.0f;
		opt.use_vertex_colors = p_gradient;
		opt.vertex_colors_srgb = true;
		if (p_tex) {
			opt.texture_width = p_tex->data.width;
			opt.texture_height = p_tex->data.height;
			opt.texture_repeat = true;
			opt.uv_scale[0] = p_mat.uv_scale.x;
			opt.uv_scale[1] = p_mat.uv_scale.y;
			opt.uv_offset[0] = p_mat.uv_offset.x;
			opt.uv_offset[1] = p_mat.uv_offset.y;
		}
		psx::convert_mesh(in, opt, sky_mesh);
	}


	// Ambient light level. Colours are as picked (sRGB); Godot-match mode
	// converts them to linear before the energy is applied, as Godot does.
	Color _ambient(RendererSceneRender *p_scene_render, RID p_environment) const {
		const bool linear = settings.lighting == psx::LIGHTING_LINEAR;
		auto lin = [linear](const Color &p_c) { return linear ? Color(p_c.r, p_c.g, p_c.b).srgb_to_linear() : p_c; };
		if (!p_environment.is_valid() || !p_scene_render->is_environment(p_environment)) {
			return settings.default_ambient;
		}
		const float energy = p_scene_render->environment_get_ambient_light_energy(p_environment);
		switch (p_scene_render->environment_get_ambient_source(p_environment)) {
			case RSE::ENV_AMBIENT_SOURCE_DISABLED:
				return Color(0, 0, 0);
			case RSE::ENV_AMBIENT_SOURCE_COLOR:
				return lin(p_scene_render->environment_get_ambient_light(p_environment)) * energy;
			default:
				// Background or sky: approximated by the background colour
				// when there is one; with a sky, its mean colour mixed with the
				// ambient colour by sky_contribution (Godot mixes the sky's
				// radiance in that direction); else the ambient colour.
				if (p_scene_render->environment_get_background(p_environment) == RSE::ENV_BG_COLOR) {
					return lin(p_scene_render->environment_get_bg_color(p_environment)) * energy;
				}
				if (sky.kind != SKY_NONE) {
					const float mix = CLAMP(p_scene_render->environment_get_ambient_sky_contribution(p_environment), 0.0f, 1.0f);
					return lin(p_scene_render->environment_get_ambient_light(p_environment)).lerp(lin(sky.mean), mix) * energy;
				}
				return lin(p_scene_render->environment_get_ambient_light(p_environment)) * energy;
		}
	}

	// Environment fog as the GTE depth cue. The hardware interpolates by
	// IR0 = DQB + DQA * H / SZ, a curve in 1/Z, so Godot's fog curve is fitted
	// at two depths: exponential fog matches at 50% and is full at infinity;
	// depth fog matches at its begin (0%) and end (100%) distances.
	bool _setup_fog(RendererSceneRender *p_scene_render, RID p_environment, const Projection &p_proj, Frame &f) {
		fog_sky_affect = 0.0f;
		if (!p_environment.is_valid() || !p_scene_render->is_environment(p_environment) || !p_scene_render->environment_get_fog_enabled(p_environment)) {
			return false;
		}
		const float energy = p_scene_render->environment_get_fog_light_energy(p_environment);
		fog_color = p_scene_render->environment_get_fog_light_color(p_environment) * energy;
		fog_color.a = 1.0f;
		fog_sky_affect = CLAMP(p_scene_render->environment_get_fog_sky_affect(p_environment), 0.0f, 1.0f);
		f.gte.far_color = { (int32_t)_to_byte(fog_color.r) * 16, (int32_t)_to_byte(fog_color.g) * 16, (int32_t)_to_byte(fog_color.b) * 16 };

		const double h = f.gte.h;
		double a = 0.0, b = 0.0; // fog(z) = a + b * H / z (z in GTE units)
		if (p_scene_render->environment_get_fog_mode(p_environment) == RSE::ENV_FOG_MODE_DEPTH) {
			const double zb = MAX(0.001, (double)p_scene_render->environment_get_fog_depth_begin(p_environment)) * f.world;
			double ze = MIN((double)p_scene_render->environment_get_fog_depth_end(p_environment), p_proj.get_z_far()) * f.world;
			if (ze <= zb) {
				ze = zb + 1.0;
			}
			b = 1.0 / (h / ze - h / zb);
			a = -b * h / zb;
		} else {
			const double density = p_scene_render->environment_get_fog_density(p_environment);
			if (density <= 0.0) {
				return false;
			}
			const double z_half = std::log(2.0) / density * f.world;
			a = 1.0;
			b = -0.5 * z_half / h;
		}
		// IR0 = (DQA * (H / SZ in 1.16) + DQB) >> 12, with 4096 = full fog.
		f.gte.dqa = (int16_t)CLAMP(std::lround(b * 256.0), -32768L, 32767L);
		f.gte.dqb = (int32_t)CLAMP(std::llround(a * 16777216.0), (long long)INT32_MIN, (long long)INT32_MAX);
		return true;
	}

	void _gather_lights(const PagedArray<RID> &p_lights) {
		lights.clear();
		RendererRD::LightStorage *ls = RendererRD::LightStorage::get_singleton();
		for (uint32_t i = 0; i < p_lights.size(); i++) {
			const RID base = ls->light_instance_get_base_light(p_lights[i]);
			if (!base.is_valid()) {
				continue;
			}
			const Transform3D xf = ls->light_instance_get_base_transform(p_lights[i]);
			Light l;
			const float energy = ls->light_get_param(base, RSE::LIGHT_PARAM_ENERGY);
			// Godot lights in linear space; PS1 mode keeps the picked colour
			// as a display colour, the way it would have been authored.
			const Color picked = ls->light_get_color(base);
			l.color = (settings.lighting == psx::LIGHTING_LINEAR ? picked.srgb_to_linear() : picked) * energy;
			l.color.a = 1.0f;
			const Vector3 forward = -xf.basis.get_column(2).normalized(); // Lights shine along -Z.
			switch (ls->light_get_type(base)) {
				case RSE::LIGHT_DIRECTIONAL:
					l.directional = true;
					l.to_light = -forward;
					break;
				case RSE::LIGHT_SPOT:
					l.spot_axis = forward;
					l.spot_cos = Math::cos(Math::deg_to_rad(ls->light_get_param(base, RSE::LIGHT_PARAM_SPOT_ANGLE)));
					{
						// As Godot's shader: 0 makes a hard edge.
						const float spot_atten = ls->light_get_param(base, RSE::LIGHT_PARAM_SPOT_ATTENUATION);
						l.spot_edge_exponent = Math::abs(spot_atten) > 1e-6f ? 1.0f / spot_atten : 1e6f;
					}
					[[fallthrough]];
				case RSE::LIGHT_OMNI:
					l.directional = false;
					l.position = xf.origin;
					l.range = MAX(ls->light_get_param(base, RSE::LIGHT_PARAM_RANGE), 0.001f);
					l.decay = ls->light_get_param(base, RSE::LIGHT_PARAM_ATTENUATION);
					break;
				default:
					continue;
			}
			lights.push_back(l);
		}
	}

	// Picks up to three directional lights for an object and loads LLM in
	// its object space; their colours go to f.light_colors (LCM is filled per
	// surface). Omni and spot lights whose range reaches the object's
	// bounding sphere go to f.near_lights (strongest first, up to
	// MAX_POINT_LIGHTS) and are evaluated per vertex.
	void _load_lights(Frame &f, const Transform3D &p_object, const Vector3 &p_center, double p_radius) {
		f.near_light_count = 0;
		float near_weight[psx::MAX_POINT_LIGHTS] = {};
		for (const Light &l : lights) {
			if (l.directional) {
				continue;
			}
			const double gap = (l.position - p_center).length() - p_radius;
			if (gap >= l.range) {
				continue;
			}
			const float w = (l.color.r + l.color.g + l.color.b) * (float)(1.0 - MAX(gap, 0.0) / l.range);
			if (f.near_light_count < psx::MAX_POINT_LIGHTS) {
				near_weight[f.near_light_count] = w;
				f.near_lights[f.near_light_count++] = &l;
			} else {
				int weakest = 0;
				for (int k = 1; k < psx::MAX_POINT_LIGHTS; k++) {
					weakest = near_weight[k] < near_weight[weakest] ? k : weakest;
				}
				if (w > near_weight[weakest]) {
					near_weight[weakest] = w;
					f.near_lights[weakest] = &l;
				}
			}
		}
		struct Pick {
			Vector3 dir;
			Color color;
			float weight;
		};
		Pick picks[3];
		int count = 0;
		for (const Light &l : lights) {
			if (!l.directional) {
				continue;
			}
			Pick p;
			p.dir = l.to_light;
			p.color = l.color;
			p.weight = p.color.r + p.color.g + p.color.b;
			if (p.weight <= 0.0f) {
				continue;
			}
			if (count < 3) {
				picks[count++] = p;
			} else {
				int weakest = 0;
				for (int k = 1; k < 3; k++) {
					weakest = picks[k].weight < picks[weakest].weight ? k : weakest;
				}
				if (p.weight > picks[weakest].weight) {
					picks[weakest] = p;
				}
			}
		}
		// The rotation only: normals already carry the inverse scale.
		const Basis inv = p_object.basis.orthonormalized().transposed();
		f.gte.light_dirs = psx::Matrix();
		for (int k = 0; k < 3; k++) {
			f.light_colors[k][0] = f.light_colors[k][1] = f.light_colors[k][2] = 0.0f;
		}
		for (int k = 0; k < count; k++) {
			const Vector3 d = (inv.xform(picks[k].dir)).normalized();
			f.gte.light_dirs.m[k][0] = (int16_t)CLAMP(std::lround(d.x * 4096.0), -4096L, 4096L);
			f.gte.light_dirs.m[k][1] = (int16_t)CLAMP(std::lround(d.y * 4096.0), -4096L, 4096L);
			f.gte.light_dirs.m[k][2] = (int16_t)CLAMP(std::lround(d.z * 4096.0), -4096L, 4096L);
			f.light_colors[k][0] = picks[k].color.r;
			f.light_colors[k][1] = picks[k].color.g;
			f.light_colors[k][2] = picks[k].color.b;
		}
	}
};

#endif // RD_ENABLED
