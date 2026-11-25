// Debug and test hooks for the PSX renderer, driven by project settings so the
// test runner (modules/psx/testing/psx_test.ps1) can turn them on through an
// override.cfg without rebuilding.
//
//   rendering/psx/debug/dump_dir      Absolute folder. When set, the raw PSX
//                                     framebuffer (native resolution, before
//                                     upscale) is saved as psx_NNNN.png.
//   rendering/psx/debug/dump_frames   How many frames to dump, from frame 0.
//   rendering/psx/debug/print_stats   Print one PSX_STATS line per frame.
//
// Output lines start with "PSX_" so the runner can grep them.

#pragma once

#include "core/config/project_settings.h"
#include "core/io/image.h"
#include "core/os/os.h"
#include "core/string/print_string.h"

struct PSXDebug {
	String dump_dir;
	int dump_frames = 0;
	bool print_stats = false;

	static void define_settings() {
		// Live: active switches between the PS1 picture and Godot's own 3D
		// without a restart (rendering/psx/enabled installs the renderer);
		// dither is the GPU's ordered dither. Both can be overridden per scene
		// (PSXSettings) and by scripts (PSX.set_setting); see psx_settings.h.
		GLOBAL_DEF_BASIC("rendering/psx/active", true);
		GLOBAL_DEF_BASIC("rendering/psx/dither", true);
		// Display and VRAM (restart needed). With vram_scale N, VRAM is
		// (1024 N) x (512 N) and coordinate limits grow by N; 1 is the hardware.
		GLOBAL_DEF_RST(PropertyInfo(Variant::INT, "rendering/psx/vram_scale", PROPERTY_HINT_RANGE, "1,4,1"), 1);
		GLOBAL_DEF_RST(PropertyInfo(Variant::INT, "rendering/psx/resolution_width", PROPERTY_HINT_RANGE, "64,4096,1"), 320);
		GLOBAL_DEF_RST(PropertyInfo(Variant::INT, "rendering/psx/resolution_height", PROPERTY_HINT_RANGE, "64,2048,1"), 240);
		// The settings below change without a restart (the next frame
		// converts what it draws again when a rebuild setting changes).
		// Scene conversion. world_scale: GTE units per metre. 16-bit screen Z
		// makes the far limit 65535 / world_scale metres, and the near limit
		// is H / 2 units (about 0.15 m at 512 and 320x240).
		GLOBAL_DEF(PropertyInfo(Variant::FLOAT, "rendering/psx/world_scale", PROPERTY_HINT_RANGE, "16,4096,1"), 512.0);
		// Lighting maths. PS1: the GTE's NCCS, display colour times light
		// (darker midtones, as on the hardware). Godot match: the light level
		// applied to linear colour, so lit surfaces match Godot's brightness.
		GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/psx/lighting_mode", PROPERTY_HINT_ENUM, "PS1,Godot Match"), 0);
		// Meshes are subdivided until no edge is longer than this (metres; 0 =
		// off), so big polygons sort, near-clip and light like small ones.
		GLOBAL_DEF(PropertyInfo(Variant::FLOAT, "rendering/psx/subdivide_size", PROPERTY_HINT_RANGE, "0,16,0.05,or_greater"), 1.0);
		// Textures, converted once when first drawn (design doc, section 11).
		// max_size: largest side in texels (bigger ones are scaled down).
		// depth: Auto picks 4-bit when 16 colours are enough, else 8-bit with
		// a median-cut palette. max_colors: palette size limit, 0 = 16 or 256.
		GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/psx/textures/max_size", PROPERTY_HINT_ENUM, "8:8,16:16,32:32,64:64,128:128,256:256"), 256);
		GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/psx/textures/depth", PROPERTY_HINT_ENUM, "Auto,4-bit,8-bit,15-bit"), 0);
		GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/psx/textures/max_colors", PROPERTY_HINT_RANGE, "0,256,1"), 0);
		// Ordered dither where a texture loses colours (its palette, or the
		// 8- to 5-bit step of 15-bit textures).
		GLOBAL_DEF("rendering/psx/textures/dither", false);
		// Texture memory in 1024 x 512 banks. 1 is the PS1: textures share
		// VRAM's texture region and the least recently used are evicted when
		// it is full. More banks (a GP0 extension) hold textures beyond it.
		GLOBAL_DEF_RST(PropertyInfo(Variant::INT, "rendering/psx/textures/vram_banks", PROPERTY_HINT_ENUM, "1 (PS1):1,2:2,4:4,8:8,16:16,Unlimited:0"), 0);
		// Ambient light when the scene has no WorldEnvironment.
		GLOBAL_DEF("rendering/psx/default_ambient", Color(0.2, 0.2, 0.2));
		// Draw the built-in test cube instead of the scene.
		GLOBAL_DEF_RST("rendering/psx/debug/demo_scene", false);
		GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/psx/debug/demo_speed_percent", PROPERTY_HINT_RANGE, "0,400,1"), 100);
		GLOBAL_DEF("rendering/psx/debug/dump_dir", "");
		GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/psx/debug/dump_frames", PROPERTY_HINT_RANGE, "0,10000,1"), 0);
		GLOBAL_DEF("rendering/psx/debug/print_stats", false);
	}

