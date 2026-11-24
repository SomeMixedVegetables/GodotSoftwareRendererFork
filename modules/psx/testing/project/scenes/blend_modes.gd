extends Node3D

## PSXMaterial3D psx_blend: the PS1's four blend modes, chosen per material.
## Five quads over a background of black, grey and white bands, left to
## right: Material (opaque), Half (every triangle 50%), Add (B + F),
## Subtract (B - F) and Add Quarter (B + F/4, which Godot has no mode for).
## Each quad is half red, half textured with a checker, so the modes show on
## flat and textured polygons. Godot draws all but the first as opaque in
## the side-by-side view (psx_blend is a PS1-only setting).

const MODES := [0, 1, 2, 3, 4] # PSXMaterial3D.PSX_BLEND_*


func _checker() -> ImageTexture:
	var img := Image.create_empty(8, 8, false, Image.FORMAT_RGBA8)
	for y in 8:
		for x in 8:
			img.set_pixel(x, y, Color(0.2, 0.6, 1.0) if ((x / 2) + (y / 2)) % 2 == 0 else Color(1.0, 0.8, 0.2))
	return ImageTexture.create_from_image(img)


func _quad(p_mat: Material, p_size: Vector2, p_pos: Vector3) -> void:
	var mesh := QuadMesh.new()
	mesh.size = p_size
	mesh.material = p_mat
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	mi.position = p_pos
	add_child(mi)


func _ready() -> void:
	# Background bands: black, grey, white (top to bottom).
	var shades := [Color(0, 0, 0), Color(0.5, 0.5, 0.5), Color(1, 1, 1)]
	for i in shades.size():
		var bg := StandardMaterial3D.new()
		bg.albedo_color = shades[i]
		bg.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		_quad(bg, Vector2(9, 1.2), Vector3(0, 1.2 - i * 1.2, -0.5))

	var tex := _checker()
	for i in MODES.size():
		var x := (i - 2) * 1.7
		var flat := PSXMaterial3D.new()
		flat.albedo_color = Color(0.8, 0.2, 0.2)
		flat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		flat.psx_blend = MODES[i]
		_quad(flat, Vector2(0.7, 3.4), Vector3(x - 0.37, 0, 0))
		var textured := PSXMaterial3D.new()
		textured.albedo_texture = tex
		textured.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
		textured.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		textured.psx_blend = MODES[i]
		_quad(textured, Vector2(0.7, 3.4), Vector3(x + 0.37, 0, 0))
