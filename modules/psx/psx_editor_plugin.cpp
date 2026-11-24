#ifdef TOOLS_ENABLED

#include "psx_editor_plugin.h"

#include "psx_settings.h"
#include "psx_settings_node.h"

#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "editor/editor_node.h"
#include "editor/editor_data.h"
#include "editor/inspector/editor_inspector.h"
#include "editor/inspector/editor_property_name_processor.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/check_button.h"
#include "scene/gui/label.h"
#include "scene/gui/separator.h"
#include "scene/main/timer.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/world_3d.h"

// --- Settings proxy ----------------------------------------------------------

namespace {

struct ProxyEntry {
	const char *name; // Section/name in the dock
	const char *path; // Project setting
	bool restart;
};

const ProxyEntry PROXY_ENTRIES[] = {
	{ "look/active_in_game", "rendering/psx/active", false },
	{ "look/dither", "rendering/psx/dither", false },
	{ "look/lighting_mode", "rendering/psx/lighting_mode", false },
	{ "look/default_ambient", "rendering/psx/default_ambient", false },
	{ "geometry/world_scale", "rendering/psx/world_scale", false },
	{ "geometry/subdivide_size", "rendering/psx/subdivide_size", false },
	{ "textures/max_size", "rendering/psx/textures/max_size", false },
	{ "textures/depth", "rendering/psx/textures/depth", false },
	{ "textures/max_colors", "rendering/psx/textures/max_colors", false },
	{ "display_restart/resolution_width", "rendering/psx/resolution_width", true },
	{ "display_restart/resolution_height", "rendering/psx/resolution_height", true },
	{ "display_restart/vram_scale", "rendering/psx/vram_scale", true },
	{ "display_restart/texture_banks", "rendering/psx/textures/vram_banks", true },
	{ "debug/print_stats", "rendering/psx/debug/print_stats", false },
};

const ProxyEntry *find_entry(const StringName &p_name) {
	const String name = p_name;
	for (const ProxyEntry &e : PROXY_ENTRIES) {
		if (name == e.name) {
			return &e;
		}
	}
	return nullptr;
}

} // namespace

String PSXProjectSettingsProxy::project_path(const StringName &p_name) {
	const ProxyEntry *e = find_entry(p_name);
	return e ? String(e->path) : String();
}

bool PSXProjectSettingsProxy::needs_restart(const StringName &p_name) {
	const ProxyEntry *e = find_entry(p_name);
	return e && e->restart;
}

bool PSXProjectSettingsProxy::_set(const StringName &p_name, const Variant &p_value) {
	const ProxyEntry *e = find_entry(p_name);
	if (!e) {
		return false;
	}
	ProjectSettings *ps = ProjectSettings::get_singleton();
	ps->set_setting(e->path, p_value);
	ps->save();
	return true;
}

bool PSXProjectSettingsProxy::_get(const StringName &p_name, Variant &r_ret) const {
	const ProxyEntry *e = find_entry(p_name);
	if (!e) {
		return false;
	}
	r_ret = ProjectSettings::get_singleton()->get_setting(e->path);
	return true;
}

void PSXProjectSettingsProxy::_get_property_list(List<PropertyInfo> *p_list) const {
	// Types and hints as the settings were defined (psx_debug.h).
	List<PropertyInfo> all;
	ProjectSettings::get_singleton()->get_property_list(&all);
	for (const ProxyEntry &e : PROXY_ENTRIES) {
		for (const PropertyInfo &pi : all) {
			if (pi.name == e.path) {
				PropertyInfo out = pi;
				out.name = e.name;
				out.usage = PROPERTY_USAGE_EDITOR;
				p_list->push_back(out);
				break;
			}
		}
	}
}

bool PSXProjectSettingsProxy::_property_can_revert(const StringName &p_name) const {
	return find_entry(p_name) != nullptr;
}

bool PSXProjectSettingsProxy::_property_get_revert(const StringName &p_name, Variant &r_property) const {
	const ProxyEntry *e = find_entry(p_name);
	if (!e) {
		return false;
	}
	r_property = ProjectSettings::get_singleton()->property_get_revert(e->path);
	return true;
}

// --- Dock --------------------------------------------------------------------

void PSXDock::_enabled_toggled(bool p_on) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	ps->set_setting("rendering/psx/enabled", p_on);
	ps->save();
	_update_status();
}

void PSXDock::_editor_view_toggled(bool p_on) {
	// This editor only: a runtime override, not saved.
	if (p_on) {
		PSXSettingsRegistry::clear_runtime(PSX_SETTING_ACTIVE);
	} else {
		PSXSettingsRegistry::set_runtime(PSX_SETTING_ACTIVE, false);
	}
	_update_stats();
}

void PSXDock::_restart() {
	EditorNode::get_singleton()->restart_editor();
}

void PSXDock::_settings_changed() {
	enabled->set_pressed_no_signal(GLOBAL_GET("rendering/psx/enabled"));
	_update_status();
}

