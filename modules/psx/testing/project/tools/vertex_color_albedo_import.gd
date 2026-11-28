@tool
extends EditorScenePostImport

## Import script for vertex-coloured test models (the default for scenes in
## this project, see [importer_defaults] in project.godot).
##
## glTF files that colour their meshes with COLOR_0 instead of textures import
## with "Vertex Color > Use as Albedo" off, so they render white. This turns it
## on for every surface that has vertex colours. glTF stores vertex colours in
## linear space, so no sRGB conversion is requested.

func _post_import(scene: Node) -> Object:
	for node in scene.find_children("*", "MeshInstance3D", true, false):
		var mesh: Mesh = (node as MeshInstance3D).mesh
		if mesh == null:
			continue
		for surface in mesh.get_surface_count():
			if not (mesh.surface_get_format(surface) & Mesh.ARRAY_FORMAT_COLOR):
				continue
			var material := mesh.surface_get_material(surface) as BaseMaterial3D
			if material == null:
				material = StandardMaterial3D.new()
				mesh.surface_set_material(surface, material)
			material.vertex_color_use_as_albedo = true
			material.vertex_color_is_srgb = false
	return scene
