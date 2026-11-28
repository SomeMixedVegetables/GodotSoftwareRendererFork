extends Node3D

## Builds a two-bone skinned column in code (no external assets) and bends
## it over time, to check CPU skinning against Godot's GPU skinning.

const SIDES := 6
const RINGS := 10
const HEIGHT := 2.0
const RADIUS := 0.25

var _skeleton: Skeleton3D
var _time := 0.0


func _ready() -> void:
	_skeleton = Skeleton3D.new()
	_skeleton.add_bone("root")
	_skeleton.add_bone("tip")
	_skeleton.set_bone_parent(1, 0)
	_skeleton.set_bone_rest(1, Transform3D(Basis(), Vector3(0, HEIGHT * 0.5, 0)))
	_skeleton.reset_bone_poses()
	add_child(_skeleton)

	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var material := StandardMaterial3D.new()
	material.vertex_color_use_as_albedo = true
	st.set_material(material)
	for ring in RINGS + 1:
		var y := HEIGHT * ring / RINGS
		var t := clampf((y - HEIGHT * 0.3) / (HEIGHT * 0.4), 0.0, 1.0)
		for side in SIDES + 1:
			var a := TAU * side / SIDES
			var n := Vector3(cos(a), 0, sin(a))
			st.set_color(Color.from_hsv(float(ring) / RINGS, 0.6, 0.9))
			st.set_normal(n)
			st.set_bones(PackedInt32Array([0, 1, 0, 0]))
			st.set_weights(PackedFloat32Array([1.0 - t, t, 0.0, 0.0]))
			st.add_vertex(n * RADIUS + Vector3(0, y, 0))
	for ring in RINGS:
		for side in SIDES:
			var i0 := ring * (SIDES + 1) + side
			var i1 := i0 + 1
			var i2 := i0 + SIDES + 1
			var i3 := i2 + 1
			st.add_index(i0)
			st.add_index(i1)
			st.add_index(i2)
			st.add_index(i1)
			st.add_index(i3)
			st.add_index(i2)
	var mesh := st.commit()

	var skin := Skin.new()
	skin.add_named_bind("root", Transform3D())
	skin.add_named_bind("tip", Transform3D(Basis(), Vector3(0, -HEIGHT * 0.5, 0)))

	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	mi.skin = skin
	_skeleton.add_child(mi)
	mi.skeleton = mi.get_path_to(_skeleton)


func _process(delta: float) -> void:
	_time += delta
	_skeleton.set_bone_pose_rotation(1, Quaternion(Vector3.BACK, sin(_time * 2.0) * 0.9))
