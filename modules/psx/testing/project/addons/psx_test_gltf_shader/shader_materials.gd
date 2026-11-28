@tool
extends GLTFDocumentExtension

## For the psx_shader_*.gltf test models only: replaces each imported
## StandardMaterial3D with a ShaderMaterial drawing the same texture through
## a source_color sampler, as a project's own glTF extension might. The PSX
## scene importer then bakes that sampler (psx_shader_baked.gltf) or the
## renderer converts it when drawn (psx_shader_runtime.gltf); both must draw
## exactly like the StandardMaterial3D models (gltf_runtime goldens).

const OPAQUE := preload("res://addons/psx_test_gltf_shader/albedo.gdshader")
const CUT := preload("res://addons/psx_test_gltf_shader/albedo_cut.gdshader")


# After the scene is generated: meshes hold their own references to the
# materials, so the swap happens on each surface.
func _import_post(state: GLTFState, root: Node) -> Error:
	if not state.filename.begins_with("psx_shader"):
		return OK
	var swapped := {}
	for node in root.find_children("*", "ImporterMeshInstance3D", true, false):
		var mesh: ImporterMesh = node.mesh
		if mesh == null:
			continue
		for i in mesh.get_surface_count():
			var base := mesh.get_surface_material(i) as BaseMaterial3D
			if base == null:
				continue
			if not swapped.has(base):
				swapped[base] = _shader_material(base)
			mesh.set_surface_material(i, swapped[base])
	return OK


func _shader_material(base: BaseMaterial3D) -> ShaderMaterial:
	var m := ShaderMaterial.new()
	m.resource_name = base.resource_name
	for key in base.get_meta_list(): # import_id: the per-material import options find it
		m.set_meta(key, base.get_meta(key))
	var cut := base.transparency == BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	m.shader = CUT if cut else OPAQUE
	m.set_shader_parameter("skin", base.albedo_texture)
	if cut:
		m.set_shader_parameter("alpha_scissor_threshold", base.alpha_scissor_threshold)
	return m
