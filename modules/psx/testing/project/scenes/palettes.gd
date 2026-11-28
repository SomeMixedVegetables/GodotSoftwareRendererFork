extends Node3D

## PSXPalette: shared palettes and palette swaps.
## One 16-entry palette is generated from three textures together.
## Top row: the three textures baked with it (PSXTexture.bake), the last with
## cut-out holes; they share one CLUT in texture memory.
## Middle row: the first texture again, with the palette swapped per
## material (psx_palette): the same palette, a warm one, a grey one. The
## swaps share the texture's texels; only the CLUT changes.
## Bottom row: an ordinary ImageTexture converted to the palette when drawn
## (psx_palette on a runtime texture), sharing the palette's CLUT too; and
## the same image with its own palette, for comparison.
## Godot draws the textures as they are in the side-by-side view (palettes
## are PS1-only), except the baked ones, which show their PS1 preview.

const SIZE := 32


func _image(p_kind: int) -> Image:
	var img := Image.create_empty(SIZE, SIZE, false, Image.FORMAT_RGBA8)
	for y in SIZE:
		for x in SIZE:
			var u := float(x) / (SIZE - 1)
			var v := float(y) / (SIZE - 1)
			var c := Color()
			match p_kind:
				0: # Red to yellow, blue stripes
					c = Color(1.0, u, 0.1) if (y / 4) % 2 == 0 else Color(0.1, 0.2, 0.4 + 0.6 * v)
				1: # Green-blue rings
					var d := Vector2(u - 0.5, v - 0.5).length()
					c = Color(0.1, 0.4 + 0.6 * (1.0 - d), 0.3 + 0.7 * d)
				2: # Purple checker with round holes
					c = Color(0.6, 0.2, 0.8) if ((x / 8) + (y / 8)) % 2 == 0 else Color(0.95, 0.9, 0.7)
					var cell := Vector2(fmod(x, 16.0) - 7.5, fmod(y, 16.0) - 7.5)
					if cell.length() < 3.5:
						c.a = 0.0
				3: # Diagonal blend of the others' colours
					c = Color(1.0, 0.5, 0.1).lerp(Color(0.2, 0.8, 0.9), (u + v) * 0.5)
			img.set_pixel(x, y, c)
	return img


func _material(p_tex: Texture2D, p_palette: PSXPalette, p_cut := false) -> PSXMaterial3D:
	var m := PSXMaterial3D.new()
	m.albedo_texture = p_tex
	m.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	m.psx_palette = p_palette
	if p_cut:
		m.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
		m.alpha_scissor_threshold = 0.5
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
	var images := [_image(0), _image(1), _image(2)]
	var palette := PSXPalette.new()
	palette.generate_from_images(images)

	# Swaps: the same colours, warmed (red up, blue down) and grey.
	var warm := PSXPalette.new()
	var grey := PSXPalette.new()
	var warm_colors := PackedColorArray()
	var grey_colors := PackedColorArray()
	for c in palette.colors:
		warm_colors.push_back(Color(minf(c.r * 1.3 + 0.1, 1.0), c.g * 0.9, c.b * 0.5))
		var l := c.r * 0.3 + c.g * 0.59 + c.b * 0.11
		grey_colors.push_back(Color(l, l, l))
	warm.colors = warm_colors
	grey.colors = grey_colors

	var baked := []
	for img in images:
		baked.push_back(PSXTexture.bake(img, 0, -1, -1, 0.5, true, palette))

	for i in 3:
		_quad(_material(baked[i], null, i == 2), Vector3((i - 1) * 1.5, 1.4, 0))
	var swaps := [palette, warm, grey]
	for i in 3:
		_quad(_material(baked[0], swaps[i]), Vector3((i - 1) * 1.5, 0, 0))

	var runtime := ImageTexture.create_from_image(_image(3))
	_quad(_material(runtime, palette), Vector3(-0.75, -1.4, 0))
	_quad(_material(runtime, null), Vector3(0.75, -1.4, 0))
