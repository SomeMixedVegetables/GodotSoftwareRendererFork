// PSX scene renderer for the RenderingDevice backends (Forward+ and Mobile).
//
// It subclasses Godot's own scene renderer instead of wrapping it. The base
// class keeps environment and compositor data as private members, and owns
// the geometry instances RendererSceneCull creates, so a wrapper could not
// hand a frame back to it. As a subclass, every call except render_scene()
// is inherited unchanged, and render_scene() can fall back to Base.
//
// Every 3D viewport is drawn by the PSX core: PSXSceneBridge turns the scene
// (camera, meshes, materials, lights, environment) into GPU packets in an
// ordering table, the GPU rasterizes them into VRAM, and the displayed buffer
// is scanned out and stretched over the render target. With
// rendering/psx/debug/demo_scene the built-in test cube is drawn instead.
//
// Editor helpers (grid, origin axes, gizmos, selection boxes; layers 21-32)
// are not part of the PSX picture. When a viewport has any, Godot's own
// renderer draws just those over the PSX frame: a private environment with
// the Canvas background makes it start from the render target (the PSX
// frame) instead of clearing. The PS1 has no depth buffer, so the frame's
// depth is made for them: the core records the opaque triangles it draws,
// psx::paint_depth paints them in drawing order, and a full-screen quad in
// the same pass draws the PS1 frame back with that depth (and nothing where
// the frame shows only the background), so helpers behind models are hidden.
//
// ShaderDataT is the base renderer's compiled-shader type, which is where
// cull mode, blending and shading live.

#pragma once

#ifdef RD_ENABLED

#include "psx_core/psx_frame.h"
#include "psx_core/psx_gpu.h"
#include "psx_core/psx_ot.h"
#include "psx_scene_bridge.h"
#include "psx_debug.h"
#include "psx_settings.h"

#include "core/config/project_settings.h"
#include "core/os/memory.h"
#include "core/templates/local_vector.h"
#include "core/templates/paged_array.h"
#include "servers/rendering/renderer_rd/effects/copy_effects.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/renderer_viewport.h"
#include "servers/rendering/rendering_server_globals.h"

template <class Base, class ShaderDataT>
class PSXSceneRenderRD : public Base {
	psx::GPU *gpu = nullptr;
	psx::FrameLayout layout;
	psx::DemoScene demo;
	bool demo_scene = false;
	PSXSceneBridge<ShaderDataT> bridge;
	PSXSceneStats scene_stats;
	psx::PacketArena arena{ 1u << 21 };
	LocalVector<uint32_t> frame_pixels;
	RID frame_texture;
	uint64_t frame_count = 0;
	PSXDebug debug;
	// Editor helpers for the overlay pass (the pool must outlive the array).
	PagedArrayPool<RenderGeometryInstance *> helper_pool{ 256 };
	PagedArray<RenderGeometryInstance *> helpers;
	RID overlay_environment;
	// Depth under the helpers (see the top of this file).
	std::vector<psx::DepthTriangle> depth_triangles;
	std::vector<uint16_t> depth_sz;
	LocalVector<float> depth_metres;
	RID depth_texture; // RD R32F: view depth in metres, 0 = nothing drawn
	RID frame_texture_ref, depth_texture_ref; // Texture RIDs for the proxy material
	RID proxy_mesh, proxy_shader, proxy_material;
	RenderGeometryInstance *proxy = nullptr;
	bool depth_ready = false;

