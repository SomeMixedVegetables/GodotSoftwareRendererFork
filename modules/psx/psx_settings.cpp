#include "psx_settings.h"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"

static const PSXSettingDef SETTING_DEFS[PSX_SETTING_MAX] = {
	{ "active", "rendering/psx/active", Variant::BOOL, PROPERTY_HINT_NONE, "", false,
			"Draw 3D with the PS1 renderer; off shows Godot's own 3D. Changes at once." },
	{ "dither", "rendering/psx/dither", Variant::BOOL, PROPERTY_HINT_NONE, "", false,
			"The GPU's 4x4 ordered dither on shaded and textured polygons." },
	{ "lighting_mode", "rendering/psx/lighting_mode", Variant::INT, PROPERTY_HINT_ENUM, "PS1,Godot Match", false,
			"PS1: display colour times light, as the GTE does. Godot Match: linear, Godot's brightness." },
	{ "default_ambient", "rendering/psx/default_ambient", Variant::COLOR, PROPERTY_HINT_NONE, "", false,
			"Ambient light when there is no WorldEnvironment." },
	{ "world_scale", "rendering/psx/world_scale", Variant::FLOAT, PROPERTY_HINT_RANGE, "16,4096,1", true,
			"GTE units per metre: far limit 65535 / scale metres." },
	{ "subdivide_size", "rendering/psx/subdivide_size", Variant::FLOAT, PROPERTY_HINT_RANGE, "0,16,0.05,or_greater", true,
			"Split polygons until no edge is longer than this (metres, 0 = off)." },
	{ "texture_max_size", "rendering/psx/textures/max_size", Variant::INT, PROPERTY_HINT_ENUM, "8:8,16:16,32:32,64:64,128:128,256:256", true,
			"Largest side of a texture converted when drawn." },
	{ "texture_depth", "rendering/psx/textures/depth", Variant::INT, PROPERTY_HINT_ENUM, "Auto,4-bit,8-bit,15-bit", true,
			"Colour depth of textures converted when drawn." },
	{ "texture_max_colors", "rendering/psx/textures/max_colors", Variant::INT, PROPERTY_HINT_RANGE, "0,256,1", true,
			"Palette limit of textures converted when drawn (0 = the depth's)." },
};

const PSXSettingDef &psx_setting_def(int p_setting) {
	return SETTING_DEFS[CLAMP(p_setting, 0, PSX_SETTING_MAX - 1)];
}

int psx_setting_find(const StringName &p_name) {
	const String name = p_name;
	for (int i = 0; i < PSX_SETTING_MAX; i++) {
		if (name == SETTING_DEFS[i].name) {
			return i;
		}
	}
	return -1;
}

Variant psx_setting_validate(int p_setting, const Variant &p_value) {
	switch (p_setting) {
		case PSX_SETTING_ACTIVE:
		case PSX_SETTING_DITHER:
			return (bool)p_value;
		case PSX_SETTING_LIGHTING_MODE:
			return CLAMP((int)p_value, 0, 1);
		case PSX_SETTING_DEFAULT_AMBIENT:
			return (Color)p_value;
		case PSX_SETTING_WORLD_SCALE:
			return MAX(1.0, (double)p_value);
		case PSX_SETTING_SUBDIVIDE_SIZE:
			return MAX(0.0, (double)p_value);
		case PSX_SETTING_TEXTURE_MAX_SIZE:
			return CLAMP((int)p_value, 8, 256);
		case PSX_SETTING_TEXTURE_DEPTH:
			return CLAMP((int)p_value, 0, 3);
		case PSX_SETTING_TEXTURE_MAX_COLORS:
			return CLAMP((int)p_value, 0, 256);
		default:
			return p_value;
	}
}

namespace {

struct SceneEntry {
	RID scenario;
	PSXSettingOverrides overrides;
	uint64_t order = 0;
};

struct Registry {
	Mutex mutex;
	HashMap<ObjectID, SceneEntry> scenes;
	uint64_t next_order = 1;
	PSXSettingOverrides runtime;
	HashMap<RID, PSXMaterialOverrides> materials;
	bool installed = false;
	String installed_description;
	PSXFrameStats stats;
};

Registry &registry() {
	static Registry r;
	return r;
}

// Project values, then the scene's and the runtime overrides. Locked by the caller.
void resolve_values(Registry &r, RID p_scenario, Variant r_values[PSX_SETTING_MAX]) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	for (int i = 0; i < PSX_SETTING_MAX; i++) {
		r_values[i] = psx_setting_validate(i, ps->get_setting_with_override(SETTING_DEFS[i].project_path));
	}
	if (p_scenario.is_valid()) {
		const SceneEntry *best = nullptr;
		for (const KeyValue<ObjectID, SceneEntry> &kv : r.scenes) {
			if (kv.value.scenario == p_scenario && (!best || kv.value.order > best->order)) {
				best = &kv.value;
			}
		}
		if (best) {
			for (int i = 0; i < PSX_SETTING_MAX; i++) {
				if (best->overrides.has(i)) {
					r_values[i] = best->overrides.values[i];
				}
			}
		}
	}
	for (int i = 0; i < PSX_SETTING_MAX; i++) {
		if (r.runtime.has(i)) {
			r_values[i] = r.runtime.values[i];
		}
	}
}

} // namespace

