#ifdef TOOLS_ENABLED

#include "psx_scene_import_plugin.h"

#include "psx_texture_resource.h"

#include "core/io/resource_loader.h"
#include "scene/resources/material.h"

void PSXSceneImportPlugin::get_import_options(const String &p_path, List<ResourceImporter::ImportOption> *r_options) {
	using IO = ResourceImporter::ImportOption;
	// Same values as the PSX Texture importer: 0 / -1 = project setting.
	r_options->push_back(IO(PropertyInfo(Variant::BOOL, "psx/bake_textures"), false));
	r_options->push_back(IO(PropertyInfo(Variant::INT, "psx/max_size", PROPERTY_HINT_ENUM, "Project Setting:0,8:8,16:16,32:32,64:64,128:128,256:256"), 0));
	r_options->push_back(IO(PropertyInfo(Variant::INT, "psx/depth", PROPERTY_HINT_ENUM, "Project Setting:-1,Auto:0,4-bit:1,8-bit:2,15-bit:3"), -1));
	r_options->push_back(IO(PropertyInfo(Variant::INT, "psx/max_colors", PROPERTY_HINT_RANGE, "-1,256,1"), -1));
	r_options->push_back(IO(PropertyInfo(Variant::BOOL, "psx/ps1_preview"), true));
	r_options->push_back(IO(PropertyInfo(Variant::INT, "psx/dither", PROPERTY_HINT_ENUM, "Project Setting:-1,Off:0,Ordered:1"), -1));
	// A PSXPalette every baked texture is made with (shared CLUT, palette
	// swaps); replaces depth and max_colors.
	r_options->push_back(IO(PropertyInfo(Variant::STRING, "psx/palette", PROPERTY_HINT_FILE, "*.tres,*.res"), ""));
}

void PSXSceneImportPlugin::get_internal_import_options(InternalImportCategory p_category, List<ResourceImporter::ImportOption> *r_options) {
	if (p_category != INTERNAL_IMPORT_CATEGORY_MATERIAL) {
		return;
	}
	using IO = ResourceImporter::ImportOption;
	r_options->push_back(IO(PropertyInfo(Variant::INT, "psx/bake", PROPERTY_HINT_ENUM, "Scene Setting,Always,Never"), 0));
	// "Scene Setting" uses the Import dock's psx/* value for this scene.
	r_options->push_back(IO(PropertyInfo(Variant::INT, "psx/max_size", PROPERTY_HINT_ENUM, "Scene Setting:-1,Project Setting:0,8:8,16:16,32:32,64:64,128:128,256:256"), -1));
	r_options->push_back(IO(PropertyInfo(Variant::INT, "psx/depth", PROPERTY_HINT_ENUM, "Scene Setting:-2,Project Setting:-1,Auto:0,4-bit:1,8-bit:2,15-bit:3"), -2));
	r_options->push_back(IO(PropertyInfo(Variant::INT, "psx/max_colors", PROPERTY_HINT_RANGE, "-2,256,1"), -2));
	r_options->push_back(IO(PropertyInfo(Variant::INT, "psx/dither", PROPERTY_HINT_ENUM, "Scene Setting:-2,Project Setting:-1,Off:0,Ordered:1"), -2));
	// Empty: the scene's psx/palette.
	r_options->push_back(IO(PropertyInfo(Variant::STRING, "psx/palette", PROPERTY_HINT_FILE, "*.tres,*.res"), ""));
}

void PSXSceneImportPlugin::pre_process(Node *p_scene, const HashMap<StringName, Variant> &p_options) {
	scene_options = p_options;
	active = true;
}

void PSXSceneImportPlugin::post_process(Node *p_scene, const HashMap<StringName, Variant> &p_options) {
	scene_options.clear();
	active = false;
}

void PSXSceneImportPlugin::internal_process(InternalImportCategory p_category, Node *p_base_scene, Node *p_node, Ref<Resource> p_resource, const Dictionary &p_options) {
	if (p_category != INTERNAL_IMPORT_CATEGORY_MATERIAL || !active) {
		return;
	}
	const Ref<BaseMaterial3D> mat = p_resource;
	if (mat.is_null()) {
		return; // ShaderMaterial and others: not baked.
	}
	auto scene = [this](const char *p_name, const Variant &p_default) -> Variant {
		const Variant *v = scene_options.getptr(StringName(p_name));
		return v ? *v : p_default;
	};
	auto own = [&p_options](const char *p_name, const Variant &p_default) -> Variant {
		return p_options.has(p_name) ? p_options[p_name] : p_default;
	};
	const int bake = own("psx/bake", 0);
	if (bake == 2 || (bake == 0 && !(bool)scene("psx/bake_textures", false))) {
		return;
	}
	const Ref<Texture2D> tex = mat->get_texture(BaseMaterial3D::TEXTURE_ALBEDO);
	if (tex.is_null() || Object::cast_to<PSXTexture>(tex.ptr())) {
		// None, already baked (a material shared by several surfaces), or an
		// image file imported as PSX Texture, which keeps its own settings.
		return;
	}
	// Embedded or extracted images alike: the baked copy lives in the
	// imported scene (an extracted image edited later needs the model
	// reimported to update it).
	const Ref<Image> image = tex->get_image();
	if (image.is_null() || image->is_empty()) {
		return;
	}
	const int size_own = own("psx/max_size", -1);
	const int depth_own = own("psx/depth", -2);
	const int colors_own = own("psx/max_colors", -2);
	const int max_size = size_own >= 0 ? size_own : (int)scene("psx/max_size", 0);
	const int depth = depth_own >= -1 ? depth_own : (int)scene("psx/depth", -1);
	const int max_colors = colors_own >= -1 ? colors_own : (int)scene("psx/max_colors", -1);
	const int dither_own = own("psx/dither", -2);
	const int dither = dither_own >= -1 ? dither_own : (int)scene("psx/dither", -1);
	// Cut-outs are baked at the material's own threshold, so they always match.
	const float cut = mat->get_transparency() == BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR ? mat->get_alpha_scissor_threshold() : 0.5f;
	String palette_path = own("psx/palette", String());
	if (palette_path.is_empty()) {
		palette_path = scene("psx/palette", String());
	}
	Ref<PSXPalette> palette;
	if (!palette_path.is_empty()) {
		palette = ResourceLoader::load(palette_path, "PSXPalette");
		if (palette.is_null()) {
			WARN_PRINT(vformat("PSX: the palette '%s' cannot be loaded as a PSXPalette; material '%s' is baked with its own palette.", palette_path, mat->get_name()));
		}
	}
	const Ref<PSXTexture> baked = PSXTexture::bake(image, max_size, depth, max_colors, cut, scene("psx/ps1_preview", true), palette, dither);
	if (baked.is_null()) {
		WARN_PRINT(vformat("PSX: could not bake the albedo texture of material '%s'; it is converted when drawn instead.", mat->get_name()));
		return;
	}
	baked->set_name(tex->get_name());
	mat->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, baked);
}

#endif // TOOLS_ENABLED
