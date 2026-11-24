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

	void report(uint64_t p_frame, uint64_t p_raster_usec, uint64_t p_upload_usec, int p_instances, int p_triangles) const {
		if (print_stats) {
			print_line(vformat("PSX_STATS frame=%d raster_us=%d upload_us=%d instances=%d triangles=%d",
					(int64_t)p_frame, (int64_t)p_raster_usec, (int64_t)p_upload_usec, p_instances, p_triangles));
		}
	}
};
