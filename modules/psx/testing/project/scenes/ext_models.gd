extends Node3D

## Lays out every model in res://external_models (copied in by
## psx_test.ps1 -ModelsDir, git-ignored) on a grid, each scaled to fit a
## CELL-sized box and resting on the ground, then frames them with the camera.

const DIR := "res://external_models"
const CELL := 1.6
const SPACING := 2.0
const COLUMNS := 5


func _ready() -> void:
	var paths: PackedStringArray = []
	for file in DirAccess.get_files_at(DIR):
		if file.get_extension().to_lower() == "glb":
			paths.append(DIR.path_join(file))
	paths.sort()
	if paths.is_empty():
		push_warning("ext_models: no models in %s (run psx_test.ps1 with -ModelsDir)" % DIR)
		return

	var rows := int(ceil(paths.size() / float(COLUMNS)))
	for i in paths.size():
		var packed := load(paths[i]) as PackedScene
		if packed == null:
			push_error("ext_models: could not load %s" % paths[i])
			continue
		var model := packed.instantiate() as Node3D
		var box := _bounds(model, Transform3D.IDENTITY)
		var size := maxf(box.size.x, maxf(box.size.y, box.size.z))
		var s := CELL / size if size > 0.0 else 1.0
		var holder := Node3D.new()
		holder.name = paths[i].get_file().get_basename()
		var col := i % COLUMNS
		var row := i / COLUMNS
		holder.position = Vector3((col - (COLUMNS - 1) * 0.5) * SPACING, 0.0, (row - (rows - 1) * 0.5) * SPACING)
		holder.rotation_degrees.y = 30.0
		model.scale = Vector3.ONE * s
		# Centre on X/Z, base on the ground.
		var c := box.get_center() * s
		model.position = Vector3(-c.x, -box.position.y * s, -c.z)
		holder.add_child(model)
		add_child(holder)

	var camera := $Camera3D as Camera3D
	var width := COLUMNS * SPACING
	camera.position = Vector3(0.0, width * 0.45, width * 0.75)
	camera.look_at(Vector3(0.0, 0.4, 0.0))


# Bounding box of every mesh under p_node, in p_node's parent space.
func _bounds(p_node: Node, p_xform: Transform3D) -> AABB:
	var xform := p_xform
	if p_node is Node3D:
		xform = p_xform * (p_node as Node3D).transform
	var box := AABB()
	var first := true
	if p_node is MeshInstance3D and (p_node as MeshInstance3D).mesh:
		box = xform * (p_node as MeshInstance3D).mesh.get_aabb()
		first = false
	for child in p_node.get_children():
		var b := _bounds(child, xform)
		if b.size == Vector3.ZERO:
			continue
		box = b if first else box.merge(b)
		first = false
	return box
