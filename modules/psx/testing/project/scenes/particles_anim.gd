extends Node3D

## Particle animation frames and per-instance MultiMesh colours.
## The sheet has 4 x 2 frames; frame k is a white border with a bar k + 1
## texels high. CPUParticles3D writes the animation phase to custom data Z.
## Top row: frames 0..6, then a phase past the end that clamps to frame 7
## (particles_anim_loop off). Bottom row: phases past 1.0 that wrap to
## frames 0..3 (loop on), tinted by the instance colour (red, green, blue,
## grey), which shows because the material uses vertex colour as albedo.

const FRAME := 8


func _sheet() -> ImageTexture:
	var img := Image.create_empty(FRAME * 4, FRAME * 2, false, Image.FORMAT_RGBA8)
	img.fill(Color(0.1, 0.1, 0.1))
	for k in 8:
		var ox := (k % 4) * FRAME
		var oy := (k / 4) * FRAME
		for y in FRAME:
			for x in FRAME:
				var border: bool = x == 0 or y == 0 or x == FRAME - 1 or y == FRAME - 1
				var bar: bool = x >= 3 and x <= 4 and y >= FRAME - 1 - (k + 1) and y < FRAME - 1
				if border or bar:
					img.set_pixel(ox + x, oy + y, Color.WHITE)
	return ImageTexture.create_from_image(img)


func _row(p_tex: ImageTexture, p_loop: bool, p_y: float, p_phases: Array, p_colors: Array) -> void:
	var mat := StandardMaterial3D.new()
	mat.albedo_texture = p_tex
	mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.billboard_mode = BaseMaterial3D.BILLBOARD_PARTICLES
	mat.vertex_color_use_as_albedo = true
	mat.particles_anim_h_frames = 4
	mat.particles_anim_v_frames = 2
	mat.particles_anim_loop = p_loop
	var quad := QuadMesh.new()
	quad.size = Vector2(0.8, 0.8)
	quad.material = mat

	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.use_colors = true
	mm.use_custom_data = true
	mm.mesh = quad
	mm.instance_count = p_phases.size()
	for i in p_phases.size():
		mm.set_instance_transform(i, Transform3D(Basis(), Vector3((i - (p_phases.size() - 1) * 0.5) * 1.0, p_y, 0)))
		mm.set_instance_color(i, p_colors[i % p_colors.size()])
		mm.set_instance_custom_data(i, Color(0, 0, p_phases[i], 0))
	var mmi := MultiMeshInstance3D.new()
	mmi.multimesh = mm
	add_child(mmi)


func _ready() -> void:
	var tex := _sheet()
	var top := []
	for k in 7:
		top.append((k + 0.5) / 8.0)
	top.append(1.5) # Past the end: clamps to the last frame
	_row(tex, false, 0.6, top, [Color.WHITE])
	var bottom := []
	for k in 4:
		bottom.append(1.0 + (k + 0.5) / 8.0) # Wraps to frames 0..3
	_row(tex, true, -0.6, bottom, [Color(1, 0.2, 0.2), Color(0.2, 1, 0.2), Color(0.3, 0.4, 1), Color(0.5, 0.5, 0.5)])
