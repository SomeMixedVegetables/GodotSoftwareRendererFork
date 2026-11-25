extends SceneTree

## Writes the meshes the lines_points scene uses: lines, line strips, points
## and a triangle strip, which cannot be made from the primitive meshes in
## the editor. Run again only to change them:
##   godot --headless --path <project> --script res://tools/make_line_meshes.gd
##
## wire_cube.tres: a 1 m cube's 12 edges (PRIMITIVE_LINES, indexed), each
##   corner coloured by its position.
## helix.tres: a 3-turn helix, 1 m tall (PRIMITIVE_LINE_STRIP), yellow to red.
## dots.tres: a 7 x 5 grid of points (PRIMITIVE_POINTS), coloured by column.
## ribbon.tres: a wavy band 2 m wide (PRIMITIVE_TRIANGLE_STRIP) with normals,
##   front faces toward +Z.

const DIR := "res://meshes"


func _init() -> void:
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(DIR))
	var meshes := {
		"wire_cube": _wire_cube(),
		"helix": _helix(),
		"dots": _dots(),
		"ribbon": _ribbon(),
	}
	for name: String in meshes:
		var path := "%s/%s.tres" % [DIR, name]
		if ResourceSaver.save(meshes[name], path) != OK:
			printerr("PSX_LINE_MESHES result=fail path=", path)
			quit(1)
			return
	print("PSX_LINE_MESHES result=pass count=", meshes.size())
	quit(0)


func _mesh(primitive: Mesh.PrimitiveType, arrays: Array) -> ArrayMesh:
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(primitive, arrays)
	return mesh


func _arrays() -> Array:
	var a := []
	a.resize(Mesh.ARRAY_MAX)
	return a


func _wire_cube() -> ArrayMesh:
	var verts := PackedVector3Array()
	var colors := PackedColorArray()
	for i in 8:
		var p := Vector3(i & 1, (i >> 1) & 1, (i >> 2) & 1)
		verts.append(p - Vector3(0.5, 0.5, 0.5))
		colors.append(Color(0.2 + 0.8 * p.x, 0.2 + 0.8 * p.y, 0.2 + 0.8 * p.z))
	var idx := PackedInt32Array()
	for i in 8:
		for bit in [1, 2, 4]:
			if i & bit == 0:
				idx.append(i)
				idx.append(i | bit)
	var a := _arrays()
	a[Mesh.ARRAY_VERTEX] = verts
	a[Mesh.ARRAY_COLOR] = colors
	a[Mesh.ARRAY_INDEX] = idx
	return _mesh(Mesh.PRIMITIVE_LINES, a)


func _helix() -> ArrayMesh:
	var verts := PackedVector3Array()
	var colors := PackedColorArray()
	var n := 60
	for i in n:
		var t := float(i) / float(n - 1)
		var angle := t * TAU * 3.0
		verts.append(Vector3(cos(angle) * 0.5, t - 0.5, sin(angle) * 0.5))
		colors.append(Color(1.0, 0.9 * (1.0 - t), 0.1))
	var a := _arrays()
	a[Mesh.ARRAY_VERTEX] = verts
	a[Mesh.ARRAY_COLOR] = colors
	return _mesh(Mesh.PRIMITIVE_LINE_STRIP, a)


func _dots() -> ArrayMesh:
	var verts := PackedVector3Array()
	var colors := PackedColorArray()
	for y in 5:
		for x in 7:
			verts.append(Vector3((x - 3) * 0.18, (y - 2) * 0.18, 0.0))
			colors.append(Color.from_hsv(x / 7.0, 0.7, 1.0))
	var a := _arrays()
	a[Mesh.ARRAY_VERTEX] = verts
	a[Mesh.ARRAY_COLOR] = colors
	return _mesh(Mesh.PRIMITIVE_POINTS, a)


func _ribbon() -> ArrayMesh:
	var verts := PackedVector3Array()
	var normals := PackedVector3Array()
	var segments := 12
	for i in segments + 1:
		var x := -1.0 + 2.0 * i / segments
		var y := sin(x * PI) * 0.15
		# Bottom then top: the first triangle is clockwise seen from +Z.
		verts.append(Vector3(x, y - 0.2, 0.0))
		verts.append(Vector3(x, y + 0.2, 0.0))
		normals.append(Vector3(0, 0, 1))
		normals.append(Vector3(0, 0, 1))
	var a := _arrays()
	a[Mesh.ARRAY_VERTEX] = verts
	a[Mesh.ARRAY_NORMAL] = normals
	return _mesh(Mesh.PRIMITIVE_TRIANGLE_STRIP, a)
