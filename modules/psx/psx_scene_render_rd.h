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
// frame) instead of clearing. There is no depth buffer under them, so they
// draw over the whole scene.
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

#include "core/config/project_settings.h"
#include "core/os/memory.h"
#include "core/templates/local_vector.h"
#include "core/templates/paged_array.h"
#include "servers/rendering/renderer_rd/effects/copy_effects.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/rendering_device.h"

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
		bridge.settings.world_scale = MAX(1.0f, (float)GLOBAL_GET("rendering/psx/world_scale"));
		bridge.settings.default_ambient = GLOBAL_GET("rendering/psx/default_ambient");
		bridge.settings.subdivide_size = MAX(0.0f, (float)GLOBAL_GET("rendering/psx/subdivide_size"));
		bridge.settings.lighting = (int)GLOBAL_GET("rendering/psx/lighting_mode") == 1 ? psx::LIGHTING_LINEAR : psx::LIGHTING_PS1;
		bridge.settings.texture.max_size = CLAMP((int)GLOBAL_GET("rendering/psx/textures/max_size"), 8, 256);
		bridge.settings.texture.depth = (uint8_t)CLAMP((int)GLOBAL_GET("rendering/psx/textures/depth"), 0, 3);
		bridge.settings.texture.max_colors = CLAMP((int)GLOBAL_GET("rendering/psx/textures/max_colors"), 0, 256);
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
		bridge.build(this, p_camera, p_instances, p_lights, p_environment, w, h, *gpu, pb, ot, frame_count, scene_stats);

		// Far end: drawing environment, then the clear (the bucket is LIFO,
		// so the environment, added last, goes first).
		const Color bg = bridge.background(this, p_environment, p_clear);
		const uint32_t far_end = PSXSceneBridge<ShaderDataT>::OT_SIZE - 1;
		ot.add(far_end, pb.fill(bx, by, w, h, (uint8_t)bg.get_r8(), (uint8_t)bg.get_g8(), (uint8_t)bg.get_b8()));
		ot.add(far_end, pb.environment(bx, by, bx + w - 1, by + h - 1, bx, by, psx::GP0Encoder::texpage(0, 0, psx::TEXTURE_4BIT, psx::BLEND_HALF, true)));
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
	void _draw_editor_helpers(const Ref<RenderSceneBuffers> &p_render_buffers, const RendererSceneRender::CameraData *p_camera_data, const RendererSceneRender::CameraData *p_prev_camera_data, const PagedArray<RenderGeometryInstance *> &p_instances, RID p_shadow_atlas, RID p_occluder_debug_tex, RID p_reflection_atlas, float p_screen_mesh_lod_threshold, float p_window_output_max_value, RenderingServerTypes::RenderInfo *r_render_info) {
		helpers.set_page_pool(&helper_pool);
		helpers.reset();
		for (uint32_t i = 0; i < p_instances.size(); i++) {
			const RenderGeometryInstanceBase *inst = static_cast<const RenderGeometryInstanceBase *>(p_instances[i]);
			if ((inst->layer_mask & PSXSceneBridge<ShaderDataT>::USER_LAYERS) == 0) {
				helpers.push_back(p_instances[i]);
			}
		}
		if (helpers.size() == 0) {
			return;
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
		const RID framebuffer = texture_storage->render_target_get_rd_framebuffer(render_target);
		ERR_FAIL_COND(!framebuffer.is_valid());
		const Size2i target_size = texture_storage->render_target_get_size(render_target);

		const Color clear = texture_storage->render_target_is_clear_requested(render_target)
				? texture_storage->render_target_get_clear_request_color(render_target)
				: (Color)GLOBAL_GET("rendering/environment/defaults/default_clear_color");
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
		debug.report_scene(frame_count, scene_stats.instances, scene_stats.surfaces, scene_stats.conversions, ds.triangles_in, ds.culled_near, ds.culled_backface, ds.culled_offscreen, ds.culled_far);
		debug.report_textures(frame_count, ds.textured, ds.windowed, scene_stats.textures_resident, scene_stats.texture_uploads, scene_stats.texture_fallbacks, scene_stats.texture_banks, scene_stats.vram_free_percent);
		frame_count++;

		// Nearest-filtered upscale of the PSX frame to the whole render target.
		this->copy_effects->copy_to_fb_rect(frame_texture, framebuffer, Rect2i(Point2i(), target_size),
				false, false, false, false, RID(), false, false, false, false, Rect2(), 1.0, false);

		_draw_editor_helpers(p_render_buffers, p_camera_data, p_prev_camera_data, p_instances, p_shadow_atlas, p_occluder_debug_tex, p_reflection_atlas, p_screen_mesh_lod_threshold, p_window_output_max_value, r_render_info);

		// Tell the viewport the render target has been drawn; otherwise
		// RendererViewport clears it after 3D, wiping our frame.
		texture_storage->render_target_disable_clear_request(render_target);
	}

	PSXSceneRenderRD() {
		debug.load();
		_setup();
	}

	~PSXSceneRenderRD() {
		helpers.reset();
		if (overlay_environment.is_valid()) {
			this->environment_free(overlay_environment);
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