	void _setup() {
		int scale = GLOBAL_GET("rendering/psx/vram_scale");
		int width = GLOBAL_GET("rendering/psx/resolution_width");
		int height = GLOBAL_GET("rendering/psx/resolution_height");
		scale = CLAMP(scale, 1, psx::MAX_VRAM_SCALE);
		gpu = memnew(psx::GPU(scale));
		layout = psx::FrameLayout::make(*gpu, width, height);
		if (layout.buffer_count == 0) {
			ERR_PRINT(vformat("PSX: a %dx%d display does not fit in %dx%d VRAM (vram_scale %d). Using 320x240.", width, height, gpu->get_vram_width(), gpu->get_vram_height(), scale));
			layout = psx::FrameLayout::make(*gpu, 320, 240);
		}
		if (layout.overlaps_texture_region) {
			WARN_PRINT("PSX: the display buffer overlaps the 1024x512 texture region; raise rendering/psx/vram_scale.");
		}
		demo_scene = GLOBAL_GET("rendering/psx/debug/demo_scene");
		// The live settings (psx_settings.h) are resolved per frame in render_scene().
		bridge.settings.texture_banks = (uint32_t)MAX(0, (int)GLOBAL_GET("rendering/psx/textures/vram_banks"));
		// Textures share the 1024x512 texture region with the display buffers
		// (at scale 1) and, when it is drawn, the demo's textures.
		for (int i = 0; i < layout.buffer_count; i++) {
			bridge.reserve_vram(layout.buffer_x[i], layout.buffer_y[i], layout.width, layout.height);
		}
		if (demo_scene) {
			demo.upload_textures(*gpu);
			bridge.reserve_vram(psx::DemoScene::TEX4_X, 0, psx::VRAM_BASE_WIDTH - psx::DemoScene::TEX4_X, psx::DemoScene::USED_ROWS);
		}
		const int speed_percent = GLOBAL_GET("rendering/psx/debug/demo_speed_percent");
		demo.speed = (uint32_t)CLAMP(speed_percent, 0, 400) * 256 / 100;
		print_line(vformat("PSX: VRAM %dx%d (scale %d), display %dx%d, %d buffer(s).", gpu->get_vram_width(), gpu->get_vram_height(), scale, layout.width, layout.height, layout.buffer_count));
		PSXSettingsRegistry::set_installed(true, OS::get_singleton()->get_current_rendering_method(), layout.width, layout.height, scale);
	}

	// The scenario (World3D) the viewport drawing into p_render_target shows:
	// the key for PSXSettings nodes. Called on the render thread, where the
	// viewport list is not changing.
	static RID _scenario_for(RID p_render_target) {
		RendererViewport *rv = RSG::viewport;
		if (!rv) {
			return RID();
		}
		for (const RendererViewport::Viewport *vp : rv->active_viewports) {
			if (vp->render_target == p_render_target) {
				return vp->scenario;
			}
		}
		return RID();
	}

	void _apply(const PSXLiveSettings &p_live) {
		PSXSceneSettings &s = bridge.settings;
		s.dither = p_live.dither;
		s.lighting = p_live.lighting;
		s.default_ambient = p_live.default_ambient;
		s.world_scale = MAX(1.0f, p_live.world_scale);
		s.subdivide_size = MAX(0.0f, p_live.subdivide_size);
		s.texture.max_size = CLAMP(p_live.texture.max_size, 8, 256);
		s.texture.depth = (uint8_t)CLAMP((int)p_live.texture.depth, 0, 3);
		s.texture.max_colors = CLAMP(p_live.texture.max_colors, 0, 256);
		s.texture.dither = p_live.texture.dither;
	}

	void _ensure_frame_texture() {
		if (frame_texture.is_valid()) {
			return;
		}
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
		tf.width = layout.width;
		tf.height = layout.height;
		tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
		frame_texture = RD::get_singleton()->texture_create(tf, RD::TextureView());
		RD::get_singleton()->set_resource_name(frame_texture, "PSX frame");
	}

	void _render_scene(const RendererSceneRender::CameraData &p_camera, const PagedArray<RenderGeometryInstance *> &p_instances, const PagedArray<RID> &p_lights, RID p_environment, const Color &p_clear) {
		const int buffer = layout.buffer_count > 1 ? (int)(frame_count & 1) : 0;
		const int32_t bx = layout.buffer_x[buffer], by = layout.buffer_y[buffer];
		const int32_t w = layout.width, h = layout.height;
		arena.clear();
		psx::PacketBuilder pb(arena, gpu->get_encoder());
		psx::OrderingTable ot(arena, PSXSceneBridge<ShaderDataT>::OT_SIZE);
		ot.clear();
		// The bridge counts engine frames, not render_scene() calls: with several
		// viewports, a texture one of them drew this frame must not be evicted
		// for another.
		bridge.build(this, p_camera, p_instances, p_lights, p_environment, w, h, *gpu, pb, ot, RSG::rasterizer->get_frame_number(), scene_stats);

		// Far end: drawing environment, then the clear (the bucket is LIFO,
		// so the environment, added last, goes first).
		const Color bg = bridge.background(this, p_environment, p_clear);
		const uint32_t far_end = PSXSceneBridge<ShaderDataT>::OT_SIZE - 1;
		ot.add(far_end, pb.fill(bx, by, w, h, (uint8_t)bg.get_r8(), (uint8_t)bg.get_g8(), (uint8_t)bg.get_b8()));
		ot.add(far_end, pb.environment(bx, by, bx + w - 1, by + h - 1, bx, by, psx::GP0Encoder::texpage(0, 0, psx::TEXTURE_4BIT, psx::BLEND_HALF, bridge.settings.dither)));
		ot.draw(*gpu);
		gpu->display.x = bx;
		gpu->display.y = by;
		gpu->display.width = w;
		gpu->display.height = h;
		if (arena.is_full()) {
			WARN_PRINT_ONCE("PSX: the packet arena is full; some triangles were not drawn.");
		}
	}

