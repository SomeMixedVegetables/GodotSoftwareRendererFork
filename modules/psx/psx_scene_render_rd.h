// PSX scene renderer for the RenderingDevice backends (Forward+ and Mobile).
//
// It subclasses Godot's own scene renderer instead of wrapping it. The base
// class keeps environment and compositor data as private members, and owns
// the geometry instances RendererSceneCull creates, so a wrapper could not
// hand a frame back to it. As a subclass, every call except render_scene()
// is inherited unchanged, and render_scene() can fall back to Base.
//
// Spike: draws one CPU-rasterized triangle at 320x240 into every 3D viewport.

#pragma once

#ifdef RD_ENABLED

#include "psx_core/psx_raster.h"
#include "psx_debug.h"

#include "core/math/math_funcs.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/effects/copy_effects.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/rendering_device.h"

template <class Base>
class PSXSceneRenderRD : public Base {
	static constexpr int32_t FRAME_WIDTH = 320;
	static constexpr int32_t FRAME_HEIGHT = 240;

	LocalVector<uint32_t> frame_pixels;
	RID frame_texture;
	uint64_t frame_count = 0;
	PSXDebug debug;

	void _ensure_frame_texture() {
		if (frame_texture.is_valid()) {
			return;
		}
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
		tf.width = FRAME_WIDTH;
		tf.height = FRAME_HEIGHT;
		tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
		frame_texture = RD::get_singleton()->texture_create(tf, RD::TextureView());
		RD::get_singleton()->set_resource_name(frame_texture, "PSX frame");
	}

	void _rasterize_test_frame() {
		frame_pixels.resize(FRAME_WIDTH * FRAME_HEIGHT);
		psx::Framebuffer fb;
		fb.width = FRAME_WIDTH;
		fb.height = FRAME_HEIGHT;
		fb.pixels = frame_pixels.ptr();

		psx::clear(fb, psx::pack_rgba(16, 16, 48));

		// Vertex setup uses floats only to spin the test triangle; the
		// rasterizer itself gets integer screen coordinates, as on PS1.
		const float angle = (float)frame_count * 0.02f;
		const float cx = FRAME_WIDTH * 0.5f;
		const float cy = FRAME_HEIGHT * 0.5f;
		const float radius = 100.0f;
		psx::Vertex v[3];
		const uint8_t colors[3][3] = { { 255, 40, 40 }, { 40, 255, 40 }, { 40, 40, 255 } };
		for (int i = 0; i < 3; i++) {
			const float a = angle + (float)i * (float)Math::TAU / 3.0f;
			v[i].x = (int32_t)Math::round(cx + Math::cos(a) * radius);
			v[i].y = (int32_t)Math::round(cy + Math::sin(a) * radius);
			v[i].r = colors[i][0];
			v[i].g = colors[i][1];
			v[i].b = colors[i][2];
		}
		psx::draw_triangle_gouraud(fb, v[0], v[1], v[2]);
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

		const uint64_t raster_begin = OS::get_singleton()->get_ticks_usec();
		_rasterize_test_frame();
		const uint64_t raster_usec = OS::get_singleton()->get_ticks_usec() - raster_begin;

		if (debug.wants_dump(frame_count)) {
			debug.dump(frame_count, frame_pixels.ptr(), FRAME_WIDTH, FRAME_HEIGHT);
		}

		const uint64_t upload_begin = OS::get_singleton()->get_ticks_usec();
		_ensure_frame_texture();
		Vector<uint8_t> bytes;
		bytes.resize(frame_pixels.size() * sizeof(uint32_t));
		memcpy(bytes.ptrw(), frame_pixels.ptr(), bytes.size());
		RD::get_singleton()->texture_update(frame_texture, 0, bytes);
		const uint64_t upload_usec = OS::get_singleton()->get_ticks_usec() - upload_begin;

		debug.report(frame_count, raster_usec, upload_usec, (int)p_instances.size(), 1);
		frame_count++;

		// Nearest-filtered upscale of the 320x240 frame to the whole render target.
		this->copy_effects->copy_to_fb_rect(frame_texture, framebuffer, Rect2i(Point2i(), target_size),
				false, false, false, false, RID(), false, false, false, false, Rect2(), 1.0, false);

		// Tell the viewport the render target has been drawn; otherwise
		// RendererViewport clears it after 3D, wiping our frame.
		texture_storage->render_target_disable_clear_request(render_target);
	}

	PSXSceneRenderRD() {
		debug.load();
	}

	~PSXSceneRenderRD() {
		if (frame_texture.is_valid()) {
			RD::get_singleton()->free_rid(frame_texture);
		}
	}
};

#endif // RD_ENABLED