void PSXSettingsRegistry::set_scene(ObjectID p_node, RID p_scenario, const PSXSettingOverrides &p_overrides) {
	Registry &r = registry();
	MutexLock lock(r.mutex);
	SceneEntry *e = r.scenes.getptr(p_node);
	if (!e || e->scenario != p_scenario) {
		e = &r.scenes[p_node];
		e->order = r.next_order++; // Newly entered (or moved) nodes win.
	}
	e->scenario = p_scenario;
	e->overrides = p_overrides;
}

void PSXSettingsRegistry::remove_scene(ObjectID p_node) {
	Registry &r = registry();
	MutexLock lock(r.mutex);
	r.scenes.erase(p_node);
}

void PSXSettingsRegistry::set_runtime(int p_setting, const Variant &p_value) {
	ERR_FAIL_INDEX(p_setting, PSX_SETTING_MAX);
	Registry &r = registry();
	MutexLock lock(r.mutex);
	r.runtime.set(p_setting, p_value);
}

void PSXSettingsRegistry::clear_runtime(int p_setting) {
	ERR_FAIL_INDEX(p_setting, PSX_SETTING_MAX);
	Registry &r = registry();
	MutexLock lock(r.mutex);
	r.runtime.clear(p_setting);
}

void PSXSettingsRegistry::clear_runtime_all() {
	Registry &r = registry();
	MutexLock lock(r.mutex);
	r.runtime = PSXSettingOverrides();
}

PSXSettingOverrides PSXSettingsRegistry::get_runtime() {
	Registry &r = registry();
	MutexLock lock(r.mutex);
	return r.runtime;
}

void PSXSettingsRegistry::set_material(RID p_material, const PSXMaterialOverrides &p_overrides) {
	Registry &r = registry();
	MutexLock lock(r.mutex);
	if (p_overrides.is_default()) {
		r.materials.erase(p_material);
	} else {
		r.materials[p_material] = p_overrides;
	}
}

void PSXSettingsRegistry::remove_material(RID p_material) {
	Registry &r = registry();
	MutexLock lock(r.mutex);
	r.materials.erase(p_material);
}

PSXMaterialOverrides PSXSettingsRegistry::get_material(RID p_material) {
	Registry &r = registry();
	MutexLock lock(r.mutex);
	const PSXMaterialOverrides *o = r.materials.getptr(p_material);
	return o ? *o : PSXMaterialOverrides();
}

PSXLiveSettings PSXSettingsRegistry::resolve(RID p_scenario) {
	Variant v[PSX_SETTING_MAX];
	{
		Registry &r = registry();
		MutexLock lock(r.mutex);
		resolve_values(r, p_scenario, v);
	}
	PSXLiveSettings s;
	s.active = v[PSX_SETTING_ACTIVE];
	s.dither = v[PSX_SETTING_DITHER];
	s.lighting = (int)v[PSX_SETTING_LIGHTING_MODE] == 1 ? psx::LIGHTING_LINEAR : psx::LIGHTING_PS1;
	s.default_ambient = v[PSX_SETTING_DEFAULT_AMBIENT];
	s.world_scale = (float)(double)v[PSX_SETTING_WORLD_SCALE];
	s.subdivide_size = (float)(double)v[PSX_SETTING_SUBDIVIDE_SIZE];
	s.texture.max_size = v[PSX_SETTING_TEXTURE_MAX_SIZE];
	s.texture.depth = (uint8_t)(int)v[PSX_SETTING_TEXTURE_DEPTH];
	s.texture.max_colors = v[PSX_SETTING_TEXTURE_MAX_COLORS];
	return s;
}

Variant PSXSettingsRegistry::resolve_value(int p_setting, RID p_scenario) {
	ERR_FAIL_INDEX_V(p_setting, PSX_SETTING_MAX, Variant());
	Variant v[PSX_SETTING_MAX];
	Registry &r = registry();
	MutexLock lock(r.mutex);
	resolve_values(r, p_scenario, v);
	return v[p_setting];
}

