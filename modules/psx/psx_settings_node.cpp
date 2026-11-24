#include "psx_settings_node.h"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/world_3d.h"

// Inspector groups, by setting.
static const char *_group_of(int p_setting) {
	switch (p_setting) {
		case PSX_SETTING_WORLD_SCALE:
		case PSX_SETTING_SUBDIVIDE_SIZE:
			return "Geometry";
		case PSX_SETTING_TEXTURE_MAX_SIZE:
		case PSX_SETTING_TEXTURE_DEPTH:
		case PSX_SETTING_TEXTURE_MAX_COLORS:
			return "Textures";
		default:
			return "Look";
	}
}

static Variant _project_value(int p_setting) {
	return psx_setting_validate(p_setting, ProjectSettings::get_singleton()->get_setting_with_override(psx_setting_def(p_setting).project_path));
}

// "override_dither" -> PSX_SETTING_DITHER, r_switch = true; "dither" -> r_switch = false.
static int _parse(const StringName &p_name, bool &r_switch) {
	String name = p_name;
	r_switch = name.begins_with("override_");
	if (r_switch) {
		name = name.substr(9);
	}
	return psx_setting_find(name);
}

// --- PSXSettings -------------------------------------------------------------

RID PSXSettings::_current_scenario() const {
	if (!is_inside_tree() || !get_viewport()) {
		return RID();
	}
	const Ref<World3D> world = get_viewport()->find_world_3d();
	return world.is_valid() ? world->get_scenario() : RID();
}

void PSXSettings::_update_registration() {
	const RID scenario = _current_scenario();
	if (!scenario.is_valid()) {
		if (registered) {
			PSXSettingsRegistry::remove_scene(get_instance_id());
			registered = false;
		}
		return;
	}
	PSXSettingsRegistry::set_scene(get_instance_id(), scenario, overrides);
	registered_scenario = scenario;
	registered = true;
}

bool PSXSettings::_set(const StringName &p_name, const Variant &p_value) {
	bool is_switch = false;
	const int s = _parse(p_name, is_switch);
	if (s < 0) {
		return false;
	}
	if (is_switch) {
		if ((bool)p_value == overrides.has(s)) {
			return true;
		}
		if ((bool)p_value) {
			overrides.set(s, _project_value(s)); // Start from the value in use.
		} else {
			overrides.clear(s);
		}
		notify_property_list_changed();
	} else {
		if (!overrides.has(s)) {
			// A saved scene lists the value after its switch, so this is a
			// script setting a value directly: that turns the override on.
			notify_property_list_changed();
		}
		overrides.set(s, p_value);
	}
	if (is_inside_tree()) {
		_update_registration();
	}
	return true;
}

bool PSXSettings::_get(const StringName &p_name, Variant &r_ret) const {
	bool is_switch = false;
	const int s = _parse(p_name, is_switch);
	if (s < 0) {
		return false;
	}
	if (is_switch) {
		r_ret = overrides.has(s);
	} else {
		r_ret = overrides.has(s) ? overrides.values[s] : _project_value(s);
	}
	return true;
}

void PSXSettings::_get_property_list(List<PropertyInfo> *p_list) const {
	const char *group = nullptr;
	for (int i = 0; i < PSX_SETTING_MAX; i++) {
		const PSXSettingDef &def = psx_setting_def(i);
		const char *g = _group_of(i);
		if (g != group) {
			group = g;
			p_list->push_back(PropertyInfo(Variant::NIL, g, PROPERTY_HINT_NONE, "", PROPERTY_USAGE_GROUP));
		}
		p_list->push_back(PropertyInfo(Variant::BOOL, vformat("override_%s", def.name)));
		// Not overridden: shown (the project value), read-only, not saved.
		const uint32_t usage = overrides.has(i) ? PROPERTY_USAGE_DEFAULT : (PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY);
		p_list->push_back(PropertyInfo(def.type, def.name, def.hint, def.hint_string, usage));
	}
}

bool PSXSettings::_property_can_revert(const StringName &p_name) const {
	bool is_switch = false;
	return _parse(p_name, is_switch) >= 0;
}

bool PSXSettings::_property_get_revert(const StringName &p_name, Variant &r_property) const {
	bool is_switch = false;
	const int s = _parse(p_name, is_switch);
	if (s < 0) {
		return false;
	}
	r_property = is_switch ? Variant(false) : _project_value(s);
	return true;
}

void PSXSettings::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			_update_registration();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			if (registered) {
				PSXSettingsRegistry::remove_scene(get_instance_id());
				registered = false;
			}
		} break;
	}
}

void PSXSettings::set_override(const StringName &p_name, const Variant &p_value) {
	const int s = psx_setting_find(p_name);
	ERR_FAIL_COND_MSG(s < 0, vformat("PSX: unknown setting '%s'.", p_name));
	_set(p_name, p_value);
}

void PSXSettings::clear_override(const StringName &p_name) {
	const int s = psx_setting_find(p_name);
	ERR_FAIL_COND_MSG(s < 0, vformat("PSX: unknown setting '%s'.", p_name));
	_set(vformat("override_%s", String(p_name)), false);
}

