// PSX options for 3D scene imports (glTF, .blend, FBX ...): bakes each
// material's albedo texture into a PSXTexture (psx_texture_resource.h) as
// the scene is imported, so the PSX renderer does not convert it when drawn.
// BaseMaterial3D bakes its albedo texture; a ShaderMaterial (made by a glTF
// document extension, say) bakes the sampler the renderer draws with:
// texture_albedo, or the first sampler2D marked source_color. Materials
// swapped in afterwards (use_external, import scripts) are not baked: import
// their image files as PSX Texture instead.
//
// Import dock (whole scene): psx/bake_textures turns it on; psx/max_size,
// psx/depth, psx/max_colors, psx/dither, psx/palette and psx/ps1_preview
// work as in the PSX Texture importer. Advanced Import Settings (per
// material): psx/bake (scene setting, always, never) and overrides of
// size, depth, colours, dither and palette.
//
// Embedded and extracted textures are both baked, as a copy saved in the
// imported scene. An image file already imported as PSX Texture is left
// linked and keeps its own settings.

#pragma once

#ifdef TOOLS_ENABLED

#include "editor/import/3d/resource_importer_scene.h"

class PSXSceneImportPlugin : public EditorScenePostImportPlugin {
	GDCLASS(PSXSceneImportPlugin, EditorScenePostImportPlugin);

	// The scene's options, kept from pre_process for the per-material pass
	// (internal_process only gets the material's own options).
	HashMap<StringName, Variant> scene_options;
	bool active = false;

public:
	virtual void get_import_options(const String &p_path, List<ResourceImporter::ImportOption> *r_options) override;
	virtual void get_internal_import_options(InternalImportCategory p_category, List<ResourceImporter::ImportOption> *r_options) override;
	virtual void pre_process(Node *p_scene, const HashMap<StringName, Variant> &p_options) override;
	virtual void internal_process(InternalImportCategory p_category, Node *p_base_scene, Node *p_node, Ref<Resource> p_resource, const Dictionary &p_options) override;
	virtual void post_process(Node *p_scene, const HashMap<StringName, Variant> &p_options) override;
};

#endif // TOOLS_ENABLED
