shader_type spatial;

// PSX test (gltf_shader_*): the texture through a source_color sampler.
uniform sampler2D skin : source_color, filter_nearest;

void fragment() {
	ALBEDO = texture(skin, UV).rgb;
	ROUGHNESS = 1.0;
}
