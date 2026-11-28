extends Node3D

## ShaderMaterial cases, left to right:
## 1. a colour uniform with source_color   2. a texture uniform with source_color
## 3. vertex displacement (a wave)          4. render_mode unshaded + colour
## 5. ALPHA from a uniform (blended)        6. a colour computed in code only


const SHADERS := [
	"""shader_type spatial;
uniform vec4 tint : source_color = vec4(0.9, 0.3, 0.2, 1.0);
void fragment() { ALBEDO = tint.rgb; }""",
	"""shader_type spatial;
uniform sampler2D pattern : source_color, filter_nearest;
void fragment() { ALBEDO = texture(pattern, UV).rgb; }""",
	"""shader_type spatial;
void vertex() { VERTEX.y += sin(VERTEX.x * 6.0) * 0.15; }
void fragment() { ALBEDO = vec3(0.3, 0.8, 0.4); }""",
	"""shader_type spatial;
render_mode unshaded;
uniform vec4 tint : source_color = vec4(0.2, 0.5, 1.0, 1.0);
void fragment() { ALBEDO = tint.rgb; }""",
	"""shader_type spatial;
uniform vec4 tint : source_color = vec4(1.0, 0.9, 0.2, 0.5);
void fragment() { ALBEDO = tint.rgb; ALPHA = tint.a; }""",
	"""shader_type spatial;
void fragment() { ALBEDO = vec3(UV.x, 0.2, UV.y); }""",
]


func _ready() -> void:
	var checker := Image.create_empty(16, 16, false, Image.FORMAT_RGBA8)
	for y in 16:
		for x in 16:
			checker.set_pixel(x, y, Color(0.9, 0.9, 0.9) if (x / 4 + y / 4) % 2 == 0 else Color(0.6, 0.1, 0.6))
	var tex := ImageTexture.create_from_image(checker)
	for i in SHADERS.size():
		var shader := Shader.new()
		shader.code = SHADERS[i]
		var mat := ShaderMaterial.new()
		mat.shader = shader
		if i == 1:
			mat.set_shader_parameter("pattern", tex)
		var box := BoxMesh.new()
		box.subdivide_width = 8
		var mi := MeshInstance3D.new()
		mi.mesh = box
		mi.material_override = mat
		mi.position = Vector3((i - 2.5) * 1.5, 0.6, 0)
		mi.rotation.y = 0.5
		add_child(mi)
