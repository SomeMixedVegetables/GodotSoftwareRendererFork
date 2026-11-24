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

#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_geometry_instance.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_scene_render.h"
#include "servers/rendering/rendering_server.h"

#include <cmath>

// What the PSX path takes from a Godot material.
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

	uint32_t hash() const { // Of what is baked into converted meshes.
		uint32_t h = hash_murmur3_one_32((uint32_t)cull);
		h = hash_murmur3_one_32((lit ? 1u : 0u) | (semi ? 2u : 0u) | (vertex_colors ? 4u : 0u) | (vertex_colors_srgb ? 8u : 0u) | ((uint32_t)blend << 4), h);
		h = hash_murmur3_one_float(albedo.r, h);
		h = hash_murmur3_one_float(albedo.g, h);
		h = hash_murmur3_one_float(albedo.b, h);
		h = hash_murmur3_one_float(albedo.a, h);
		return hash_fmix32(h);
	}
};

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
	const ShaderDataT *sd = static_cast<const ShaderDataT *>(ms->material_get_shader_data(p_material));
	bool emissive = false;
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
	}
	const Variant albedo = ms->material_get_param(p_material, "albedo");
	if (albedo.get_type() == Variant::COLOR) {
		info.albedo = albedo;
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
	const Variant metallic = ms->material_get_param(p_material, "metallic");
	if (metallic.get_type() == Variant::FLOAT) {
		info.metallic = CLAMP((float)metallic, 0.0f, 1.0f);
	}
	return info;
}

struct PSXSceneSettings {
	float world_scale = 512.0f; // GTE units per metre
	Color default_ambient = Color(0.2, 0.2, 0.2);
	psx::LightingMode lighting = psx::LIGHTING_PS1;
	float subdivide_size = 1.0f; // Metres; 0 = off
};

struct PSXSceneStats {
	uint32_t instances = 0;
	uint32_t surfaces = 0;
	uint32_t conversions = 0;
	uint32_t skinned = 0;
	psx::DrawStats draw;
};

template <class ShaderDataT>
class PSXSceneBridge {
public:
	static constexpr uint32_t OT_SIZE = 4096;
	static constexpr uint64_t CACHE_KEEP_FRAMES = 300;

	PSXSceneSettings settings;

