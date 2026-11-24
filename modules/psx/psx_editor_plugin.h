// The PSX dock (right side, next to the Inspector) and the "Convert to
// PSXMaterial3D" option of StandardMaterial3D resources.
//
// The dock is the PS1 renderer's control panel:
//   - whether it is installed (rendering/psx/enabled, a restart) and a
//     Restart button when that or a display setting has changed;
//   - "PS1 view in the editor": a live switch to compare with Godot's own 3D
//     in the editor's viewports (not saved, the game is unaffected);
//   - the project settings, grouped, saved as they change: Look and Geometry
//     and Textures apply at once; Display needs a restart;
//   - the last frame's statistics.

#pragma once

#ifdef TOOLS_ENABLED

#include "editor/docks/editor_dock.h"
#include "editor/plugins/editor_plugin.h"
#include "editor/plugins/editor_resource_conversion_plugin.h"

class Button;
class CheckBox;
class CheckButton;
class EditorInspector;
class HBoxContainer;
class Label;
class Timer;

// The project settings the dock edits, as one object for an EditorInspector.
class PSXProjectSettingsProxy : public Object {
	GDCLASS(PSXProjectSettingsProxy, Object);

protected:
	bool _set(const StringName &p_name, const Variant &p_value);
	bool _get(const StringName &p_name, Variant &r_ret) const;
	void _get_property_list(List<PropertyInfo> *p_list) const;
	bool _property_can_revert(const StringName &p_name) const;
	bool _property_get_revert(const StringName &p_name, Variant &r_property) const;

public:
	static String project_path(const StringName &p_name); // Empty when not ours
	static bool needs_restart(const StringName &p_name);
};

class PSXDock : public EditorDock {
	GDCLASS(PSXDock, EditorDock);

	Label *status = nullptr;
	CheckBox *enabled = nullptr;
	HBoxContainer *restart_box = nullptr;
	CheckButton *editor_view = nullptr;
	EditorInspector *inspector = nullptr;
	PSXProjectSettingsProxy *proxy = nullptr;
	Label *stats = nullptr;
	Timer *timer = nullptr;
	// Restart settings as they were when the editor started.
	HashMap<String, Variant> startup_values;

	void _enabled_toggled(bool p_on);
	void _editor_view_toggled(bool p_on);
	void _restart();
	void _settings_changed();
	void _update_status();
	void _update_stats();

protected:
	void _notification(int p_what);

public:
	PSXDock();
	~PSXDock();
};

class PSXMaterial3DConversionPlugin : public EditorResourceConversionPlugin {
	GDCLASS(PSXMaterial3DConversionPlugin, EditorResourceConversionPlugin);

public:
	virtual String converts_to() const override;
	virtual bool handles(const Ref<Resource> &p_resource) const override;
	virtual Ref<Resource> convert(const Ref<Resource> &p_resource) const override;
};

class PSXEditorPlugin : public EditorPlugin {
	GDCLASS(PSXEditorPlugin, EditorPlugin);

	PSXDock *dock = nullptr;

public:
	virtual String get_plugin_name() const override { return "PSX"; }

	PSXEditorPlugin();
};

#endif // TOOLS_ENABLED
