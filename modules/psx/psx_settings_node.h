// Per-scene and per-material PSX overrides (see psx_settings.h for the order
// they apply in).
//
// PSXSettings: put one in a scene to override project settings for the
// World3D it is in, so every viewport showing that world (the game's, the
// editor's 3D views, a SubViewport sharing the world) uses them. Each setting
// has an override_<name> switch; while it is off, the inspector shows the
// project value, read-only.
//
// PSXMaterial3D: a StandardMaterial3D with PS1 options: dither on or off for
// this material whatever the setting says, and no subdivision. Godot's own
// renderer ignores them.

#pragma once

#include "psx_palette.h"
#include "psx_settings.h"

#include "scene/main/node.h"
#include "scene/resources/material.h"

class PSXSettings : public Node {
	GDCLASS(PSXSettings, Node);

	PSXSettingOverrides overrides;
	RID registered_scenario;
	bool registered = false;

	void _update_registration();
	RID _current_scenario() const;

protected:
	bool _set(const StringName &p_name, const Variant &p_value);
	bool _get(const StringName &p_name, Variant &r_ret) const;
	void _get_property_list(List<PropertyInfo> *p_list) const;
	bool _property_can_revert(const StringName &p_name) const;
	bool _property_get_revert(const StringName &p_name, Variant &r_property) const;
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_override(const StringName &p_name, const Variant &p_value);
	void clear_override(const StringName &p_name);
	bool has_override(const StringName &p_name) const;
	// The override when set, else the project setting.
	Variant get_value(const StringName &p_name) const;

	~PSXSettings();
};

class PSXMaterial3D : public StandardMaterial3D {
	GDCLASS(PSXMaterial3D, StandardMaterial3D);

public:
	enum Dither {
		DITHER_PROJECT, // The resolved dither setting
		DITHER_ON,
		DITHER_OFF,
	};
	enum Subdivide {
		SUBDIVIDE_PROJECT, // rendering/psx/subdivide_size (or its override)
		SUBDIVIDE_OFF,
	};
	enum PSXBlend {
		PSX_BLEND_MATERIAL, // Godot's blend mode and transparency
		PSX_BLEND_HALF,
		PSX_BLEND_ADD,
		PSX_BLEND_SUBTRACT,
		PSX_BLEND_ADD_QUARTER,
	};

private:
	Dither psx_dither = DITHER_PROJECT;
	Subdivide psx_subdivide = SUBDIVIDE_PROJECT;
	PSXBlend psx_blend = PSX_BLEND_MATERIAL;
	Ref<PSXPalette> psx_palette;
	RID registered_rid;

	void _palette_changed();

	void _update_registration();

protected:
	static void _bind_methods();

public:
	void set_psx_dither(Dither p_dither);
	Dither get_psx_dither() const;
	void set_psx_subdivide(Subdivide p_subdivide);
	Subdivide get_psx_subdivide() const;
	void set_psx_blend(PSXBlend p_blend);
	PSXBlend get_psx_blend() const;
	void set_psx_palette(const Ref<PSXPalette> &p_palette);
	Ref<PSXPalette> get_psx_palette() const;

	~PSXMaterial3D();
};

VARIANT_ENUM_CAST(PSXMaterial3D::Dither);
VARIANT_ENUM_CAST(PSXMaterial3D::Subdivide);
VARIANT_ENUM_CAST(PSXMaterial3D::PSXBlend);
