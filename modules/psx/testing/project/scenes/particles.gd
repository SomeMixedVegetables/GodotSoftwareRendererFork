extends Node3D

## Particle billboards (BaseMaterial3D billboard_mode = Particles): each
## instance faces the camera and is turned about the view axis by its
## custom data X, as CPUParticles3D writes it. A MultiMesh with fixed spins
## (0, 30, 60, 90, 180 degrees) keeps the test deterministic; CPUParticles3D
## draws through the same MultiMesh path. The arrow texture shows the turn.

const SPINS := [0.0, 30.0, 60.0, 90.0, 180.0]


func _ready() -> void:
	var img := Image.create_empty(16, 16, false, Image.FORMAT_RGBA8)
	for y in 16:
		for x in 16:
			# An arrow pointing up (+V is down in the image).
			var shaft: bool = x >= 6 and x <= 9 and y >= 6
			var head: bool = y < 8 and absf(x - 7.5) <= y
			img.set_pixel(x, y, Color(1.0, 0.85, 0.2) if shaft or head else Color(0, 0, 0, 0))
	var mat := StandardMaterial3D.new()
	mat.albedo_texture = ImageTexture.create_from_image(img)
	mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.billboard_mode = BaseMaterial3D.BILLBOARD_PARTICLES
	var quad := QuadMesh.new()
	quad.size = Vector2(0.8, 0.8)
	quad.material = mat

	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.use_colors = true # Colour before custom data in the buffer
	mm.use_custom_data = true
	mm.mesh = quad
	mm.instance_count = SPINS.size()
	for i in SPINS.size():
		mm.set_instance_transform(i, Transform3D(Basis(), Vector3((i - 2) * 1.0, 0, 0)))
		mm.set_instance_color(i, Color.WHITE)
		mm.set_instance_custom_data(i, Color(deg_to_rad(SPINS[i]), 0, 0, 0))
	var mmi := MultiMeshInstance3D.new()
	mmi.multimesh = mm
	add_child(mmi)