	void _render_frame(const RendererSceneRender::CameraData &p_camera, const PagedArray<RenderGeometryInstance *> &p_instances, const PagedArray<RID> &p_lights, RID p_environment, const Color &p_clear) {
		gpu->stats.reset();
		if (demo_scene) {
			demo.render(*gpu, layout, (int)(frame_count & 1), (uint32_t)frame_count);
		} else {
			_render_scene(p_camera, p_instances, p_lights, p_environment, p_clear);
		}
		frame_pixels.resize(layout.width * layout.height);
		gpu->scanout_rgba8(frame_pixels.ptr());
	}
	// Draws the instances on editor-only layers over the PSX frame already in
	// the render target, with Godot's renderer. Nothing to do in a game.
	void _collect_helpers(const PagedArray<RenderGeometryInstance *> &p_instances) {
		helpers.set_page_pool(&helper_pool);
		helpers.reset();
		for (uint32_t i = 0; i < p_instances.size(); i++) {
			const RenderGeometryInstanceBase *inst = static_cast<const RenderGeometryInstanceBase *>(p_instances[i]);
			if ((inst->layer_mask & PSXSceneBridge<ShaderDataT>::USER_LAYERS) == 0) {
				helpers.push_back(p_instances[i]);
			}
		}
	}

	// The frame's depth image from the recorded triangles, uploaded for the proxy.
	void _upload_depth() {
		depth_ready = false;
		if (!frame_texture.is_valid()) {
			return;
		}
		psx::paint_depth(depth_triangles, layout.width, layout.height, depth_sz);
		const float metres_per_unit = 1.0f / MAX(1.0f, bridge.settings.world_scale);
		depth_metres.resize(depth_sz.size());
		for (uint32_t i = 0; i < depth_sz.size(); i++) {
			depth_metres[i] = depth_sz[i] * metres_per_unit;
		}
		RD *rd = RD::get_singleton();
		if (!depth_texture.is_valid()) {
			RD::TextureFormat tf;
			tf.format = RD::DATA_FORMAT_R32_SFLOAT;
			tf.width = layout.width;
			tf.height = layout.height;
			tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
			depth_texture = rd->texture_create(tf, RD::TextureView());
			rd->set_resource_name(depth_texture, "PSX frame depth");
		}
		Vector<uint8_t> bytes;
		bytes.resize(depth_metres.size() * sizeof(float));
		memcpy(bytes.ptrw(), depth_metres.ptr(), bytes.size());
		rd->texture_update(depth_texture, 0, bytes);
		depth_ready = true;
	}

