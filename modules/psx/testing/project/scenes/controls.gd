extends Node3D

## PSX controls (psx_settings.h): the scene's PSXSettings node, PSXMaterial3D
## overrides and runtime overrides through the PSX singleton. Prints one
## PSX_CONTROLS line that psx_test.ps1 checks.
##
## controls.tscn: PSXSettings turns dither off; one sphere's PSXMaterial3D
## turns it back on; the right box's turns subdivision off; this script sets
## the default ambient (there is no WorldEnvironment).
## controls_off.tscn: PSXSettings turns the PS1 view off, so Godot's own
## renderer draws (no PSX frames are dumped).

const AMBIENT := Color(0.35, 0.3, 0.25)

@export var expect_active := true

var _frames := 0


func _ready() -> void:
	PSX.set_setting("default_ambient", AMBIENT)


func _process(_delta: float) -> void:
	_frames += 1
	if _frames != 10:
		return
	var stats: Dictionary = PSX.get_stats()
	var settings: PSXSettings = $PSXSettings
	var checks := {
		"available": PSX.is_available(),
		"active": stats["active"] == expect_active,
		"runtime": PSX.get_setting("default_ambient") == AMBIENT,
		"names": PSX.get_setting_names().has("dither"),
	}
	if expect_active:
		checks["scene"] = settings.has_override("dither") and settings.get_value("dither") == false
		checks["drawn"] = int(stats["triangles_drawn"]) > 0
	else:
		checks["scene"] = settings.has_override("active") and settings.get_value("active") == false
	var failed := checks.keys().filter(func(k: String) -> bool: return not checks[k])
	print("PSX_CONTROLS result=", "fail" if failed.size() else "pass", " failed=", ",".join(failed), " active=", stats["active"])