void PSXSettingsRegistry::set_installed(bool p_installed, const String &p_renderer, int p_width, int p_height, int p_vram_scale) {
	Registry &r = registry();
	MutexLock lock(r.mutex);
	r.installed = p_installed;
	r.installed_description = p_installed ? vformat("%s, %dx%d, VRAM x%d", p_renderer, p_width, p_height, p_vram_scale) : String();
}

bool PSXSettingsRegistry::is_installed() {
	Registry &r = registry();
	MutexLock lock(r.mutex);
	return r.installed;
}

String PSXSettingsRegistry::get_installed_description() {
	Registry &r = registry();
	MutexLock lock(r.mutex);
	return r.installed_description;
}

void PSXSettingsRegistry::publish_stats(const PSXFrameStats &p_stats) {
	Registry &r = registry();
	MutexLock lock(r.mutex);
	r.stats = p_stats;
}

PSXFrameStats PSXSettingsRegistry::get_stats() {
	Registry &r = registry();
	MutexLock lock(r.mutex);
	return r.stats;
}

// --- PSX singleton -----------------------------------------------------------

PSX *PSX::singleton = nullptr;

bool PSX::is_available() const {
	return PSXSettingsRegistry::is_installed();
}

void PSX::set_active(bool p_active) {
	PSXSettingsRegistry::set_runtime(PSX_SETTING_ACTIVE, p_active);
}

bool PSX::is_active() const {
	return PSXSettingsRegistry::resolve_value(PSX_SETTING_ACTIVE);
}

void PSX::set_setting(const StringName &p_name, const Variant &p_value) {
	const int s = psx_setting_find(p_name);
	ERR_FAIL_COND_MSG(s < 0, vformat("PSX: unknown setting '%s'. Known: %s.", p_name, String(", ").join(get_setting_names())));
	PSXSettingsRegistry::set_runtime(s, p_value);
}

Variant PSX::get_setting(const StringName &p_name) const {
	const int s = psx_setting_find(p_name);
	ERR_FAIL_COND_V_MSG(s < 0, Variant(), vformat("PSX: unknown setting '%s'.", p_name));
	return PSXSettingsRegistry::resolve_value(s);
}

void PSX::clear_setting(const StringName &p_name) {
	const int s = psx_setting_find(p_name);
	ERR_FAIL_COND_MSG(s < 0, vformat("PSX: unknown setting '%s'.", p_name));
	PSXSettingsRegistry::clear_runtime(s);
}

void PSX::clear_settings() {
	PSXSettingsRegistry::clear_runtime_all();
}

PackedStringArray PSX::get_setting_names() const {
	PackedStringArray names;
	for (int i = 0; i < PSX_SETTING_MAX; i++) {
		names.push_back(SETTING_DEFS[i].name);
	}
	return names;
}

Dictionary PSX::get_stats() const {
	const PSXFrameStats s = PSXSettingsRegistry::get_stats();
	Dictionary d;
	d["frame"] = (int64_t)s.frame;
	d["active"] = s.active;
	d["instances"] = s.instances;
	d["triangles_in"] = s.triangles_in;
	d["triangles_drawn"] = s.triangles_drawn;
	d["conversions"] = s.conversions;
	d["textures_resident"] = s.textures_resident;
	d["textures_baked"] = s.textures_baked;
	d["texture_fallbacks"] = s.texture_fallbacks;
	d["texture_banks"] = s.texture_banks;
	d["vram_free_percent"] = s.vram_free_percent;
	d["raster_usec"] = (int64_t)s.raster_usec;
	return d;
}

void PSX::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_available"), &PSX::is_available);
	ClassDB::bind_method(D_METHOD("set_active", "active"), &PSX::set_active);
	ClassDB::bind_method(D_METHOD("is_active"), &PSX::is_active);
	ClassDB::bind_method(D_METHOD("set_setting", "name", "value"), &PSX::set_setting);
	ClassDB::bind_method(D_METHOD("get_setting", "name"), &PSX::get_setting);
	ClassDB::bind_method(D_METHOD("clear_setting", "name"), &PSX::clear_setting);
	ClassDB::bind_method(D_METHOD("clear_settings"), &PSX::clear_settings);
	ClassDB::bind_method(D_METHOD("get_setting_names"), &PSX::get_setting_names);
	ClassDB::bind_method(D_METHOD("get_stats"), &PSX::get_stats);
}

PSX::PSX() {
	singleton = this;
}

PSX::~PSX() {
	if (singleton == this) {
		singleton = nullptr;
	}
}
