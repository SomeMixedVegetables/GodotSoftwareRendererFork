#include "psx_palette.h"

#include "psx_core/psx_texture.h"

#include "core/io/image.h"
#include "core/object/class_db.h"
#include "core/templates/hash_map.h"
#include "scene/resources/texture.h"

namespace {

struct PaletteRegistry {
	Mutex mutex;
	HashMap<ObjectID, PSXPaletteData> entries;
	uint32_t next_version = 1;
};

PaletteRegistry &registry() {
	static PaletteRegistry r;
	return r;
}

uint16_t color_key(const Color &p_color) {
	uint16_t key = 0;
	const float ch[3] = { p_color.r, p_color.g, p_color.b };
	for (int c = 0; c < 3; c++) {
		const uint32_t v8 = (uint32_t)CLAMP(Math::round(ch[c] * 255.0f), 0.0f, 255.0f);
		key |= (uint16_t)(((v8 * 31 + 127) / 255) << (c * 5));
	}
	return key;
}

Color key_color(uint16_t p_key) {
	float ch[3];
	for (int c = 0; c < 3; c++) {
		const uint32_t v = (p_key >> (c * 5)) & 31;
		ch[c] = (float)((v << 3) | (v >> 2)) / 255.0f;
	}
	return Color(ch[0], ch[1], ch[2]);
}

} // namespace

void PSXPalette::_register() {
	PSXPaletteData data;
	get_data(data);
	PaletteRegistry &r = registry();
	MutexLock lock(r.mutex);
	data.version = r.next_version++;
	r.entries[get_instance_id()] = std::move(data);
}

void PSXPalette::set_size(PaletteSize p_size) {
	size = p_size;
	if (colors.size() > get_max_colors()) {
		colors.resize(get_max_colors());
	}
	_register();
	emit_changed();
}

PSXPalette::PaletteSize PSXPalette::get_size() const {
	return size;
}

int PSXPalette::get_entries() const {
	return size == SIZE_256 ? 256 : 16;
}

int PSXPalette::get_max_colors() const {
	return get_entries() - 1;
}

void PSXPalette::set_colors(const PackedColorArray &p_colors) {
	colors = p_colors;
	if (colors.size() > get_max_colors()) {
		WARN_PRINT(vformat("PSX: a palette of %d entries holds up to %d colours (entry 0 is transparent); the rest are dropped.", get_entries(), get_max_colors()));
		colors.resize(get_max_colors());
	}
	_register();
	emit_changed();
}

PackedColorArray PSXPalette::get_colors() const {
	return colors;
}

void PSXPalette::set_source_textures(const TypedArray<Texture2D> &p_textures) {
	source_textures = p_textures;
}

TypedArray<Texture2D> PSXPalette::get_source_textures() const {
	return source_textures;
}

bool PSXPalette::generate_from_images(const TypedArray<Image> &p_images) {
	LocalVector<Ref<Image>> images;
	std::vector<psx::PaletteSource> sources;
	for (int i = 0; i < p_images.size(); i++) {
		Ref<Image> src = p_images[i];
		if (src.is_null() || src->is_empty()) {
			continue;
		}
		Ref<Image> image = src->duplicate();
		if (image->is_compressed()) {
			image->decompress();
			if (image->is_compressed()) {
				WARN_PRINT("PSX: a compressed image cannot be decompressed; it is left out of the palette.");
				continue;
			}
		}
		image->clear_mipmaps();
		image->convert(Image::FORMAT_RGBA8);
		images.push_back(image);
	}
	// Images hold their pixels until the palette is made.
	LocalVector<Vector<uint8_t>> pixels;
	for (const Ref<Image> &image : images) {
		pixels.push_back(image->get_data());
	}
	for (uint32_t i = 0; i < images.size(); i++) {
		psx::PaletteSource s;
		s.rgba = pixels[i].ptr();
		s.width = images[i]->get_width();
		s.height = images[i]->get_height();
		sources.push_back(s);
	}
	std::vector<uint16_t> keys;
	if (!psx::make_palette(sources.data(), (int32_t)sources.size(), get_max_colors(), keys)) {
		return false;
	}
	PackedColorArray result;
	for (uint16_t key : keys) {
		result.push_back(key_color(key));
	}
	set_colors(result);
	notify_property_list_changed();
	return true;
}

