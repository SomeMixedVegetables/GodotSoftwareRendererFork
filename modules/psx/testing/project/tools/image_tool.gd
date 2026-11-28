# Image comparison and contact sheets for PSX tests. Runs in headless mode:
#
#   godot --headless --path <project> --script res://tools/image_tool.gd -- compare <expected> <actual> <diff_out> [tolerance]
#   godot --headless --path <project> --script res://tools/image_tool.gd -- sheet <out> <image> [<image> ...]
#
# compare: prints one PSX_COMPARE line. Exit 0 = match, 1 = differs, 2 = error.
#   A pixel differs when any RGBA channel differs by more than tolerance
#   (default 0: bit-exact). The diff image shows the actual frame dimmed, with
#   differing pixels in red.
# sheet: places images side by side at a common height (nearest scaling), for
#   reviewing several frames as one picture.
extends SceneTree


func _initialize() -> void:
	var args := OS.get_cmdline_user_args()
	var code := 2
	if args.size() >= 4 and args[0] == "compare":
		var tolerance := int(args[4]) if args.size() >= 5 else 0
		code = _compare(args[1], args[2], args[3], tolerance)
	elif args.size() >= 3 and args[0] == "sheet":
		code = _sheet(args[1], args.slice(2))
	else:
		printerr("PSX_TOOL usage: compare <expected> <actual> <diff_out> [tolerance] | sheet <out> <image>...")
	quit(code)


func _load_rgba8(path: String) -> Image:
	var image := Image.load_from_file(path)
	if image == null or image.is_empty():
		printerr("PSX_TOOL cannot load image: " + path)
		return null
	if image.get_format() != Image.FORMAT_RGBA8:
		image.convert(Image.FORMAT_RGBA8)
	return image


func _compare(expected_path: String, actual_path: String, diff_path: String, tolerance: int) -> int:
	var expected := _load_rgba8(expected_path)
	var actual := _load_rgba8(actual_path)
	if expected == null or actual == null:
		return 2
	if expected.get_size() != actual.get_size():
		print("PSX_COMPARE result=fail reason=size expected=%dx%d actual=%dx%d" % [expected.get_width(), expected.get_height(), actual.get_width(), actual.get_height()])
		return 1

	var a := expected.get_data()
	var b := actual.get_data()
	var diff := PackedByteArray()
	diff.resize(b.size())
	var differing := 0
	var max_delta := 0
	var first_x := -1
	var first_y := -1
	var width := actual.get_width()
	for i in range(0, b.size(), 4):
		var delta := 0
		for c in 4:
			delta = maxi(delta, absi(a[i + c] - b[i + c]))
		max_delta = maxi(max_delta, delta)
		if delta > tolerance:
			if differing == 0:
				first_x = (i / 4) % width
				first_y = (i / 4) / width
			differing += 1
			diff[i] = 255
			diff[i + 1] = 0
			diff[i + 2] = 0
		else:
			diff[i] = b[i] / 3
			diff[i + 1] = b[i + 1] / 3
			diff[i + 2] = b[i + 2] / 3
		diff[i + 3] = 255

	var result := "pass" if differing == 0 else "fail"
	if differing > 0 and not diff_path.is_empty():
		Image.create_from_data(width, actual.get_height(), false, Image.FORMAT_RGBA8, diff).save_png(diff_path)
	print("PSX_COMPARE result=%s size=%dx%d differing=%d max_delta=%d tolerance=%d first=%d,%d" % [result, width, actual.get_height(), differing, max_delta, tolerance, first_x, first_y])
	return 0 if differing == 0 else 1


func _sheet(out_path: String, paths: Array) -> int:
	var images: Array[Image] = []
	var height := 0
	for path in paths:
		var image := _load_rgba8(path)
		if image == null:
			return 2
		images.append(image)
		height = maxi(height, image.get_height())
	var gap := 8
	var total_width := gap
	for image in images:
		total_width += int(round(image.get_width() * float(height) / image.get_height())) + gap
	var sheet := Image.create_empty(total_width, height + 2 * gap, false, Image.FORMAT_RGBA8)
	sheet.fill(Color(0.12, 0.12, 0.12))
	var x := gap
	for image in images:
		var w := int(round(image.get_width() * float(height) / image.get_height()))
		if image.get_height() != height:
			image.resize(w, height, Image.INTERPOLATE_NEAREST)
		sheet.blit_rect(image, Rect2i(Vector2i.ZERO, image.get_size()), Vector2i(x, gap))
		x += w + gap
	var err := sheet.save_png(out_path)
	print("PSX_SHEET out=%s images=%d size=%dx%d" % [out_path, images.size(), sheet.get_width(), sheet.get_height()])
	return 0 if err == OK else 2
