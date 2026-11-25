// PSX settings that can change while the game runs, and where overrides of
// them come from.
//
// Each frame, for each viewport, the renderer resolves the settings in this
// order (later wins):
//   1. Project settings (rendering/psx/*), edited in Project Settings or the
//      PSX dock.
//   2. The scene's PSXSettings node (psx_settings_node.h), found through the
//      World3D the viewport draws, so the editor's 3D views use it too.
//   3. Script overrides through the PSX singleton (PSX.set_setting), e.g. a
//      game's graphics menu.
// Materials can then override dithering and subdivision (PSXMaterial3D).
//
// Live settings cost nothing to change. Rebuild settings (world scale,
// subdivision, texture limits) make the next frame convert the meshes and
// textures it draws again: a one-frame hitch. Resolution, VRAM scale and
// texture banks are not here: they need a restart.

#pragma once

#include "psx_core/psx_render.h"
#include "psx_core/psx_texture.h"

#include "core/math/color.h"
#include "core/object/object.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/rid.h"
#include "core/variant/variant.h"

enum PSXSetting {
	PSX_SETTING_ACTIVE, // Live: false hands the frame to Godot's own renderer
	PSX_SETTING_DITHER, // Live: the GPU's 4x4 ordered dither
	PSX_SETTING_LIGHTING_MODE, // Live: PS1 or Godot Match
	PSX_SETTING_DEFAULT_AMBIENT, // Live
	PSX_SETTING_WORLD_SCALE, // Rebuild
	PSX_SETTING_SUBDIVIDE_SIZE, // Rebuild
	PSX_SETTING_TEXTURE_MAX_SIZE, // Rebuild
	PSX_SETTING_TEXTURE_DEPTH, // Rebuild
	PSX_SETTING_TEXTURE_MAX_COLORS, // Rebuild
	PSX_SETTING_TEXTURE_DITHER, // Rebuild
	PSX_SETTING_MAX
};

struct PSXSettingDef {
	const char *name; // Script and node name, e.g. "dither"
	const char *project_path; // rendering/psx/...
	Variant::Type type;
	PropertyHint hint;
	const char *hint_string;
	bool rebuild; // Changing it re-converts meshes or textures
	const char *description;
};

const PSXSettingDef &psx_setting_def(int p_setting);
int psx_setting_find(const StringName &p_name); // -1 when unknown
// Coerces p_value to the setting's type and range.
Variant psx_setting_validate(int p_setting, const Variant &p_value);
// p_value as the inspector would show it: "On", "Godot Match", "256".
String psx_setting_format(int p_setting, const Variant &p_value);

struct PSXSettingOverrides {
	Variant values[PSX_SETTING_MAX];
	uint32_t mask = 0; // Bit per PSXSetting that is overridden

	bool has(int p_setting) const { return (mask >> p_setting) & 1; }
	void set(int p_setting, const Variant &p_value) {
		values[p_setting] = psx_setting_validate(p_setting, p_value);
		mask |= 1u << p_setting;
	}
	void clear(int p_setting) {
		values[p_setting] = Variant();
		mask &= ~(1u << p_setting);
	}
};

// The resolved settings for one viewport's frame.
struct PSXLiveSettings {
	bool active = true;
	bool dither = true;
	psx::LightingMode lighting = psx::LIGHTING_PS1;
	Color default_ambient = Color(0.2, 0.2, 0.2);
	float world_scale = 512.0f;
	float subdivide_size = 1.0f;
	psx::TextureOptions texture;
};

// Per-material overrides (PSXMaterial3D).
struct PSXMaterialOverrides {
	enum Dither : uint8_t {
		DITHER_DEFAULT = 0,
		DITHER_ON = 1,
		DITHER_OFF = 2,
	};
	// The PS1 blend mode, replacing the material's own transparency.
	enum Blend : uint8_t {
		BLEND_MATERIAL = 0, // From Godot's blend mode and transparency
		BLEND_HALF = 1, // B/2 + F/2 on every triangle
		BLEND_ADD = 2, // B + F
		BLEND_SUBTRACT = 3, // B - F
		BLEND_ADD_QUARTER = 4, // B + F/4
	};
	uint8_t dither = DITHER_DEFAULT;
	bool no_subdivide = false;
	uint8_t blend = BLEND_MATERIAL;
	// The PSXPalette to draw the texture with (its object ID), 0 = none.
	uint64_t palette = 0;
	bool is_default() const { return dither == DITHER_DEFAULT && !no_subdivide && blend == BLEND_MATERIAL && palette == 0; }
};

// The last frame the PSX renderer drew, for the dock and PSX.get_stats().
struct PSXFrameStats {
	uint64_t frame = 0;
	bool active = true;
	uint32_t instances = 0;
	uint32_t triangles_in = 0;
	uint32_t triangles_drawn = 0;
	uint32_t conversions = 0;
	uint32_t textures_resident = 0;
	uint32_t textures_baked = 0;
	uint32_t texture_fallbacks = 0;
	uint32_t texture_banks = 1;
	uint32_t vram_free_percent = 100;
	uint64_t raster_usec = 0;
};

// Shared by the renderer (render thread) and nodes, materials and scripts
// (main thread). Every method locks.
class PSXSettingsRegistry {
public:
	// Scene overrides, one entry per PSXSettings node in the tree. The most
	// recently registered node for a scenario wins.
	static void set_scene(ObjectID p_node, RID p_scenario, const PSXSettingOverrides &p_overrides);
	static void remove_scene(ObjectID p_node);
	// The PSXSettings node that wins for p_scenario, and its overrides.
	// False when the scenario has none.
	static bool get_scene(RID p_scenario, ObjectID &r_node, PSXSettingOverrides &r_overrides);

	static void set_runtime(int p_setting, const Variant &p_value);
	static void clear_runtime(int p_setting);
	static void clear_runtime_all();
	static PSXSettingOverrides get_runtime();

	static void set_material(RID p_material, const PSXMaterialOverrides &p_overrides);
	static void remove_material(RID p_material);
	static PSXMaterialOverrides get_material(RID p_material);

	// Project settings, then p_scenario's scene overrides, then runtime ones.
	static PSXLiveSettings resolve(RID p_scenario);
	// The same, as Variants (the dock and PSX.get_setting).
	static Variant resolve_value(int p_setting, RID p_scenario = RID());

	// Set by the PSX renderer when it is created (rendering/psx/enabled and
	// a supported backend).
	static void set_installed(bool p_installed, const String &p_renderer, int p_width, int p_height, int p_vram_scale);
	static bool is_installed();
	static String get_installed_description();

	static void publish_stats(const PSXFrameStats &p_stats);
	static PSXFrameStats get_stats();
};

// The PSX singleton for scripts: runtime overrides and stats.
class PSX : public Object {
	GDCLASS(PSX, Object);
	static PSX *singleton;

protected:
	static void _bind_methods();

public:
	static PSX *get_singleton() { return singleton; }

	bool is_available() const;
	void set_active(bool p_active);
	bool is_active() const;
	void set_setting(const StringName &p_name, const Variant &p_value);
	Variant get_setting(const StringName &p_name) const;
	void clear_setting(const StringName &p_name);
	void clear_settings();
	PackedStringArray get_setting_names() const;
	Dictionary get_stats() const;

	PSX();
	~PSX();
};