	// Draws the scene into p_ot: geometry only. The caller adds the clear and
	// the drawing environment at the far end.
	void build(RendererSceneRender *p_scene_render, const RendererSceneRender::CameraData &p_camera, const PagedArray<RenderGeometryInstance *> &p_instances,
			const PagedArray<RID> &p_lights, RID p_environment, int32_t p_width, int32_t p_height, int p_vram_scale,
			psx::PacketBuilder &p_builder, psx::OrderingTable &p_ot, uint64_t p_frame, PSXSceneStats &r_stats) {
		r_stats = PSXSceneStats();
		frame = p_frame;
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
		f.gte.scale = p_vram_scale;
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
		RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();

		for (uint32_t i = 0; i < p_instances.size(); i++) {
			const RenderGeometryInstanceBase *inst = static_cast<const RenderGeometryInstanceBase *>(p_instances[i]);
			if (!inst->data) {
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
	};

	struct MeshEntry {
		AABB aabb;
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
	psx::MeshData skin_mesh;
	Color fog_color;
	float fog_sky_affect = 0.0f;
	uint64_t frame = 0;

	static int32_t _to_int32(double p_value) {
		return (int32_t)CLAMP(std::llround(p_value), (long long)INT32_MIN, (long long)INT32_MAX);
	}

	static int32_t _to_fixed(float p_value) { // 1.19.12
		return (int32_t)CLAMP(std::lround(p_value * 4096.0), 0L, 0x7FFFFL);
	}

	static uint8_t _to_byte(float p_value) {
		return (uint8_t)CLAMP(std::lround(p_value * 255.0f), 0L, 255L);
	}

	// One mesh instance (or one multimesh instance) at p_transform.
	void _draw_instance(Frame &f, RendererRD::MeshStorage *p_storage, const RenderGeometryInstanceBase *p_inst, RID p_mesh, const Transform3D &p_transform) {
		MeshEntry *entry = _get_mesh(p_storage, p_mesh);
		if (!entry) {
			return;
		}
		f.stats->instances++;

		const Transform3D view = f.cam_inv * p_transform;
		const Basis &b = view.basis;
		const double s = MAX(MAX(b.get_column(0).length(), b.get_column(1).length()), b.get_column(2).length());
		if (s < 1e-6) {
			return;
		}
		const int32_t bucket = (int32_t)std::lround(std::log2(s) * 8.0);
		const double sb = std::exp2(bucket / 8.0);

		f.gte.translation.x = _to_int32(f.row_scale[0] * f.world * view.origin.x);
		f.gte.translation.y = _to_int32(f.row_scale[1] * f.world * view.origin.y);
		f.gte.translation.z = _to_int32(f.row_scale[2] * f.world * view.origin.z);
		const double radius = entry->aabb.size.length() * 0.5 * s;
		_load_lights(f, p_transform, p_transform.xform(entry->aabb.get_center()), radius);
		const Transform3D object_inv = p_transform.affine_inverse();

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

		for (uint32_t surface = 0; surface < entry->surfaces.size(); surface++) {
			const SourceSurface &src = entry->surfaces[surface];
			if (src.vertex_count == 0) {
				continue;
			}
			RID material = p_inst->data->material_override;
			if (!material.is_valid() && surface < (uint32_t)p_inst->data->surface_materials.size()) {
				material = p_inst->data->surface_materials[surface];
			}
			if (!material.is_valid()) {
				material = p_storage->mesh_surface_get_material(p_mesh, surface);
			}
			const PSXMaterialInfo mat = psx_read_material<ShaderDataT>(material);

			const psx::MeshData *data = nullptr;
			if (skinned && src.influences > 0) {
				data = _skin(src, mat, sb, f);
			} else {
				data = _get_converted(*entry, surface, bucket, sb, mat, *f.stats);
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
				const Vector3 p = object_inv.xform(src_light.position) * units_per_mesh_unit;
				pl.position[0] = (float)p.x;
				pl.position[1] = (float)p.y;
				pl.position[2] = (float)p.z;
				pl.metres_per_unit = (float)((double)(1 << data->shift) / settings.world_scale);
				pl.range = src_light.range;
				pl.decay = src_light.decay;
				pl.color[0] = src_light.color.r * diffuse;
				pl.color[1] = src_light.color.g * diffuse;
				pl.color[2] = src_light.color.b * diffuse;
				const Vector3 axis = object_inv.basis.xform(src_light.spot_axis).normalized();
				pl.spot_axis[0] = (float)axis.x;
				pl.spot_axis[1] = (float)axis.y;
				pl.spot_axis[2] = (float)axis.z;
				pl.spot_cos = src_light.spot_cos;
			}

			psx::DrawParams params;
			params.point_lights = point_lights;
			params.point_light_count = mat.lit ? f.near_light_count : 0;
			params.cull = mat.cull;
			params.lit = mat.lit;
			params.lighting = settings.lighting;
			const Color em = Color(MIN(mat.emission.r, 1.0f), MIN(mat.emission.g, 1.0f), MIN(mat.emission.b, 1.0f)).linear_to_srgb();
			params.emission[0] = _to_byte(em.r);
			params.emission[1] = _to_byte(em.g);
			params.emission[2] = _to_byte(em.b);
			params.fog = f.fog;
			params.semi = mat.semi;
			params.blend = mat.blend;
			params.ortho = f.ortho;
			params.near_z = f.near_z;
			params.ot_shift = f.ot_shift;
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
		const float *r = buffer.ptr();
		for (int k = 0; k < visible; k++) {
			const float *d = r + k * stride;
			const Transform3D local(Basis(d[0], d[1], d[2], d[4], d[5], d[6], d[8], d[9], d[10]), Vector3(d[3], d[7], d[11]));
			_draw_instance(f, p_storage, p_inst, mesh, p_inst->transform * local);
		}
	}

	// Linear-blend skinning on the CPU, then a fresh conversion (not cached:
	// the pose changes every frame). Returns nullptr if nothing to draw.
	const psx::MeshData *_skin(const SourceSurface &p_src, const PSXMaterialInfo &p_mat, double p_scale, Frame &f) {
		const uint32_t n = p_src.vertex_count;
		const int inf = p_src.influences;
		const bool has_normals = !p_src.normals.is_empty();
		skin_positions.resize(n * 3);
		skin_normals.resize(has_normals ? n * 3 : 0);
		const int bone_count = (int)bone_cache.size();
		for (uint32_t v = 0; v < n; v++) {
			const Vector3 p(p_src.positions[v * 3], p_src.positions[v * 3 + 1], p_src.positions[v * 3 + 2]);
			const Vector3 nrm = has_normals ? Vector3(p_src.normals[v * 3], p_src.normals[v * 3 + 1], p_src.normals[v * 3 + 2]) : Vector3();
			Vector3 sp, sn;
			float total = 0.0f;
			for (int j = 0; j < inf; j++) {
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
		const psx::ConvertOptions opt = _options(p_mat, p_scale);
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

	psx::ConvertOptions _options(const PSXMaterialInfo &p_mat, double p_scale) const {
		psx::ConvertOptions opt;
		opt.scale = (float)(settings.world_scale * p_scale);
		opt.albedo[0] = p_mat.albedo.r;
		opt.albedo[1] = p_mat.albedo.g;
		opt.albedo[2] = p_mat.albedo.b;
		opt.albedo[3] = p_mat.albedo.a;
		opt.use_vertex_colors = p_mat.vertex_colors;
		opt.vertex_colors_srgb = p_mat.vertex_colors_srgb;
		opt.use_alpha = p_mat.semi && p_mat.blend == psx::BLEND_HALF;
		opt.max_edge = settings.subdivide_size * settings.world_scale;
		return opt;
	}

	MeshEntry *_get_mesh(RendererRD::MeshStorage *p_storage, RID p_mesh) {
		const AABB aabb = p_storage->mesh_get_aabb(p_mesh);
		const int surface_count = p_storage->mesh_get_surface_count(p_mesh);
		MeshEntry *entry = meshes.getptr(p_mesh);
		if (entry && entry->aabb == aabb && (int)entry->surfaces.size() == surface_count) {
			entry->last_used = frame;
			return entry;
		}
		// New or changed: read every surface back from the mesh storage.
		if (!entry) {
			entry = &meshes.insert(p_mesh, MeshEntry())->value;
		}
		entry->aabb = aabb;
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
			if (uvs.size() == n) {
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
		}
		return entry;
	}

	const psx::MeshData *_get_converted(MeshEntry &p_entry, uint32_t p_surface, int32_t p_bucket, double p_scale, const PSXMaterialInfo &p_mat, PSXSceneStats &r_stats) {
		uint64_t key = ((uint64_t)p_mat.hash() << 32) | ((uint64_t)(uint16_t)p_bucket << 16) | (p_surface & 0xFFFF);
		psx::MeshData *found = p_entry.converted.getptr(key);
		if (found) {
			return found->indices.empty() ? nullptr : found;
		}
		const psx::MeshInput in = _input(p_entry.surfaces[p_surface]);
		const psx::ConvertOptions opt = _options(p_mat, p_scale);
		psx::MeshData &out = p_entry.converted.insert(key, psx::MeshData())->value;
		psx::convert_mesh(in, opt, out); // Empty on failure: remembered, not retried.
		r_stats.conversions++;
		if (out.clamped) {
			WARN_PRINT_ONCE("PSX: a mesh is too large for 16-bit GTE coordinates and was clamped; lower rendering/psx/world_scale or split the mesh.");
		}
		return out.indices.empty() ? nullptr : &out;
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
		const Basis inv = p_object.basis.inverse();
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
