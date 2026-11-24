extends Node3D

## Near plane: the camera stands low over a checkered floor, with a wall
## beside it running past the camera. The floor and the wall cross the near
## plane; their triangles are split near the camera instead of dropped, so
## the floor reaches the bottom of the screen and the wall its left edge.
## The left floor uses PSXMaterial3D with subdivision off (one big quad, the
## worst case); the right floor is subdivided as usual.


func _checker() -> ImageTexture:
	var img := Image.create_empty(16, 16, false, Image.FORMAT_RGBA8)
	for y in 16:
		for x in 16:
			var odd: bool = ((x / 8) + (y / 8)) % 2 == 1
			img.set_pixel(x, y, Color(0.85, 0.75, 0.5) if odd else Color(0.35, 0.3, 0.25))
	return ImageTexture.create_from_image(img)


func _plane(p_mat: Material, p_size: Vector2, p_xform: Transform3D) -> void:
	var mesh := PlaneMesh.new()
	mesh.size = p_size
	mesh.material = p_mat
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	mi.transform = p_xform
	add_child(mi)


func _ready() -> void:
	var tex := _checker()
	var big := PSXMaterial3D.new()
	big.albedo_texture = tex
	big.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	big.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	big.uv1_scale = Vector3(4, 4, 1)
	big.set("psx_subdivide", 1) # Off: one big quad
	_plane(big, Vector2(8, 8), Transform3D(Basis(), Vector3(-4, 0, -2)))

	var small := StandardMaterial3D.new()
	small.albedo_texture = tex
	small.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	small.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	small.uv1_scale = Vector3(4, 4, 1)
	_plane(small, Vector2(8, 8), Transform3D(Basis(), Vector3(4, 0, -2)))

	# A wall to the left, along the view, passing beside the camera.
	var wall := StandardMaterial3D.new()
	wall.albedo_color = Color(0.6, 0.3, 0.3)
	wall.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	wall.cull_mode = BaseMaterial3D.CULL_DISABLED
	_plane(wall, Vector2(2, 6), Transform3D(Basis(Vector3(0, 0, 1), -PI / 2), Vector3(-0.35, 1, -1)))