	// The full-screen quad that draws the PS1 frame back with its depth.
	bool _ensure_proxy() {
		if (proxy) {
			return true;
		}
		RendererRD::TextureStorage *ts = RendererRD::TextureStorage::get_singleton();
		RendererRD::MaterialStorage *ms = RendererRD::MaterialStorage::get_singleton();
		RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
		frame_texture_ref = ts->texture_allocate();
		ts->texture_rd_initialize(frame_texture_ref, frame_texture);
		depth_texture_ref = ts->texture_allocate();
		ts->texture_rd_initialize(depth_texture_ref, depth_texture);

		proxy_shader = ms->shader_allocate();
		ms->shader_initialize(proxy_shader);
		ms->shader_set_code(proxy_shader, R"(
shader_type spatial;
render_mode unshaded, cull_disabled, shadows_disabled, fog_disabled;

uniform sampler2D psx_frame : source_color, filter_nearest, repeat_disable;
uniform sampler2D psx_depth : filter_nearest, repeat_disable;

void vertex() {
	POSITION = vec4(VERTEX.xy, 0.5, 1.0);
}

void fragment() {
	float d = texture(psx_depth, SCREEN_UV).r;
	if (d <= 0.0) {
		discard; // Background: nothing to hide helpers behind.
	}
	ALBEDO = texture(psx_frame, SCREEN_UV).rgb;
	vec4 clip = PROJECTION_MATRIX * vec4(0.0, 0.0, -d, 1.0);
	DEPTH = clip.z / clip.w;
}
)");
		proxy_material = ms->material_allocate();
		ms->material_initialize(proxy_material);
		ms->material_set_shader(proxy_material, proxy_shader);
		ms->material_set_param(proxy_material, "psx_frame", frame_texture_ref);
		ms->material_set_param(proxy_material, "psx_depth", depth_texture_ref);
		// Made during a frame, after Godot updated its materials: build its
		// uniforms now rather than at the next frame.
		ms->_update_queued_materials();

		Array arrays;
		arrays.resize(RSE::ARRAY_MAX);
		PackedVector3Array verts;
		verts.push_back(Vector3(-1, -1, 0));
		verts.push_back(Vector3(1, -1, 0));
		verts.push_back(Vector3(-1, 1, 0));
		verts.push_back(Vector3(1, 1, 0));
		PackedInt32Array indices;
		for (int i : { 0, 1, 2, 2, 1, 3 }) {
			indices.push_back(i);
		}
		arrays[RSE::ARRAY_VERTEX] = verts;
		arrays[RSE::ARRAY_INDEX] = indices;
		RenderingServerTypes::SurfaceData sd;
		if (RenderingServer::get_singleton()->mesh_create_surface_data_from_arrays(&sd, RSE::PRIMITIVE_TRIANGLES, arrays) != OK) {
			return false;
		}
		proxy_mesh = mesh_storage->mesh_allocate();
		mesh_storage->mesh_initialize(proxy_mesh);
		mesh_storage->mesh_add_surface(proxy_mesh, sd);

		proxy = this->geometry_instance_create(proxy_mesh);
		if (!proxy) {
			return false;
		}
		proxy->set_material_override(proxy_material);
		proxy->set_layer_mask(1);
		const AABB everywhere(Vector3(-1e6, -1e6, -1e6), Vector3(2e6, 2e6, 2e6));
		proxy->set_transform(Transform3D(), everywhere, everywhere);
		return true;
	}

