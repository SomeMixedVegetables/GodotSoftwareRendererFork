extends Node3D

## Ordered dither when textures lose colours (PSXTexture.bake's dither,
## the importers' psx/dither, rendering/psx/textures/dither).
## Top row without dither, middle row with it:
## - an 8-colour palette made for a colourful gradient (4-bit);
## - a dark, gentle gradient at 15-bit, where the 8- to 5-bit step bands;
## - the colourful gradient with a fixed 4-colour PSXPalette.
## Bottom row: ordinary ImageTextures converted when drawn, dithered by the
## project setting (texture_dither.override.cfg, which also limits them to 8
## colours): the gradient with its own palette and with the fixed palette.
## Godot shows the baked textures' PS1 preview in the side-by-side view, so
## it shows the dither too; the runtime ones it draws as they are.

const SIZE := 32


func _image(p_kind: int) -> Image:
	var img := Image.create_empty(SIZE, SIZE, false, Image.FORMAT_RGBA8)
	for y in SIZE:
		for x in SIZE:
			var u := float(x) / (SIZE - 1)
			var v := float(y) / (SIZE - 1)
			var c := Color()
			match p_kind:
				0: # Colourful: orange to teal across, darker down
					c = Color(1.0, 0.45, 0.1).lerp(Color(0.1, 0.7, 0.8), u) * (1.0 - 0.6 * v)
					c.a = 1.0
				1: # Dark and gentle: a few 5-bit steps across and down
					c = Color(0.16 + 0.12 * u, 0.12 + 0.08 * v, 0.22 + 0.06 * u)
			img.set_pixel(x, y, c)
	return img


func _material(p_tex: Texture2D, p_palette: PSXPalette = null) -> PSXMaterial3D:
	var m := PSXMaterial3D.new()
	m.albedo_texture = p_tex
	m.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	m.psx_palette = p_palette
	return m


func _quad(p_mat: Material, p_pos: Vector3) -> void:
	var mesh := QuadMesh.new()
	mesh.size = Vector2(1.2, 1.2)
	mesh.material = p_mat
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	mi.position = p_pos
	add_child(mi)


func _ready() -> void:
	var colourful := _image(0)
	var dark := _image(1)
	var fixed := PSXPalette.new()
	fixed.colors = PackedColorArray([Color(0.1, 0.1, 0.15), Color(0.9, 0.4, 0.1), Color(0.2, 0.7, 0.75), Color(0.95, 0.9, 0.8)])

	for dither in 2:
		var y := 1.4 - 1.4 * dither
		_quad(_material(PSXTexture.bake(colourful, 0, 1, 8, 0.5, true, null, dither)), Vector3(-1.5, y, 0))
		_quad(_material(PSXTexture.bake(dark, 0, 3, -1, 0.5, true, null, dither)), Vector3(0, y, 0))
		_quad(_material(PSXTexture.bake(colourful, 0, -1, -1, 0.5, true, fixed, dither)), Vector3(1.5, y, 0))

	var runtime := ImageTexture.create_from_image(colourful)
	_quad(_material(runtime), Vector3(-0.75, -1.4, 0))
	_quad(_material(runtime, fixed), Vector3(0.75, -1.4, 0))
