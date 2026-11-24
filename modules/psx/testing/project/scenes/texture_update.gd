extends Node3D

## Textures that change after their first draw. Left: an ImageTexture
## changed in place with update() (red to green). Middle: one given a new,
## bigger image with set_image() (red to blue stripes). Right: a
## ViewportTexture of a SubViewport whose quad turns from yellow to
## magenta. All change at frame 10; the PS1 renderer must show the new
## images (it reads a texture again when its content version moves).

const CHANGE_FRAME := 10

var _frame := 0
var _updated: ImageTexture
var _replaced: ImageTexture
var _sub_mat: StandardMaterial3D


func _solid(p_size: int, p_color: Color) -> Image:
	var img := Image.create_empty(p_size, p_size, false, Image.FORMAT_RGBA8)
	img.fill(p_color)
	for i in p_size:
		img.set_pixel(i, i, Color.WHITE) # A diagonal, so it is visibly textured
	return img


func _quad(p_tex: Texture2D, p_x: float) -> void:
	var mat := StandardMaterial3D.new()
	mat.albedo_texture = p_tex
	mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	var mesh := QuadMesh.new()
	mesh.size = Vector2(1.6, 1.6)
	mesh.material = mat
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	mi.position = Vector3(p_x, 0, 0)
	add_child(mi)


func _ready() -> void:
	_updated = ImageTexture.create_from_image(_solid(16, Color(0.9, 0.1, 0.1)))
	_quad(_updated, -2.0)
	_replaced = ImageTexture.create_from_image(_solid(16, Color(0.9, 0.1, 0.1)))
	_quad(_replaced, 0.0)

	var sub := SubViewport.new()
	sub.size = Vector2i(64, 64)
	sub.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	sub.own_world_3d = true
	add_child(sub)
	var cam := Camera3D.new()
	cam.position = Vector3(0, 0, 2)
	sub.add_child(cam)
	_sub_mat = StandardMaterial3D.new()
	_sub_mat.albedo_color = Color(1, 0.9, 0.1)
	_sub_mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	var box := BoxMesh.new()
	box.material = _sub_mat
	var mi := MeshInstance3D.new()
	mi.mesh = box
	mi.rotation = Vector3(0.5, 0.6, 0)
	sub.add_child(mi)
	_quad(sub.get_texture(), 2.0)


func _process(_delta: float) -> void:
	_frame += 1
	if _frame != CHANGE_FRAME:
		return
	_updated.update(_solid(16, Color(0.1, 0.8, 0.2)))
	var stripes := Image.create_empty(32, 32, false, Image.FORMAT_RGBA8)
	for y in 32:
		for x in 32:
			stripes.set_pixel(x, y, Color(0.1, 0.3, 1.0) if (x / 4) % 2 == 0 else Color(0.9, 0.9, 1.0))
	_replaced.set_image(stripes)
	_sub_mat.albedo_color = Color(0.9, 0.1, 0.9)
