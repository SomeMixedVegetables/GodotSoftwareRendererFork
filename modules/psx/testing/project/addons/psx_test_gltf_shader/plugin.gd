@tool
extends EditorPlugin

## Registers shader_materials.gd for every glTF import in the editor.

var _extension: GLTFDocumentExtension


func _enter_tree() -> void:
	_extension = preload("res://addons/psx_test_gltf_shader/shader_materials.gd").new()
	GLTFDocument.register_gltf_document_extension(_extension, true)


func _exit_tree() -> void:
	GLTFDocument.unregister_gltf_document_extension(_extension)