	void _draw_editor_helpers(const Ref<RenderSceneBuffers> &p_render_buffers, const RendererSceneRender::CameraData *p_camera_data, const RendererSceneRender::CameraData *p_prev_camera_data, RID p_shadow_atlas, RID p_occluder_debug_tex, RID p_reflection_atlas, float p_screen_mesh_lod_threshold, float p_window_output_max_value, RenderingServerTypes::RenderInfo *r_render_info) {
		if (helpers.size() == 0) {
			return;
		}
		if (depth_ready && _ensure_proxy()) {
			helpers.push_back(proxy);
		}
		if (!overlay_environment.is_valid()) {
			// Defaults otherwise: linear tonemap at exposure 1, no fog or
			// glow, so the PSX frame comes back out unchanged.
			overlay_environment = this->environment_allocate();
			this->environment_initialize(overlay_environment);
			this->environment_set_background(overlay_environment, RSE::ENV_BG_CANVAS);
		}
		const PagedArray<RID> none;
		Base::render_scene(p_render_buffers, p_camera_data, p_prev_camera_data, helpers, none, none, none, none, none, none,
				overlay_environment, RID(), RID(), p_shadow_atlas, p_occluder_debug_tex, p_reflection_atlas, RID(), 0, p_screen_mesh_lod_threshold,
				nullptr, 0, nullptr, 0, p_window_output_max_value, nullptr, r_render_info);
		helpers.reset();
	}

public:
	virtual void render_scene(const Ref<RenderSceneBuffers> &p_render_buffers, const RendererSceneRender::CameraData *p_camera_data, const RendererSceneRender::CameraData *p_prev_camera_data, const PagedArray<RenderGeometryInstance *> &p_instances, const PagedArray<RID> &p_lights, const PagedArray<RID> &p_reflection_probes, const PagedArray<RID> &p_voxel_gi_instances, const PagedArray<RID> &p_decals, const PagedArray<RID> &p_lightmaps, const PagedArray<RID> &p_fog_volumes, RID p_environment, RID p_camera_attributes, RID p_compositor, RID p_shadow_atlas, RID p_occluder_debug_tex, RID p_reflection_atlas, RID p_reflection_probe, int p_reflection_probe_pass, float p_screen_mesh_lod_threshold, const RendererSceneRender::RenderShadowData *p_render_shadows, int p_render_shadow_count, const RendererSceneRender::RenderSDFGIData *p_render_sdfgi_regions, int p_render_sdfgi_region_count, float p_window_output_max_value, const RendererSceneRender::RenderSDFGIUpdateData *p_sdfgi_update_data = nullptr, RenderingServerTypes::RenderInfo *r_render_info = nullptr) override {
		Ref<RenderSceneBuffersRD> rb = p_render_buffers;

		// Reflection probe passes have no viewport to draw into: leave them to Godot.
		if (rb.is_null() || p_reflection_probe.is_valid()) {
			Base::render_scene(p_render_buffers, p_camera_data, p_prev_camera_data, p_instances, p_lights, p_reflection_probes, p_voxel_gi_instances, p_decals, p_lightmaps, p_fog_volumes, p_environment, p_camera_attributes, p_compositor, p_shadow_atlas, p_occluder_debug_tex, p_reflection_atlas, p_reflection_probe, p_reflection_probe_pass, p_screen_mesh_lod_threshold, p_render_shadows, p_render_shadow_count, p_render_sdfgi_regions, p_render_sdfgi_region_count, p_window_output_max_value, p_sdfgi_update_data, r_render_info);
			return;
		}

		RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
		const RID render_target = rb->get_render_target();

		// Project settings, then the scene's PSXSettings node, then scripts.
		const PSXLiveSettings live = PSXSettingsRegistry::resolve(_scenario_for(render_target));
		if (!live.active) {
			// Off (PSX.set_active(false), the dock's PS1 view toggle): Godot's own 3D.
			Base::render_scene(p_render_buffers, p_camera_data, p_prev_camera_data, p_instances, p_lights, p_reflection_probes, p_voxel_gi_instances, p_decals, p_lightmaps, p_fog_volumes, p_environment, p_camera_attributes, p_compositor, p_shadow_atlas, p_occluder_debug_tex, p_reflection_atlas, p_reflection_probe, p_reflection_probe_pass, p_screen_mesh_lod_threshold, p_render_shadows, p_render_shadow_count, p_render_sdfgi_regions, p_render_sdfgi_region_count, p_window_output_max_value, p_sdfgi_update_data, r_render_info);
			PSXFrameStats st;
			st.frame = frame_count;
			st.active = false;
			PSXSettingsRegistry::publish_stats(st);
			return;
		}
		_apply(live);
		const RID framebuffer = texture_storage->render_target_get_rd_framebuffer(render_target);
		ERR_FAIL_COND(!framebuffer.is_valid());
		const Size2i target_size = texture_storage->render_target_get_size(render_target);

		const Color clear = texture_storage->render_target_is_clear_requested(render_target)
				? texture_storage->render_target_get_clear_request_color(render_target)
				: (Color)GLOBAL_GET("rendering/environment/defaults/default_clear_color");
		// Helpers first: only a viewport that has some records its triangles.
		_collect_helpers(p_instances);
		depth_triangles.clear();
		bridge.depth_triangles = (helpers.size() > 0 && !demo_scene) ? &depth_triangles : nullptr;
		depth_ready = false;
		const uint64_t raster_begin = OS::get_singleton()->get_ticks_usec();
		_render_frame(*p_camera_data, p_instances, p_lights, p_environment, clear);
		const uint64_t raster_usec = OS::get_singleton()->get_ticks_usec() - raster_begin;

		if (debug.wants_dump(frame_count)) {
			debug.dump(frame_count, frame_pixels.ptr(), layout.width, layout.height);
		}

		const uint64_t upload_begin = OS::get_singleton()->get_ticks_usec();
		_ensure_frame_texture();
		Vector<uint8_t> bytes;
		bytes.resize(frame_pixels.size() * sizeof(uint32_t));
		memcpy(bytes.ptrw(), frame_pixels.ptr(), bytes.size());
		RD::get_singleton()->texture_update(frame_texture, 0, bytes);
		const uint64_t upload_usec = OS::get_singleton()->get_ticks_usec() - upload_begin;

		debug.report(frame_count, raster_usec, upload_usec, (int)p_instances.size(), (int)gpu->stats.triangles);
		const psx::DrawStats &ds = scene_stats.draw;
		debug.report_convert(frame_count, scene_stats.mesh_read_usec, scene_stats.meshes_read, scene_stats.mesh_convert_usec, scene_stats.meshes_converted, scene_stats.skin_usec, scene_stats.skinned,
				scene_stats.texture_read_usec, scene_stats.textures_read, scene_stats.texture_convert_usec, scene_stats.textures_converted, scene_stats.particles_read_usec, scene_stats.particles);
		debug.report_scene(frame_count, scene_stats.instances, scene_stats.surfaces, scene_stats.conversions, ds.triangles_in, ds.culled_near, ds.culled_backface, ds.culled_offscreen, ds.culled_far, ds.lines, ds.points);
		debug.report_textures(frame_count, ds.textured, ds.windowed, scene_stats.textures_resident, scene_stats.textures_baked, scene_stats.texture_uploads, scene_stats.texture_fallbacks, scene_stats.texture_banks, scene_stats.vram_free_percent, scene_stats.clut_slots, scene_stats.shared_slots);
		{
			PSXFrameStats st;
			st.frame = frame_count;
			st.instances = scene_stats.instances;
			st.triangles_in = ds.triangles_in;
			st.triangles_drawn = (uint32_t)gpu->stats.triangles;
			st.conversions = scene_stats.conversions;
			st.textures_resident = scene_stats.textures_resident;
			st.textures_baked = scene_stats.textures_baked;
			st.texture_fallbacks = scene_stats.texture_fallbacks;
			st.texture_banks = scene_stats.texture_banks;
			st.vram_free_percent = scene_stats.vram_free_percent;
			st.raster_usec = raster_usec;
			PSXSettingsRegistry::publish_stats(st);
		}
		frame_count++;

		// Nearest-filtered upscale of the PSX frame to the whole render target.
		this->copy_effects->copy_to_fb_rect(frame_texture, framebuffer, Rect2i(Point2i(), target_size),
				false, false, false, false, RID(), false, false, false, false, Rect2(), 1.0, false);

		if (helpers.size() > 0 && bridge.depth_triangles) {
			_upload_depth();
		}
		_draw_editor_helpers(p_render_buffers, p_camera_data, p_prev_camera_data, p_shadow_atlas, p_occluder_debug_tex, p_reflection_atlas, p_screen_mesh_lod_threshold, p_window_output_max_value, r_render_info);

		// Tell the viewport the render target has been drawn; otherwise
		// RendererViewport clears it after 3D, wiping our frame.
		texture_storage->render_target_disable_clear_request(render_target);
	}

