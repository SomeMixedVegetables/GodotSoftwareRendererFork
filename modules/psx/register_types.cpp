#include "register_types.h"

#include "psx_debug.h"
#include "psx_settings.h"
#include "psx_settings_node.h"
#include "psx_texture_resource.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/resource_importer.h"
#include "core/object/class_db.h"
#include "servers/rendering/renderer_compositor.h"

#ifdef RD_ENABLED
#include "psx_compositor_rd.h"
#endif

#ifdef TOOLS_ENABLED
#include "psx_editor_plugin.h"
#include "psx_scene_import_plugin.h"
#include "resource_importer_psx_texture.h"

#include "editor/editor_node.h"
#include "editor/plugins/editor_plugin.h"

static void psx_editor_init() {
	Ref<ResourceImporterPSXTexture> importer;
	importer.instantiate();
	ResourceFormatImporter::get_singleton()->add_importer(importer);

	Ref<PSXSceneImportPlugin> scene_plugin;
	scene_plugin.instantiate();
	ResourceImporterScene::add_post_importer_plugin(scene_plugin);
}
#endif

static RendererCompositor *psx_create_compositor(RendererCompositor *(*p_default)()) {
	const bool enabled = GLOBAL_GET("rendering/psx/enabled");
	if (!enabled || Engine::get_singleton()->is_project_manager_hint()) {
		return p_default();
	}
#ifdef RD_ENABLED
	if (p_default == &RendererCompositorRD::_create_current) {
		return PSXCompositorRD::_create_current();
	}
#endif
	// Compatibility (GLES3) and dummy backends are not supported yet.
	WARN_PRINT("PSX: rendering/psx/enabled is on, but this backend is not supported yet. Using the normal renderer.");
	return p_default();
}

static PSX *psx_singleton = nullptr;

void initialize_psx_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_CLASS(PSXTexture);
		GDREGISTER_CLASS(PSXSettings);
		GDREGISTER_CLASS(PSXMaterial3D);
		GDREGISTER_ABSTRACT_CLASS(PSX);
		psx_singleton = memnew(PSX);
		Engine::get_singleton()->add_singleton(Engine::Singleton("PSX", PSX::get_singleton()));
	}
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		GDREGISTER_CLASS(ResourceImporterPSXTexture);
		GDREGISTER_INTERNAL_CLASS(PSXSceneImportPlugin);
		EditorNode::add_init_callback(psx_editor_init);
		GDREGISTER_INTERNAL_CLASS(PSXProjectSettingsProxy);
		GDREGISTER_INTERNAL_CLASS(PSXDock);
		GDREGISTER_INTERNAL_CLASS(PSXMaterial3DConversionPlugin);
		GDREGISTER_INTERNAL_CLASS(PSXEditorPlugin);
		EditorPlugins::add_by_type<PSXEditorPlugin>();
	}
#endif
	if (p_level != MODULE_INITIALIZATION_LEVEL_SERVERS) {
		return;
	}
	GLOBAL_DEF_RST_BASIC("rendering/psx/enabled", false);
	PSXDebug::define_settings();
	RendererCompositor::create_override = psx_create_compositor;
}

void uninitialize_psx_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE && psx_singleton) {
		Engine::get_singleton()->remove_singleton("PSX");
		memdelete(psx_singleton);
		psx_singleton = nullptr;
	}
	if (p_level != MODULE_INITIALIZATION_LEVEL_SERVERS) {
		return;
	}
	if (RendererCompositor::create_override == psx_create_compositor) {
		RendererCompositor::create_override = nullptr;
	}
}
