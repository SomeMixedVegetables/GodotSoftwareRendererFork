// PSX compositor for the RenderingDevice backends (Forward+ and Mobile).
//
// Subclasses RendererCompositorRD, so canvas, storage, blit and frame pacing
// all stay Godot's. Only the 3D scene renderer is swapped for the PSX one.

#pragma once

#ifdef RD_ENABLED

#include "servers/rendering/renderer_rd/renderer_compositor_rd.h"

class PSXCompositorRD : public RendererCompositorRD {
public:
	static RendererCompositor *_create_current();

	PSXCompositorRD();
};

#endif // RD_ENABLED
