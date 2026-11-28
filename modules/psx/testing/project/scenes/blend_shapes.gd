extends Node3D

## Builds meshes with blend shapes in code (no external assets) and changes
## their weights over time, to check CPU blend shapes against Godot's GPU
## ones: normalized (left), relative (middle), and blend shapes on a skinned
## mesh (right).

const SIDES := 10
const RINGS := 8
const RADIUS := 0.5

var _instances: Array[MeshInstance3D] = []
var _skeleton: Skeleton3D
var _time := 0.0


func _ready() -> void:
	var normalized := _make_mesh(Mesh.BLEND_SHAPE_MODE_NORMALIZED, false)
	var relative := _make_mesh(Mesh.BLEND_SHAPE_MODE_RELATIVE, false)
	_add(normalized, Vector3(-1.4, 0.6, 0), null)
	_add(relative, Vector3(0, 0.6, 0), null)

	_skeleton = Skeleton3D.new()
	_skeleton.add_bone("root")
	_skeleton.add_bone("top")
	_skeleton.set_bone_parent(1, 0)
	_skeleton.set_bone_rest(1, Transform3D(Basis(), Vector3(0, RADIUS, 0)))
	_skeleton.reset_bone_poses()
	_skeleton.position = Vector3(1.4, 0.6, 0)
	add_child(_skeleton)
	var skin := Skin.new()
	skin.add_named_bind("root", Transform3D())
	skin.add_named_bind("top", Transform3D(Basis(), Vector3(0, -RADIUS, 0)))
	var skinned := _make_mesh(Mesh.BLEND_SHAPE_MODE_NORMALIZED, true)
	var mi := _add(skinned, Vector3.ZERO, _skeleton)
	mi.skin = skin
	mi.skeleton = mi.get_path_to(_skeleton)


func _add(p_mesh: ArrayMesh, p_position: Vector3, p_parent: Node3D) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	mi.mesh = p_mesh
	mi.position = p_position
	(p_parent if p_parent else self).add_child(mi)
	_instances.append(mi)
	return mi


# A sphere with two shapes: "stretch" (taller, thinner) and "bulge" (a ring
# pushed out around the middle). Relative shapes store offsets from the base.
func _make_mesh(p_mode: Mesh.BlendShapeMode, p_bones: bool) -> ArrayMesh:
	var vertices := PackedVector3Array()
	var normals := PackedVector3Array()
	var colors := PackedColorArray()
	var bones := PackedInt32Array()
	var weights := PackedFloat32Array()
	var stretch_v := PackedVector3Array()
	var bulge_v := PackedVector3Array()
	var indices := PackedInt32Array()
	for ring in RINGS + 1:
		var phi := PI * ring / RINGS
		for side in SIDES + 1:
			var theta := TAU * side / SIDES
			var n := Vector3(sin(phi) * cos(theta), cos(phi), sin(phi) * sin(theta))
			var p := n * RADIUS
			vertices.append(p)
			normals.append(n)
			colors.append(Color.from_hsv(float(side) / SIDES, 0.5, 0.95))
			if p_bones:
				var t := clampf(n.y * 0.5 + 0.5, 0.0, 1.0)
				bones.append_array([0, 1, 0, 0])
				weights.append_array([1.0 - t, t, 0.0, 0.0])
			var stretch := Vector3(p.x * 0.6, p.y * 1.7, p.z * 0.6)
			var bulge := p + Vector3(n.x, 0, n.z) * 0.35 * sin(phi) * sin(phi)
			if p_mode == Mesh.BLEND_SHAPE_MODE_RELATIVE:
				stretch -= p
				bulge -= p
			stretch_v.append(stretch)
			bulge_v.append(bulge)
	for ring in RINGS:
		for side in SIDES:
			var i0 := ring * (SIDES + 1) + side
			var i1 := i0 + 1
			var i2 := i0 + SIDES + 1
			var i3 := i2 + 1
			indices.append_array([i0, i2, i1, i1, i2, i3])

	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_COLOR] = colors
	arrays[Mesh.ARRAY_INDEX] = indices
	if p_bones:
		arrays[Mesh.ARRAY_BONES] = bones
		arrays[Mesh.ARRAY_WEIGHTS] = weights
	# Shape normals: the base's (normalized) or no change (relative).
	var shape_normals := normals
	if p_mode == Mesh.BLEND_SHAPE_MODE_RELATIVE:
		shape_normals = PackedVector3Array()
		shape_normals.resize(normals.size())
	var shapes := []
	for v in [stretch_v, bulge_v]:
		var shape := []
		shape.resize(Mesh.ARRAY_MAX)
		shape[Mesh.ARRAY_VERTEX] = v
		shape[Mesh.ARRAY_NORMAL] = shape_normals
		shapes.append(shape)

	var mesh := ArrayMesh.new()
	mesh.blend_shape_mode = p_mode
	mesh.add_blend_shape("stretch")
	mesh.add_blend_shape("bulge")
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays, shapes)
	var material := StandardMaterial3D.new()
	material.vertex_color_use_as_albedo = true
	mesh.surface_set_material(0, material)
	return mesh


func _process(delta: float) -> void:
	_time += delta
	for i in _instances.size():
		var mi := _instances[i]
		mi.set_blend_shape_value(0, 0.5 + 0.5 * sin(_time * 2.0 + i))
		mi.set_blend_shape_value(1, 0.5 + 0.5 * sin(_time * 3.0 + i * 2.0))
	_skeleton.set_bone_pose_rotation(1, Quaternion(Vector3.BACK, sin(_time * 2.0) * 0.6))
