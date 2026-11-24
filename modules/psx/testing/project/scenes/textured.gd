extends Node3D

## Textured materials, with textures generated in code (no image files):
## a repeating 4-bit checker floor, an 8-bit gradient box (palette
## quantisation), a 256 x 256 wall repeated twice (split at tile edges), an
## alpha-scissor fence, an alpha-blended glass pane, a clamped decal and an
## unshaded sign.


func _ready() -> void:
	var checker := _image(32, 32, func(x: int, y: int) -> Color:
		return Color(0.85, 0.8, 0.7) if ((x / 4 + y / 4) % 2 == 0) else Color(0.3, 0.35, 0.45))
	var gradient := _image(64, 64, func(x: int, y: int) -> Color:
		return Color(x / 63.0, y / 63.0, 1.0 - (x + y) / 126.0))
	var bricks := _image(256, 256, func(x: int, y: int) -> Color:
		var row := y / 32
		var bx := (x + (16 if row % 2 == 1 else 0)) % 64
		if y % 32 < 3 or bx < 3:
			return Color(0.75, 0.72, 0.65)
		var shade := 0.55 + 0.1 * float((x * 7 + y * 3) % 5) / 4.0
		return Color(shade, 0.25 + 0.05 * float(row % 3), 0.18))
	var fence := _image(32, 32, func(x: int, y: int) -> Color:
		var bar := x % 16 < 4 or y % 16 < 4
		return Color(0.55, 0.4, 0.2, 1.0) if bar else Color(0, 0, 0, 0))
	var glass := _image(16, 16, func(x: int, y: int) -> Color:
		var edge := x == 0 or y == 0 or x == 15 or y == 15
		return Color(0.9, 0.95, 1.0, 1.0) if edge else Color(0.4, 0.7, 1.0, 0.5))
	var decal := _image(16, 16, func(x: int, y: int) -> Color:
		var d := Vector2(x - 7.5, y - 7.5).length()
		return Color(1.0, 0.85, 0.1) if d < 6.0 else Color(0.2, 0.1, 0.05))

	var floor_mat := _material(checker)
	floor_mat.uv1_scale = Vector3(8, 8, 1)
	_add(PlaneMesh.new(), floor_mat, Transform3D(Basis.from_scale(Vector3(12, 1, 12)), Vector3(0, 0, -3)))

	var box := BoxMesh.new()
	_add(box, _material(gradient), Transform3D(Basis(Vector3.UP, 0.6), Vector3(-2.2, 0.5, -1)))

	var wall_mat := _material(bricks)
	wall_mat.uv1_scale = Vector3(2, 1, 1)
	var wall := QuadMesh.new()
	wall.size = Vector2(6, 3)
	_add(wall, wall_mat, Transform3D(Basis(), Vector3(0, 1.5, -6)))

	var fence_mat := _material(fence)
	fence_mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	fence_mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	fence_mat.uv1_scale = Vector3(3, 1, 1)
	var fence_mesh := QuadMesh.new()
	fence_mesh.size = Vector2(3, 1)
	_add(fence_mesh, fence_mat, Transform3D(Basis(Vector3.UP, -0.5), Vector3(2.2, 0.5, -2)))

	var glass_mat := _material(glass)
	glass_mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	var pane := QuadMesh.new()
	pane.size = Vector2(1.2, 1.6)
	_add(pane, glass_mat, Transform3D(Basis(Vector3.UP, 0.3), Vector3(0.2, 0.9, 0.2)))

	var decal_mat := _material(decal)
	decal_mat.texture_repeat = false
	decal_mat.uv1_scale = Vector3(1.4, 1.4, 1)
	decal_mat.uv1_offset = Vector3(-0.2, -0.2, 0)
	var decal_mesh := QuadMesh.new()
	decal_mesh.size = Vector2(1, 1)
	_add(decal_mesh, decal_mat, Transform3D(Basis(), Vector3(-2.2, 1.6, -5.9)))

	var sign_mat := _material(gradient)
	sign_mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	var sign_mesh := QuadMesh.new()
	sign_mesh.size = Vector2(1, 0.6)
	_add(sign_mesh, sign_mat, Transform3D(Basis(), Vector3(2.2, 2.2, -5.9)))


func _image(w: int, h: int, fn: Callable) -> ImageTexture:
	var img := Image.create_empty(w, h, false, Image.FORMAT_RGBA8)
	for y in h:
		for x in w:
			img.set_pixel(x, y, fn.call(x, y))
	return ImageTexture.create_from_image(img)


func _material(tex: Texture2D) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.albedo_texture = tex
	m.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	return m


func _add(mesh: Mesh, mat: Material, xform: Transform3D) -> void:
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	mi.material_override = mat
	mi.transform = xform
	add_child(mi)