void PSXDock::_update_status() {
	const bool want = GLOBAL_GET("rendering/psx/enabled");
	const bool installed = PSXSettingsRegistry::is_installed();
	String text;
	if (installed) {
		text = vformat(TTR("In use: %s."), PSXSettingsRegistry::get_installed_description());
	} else if (want) {
		const String method = OS::get_singleton()->get_current_rendering_method();
		text = (method == "gl_compatibility" || method == "dummy")
				? TTR("Not in use: the Compatibility renderer is not supported yet. Use Forward+ or Mobile.")
				: TTR("Turned on: restart the editor to use it.");
	} else {
		text = TTR("Off: Godot's own renderer draws 3D. Turn on below, then restart.");
	}
	bool restart = want != installed;
	for (const KeyValue<String, Variant> &kv : startup_values) {
		if (GLOBAL_GET(kv.key) != kv.value) {
			restart = true;
		}
	}
	if (restart && installed) {
		text += "\n" + TTR("Restart the editor to apply the changed settings.");
	}
	status->set_text(text);
	restart_box->set_visible(restart);
	editor_view->set_disabled(!installed);
}

void PSXDock::_update_overrides() {
	// The edited scene's world: the one the editor's 3D views draw.
	RID scenario;
	Node *scene = EditorNode::get_singleton()->get_edited_scene();
	if (scene && scene->is_inside_tree() && scene->get_viewport()) {
		const Ref<World3D> world = scene->get_viewport()->find_world_3d();
		if (world.is_valid()) {
			scenario = world->get_scenario();
		}
	}
	ObjectID node;
	PSXSettingOverrides o;
	String text;
	if (scenario.is_valid() && PSXSettingsRegistry::get_scene(scenario, node, o) && o.mask != 0) {
		const Node *n = ObjectDB::get_instance<Node>(node);
		text = vformat(TTR("This scene's %s node overrides:"), n ? String(n->get_name()) : String("PSXSettings"));
		for (int i = 0; i < PSX_SETTING_MAX; i++) {
			if (o.has(i)) {
				text += vformat("\n  %s = %s", String(psx_setting_def(i).name).capitalize(), psx_setting_format(i, o.values[i]));
			}
		}
	} else {
		node = ObjectID();
	}
	// Script overrides (PSX.set_setting) in this editor, apart from the
	// PS1 view switch above.
	const PSXSettingOverrides rt = PSXSettingsRegistry::get_runtime();
	String scripts;
	for (int i = 0; i < PSX_SETTING_MAX; i++) {
		if (i != PSX_SETTING_ACTIVE && rt.has(i)) {
			scripts += vformat("\n  %s = %s", String(psx_setting_def(i).name).capitalize(), psx_setting_format(i, rt.values[i]));
		}
	}
	if (!scripts.is_empty()) {
		text += (text.is_empty() ? String() : String("\n")) + TTR("Overridden by scripts (PSX.set_setting):") + scripts;
	}
	overrides_node = node;
	if (overrides_label->get_text() != text) {
		overrides_label->set_text(text);
	}
	overrides_box->set_visible(!text.is_empty());
	select_button->set_visible(node.is_valid());
}

void PSXDock::_select_overrides_node() {
	Node *n = ObjectDB::get_instance<Node>(overrides_node);
	if (n) {
		EditorNode::get_singleton()->get_editor_selection()->clear();
		EditorNode::get_singleton()->get_editor_selection()->add_node(n);
		EditorNode::get_singleton()->edit_node(n);
	}
}

void PSXDock::_update_stats() {
	_update_overrides();
	if (!PSXSettingsRegistry::is_installed()) {
		stats->set_text(String());
		return;
	}
	const PSXFrameStats s = PSXSettingsRegistry::get_stats();
	if (!s.active) {
		stats->set_text(TTR("PS1 view off: Godot's renderer is drawing."));
		return;
	}
	stats->set_text(vformat(TTR("Last frame: %d objects, %d of %d triangles drawn\nTextures: %d in VRAM (%d baked), %d drawn untextured\nVRAM %d%% free in %d bank(s), raster %.2f ms"),
			s.instances, s.triangles_drawn, s.triangles_in, s.textures_resident, s.textures_baked, s.texture_fallbacks,
			s.vram_free_percent, s.texture_banks, s.raster_usec / 1000.0));
}

void PSXDock::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			ProjectSettings::get_singleton()->connect("settings_changed", callable_mp(this, &PSXDock::_settings_changed));
			_settings_changed();
			_update_stats();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			ProjectSettings::get_singleton()->disconnect("settings_changed", callable_mp(this, &PSXDock::_settings_changed));
		} break;
		case NOTIFICATION_VISIBILITY_CHANGED: {
			if (timer) {
				if (is_visible_in_tree()) {
					timer->start();
				} else {
					timer->stop();
				}
			}
		} break;
	}
}

