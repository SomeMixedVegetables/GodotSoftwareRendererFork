extends Node3D

## Sprite3D, AnimatedSprite3D, Label3D and billboard materials, with
## textures generated in code. Each sprite node is turned 60 degrees away
## from the camera, so billboarding shows: a billboarded sprite still faces
## the camera, a plain one is seen at an angle.


func _ready() -> void:
	var face := _image(16, 16, func(x: int, y: int) -> Color:
		var d := Vector2(x - 7.5, y - 7.5).length()
		if d >= 7.5:
			return Color(0, 0, 0, 0)
		if (y == 5 and (x == 5 or x == 10)) or (y == 10 and x > 4 and x < 11):
			return Color(0.1, 0.1, 0.1)
		return Color(1.0, 0.85, 0.2))
	var frame_b := _image(16, 16, func(x: int, y: int) -> Color:
		return Color(0.2, 0.6, 1.0) if (x / 4 + y / 4) % 2 == 0 else Color(0, 0, 0, 0))
	var turned := Basis(Vector3.UP, deg_to_rad(60))

	# Row 1: Sprite3D without and with billboard, alpha cut and alpha blend.
	var plain := Sprite3D.new()
	plain.texture = face
	plain.pixel_size = 0.06
	plain.alpha_cut = SpriteBase3D.ALPHA_CUT_DISCARD
	plain.transform = Transform3D(turned, Vector3(-3, 1.2, 0))
	add_child(plain)

	var bill := Sprite3D.new()
	bill.texture = face
	bill.pixel_size = 0.06
	bill.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	bill.alpha_cut = SpriteBase3D.ALPHA_CUT_DISCARD
	bill.transform = Transform3D(turned, Vector3(-1, 1.2, 0))
	add_child(bill)

	var bill_y := Sprite3D.new()
	bill_y.texture = face
	bill_y.pixel_size = 0.06
	bill_y.billboard = BaseMaterial3D.BILLBOARD_FIXED_Y
	bill_y.transform = Transform3D(turned, Vector3(1, 1.2, 0)) # default alpha: blended
	add_child(bill_y)

	var frames := SpriteFrames.new()
	frames.add_frame("default", face)
	frames.add_frame("default", frame_b)
	frames.set_animation_speed("default", 5.0)
	var anim := AnimatedSprite3D.new()
	anim.sprite_frames = frames
	anim.pixel_size = 0.06
	anim.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	anim.alpha_cut = SpriteBase3D.ALPHA_CUT_DISCARD
	anim.transform = Transform3D(turned, Vector3(3, 1.2, 0))
	add_child(anim)
	anim.play()

	# Row 2: Label3D (billboard and plain) and a quad with a billboard material.
	var label := Label3D.new()
	label.text = "PSX"
	label.font_size = 64
	label.pixel_size = 0.01
	label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	label.modulate = Color(1, 0.4, 0.3)
	label.transform = Transform3D(turned, Vector3(-2, 2.6, -1))
	add_child(label)

	var label_plain := Label3D.new()
	label_plain.text = "flat"
	label_plain.font_size = 64
	label_plain.pixel_size = 0.01
	label_plain.transform = Transform3D(Basis(), Vector3(0, 2.6, -1))
	add_child(label_plain)

	var mat := StandardMaterial3D.new()
	mat.albedo_texture = face
	mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	mat.billboard_mode = BaseMaterial3D.BILLBOARD_ENABLED
	var quad := MeshInstance3D.new()
	quad.mesh = QuadMesh.new()
	quad.material_override = mat
	quad.transform = Transform3D(turned, Vector3(2, 2.6, -1))
	add_child(quad)


func _image(w: int, h: int, fn: Callable) -> ImageTexture:
	var img := Image.create_empty(w, h, false, Image.FORMAT_RGBA8)
	for y in h:
		for x in w:
			img.set_pixel(x, y, fn.call(x, y))
	return ImageTexture.create_from_image(img)
