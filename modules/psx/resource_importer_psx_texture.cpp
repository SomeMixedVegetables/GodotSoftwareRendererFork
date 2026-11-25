#ifdef TOOLS_ENABLED

#include "resource_importer_psx_texture.h"

#include "psx_texture_resource.h"

#include "core/io/image_loader.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"

void ResourceImporterPSXTexture::get_recognized_extensions(List<String> *p_extensions) const {
	ImageLoader::get_recognized_extensions(p_extensions);
}

void ResourceImporterPSXTexture::get_import_options(const String &p_path, List<ImportOption> *r_options, int p_preset) const {
	// "Project Setting" uses rendering/psx/textures/*, as runtime conversion does.
	r_options->push_back(ImportOption(PropertyInfo(Variant::INT, "psx/max_size", PROPERTY_HINT_ENUM, "Project Setting:0,8:8,16:16,32:32,64:64,128:128,256:256"), 0));
	r_options->push_back(ImportOption(PropertyInfo(Variant::INT, "psx/depth", PROPERTY_HINT_ENUM, "Project Setting:-1,Auto:0,4-bit:1,8-bit:2,15-bit:3"), -1));
	// -1 = project setting; 0 = as many as the depth allows.
	r_options->push_back(ImportOption(PropertyInfo(Variant::INT, "psx/max_colors", PROPERTY_HINT_RANGE, "-1,256,1"), -1));
	// Ordered dither where colours are lost (palette, or 15-bit).
	r_options->push_back(ImportOption(PropertyInfo(Variant::INT, "psx/dither", PROPERTY_HINT_ENUM, "Project Setting:-1,Off:0,Ordered:1"), -1));
	// A PSXPalette to bake with (shared CLUT, palette swaps); replaces
	// depth and max_colors. Reimport after changing the palette.
	r_options->push_back(ImportOption(PropertyInfo(Variant::STRING, "psx/palette", PROPERTY_HINT_FILE, "*.tres,*.res"), ""));
	// Materials with alpha scissor at this threshold use the baked cut-out;
	// other thresholds convert when drawn.
	r_options->push_back(ImportOption(PropertyInfo(Variant::FLOAT, "alpha/cut_threshold", PROPERTY_HINT_RANGE, "0,1,0.001"), 0.5));
	// Godot (the editor, other renderers) shows the PS1 result instead of the source image.
	r_options->push_back(ImportOption(PropertyInfo(Variant::BOOL, "preview/ps1_look"), true));
}

Error ResourceImporterPSXTexture::import(ResourceUID::ID p_source_id, const String &p_source_file, const String &p_save_path, const HashMap<StringName, Variant> &p_options, List<String> *r_platform_variants, List<String> *r_gen_files, Variant *r_metadata) {
	Ref<Image> image;
	image.instantiate();
	const Error err = ImageLoader::load_image(p_source_file, image);
	ERR_FAIL_COND_V_MSG(err != OK || image->is_empty(), err != OK ? err : ERR_FILE_CORRUPT, vformat("PSX: cannot load image '%s'.", p_source_file));

	Ref<PSXPalette> palette;
	const String palette_path = p_options.has("psx/palette") ? String(p_options["psx/palette"]) : String();
	if (!palette_path.is_empty()) {
		palette = ResourceLoader::load(palette_path, "PSXPalette");
		ERR_FAIL_COND_V_MSG(palette.is_null(), ERR_FILE_MISSING_DEPENDENCIES, vformat("PSX: '%s' needs the palette '%s', which cannot be loaded as a PSXPalette.", p_source_file, palette_path));
	}
	const Ref<PSXTexture> tex = PSXTexture::bake(image, p_options["psx/max_size"], p_options["psx/depth"], p_options["psx/max_colors"], p_options["alpha/cut_threshold"], p_options["preview/ps1_look"], palette, p_options.has("psx/dither") ? (int)p_options["psx/dither"] : -1);
	ERR_FAIL_COND_V_MSG(tex.is_null(), ERR_CANT_CREATE, vformat("PSX: cannot convert image '%s'.", p_source_file));
	return ResourceSaver::save(tex, p_save_path + ".res");
}

#endif // TOOLS_ENABLED
