shader_type spatial;
render_mode cull_disabled;

// PSX test (gltf_shader_*): a cut-out texture through a source_color
// sampler, at the threshold the PSX renderer reads from this uniform.
uniform sampler2D skin : source_color, filter_nearest;
uniform float alpha_scissor_threshold : hint_range(0.0, 1.0) = 0.5;

void fragment() {
	vec4 c = texture(skin, UV);
	ALBEDO = c.rgb;
	ROUGHNESS = 1.0;
	ALPHA = c.a;
	ALPHA_SCISSOR_THRESHOLD = alpha_scissor_threshold;
}