PSXDock::PSXDock() {
	set_name(TTRC("PSX"));
	set_icon_name("Viewport");
	set_default_slot(EditorDock::DOCK_SLOT_RIGHT_UL);

	for (const ProxyEntry &e : PROXY_ENTRIES) {
		if (e.restart) {
			startup_values[e.path] = GLOBAL_GET(e.path);
		}
	}

	VBoxContainer *vb = memnew(VBoxContainer);
	add_child(vb);

	status = memnew(Label);
	status->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	status->set_custom_minimum_size(Size2(150, 0) * EDSCALE);
	vb->add_child(status);

	enabled = memnew(CheckBox);
	enabled->set_text(TTR("Use the PS1 renderer"));
	enabled->set_tooltip_text(TTR("rendering/psx/enabled: installs the PS1 renderer for the editor and the game (Forward+ and Mobile). Needs a restart."));
	enabled->connect(SceneStringName(toggled), callable_mp(this, &PSXDock::_enabled_toggled));
	vb->add_child(enabled);

	restart_box = memnew(HBoxContainer);
	Button *restart = memnew(Button);
	restart->set_text(TTR("Restart Editor"));
	restart->connect(SceneStringName(pressed), callable_mp(this, &PSXDock::_restart));
	restart_box->add_child(restart);
	restart_box->hide();
	vb->add_child(restart_box);

	editor_view = memnew(CheckButton);
	editor_view->set_text(TTR("PS1 view in the editor"));
	editor_view->set_tooltip_text(TTR("Switch the editor's 3D views between the PS1 renderer and Godot's own, to compare. Not saved; the game uses Look > Active In Game, PSXSettings nodes and PSX.set_active()."));
	editor_view->set_pressed(true);
	editor_view->connect(SceneStringName(toggled), callable_mp(this, &PSXDock::_editor_view_toggled));
	vb->add_child(editor_view);

	vb->add_child(memnew(HSeparator));

	overrides_box = memnew(VBoxContainer);
	overrides_label = memnew(Label);
	overrides_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	overrides_label->set_custom_minimum_size(Size2(150, 0) * EDSCALE);
	overrides_label->set_tooltip_text(TTR("In this scene these values replace the project settings below, in the editor's 3D views and in the game."));
	overrides_label->set_mouse_filter(MOUSE_FILTER_PASS);
	overrides_box->add_child(overrides_label);
	select_button = memnew(Button);
	select_button->set_text(TTR("Select PSXSettings Node"));
	select_button->set_h_size_flags(SIZE_SHRINK_BEGIN);
	select_button->connect(SceneStringName(pressed), callable_mp(this, &PSXDock::_select_overrides_node));
	overrides_box->add_child(select_button);
	overrides_box->add_child(memnew(HSeparator));
	overrides_box->hide();
	vb->add_child(overrides_box);

	proxy = memnew(PSXProjectSettingsProxy);
	inspector = memnew(EditorInspector);
	inspector->set_v_size_flags(SIZE_EXPAND_FILL);
	inspector->set_use_doc_hints(false);
	inspector->set_hide_script(true);
	inspector->set_use_folding(false);
	inspector->set_property_name_style(EditorPropertyNameProcessor::get_default_inspector_style());
	vb->add_child(inspector);
	inspector->edit(proxy);
	inspector->connect("property_edited", callable_mp(this, &PSXDock::_update_status).unbind(1));

	vb->add_child(memnew(HSeparator));
	stats = memnew(Label);
	stats->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	stats->set_custom_minimum_size(Size2(150, 0) * EDSCALE);
	vb->add_child(stats);

	timer = memnew(Timer);
	timer->set_wait_time(0.5);
	timer->connect("timeout", callable_mp(this, &PSXDock::_update_stats));
	add_child(timer);
}

PSXDock::~PSXDock() {
	if (inspector) {
		inspector->edit(nullptr);
	}
	memdelete(proxy);
}

// --- Material conversion -----------------------------------------------------

String PSXMaterial3DConversionPlugin::converts_to() const {
	return "PSXMaterial3D";
}

bool PSXMaterial3DConversionPlugin::handles(const Ref<Resource> &p_resource) const {
	const Ref<StandardMaterial3D> mat = p_resource;
	return mat.is_valid() && !Object::cast_to<PSXMaterial3D>(mat.ptr());
}

Ref<Resource> PSXMaterial3DConversionPlugin::convert(const Ref<Resource> &p_resource) const {
	const Ref<StandardMaterial3D> src = p_resource;
	ERR_FAIL_COND_V(src.is_null(), Ref<Resource>());
	Ref<PSXMaterial3D> dst;
	dst.instantiate();
	List<PropertyInfo> props;
	src->get_property_list(&props);
	for (const PropertyInfo &pi : props) {
		if ((pi.usage & PROPERTY_USAGE_STORAGE) && pi.name != "script" && pi.name != "resource_path") {
			dst->set(pi.name, src->get(pi.name));
		}
	}
	return dst;
}

// --- Plugin ------------------------------------------------------------------

PSXEditorPlugin::PSXEditorPlugin() {
	dock = memnew(PSXDock);
	add_dock(dock);

	Ref<PSXMaterial3DConversionPlugin> conversion;
	conversion.instantiate();
	add_resource_conversion_plugin(conversion);
}

#endif // TOOLS_ENABLED
