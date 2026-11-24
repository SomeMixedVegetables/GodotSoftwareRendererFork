extends Node3D

## Texture memory under pressure: 24 quads, each with its own 128 x 128
## texture, forced to 15-bit with one texture bank (texture_stress.override.cfg),
## so they need more VRAM than the PS1 has. Two viewports share this world:
## the left camera sees columns 0-3, the right columns 2-5 (8 quads in both).
## psx_test.ps1 checks that the last frames upload nothing (no thrashing
## between the viewports) and that the textures that do not fit fall back
## to untextured instead of evicting each other.

const COLUMNS := 6
const ROWS := 4


func _ready() -> void:
	var quad := QuadMesh.new()
	quad.size = Vector2(0.9, 0.9)
	for i in COLUMNS * ROWS:
		var c := i % COLUMNS
		var r := i / COLUMNS
		var mat := StandardMaterial3D.new()
		mat.albedo_texture = _texture(i)
		mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
		var mi := MeshInstance3D.new()
		mi.mesh = quad
		mi.material_override = mat
		mi.position = Vector3(c - (COLUMNS - 1) * 0.5, r - (ROWS - 1) * 0.5, 0)
		add_child(mi)


func _texture(i: int) -> Texture2D:
	var img := Image.create_empty(128, 128, false, Image.FORMAT_RGBA8)
	var hue := fmod(i * 0.137, 1.0)
	for y in 128:
		for x in 128:
			var ring := int(Vector2(x - 64, y - 64).length()) / 8
			img.set_pixel(x, y, Color.from_hsv(fmod(hue + x / 512.0, 1.0), 0.35 + 0.5 * y / 128.0, 0.55 + 0.4 * (ring % 2)))
	return ImageTexture.create_from_image(img)
