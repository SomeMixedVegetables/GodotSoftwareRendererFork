extends SceneTree

## Writes the glTF test models (git-ignored; psx_test.ps1 runs this before
## importing the project). Model files are never committed, so the models
## are made here from procedural textures:
##   godot --headless --path <project> --script res://tools/make_test_models.gd
##
## psx_quads_*.gltf: three 1.6 m quads with embedded textures.
##   Gradient: opaque 48 x 40 (resampled to 64 x 64).
##   Fence: 32 x 32 cut-out with alpha 0 holes and alpha 0.4 bars,
##     alphaCutoff 0.3 (the bars stay only when baked at the material's own
##     threshold).
##   Checker: 16 x 16; psx_quads_baked.gltf.import sets it to psx/bake = Never.
## The _baked and _runtime files are identical; only their import settings differ.

const OUTPUTS := ["res://models/psx_quads_baked.gltf", "res://models/psx_quads_runtime.gltf"]


func _init() -> void:
	var text := JSON.stringify(_document(), " ")
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path("res://models"))
	for path in OUTPUTS:
		var f := FileAccess.open(path, FileAccess.WRITE)
		if f == null:
			printerr("PSX_MODELS result=fail path=", path)
			quit(1)
			return
		f.store_string(text + "\n")
	print("PSX_MODELS result=pass count=", OUTPUTS.size())
	quit(0)


func _png(w: int, h: int, fn: Callable) -> String:
	var img := Image.create_empty(w, h, false, Image.FORMAT_RGBA8)
	for y in h:
		for x in w:
			img.set_pixel(x, y, fn.call(x, y))
	return "data:image/png;base64," + Marshalls.raw_to_base64(img.save_png_to_buffer())


func _c8(r: int, g: int, b: int, a: int = 255) -> Color:
	return Color8(r, g, b, a)


func _document() -> Dictionary:
	var gradient := _png(48, 40, func(x: int, y: int) -> Color:
		return _c8(x * 5, y * 6, 255 - (x + y) * 2))
	var fence := _png(32, 32, func(x: int, y: int) -> Color:
		if x % 16 < 4 or y % 16 < 4:
			return _c8(180, 120, 60)
		if x % 8 < 2:
			return _c8(60, 200, 90, 102) # Alpha 0.4
		return _c8(0, 0, 0, 0))
	var checker := _png(16, 16, func(x: int, y: int) -> Color:
		return _c8(230, 220, 200) if (x / 4 + y / 4) % 2 == 0 else _c8(60, 70, 140))

	# One quad facing +Z, shared by the three meshes.
	var buf := PackedByteArray()
	var floats := PackedFloat32Array([
		-0.8, -0.8, 0, 0.8, -0.8, 0, 0.8, 0.8, 0, -0.8, 0.8, 0, # Positions
		0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, # Normals
		0, 1, 1, 1, 1, 0, 0, 0]) # UVs
	buf.append_array(floats.to_byte_array())
	for i in [0, 1, 2, 0, 2, 3]:
		buf.append(i & 0xFF)
		buf.append(i >> 8)

	var images := []
	var textures := []
	var materials := []
	var meshes := []
	var nodes := []
	var parts := [
		["Gradient", gradient, {}],
		["Fence", fence, {"alphaMode": "MASK", "alphaCutoff": 0.3, "doubleSided": true}],
		["Checker", checker, {}],
	]
	for i in parts.size():
		var name: String = parts[i][0]
		images.append({"name": name, "mimeType": "image/png", "uri": parts[i][1]})
		textures.append({"source": i, "sampler": 0})
		var m := {"name": name, "pbrMetallicRoughness": {"baseColorTexture": {"index": i}, "metallicFactor": 0.0, "roughnessFactor": 1.0}}
		m.merge(parts[i][2])
		materials.append(m)
		meshes.append({"name": name, "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": i}]})
		nodes.append({"name": name, "mesh": i, "translation": [(i - 1) * 1.8, 0.9, 0]})

	return {
		"asset": {"version": "2.0", "generator": "PSX test model script"},
		"scene": 0,
		"scenes": [{"nodes": [0, 1, 2]}],
		"nodes": nodes, "meshes": meshes, "materials": materials, "textures": textures, "images": images,
		"samplers": [{"magFilter": 9728, "minFilter": 9728}],
		"buffers": [{"byteLength": buf.size(), "uri": "data:application/octet-stream;base64," + Marshalls.raw_to_base64(buf)}],
		"bufferViews": [
			{"buffer": 0, "byteOffset": 0, "byteLength": 48, "target": 34962},
			{"buffer": 0, "byteOffset": 48, "byteLength": 48, "target": 34962},
			{"buffer": 0, "byteOffset": 96, "byteLength": 32, "target": 34962},
			{"buffer": 0, "byteOffset": 128, "byteLength": 12, "target": 34963}],
		"accessors": [
			{"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [-0.8, -0.8, 0], "max": [0.8, 0.8, 0]},
			{"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
			{"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
			{"bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR"}],
	}