bool PSXPalette::generate_from_sources() {
	TypedArray<Image> images;
	for (int i = 0; i < source_textures.size(); i++) {
		const Ref<Texture2D> tex = source_textures[i];
		if (tex.is_valid()) {
			const Ref<Image> image = tex->get_image();
			if (image.is_valid()) {
				images.push_back(image);
			}
		}
	}
	ERR_FAIL_COND_V_MSG(images.is_empty(), false, "PSX: add textures to source_textures to generate the palette from.");
	const bool ok = generate_from_images(images);
	ERR_FAIL_COND_V_MSG(!ok, false, "PSX: the source textures have no opaque pixels to make a palette from.");
	return true;
}

void PSXPalette::get_data(PSXPaletteData &r_data) const {
	r_data.keys.clear();
	r_data.entries = get_entries();
	for (int i = 0; i < MIN(colors.size(), get_max_colors()); i++) {
		r_data.keys.push_back(color_key(colors[i]));
	}
	PaletteRegistry &r = registry();
	MutexLock lock(r.mutex);
	const PSXPaletteData *e = r.entries.getptr(get_instance_id());
	r_data.version = e ? e->version : 0;
}

bool PSXPalette::find(ObjectID p_id, PSXPaletteData &r_data) {
	PaletteRegistry &r = registry();
	MutexLock lock(r.mutex);
	const PSXPaletteData *e = r.entries.getptr(p_id);
	if (!e || e->keys.empty()) {
		return false;
	}
	r_data = *e;
	return true;
}

// The Generate button: a tool-button property the inspector calls.
bool PSXPalette::_get(const StringName &p_name, Variant &r_ret) const {
	if (p_name == SNAME("generate")) {
		r_ret = Callable(const_cast<PSXPalette *>(this), SNAME("generate_from_sources"));
		return true;
	}
	return false;
}

void PSXPalette::_get_property_list(List<PropertyInfo> *p_list) const {
	p_list->push_back(PropertyInfo(Variant::CALLABLE, "generate", PROPERTY_HINT_TOOL_BUTTON, "Generate from Source Textures,ColorPick", PROPERTY_USAGE_EDITOR));
}

void PSXPalette::_validate_property(PropertyInfo &p_property) const {
	if (p_property.name == "colors") {
		p_property.hint = PROPERTY_HINT_COLOR_NO_ALPHA;
	}
}

void PSXPalette::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_size", "size"), &PSXPalette::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &PSXPalette::get_size);
	ClassDB::bind_method(D_METHOD("get_entries"), &PSXPalette::get_entries);
	ClassDB::bind_method(D_METHOD("get_max_colors"), &PSXPalette::get_max_colors);
	ClassDB::bind_method(D_METHOD("set_colors", "colors"), &PSXPalette::set_colors);
	ClassDB::bind_method(D_METHOD("get_colors"), &PSXPalette::get_colors);
	ClassDB::bind_method(D_METHOD("set_source_textures", "textures"), &PSXPalette::set_source_textures);
	ClassDB::bind_method(D_METHOD("get_source_textures"), &PSXPalette::get_source_textures);
	ClassDB::bind_method(D_METHOD("generate_from_images", "images"), &PSXPalette::generate_from_images);
	ClassDB::bind_method(D_METHOD("generate_from_sources"), &PSXPalette::generate_from_sources);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "size", PROPERTY_HINT_ENUM, "16 Entries (4-bit),256 Entries (8-bit)"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_COLOR_ARRAY, "colors"), "set_colors", "get_colors");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "source_textures", PROPERTY_HINT_ARRAY_TYPE, "Texture2D"), "set_source_textures", "get_source_textures");

	BIND_ENUM_CONSTANT(SIZE_16);
	BIND_ENUM_CONSTANT(SIZE_256);
}

PSXPalette::PSXPalette() {
	_register();
}

PSXPalette::~PSXPalette() {
	PaletteRegistry &r = registry();
	MutexLock lock(r.mutex);
	r.entries.erase(get_instance_id());
}
