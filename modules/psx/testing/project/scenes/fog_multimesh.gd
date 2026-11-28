extends MultiMeshInstance3D

## Fills the MultiMesh with a grid of boxes receding into the fog.

func _ready() -> void:
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.mesh = BoxMesh.new()
	var cols := 7
	var rows := 12
	mm.instance_count = cols * rows
	for z in rows:
		for x in cols:
			var height := 0.6 + 0.15 * ((x * 7 + z * 3) % 5)
			var basis := Basis(Vector3.UP, (x + z) * 0.3).scaled(Vector3(0.6, height, 0.6))
			mm.set_instance_transform(z * cols + x, Transform3D(basis, Vector3((x - (cols - 1) * 0.5) * 2.0, height * 0.5, -z * 2.5)))
	multimesh = mm
