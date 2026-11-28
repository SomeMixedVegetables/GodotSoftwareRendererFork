extends SceneTree

## Writes the crowd scene used to profile the PS1 renderer at scale: a small
## skinned walker (no external assets) and a scene of many walkers, each an
## instance of it with its own animation speed. Run again only to change
## them:
##   godot --headless --path <project> --script res://tools/make_crowd.gd
##
## meshes/walker.tres: a figure of boxes (hips, torso, head, two legs, two
##   arms), 6 bones, vertex colours. Leg and arm tops blend between bones.
## meshes/walker_skin.tres: its bind poses.
## props/walker.tscn: Skeleton3D > MeshInstance3D, and an AnimationPlayer
##   with a looping walk.
## scenes/crowd.tscn: COLUMNS x ROWS walkers on a ground plane.

const COLUMNS := 16
const ROWS := 12
const SPACING := 1.1

# Bones: name, parent, rest position (relative to the parent).
const BONES := [
	["hips", -1, Vector3(0, 0.9, 0)],
	["torso", 0, Vector3(0, 0.1, 0)],
	["head", 1, Vector3(0, 0.55, 0)],
	["leg_l", 0, Vector3(-0.12, -0.05, 0)],
	["leg_r", 0, Vector3(0.12, -0.05, 0)],
	["arm_l", 1, Vector3(-0.3, 0.45, 0)],
	["arm_r", 1, Vector3(0.3, 0.45, 0)],
]


func _init() -> void:
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path("res://meshes"))
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path("res://props"))
	var mesh := _walker_mesh()
	var skin := _walker_skin()
	if ResourceSaver.save(mesh, "res://meshes/walker.tres") != OK or ResourceSaver.save(skin, "res://meshes/walker_skin.tres") != OK:
		printerr("PSX_CROWD result=fail step=mesh")
		quit(1)
		return
	mesh = load("res://meshes/walker.tres")
	skin = load("res://meshes/walker_skin.tres")
	var walker := _walker_scene(mesh, skin)
	if ResourceSaver.save(walker, "res://props/walker.tscn") != OK:
		printerr("PSX_CROWD result=fail step=walker")
		quit(1)
		return
	var f := FileAccess.open("res://scenes/crowd.tscn", FileAccess.WRITE)
	f.store_string(_crowd_text())
	f.close()
	print("PSX_CROWD result=pass")
	quit(0)


# World-space rest position of a bone.
func _bone_global(p_bone: int) -> Vector3:
	var p := Vector3()
	var b := p_bone
	while b >= 0:
		p += BONES[b][2]
		b = BONES[b][1]
	return p


# A box from p_from to p_to (world space), skinned to the given bone; the
# vertices at or above p_blend_y also take p_blend_bone at half weight.
func _box(st: SurfaceTool, p_from: Vector3, p_to: Vector3, p_color: Color, p_bone: int, p_blend_bone := -1, p_blend_y := INF) -> void:
	var lo := p_from.min(p_to)
	var hi := p_from.max(p_to)
	var faces := [
		[Vector3.RIGHT, [Vector3(1, 0, 0), Vector3(1, 1, 0), Vector3(1, 1, 1), Vector3(1, 0, 1)]],
		[Vector3.LEFT, [Vector3(0, 0, 1), Vector3(0, 1, 1), Vector3(0, 1, 0), Vector3(0, 0, 0)]],
		[Vector3.UP, [Vector3(0, 1, 1), Vector3(1, 1, 1), Vector3(1, 1, 0), Vector3(0, 1, 0)]],
		[Vector3.DOWN, [Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(1, 0, 1), Vector3(0, 0, 1)]],
		[Vector3.BACK, [Vector3(0, 0, 1), Vector3(1, 0, 1), Vector3(1, 1, 1), Vector3(0, 1, 1)]],
		[Vector3.FORWARD, [Vector3(1, 0, 0), Vector3(0, 0, 0), Vector3(0, 1, 0), Vector3(1, 1, 0)]],
	]
	for face: Array in faces:
		var corners: Array[Vector3] = []
		for c: Vector3 in face[1]:
			corners.append(lo + (hi - lo) * c)
		for k: int in [0, 1, 2, 0, 2, 3]:
			var v: Vector3 = corners[k]
			st.set_color(p_color)
			st.set_normal(face[0])
			if p_blend_bone >= 0 and v.y >= p_blend_y - 0.001:
				st.set_bones(PackedInt32Array([p_bone, p_blend_bone, 0, 0]))
				st.set_weights(PackedFloat32Array([0.5, 0.5, 0, 0]))
			else:
				st.set_bones(PackedInt32Array([p_bone, 0, 0, 0]))
				st.set_weights(PackedFloat32Array([1, 0, 0, 0]))
			st.add_vertex(v)