bool PSXSettings::has_override(const StringName &p_name) const {
	const int s = psx_setting_find(p_name);
	return s >= 0 && overrides.has(s);
}

Variant PSXSettings::get_value(const StringName &p_name) const {
	const int s = psx_setting_find(p_name);
	ERR_FAIL_COND_V_MSG(s < 0, Variant(), vformat("PSX: unknown setting '%s'.", p_name));
	return overrides.has(s) ? overrides.values[s] : _project_value(s);
}

void PSXSettings::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_override", "name", "value"), &PSXSettings::set_override);
	ClassDB::bind_method(D_METHOD("clear_override", "name"), &PSXSettings::clear_override);
	ClassDB::bind_method(D_METHOD("has_override", "name"), &PSXSettings::has_override);
	ClassDB::bind_method(D_METHOD("get_value", "name"), &PSXSettings::get_value);
}

PSXSettings::~PSXSettings() {
	if (registered) {
		PSXSettingsRegistry::remove_scene(get_instance_id());
	}
}

// --- PSXMaterial3D -----------------------------------------------------------

void PSXMaterial3D::_update_registration() {
	PSXMaterialOverrides o;
	o.dither = psx_dither == DITHER_ON ? PSXMaterialOverrides::DITHER_ON : (psx_dither == DITHER_OFF ? PSXMaterialOverrides::DITHER_OFF : PSXMaterialOverrides::DITHER_DEFAULT);
	o.no_subdivide = psx_subdivide == SUBDIVIDE_OFF;
	o.blend = (uint8_t)psx_blend; // Same order as PSXMaterialOverrides::Blend
	if (o.is_default()) {
		if (registered_rid.is_valid()) {
			PSXSettingsRegistry::remove_material(registered_rid);
			registered_rid = RID();
		}
		return;
	}
	registered_rid = get_rid(); // Creates the material's RID if needed.
	PSXSettingsRegistry::set_material(registered_rid, o);
}

void PSXMaterial3D::set_psx_dither(Dither p_dither) {
	psx_dither = p_dither;
	_update_registration();
}

PSXMaterial3D::Dither PSXMaterial3D::get_psx_dither() const {
	return psx_dither;
}

void PSXMaterial3D::set_psx_subdivide(Subdivide p_subdivide) {
	psx_subdivide = p_subdivide;
	_update_registration();
}

PSXMaterial3D::Subdivide PSXMaterial3D::get_psx_subdivide() const {
	return psx_subdivide;
}

void PSXMaterial3D::set_psx_blend(PSXBlend p_blend) {
	psx_blend = p_blend;
	_update_registration();
}

PSXMaterial3D::PSXBlend PSXMaterial3D::get_psx_blend() const {
	return psx_blend;
}

void PSXMaterial3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_psx_dither", "dither"), &PSXMaterial3D::set_psx_dither);
	ClassDB::bind_method(D_METHOD("get_psx_dither"), &PSXMaterial3D::get_psx_dither);
	ClassDB::bind_method(D_METHOD("set_psx_subdivide", "subdivide"), &PSXMaterial3D::set_psx_subdivide);
	ClassDB::bind_method(D_METHOD("get_psx_subdivide"), &PSXMaterial3D::get_psx_subdivide);
	ClassDB::bind_method(D_METHOD("set_psx_blend", "blend"), &PSXMaterial3D::set_psx_blend);
	ClassDB::bind_method(D_METHOD("get_psx_blend"), &PSXMaterial3D::get_psx_blend);

	ADD_GROUP("PSX", "psx_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "psx_dither", PROPERTY_HINT_ENUM, "Project Setting,On,Off"), "set_psx_dither", "get_psx_dither");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "psx_subdivide", PROPERTY_HINT_ENUM, "Project Setting,Off"), "set_psx_subdivide", "get_psx_subdivide");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "psx_blend", PROPERTY_HINT_ENUM, "Material,Half,Add,Subtract,Add Quarter"), "set_psx_blend", "get_psx_blend");

	BIND_ENUM_CONSTANT(DITHER_PROJECT);
	BIND_ENUM_CONSTANT(DITHER_ON);
	BIND_ENUM_CONSTANT(DITHER_OFF);
	BIND_ENUM_CONSTANT(SUBDIVIDE_PROJECT);
	BIND_ENUM_CONSTANT(SUBDIVIDE_OFF);
	BIND_ENUM_CONSTANT(PSX_BLEND_MATERIAL);
	BIND_ENUM_CONSTANT(PSX_BLEND_HALF);
	BIND_ENUM_CONSTANT(PSX_BLEND_ADD);
	BIND_ENUM_CONSTANT(PSX_BLEND_SUBTRACT);
	BIND_ENUM_CONSTANT(PSX_BLEND_ADD_QUARTER);
}

PSXMaterial3D::~PSXMaterial3D() {
	if (registered_rid.is_valid()) {
		PSXSettingsRegistry::remove_material(registered_rid);
	}
}
