#include "psx_compositor_rd.h"

#ifdef RD_ENABLED

#include "psx_scene_render_rd.h"

#include "core/os/os.h"

#ifdef FORWARD_RD_ENABLED
#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#endif
#ifdef MOBILE_RD_ENABLED
#include "servers/rendering/renderer_rd/forward_mobile/render_forward_mobile.h"
#endif

// Builds the PSX subclass of whichever scene renderer Godot would have picked.
// Called from RendererCompositorRD's constructor through scene_create_override.
// Scene renderers cannot be swapped after construction: their destructors use
// RSG storage pointers that are only assigned once the compositor exists.
static RendererSceneRenderRD *psx_create_scene_rd(bool p_mobile) {
	if (p_mobile) {
#ifdef MOBILE_RD_ENABLED
		return memnew(PSXSceneRenderRD<RendererSceneRenderImplementation::RenderForwardMobile>());
#endif
	} else {
#ifdef FORWARD_RD_ENABLED
		return memnew(PSXSceneRenderRD<RendererSceneRenderImplementation::RenderForwardClustered>());
#endif
	}
	return nullptr;
}

RendererCompositor *PSXCompositorRD::_create_current() {
	scene_create_override = psx_create_scene_rd;
	RendererCompositor *compositor = memnew(PSXCompositorRD);
	scene_create_override = nullptr;
	return compositor;
}

PSXCompositorRD::PSXCompositorRD() {
	print_line("PSX: compositor active (RenderingDevice backend, " + OS::get_singleton()->get_current_rendering_method() + ").");
}

#endif // RD_ENABLED