func _walker_mesh() -> ArrayMesh:
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var material := StandardMaterial3D.new()
	material.vertex_color_use_as_albedo = true
	st.set_material(material)
	var hips := _bone_global(0)
	var torso := _bone_global(1)
	var head := _bone_global(2)
	var shirt := Color(0.3, 0.5, 0.85)
	var trousers := Color(0.25, 0.25, 0.3)
	var skin_col := Color(0.95, 0.75, 0.6)
	_box(st, hips + Vector3(-0.2, -0.1, -0.1), hips + Vector3(0.2, 0.1, 0.1), trousers, 0)
	_box(st, torso + Vector3(-0.22, 0, -0.12), torso + Vector3(0.22, 0.5, 0.12), shirt, 1)
	_box(st, head + Vector3(-0.12, 0, -0.12), head + Vector3(0.12, 0.25, 0.12), skin_col, 2)
	for side in 2:
		var leg := _bone_global(3 + side)
		_box(st, leg + Vector3(-0.07, -0.8, -0.07), leg + Vector3(0.07, 0, 0.07), trousers, 3 + side, 0, leg.y)
		var arm := _bone_global(5 + side)
		var dir := -1.0 if side == 0 else 1.0
		_box(st, arm + Vector3(dir * 0.06 - 0.05, -0.6, -0.05), arm + Vector3(dir * 0.06 + 0.05, 0, 0.05), shirt if side == 0 else skin_col, 5 + side, 1, arm.y)
	st.index()
	return st.commit()


func _walker_skin() -> Skin:
	var skin := Skin.new()
	for i in BONES.size():
		skin.add_named_bind(BONES[i][0], Transform3D(Basis(), -_bone_global(i)))
	return skin


func _walker_scene(p_mesh: ArrayMesh, p_skin: Skin) -> PackedScene:
	var root := Node3D.new()
	root.name = "Walker"
	var skeleton := Skeleton3D.new()
	skeleton.name = "Skeleton3D"
	root.add_child(skeleton)
	skeleton.owner = root
	for bone: Array in BONES:
		var i := skeleton.add_bone(bone[0])
		if bone[1] >= 0:
			skeleton.set_bone_parent(i, bone[1])
		skeleton.set_bone_rest(i, Transform3D(Basis(), bone[2]))
	skeleton.reset_bone_poses()
	var mi := MeshInstance3D.new()
	mi.name = "Body"
	mi.mesh = p_mesh
	mi.skin = p_skin
	skeleton.add_child(mi)
	mi.owner = root
	mi.skeleton = NodePath("..")

	var anim := Animation.new()
	anim.length = 1.0
	anim.loop_mode = Animation.LOOP_LINEAR
	var swings := {"leg_l": 0.6, "leg_r": -0.6, "arm_l": -0.5, "arm_r": 0.5}
	for bone: String in swings:
		var t := anim.add_track(Animation.TYPE_ROTATION_3D)
		anim.track_set_path(t, NodePath("Skeleton3D:" + bone))
		for k in 5:
			var time := k * 0.25
			var angle: float = swings[bone] * sin(TAU * time)
			anim.rotation_track_insert_key(t, time, Quaternion(Vector3.RIGHT, angle))
	var bob := anim.add_track(Animation.TYPE_POSITION_3D)
	anim.track_set_path(bob, NodePath("Skeleton3D:hips"))
	for k in 5:
		var time := k * 0.25
		anim.position_track_insert_key(bob, time, BONES[0][2] + Vector3(0, 0.04 * absf(cos(TAU * time)), 0))
	var library := AnimationLibrary.new()
	library.add_animation("walk", anim)
	var player := AnimationPlayer.new()
	player.name = "AnimationPlayer"
	player.add_animation_library("", library)
	player.autoplay = "walk"
	player.playback_process_mode = AnimationMixer.ANIMATION_CALLBACK_MODE_PROCESS_IDLE
	root.add_child(player)
	player.owner = root

	var packed := PackedScene.new()
	packed.pack(root)
	root.free()
	return packed


