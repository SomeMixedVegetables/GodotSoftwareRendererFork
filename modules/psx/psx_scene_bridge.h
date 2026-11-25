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

#include "core/io/image.h"
#include "core/os/os.h"
#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_geometry_instance.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
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

// Reads cull mode, shading, blending and vertex-colour use from the compiled
// scene shader (ShaderDataT is the active renderer's ShaderData), and the
// albedo, emission and metallic uniforms of StandardMaterial3D/ORMMaterial3D.
template <class ShaderDataT>
PSXMaterialInfo psx_read_material(RID p_material) {
	PSXMaterialInfo info;
	if (!p_material.is_valid()) {
		return info;
	}
	RendererRD::MaterialStorage *ms = RendererRD::MaterialStorage::get_singleton();
	if (!ms->material_get_data(p_material, RendererRD::MaterialStorage::SHADER_TYPE_3D)) {
		return info; // No spatial shader (yet): Godot's default material.
	}
	info.albedo = Color(1, 1, 1, 1); // Custom shaders without an albedo uniform.
	using SD = RendererRD::MaterialStorage::ShaderData;
	using MD = typename PSXMaterialDataOf<ShaderDataT>::Type;
	const MD *md = static_cast<const MD *>(ms->material_get_data(p_material, RendererRD::MaterialStorage::SHADER_TYPE_3D));
	info.render_priority = (int32_t)md->priority + RSE::MATERIAL_RENDER_PRIORITY_MIN; // Stored as 0..255
	const ShaderDataT *sd = static_cast<const ShaderDataT *>(ms->material_get_shader_data(p_material));
	bool emissive = false;
	// StandardMaterial3D's uniform names; a custom shader's first colour and
	// texture uniforms marked source_color stand in for them.
	StringName color_param = "albedo";
	StringName texture_param = "texture_albedo";
	if (sd) {
		const int cull = (int)sd->cull_mode;
		info.cull = cull == RSE::CULL_MODE_DISABLED ? psx::CULL_NONE : (cull == RSE::CULL_MODE_FRONT ? psx::CULL_FRONT : psx::CULL_BACK);
		info.lit = !sd->unshaded;
		const int blend = sd->blend_mode;
		if (blend == SD::BLEND_MODE_ADD) {
			info.semi = true;
			info.blend = psx::BLEND_ADD;
		} else if (blend == SD::BLEND_MODE_SUB) {
			info.semi = true;
			info.blend = psx::BLEND_SUBTRACT;
		} else if (blend == SD::BLEND_MODE_MUL || blend == SD::BLEND_MODE_PREMULTIPLIED_ALPHA || (sd->uses_alpha && !sd->uses_alpha_clip)) {
			info.semi = true; // The PS1 has one alpha: 50%.
			info.blend = psx::BLEND_HALF;
		}
		// StandardMaterial3D's generated code: see BaseMaterial3D::_update_shader().
		info.vertex_colors = sd->code.contains("albedo_tex *= COLOR");
		info.vertex_colors_srgb = sd->code.contains("COLOR.rgb = mix(");
		emissive = sd->code.contains("EMISSION = ");
		if (sd->uses_alpha_clip) {
			info.alpha_mode = psx::ALPHA_CUT;
		} else if (info.semi && info.blend == psx::BLEND_HALF) {
			info.alpha_mode = psx::ALPHA_BLEND;
		}
		// Billboards are done in the vertex shader; the bridge turns the
		// instance to face the camera instead (_billboard).
		if (sd->code.contains("// Billboard Mode: Enabled")) {
			info.billboard = BILLBOARD_ENABLED;
		} else if (sd->code.contains("// Billboard Mode: Particles")) {
			info.billboard = BILLBOARD_PARTICLES;
		} else if (sd->code.contains("// Billboard Mode: Y-Billboard")) {
			info.billboard = BILLBOARD_FIXED_Y;
		}
		info.billboard_keep_scale = sd->code.contains("// Billboard Keep Scale: Enabled");
		info.anim = sd->code.contains("particle_total_frames");
		using U = ShaderLanguage::ShaderNode::Uniform;
		if (!sd->uniforms.has(color_param)) {
			int best = INT_MAX;
			for (const KeyValue<StringName, U> &kv : sd->uniforms) {
				const U &u = kv.value;
				if (u.scope == U::SCOPE_LOCAL && u.hint == U::HINT_SOURCE_COLOR && (u.type == ShaderLanguage::TYPE_VEC3 || u.type == ShaderLanguage::TYPE_VEC4) && u.prop_order < best) {
					best = u.prop_order;
					color_param = kv.key;
				}
			}
		}
		if (!sd->uniforms.has(texture_param)) {
			int best = INT_MAX;
			for (const KeyValue<StringName, U> &kv : sd->uniforms) {
				const U &u = kv.value;
				if (u.scope == U::SCOPE_LOCAL && u.use_color && u.type == ShaderLanguage::TYPE_SAMPLER2D && u.texture_order < best) { // source_color on a sampler sets use_color
					best = u.texture_order;
					texture_param = kv.key;
				}
			}
		}
		// The albedo sampler's declaration says whether it repeats.
		const U *tex_uniform = sd->uniforms.getptr(texture_param);
		if (tex_uniform) {
			info.texture_repeat = tex_uniform->repeat != ShaderLanguage::REPEAT_DISABLE;
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
	const Variant uv_scale = ms->material_get_param(p_material, "uv1_scale");
	if (uv_scale.get_type() == Variant::VECTOR3) {
		const Vector3 s = uv_scale;
		info.uv_scale = Vector2(s.x, s.y);
	}
	const Variant uv_offset = ms->material_get_param(p_material, "uv1_offset");
	if (uv_offset.get_type() == Variant::VECTOR3) {
		const Vector3 o = uv_offset;
		info.uv_offset = Vector2(o.x, o.y);
	}
	if (info.alpha_mode == psx::ALPHA_CUT) {
		const Variant cut = ms->material_get_param(p_material, "alpha_scissor_threshold");
		if (cut.get_type() == Variant::FLOAT) {
			info.alpha_cut = CLAMP((float)cut, 0.0f, 1.0f);
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
		const Variant emission = ms->material_get_param(p_material, "emission");
		const Variant energy = ms->material_get_param(p_material, "emission_energy");
		if (emission.get_type() == Variant::COLOR) {
			const float e = energy.get_type() == Variant::FLOAT ? (float)energy : 1.0f;
			const Color c = Color(emission).srgb_to_linear() * e;
			info.emission = Color(c.r, c.g, c.b);
		}
	}
	if (info.anim) {
		const Variant h = param("particles_anim_h_frames");
		const Variant v = param("particles_anim_v_frames");
		const Variant loop = param("particles_anim_loop");
		info.anim_h = h.get_type() == Variant::INT ? CLAMP((int32_t)h, 1, 128) : 1;
		info.anim_v = v.get_type() == Variant::INT ? CLAMP((int32_t)v, 1, 128) : 1;
		info.anim_loop = loop.get_type() == Variant::BOOL && (bool)loop;
		// Every particle billboard has the animation code; one frame is
		// an ordinary texture.
		info.anim = info.anim_h * info.anim_v > 1;
	}
	const Variant metallic = ms->material_get_param(p_material, "metallic");
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
		const Color ambient = _ambient(p_scene_render, p_environment);
		f.gte.ambient = { _to_fixed(ambient.r), _to_fixed(ambient.g), _to_fixed(ambient.b) };
		f.fog = _setup_fog(p_scene_render, p_environment, proj, f);

		f.cam_inv = p_camera.main_transform.affine_inverse();
		f.cam_basis = p_camera.main_transform.basis.orthonormalized();
		RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();

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
			}
			// Particles: not drawn yet.
		}
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
	Color background(RendererSceneRender *p_scene_render, RID p_environment, const Color &p_clear) const {
		Color bg = p_clear;
		if (p_environment.is_valid() && p_scene_render->is_environment(p_environment) &&
				p_scene_render->environment_get_background(p_environment) == RSE::ENV_BG_COLOR) {
			bg = p_scene_render->environment_get_bg_color(p_environment);
		}
		if (fog_sky_affect > 0.0f) {
			bg = bg.lerp(fog_color, fog_sky_affect);
		}
		return bg;
	}

private:
	struct SourceSurface {
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
	};

	// Per-frame state shared by every object.
	struct Frame {
		psx::GTE gte;
		psx::PacketBuilder *builder = nullptr;
		psx::OrderingTable *ot = nullptr;
		PSXSceneStats *stats = nullptr;
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
	struct BlendWeight {
		uint32_t shape = 0;
		float weight = 0.0f;
	};
	LocalVector<BlendWeight> blend_cache; // This instance's blend shape weights that are not zero
	psx::MeshData skin_mesh;
	Color fog_color;
	float fog_sky_affect = 0.0f;
	uint64_t frame = 0;
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
				data = _skin(src, shape_surface, blend_normalized, skin_surface, mat, tex_data, normal_scale, sb, f);
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

	// Blend shapes (blend_cache's weights, applied as Godot's skinning shader
	// does) then linear-blend skinning on the CPU, then a fresh conversion
	// (not cached: the pose changes every frame). Returns nullptr if nothing
	// to draw.
	const psx::MeshData *_skin(const SourceSurface &p_src, bool p_shapes, bool p_normalized, bool p_skin, const PSXMaterialInfo &p_mat, const psx::TextureData *p_texture, const Vector3 &p_normal_scale, double p_scale, Frame &f) {
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
		psx::MeshInput in = _input(p_src);
		in.positions = skin_positions.ptr();
		in.normals = has_normals ? skin_normals.ptr() : nullptr;
		const psx::ConvertOptions opt = _options(p_mat, p_texture, p_normal_scale, p_scale);
		psx::convert_mesh(in, opt, skin_mesh);
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
		// New or changed: read every surface back from the mesh storage.
		PSXSceneStats &st = active_stats ? *active_stats : unused_stats;
		PSXScopedTimer timer(st.mesh_read_usec);
		st.meshes_read++;
		if (!entry) {
			entry = &meshes.insert(p_mesh, MeshEntry())->value;
		}
		entry->aabb = aabb;
		entry->mesh = p_mesh;
		entry->signature = signature;
		entry->last_used = frame;
		entry->converted.clear();
		entry->surfaces.clear();
		entry->surfaces.resize(surface_count);
		for (int i = 0; i < surface_count; i++) {
			// The const overload reads the surface back; the other returns a raw pointer.
			const RenderingServerTypes::SurfaceData sd = static_cast<const RendererRD::MeshStorage *>(p_storage)->mesh_get_surface(p_mesh, i);
			if (sd.primitive != RSE::PRIMITIVE_TRIANGLES || sd.vertex_count == 0) {
				continue; // Lines and points: not drawn yet.
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
			dst.indices.resize(indices.size());
			for (int k = 0; k < indices.size(); k++) {
				dst.indices[k] = indices[k];
			}
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
		return entry;
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

	const psx::MeshData *_get_converted(MeshEntry &p_entry, uint32_t p_surface, int32_t p_bucket, double p_scale, const PSXMaterialInfo &p_mat, const psx::TextureData *p_texture, const Vector3 &p_normal_scale, PSXSceneStats &r_stats) {
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
		const uint64_t key = ((uint64_t)hash_fmix32(h) << 32) | ((uint64_t)(uint16_t)p_bucket << 16) | (p_surface & 0xFFFF);
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
		const Ref<Image> source = p_storage->texture_2d_get(p_texture);
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
				// when there is one, else the ambient colour.
				if (p_scene_render->environment_get_background(p_environment) == RSE::ENV_BG_COLOR) {
					return lin(p_scene_render->environment_get_bg_color(p_environment)) * energy;
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
