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
		// Display and VRAM (restart needed). With vram_scale N, VRAM is
		// (1024 N) x (512 N) and coordinate limits grow by N; 1 is the hardware.
		GLOBAL_DEF_RST(PropertyInfo(Variant::INT, "rendering/psx/vram_scale", PROPERTY_HINT_RANGE, "1,4,1"), 1);
		GLOBAL_DEF_RST(PropertyInfo(Variant::INT, "rendering/psx/resolution_width", PROPERTY_HINT_RANGE, "64,4096,1"), 320);
		GLOBAL_DEF_RST(PropertyInfo(Variant::INT, "rendering/psx/resolution_height", PROPERTY_HINT_RANGE, "64,2048,1"), 240);
		// Scene conversion. world_scale: GTE units per metre. 16-bit screen Z
		// makes the far limit 65535 / world_scale metres, and the near limit
		// is H / 2 units (about 0.15 m at 512 and 320x240).
		GLOBAL_DEF_RST(PropertyInfo(Variant::FLOAT, "rendering/psx/world_scale", PROPERTY_HINT_RANGE, "16,4096,1"), 512.0);
		// Lighting maths. PS1: the GTE's NCCS, display colour times light
		// (darker midtones, as on the hardware). Godot match: the light level
		// applied to linear colour, so lit surfaces match Godot's brightness.
		GLOBAL_DEF_RST(PropertyInfo(Variant::INT, "rendering/psx/lighting_mode", PROPERTY_HINT_ENUM, "PS1,Godot Match"), 0);
		// Meshes are subdivided until no edge is longer than this (metres; 0 =
		// off), so big polygons sort, near-clip and light like small ones.
		GLOBAL_DEF_RST(PropertyInfo(Variant::FLOAT, "rendering/psx/subdivide_size", PROPERTY_HINT_RANGE, "0,16,0.05,or_greater"), 1.0);
		// Ambient light when the scene has no WorldEnvironment.
		GLOBAL_DEF_RST("rendering/psx/default_ambient", Color(0.2, 0.2, 0.2));
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

	void report_scene(uint64_t p_frame, uint32_t p_instances, uint32_t p_surfaces, uint32_t p_conversions, uint32_t p_triangles, uint32_t p_near, uint32_t p_backface, uint32_t p_offscreen, uint32_t p_far) const {
		if (print_stats) {
			print_line(vformat("PSX_SCENE frame=%d instances=%d surfaces=%d conversions=%d tris_in=%d near=%d backface=%d offscreen=%d far=%d",
					(int64_t)p_frame, p_instances, p_surfaces, p_conversions, p_triangles, p_near, p_backface, p_offscreen, p_far));
		}
	}

	void report(uint64_t p_frame, uint64_t p_raster_usec, uint64_t p_upload_usec, int p_instances, int p_triangles) const {
		if (print_stats) {
			print_line(vformat("PSX_STATS frame=%d raster_us=%d upload_us=%d instances=%d triangles=%d",
					(int64_t)p_frame, (int64_t)p_raster_usec, (int64_t)p_upload_usec, p_instances, p_triangles));
		}
	}
};
