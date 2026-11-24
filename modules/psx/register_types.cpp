#include "register_types.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "servers/rendering/renderer_compositor.h"

#ifdef RD_ENABLED
#include "psx_compositor_rd.h"
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

void initialize_psx_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SERVERS) {
		return;
	}
	GLOBAL_DEF_RST_BASIC("rendering/psx/enabled", false);
	RendererCompositor::create_override = psx_create_compositor;
}

void uninitialize_psx_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SERVERS) {
		return;
	}
	if (RendererCompositor::create_override == psx_create_compositor) {
		RendererCompositor::create_override = nullptr;
	}
}