	PSXSceneRenderRD() {
		debug.load();
		_setup();
	}

	~PSXSceneRenderRD() {
		PSXSettingsRegistry::set_installed(false, String(), 0, 0, 0);
		helpers.reset();
		if (overlay_environment.is_valid()) {
			this->environment_free(overlay_environment);
		}
		if (proxy) {
			this->geometry_instance_free(proxy);
		}
		if (proxy_material.is_valid()) {
			RendererRD::MaterialStorage::get_singleton()->material_free(proxy_material);
		}
		if (proxy_shader.is_valid()) {
			RendererRD::MaterialStorage::get_singleton()->shader_free(proxy_shader);
		}
		if (proxy_mesh.is_valid()) {
			RendererRD::MeshStorage::get_singleton()->mesh_free(proxy_mesh);
		}
		RendererRD::TextureStorage *ts = RendererRD::TextureStorage::get_singleton();
		if (frame_texture_ref.is_valid()) {
			ts->texture_free(frame_texture_ref);
		}
		if (depth_texture_ref.is_valid()) {
			ts->texture_free(depth_texture_ref);
		}
		if (depth_texture.is_valid()) {
			RD::get_singleton()->free_rid(depth_texture);
		}
		if (frame_texture.is_valid()) {
			RD::get_singleton()->free_rid(frame_texture);
		}
		if (gpu) {
			memdelete(gpu);
		}
	}
};

#endif // RD_ENABLED