func _crowd_text() -> String:
	var s := "[gd_scene load_steps=5 format=3]\n\n"
	s += "; %d skinned walkers (props/walker.tscn, made by tools/make_crowd.gd)\n" % (COLUMNS * ROWS)
	s += "; for profiling the PS1 renderer at scale: CPU skinning of every walker\n"
	s += "; every frame. Each walker's AnimationPlayer runs at its own speed.\n\n"
	s += "[ext_resource type=\"PackedScene\" path=\"res://props/walker.tscn\" id=\"1\"]\n\n"
	s += "[sub_resource type=\"PlaneMesh\" id=\"ground\"]\nsize = Vector2(%d, %d)\n\n" % [ceili(COLUMNS * SPACING) + 2, ceili(ROWS * SPACING) + 2]
	s += "[sub_resource type=\"StandardMaterial3D\" id=\"grass\"]\nalbedo_color = Color(0.35, 0.55, 0.3, 1)\n\n"
	s += "[sub_resource type=\"Environment\" id=\"env\"]\nbackground_mode = 1\nbackground_color = Color(0.5, 0.65, 0.85, 1)\nambient_light_source = 2\nambient_light_color = Color(1, 1, 1, 1)\nambient_light_energy = 0.35\n\n"
	s += "[node name=\"Crowd\" type=\"Node3D\"]\n\n"
	s += "[node name=\"WorldEnvironment\" type=\"WorldEnvironment\" parent=\".\"]\nenvironment = SubResource(\"env\")\n\n"
	s += "[node name=\"Camera3D\" type=\"Camera3D\" parent=\".\"]\ntransform = Transform3D(1, 0, 0, 0, 0.866, 0.5, 0, -0.5, 0.866, 0, 7, 12)\nfov = 60.0\n\n"
	s += "[node name=\"Sun\" type=\"DirectionalLight3D\" parent=\".\"]\ntransform = Transform3D(0.866, -0.354, 0.354, 0, 0.707, 0.707, -0.5, -0.612, 0.612, 0, 5, 0)\n\n"
	s += "[node name=\"Ground\" type=\"MeshInstance3D\" parent=\".\"]\nmesh = SubResource(\"ground\")\nsurface_material_override/0 = SubResource(\"grass\")\n\n"
	s += "[node name=\"Walkers\" type=\"Node3D\" parent=\".\"]\n\n"
	for row in ROWS:
		for col in COLUMNS:
			var i := row * COLUMNS + col
			var x := (col - (COLUMNS - 1) * 0.5) * SPACING
			var z := (row - (ROWS - 1) * 0.5) * SPACING
			var a := float((i * 37) % 360) * PI / 180.0
			var c := cos(a)
			var sn := sin(a)
			s += "[node name=\"Walker%d\" parent=\"Walkers\" instance=ExtResource(\"1\")]\n" % i
			s += "transform = Transform3D(%.4f, 0, %.4f, 0, 1, 0, %.4f, 0, %.4f, %.3f, 0, %.3f)\n\n" % [c, -sn, sn, c, x, z]
			s += "[node name=\"AnimationPlayer\" parent=\"Walkers/Walker%d\"]\n" % i
			s += "speed_scale = %.2f\n\n" % (0.8 + float((i * 13) % 40) / 100.0)
	return s