	void load() {
		dump_dir = GLOBAL_GET("rendering/psx/debug/dump_dir");
		dump_frames = GLOBAL_GET("rendering/psx/debug/dump_frames");
		print_stats = GLOBAL_GET("rendering/psx/debug/print_stats");
		if (!dump_dir.is_empty()) {
			print_line(vformat("PSX_DEBUG dump_dir=\"%s\" dump_frames=%d", dump_dir, dump_frames));
		}
	}

	bool wants_dump(uint64_t p_frame) const {
		return !dump_dir.is_empty() && p_frame < (uint64_t)dump_frames;
	}

	// p_pixels: RGBA8, p_width * p_height entries.
	void dump(uint64_t p_frame, const uint32_t *p_pixels, int p_width, int p_height) const {
		Vector<uint8_t> bytes;
		bytes.resize(p_width * p_height * 4);
		memcpy(bytes.ptrw(), p_pixels, bytes.size());
		Ref<Image> image = Image::create_from_data(p_width, p_height, false, Image::FORMAT_RGBA8, bytes);
		const String path = dump_dir.path_join(vformat("psx_%04d.png", (int64_t)p_frame));
		const Error err = image->save_png(path);
		if (err != OK) {
			ERR_PRINT("PSX: could not save debug frame to " + path);
		}
	}

	void report_scene(uint64_t p_frame, uint32_t p_instances, uint32_t p_surfaces, uint32_t p_conversions, uint32_t p_triangles, uint32_t p_near, uint32_t p_backface, uint32_t p_offscreen, uint32_t p_far, uint32_t p_lines, uint32_t p_points) const {
		if (print_stats) {
			print_line(vformat("PSX_SCENE frame=%d instances=%d surfaces=%d conversions=%d tris_in=%d near=%d backface=%d offscreen=%d far=%d lines=%d points=%d",
					(int64_t)p_frame, p_instances, p_surfaces, p_conversions, p_triangles, p_near, p_backface, p_offscreen, p_far, p_lines, p_points));
		}
	}

	void report_textures(uint64_t p_frame, uint32_t p_textured, uint32_t p_windowed, uint32_t p_resident, uint32_t p_baked, uint32_t p_uploads, uint32_t p_fallbacks, uint32_t p_banks, uint32_t p_vram_free_percent, uint32_t p_cluts, uint32_t p_shared) const {
		if (print_stats) {
			print_line(vformat("PSX_TEXTURES frame=%d textured_tris=%d windowed_tris=%d resident=%d baked=%d uploads=%d fallbacks=%d banks=%d vram_free=%d%% cluts=%d shared=%d",
					(int64_t)p_frame, p_textured, p_windowed, p_resident, p_baked, p_uploads, p_fallbacks, p_banks, p_vram_free_percent, p_cluts, p_shared));
		}
	}

	// Time spent converting Godot data this frame (PSXSceneStats).
	void report_convert(uint64_t p_frame, uint64_t p_mesh_read, uint32_t p_meshes_read, uint64_t p_mesh_convert, uint32_t p_meshes_converted, uint64_t p_skin, uint32_t p_skinned, uint64_t p_tex_read, uint32_t p_tex_reads, uint64_t p_tex_convert, uint32_t p_tex_converted, uint64_t p_particles_read, uint32_t p_particles) const {
		if (print_stats) {
			print_line(vformat("PSX_CONVERT frame=%d mesh_read_us=%d meshes_read=%d mesh_convert_us=%d meshes_converted=%d skin_us=%d skinned=%d texture_read_us=%d textures_read=%d texture_convert_us=%d textures_converted=%d particles_read_us=%d particles=%d",
					(int64_t)p_frame, (int64_t)p_mesh_read, p_meshes_read, (int64_t)p_mesh_convert, p_meshes_converted, (int64_t)p_skin, p_skinned, (int64_t)p_tex_read, p_tex_reads, (int64_t)p_tex_convert, p_tex_converted, (int64_t)p_particles_read, p_particles));
		}
	}

	// raster = build + gpu + scanout (and a little bookkeeping).
	void report(uint64_t p_frame, uint64_t p_raster_usec, uint64_t p_build_usec, uint64_t p_gpu_usec, uint64_t p_scanout_usec, uint64_t p_upload_usec, int p_instances, int p_triangles) const {
		if (print_stats) {
			print_line(vformat("PSX_STATS frame=%d raster_us=%d build_us=%d gpu_us=%d scanout_us=%d upload_us=%d instances=%d triangles=%d",
					(int64_t)p_frame, (int64_t)p_raster_usec, (int64_t)p_build_usec, (int64_t)p_gpu_usec, (int64_t)p_scanout_usec, (int64_t)p_upload_usec, p_instances, p_triangles));
		}
	}
};
