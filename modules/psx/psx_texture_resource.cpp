#include "psx_texture_resource.h"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"

#include <cmath>

namespace {

struct BakedEntry {
	std::vector<psx::BakedTexture> textures;
	uint32_t version = 0;
};

struct BakedRegistry {
	Mutex mutex;
	HashMap<RID, BakedEntry> entries;
	uint32_t next_version = 1;
};

BakedRegistry &registry() {
	static BakedRegistry r;
	return r;
}

} // namespace

void PSXTexture::_register() {
	_unregister();
	std::vector<psx::BakedTexture> textures;
	if (baked_bytes.is_empty() || !psx::read_baked_textures(baked_bytes.ptr(), (size_t)baked_bytes.size(), textures)) {
		if (!baked_bytes.is_empty()) {
			WARN_PRINT(vformat("PSX: the baked data of '%s' cannot be read; it is converted when drawn instead. Reimport it.", get_path()));
		}
		return;
	}
	const RID rid = get_rid();
	BakedRegistry &r = registry();
	MutexLock lock(r.mutex);
	BakedEntry &e = r.entries[rid];
	e.textures = std::move(textures);
	e.version = r.next_version++;
	registered = rid;
}

void PSXTexture::_unregister() {
	if (!registered.is_valid()) {
		return;
	}
	BakedRegistry &r = registry();
	MutexLock lock(r.mutex);
	r.entries.erase(registered);
	registered = RID();
}

Ref<PSXTexture> PSXTexture::bake(const Ref<Image> &p_image, int p_max_size, int p_depth, int p_max_colors, float p_alpha_cut, bool p_preview) {
	ERR_FAIL_COND_V(p_image.is_null() || p_image->is_empty(), Ref<PSXTexture>());
	Ref<Image> image = p_image->duplicate();
	if (image->is_compressed()) {
		image->decompress();
		ERR_FAIL_COND_V_MSG(image->is_compressed(), Ref<PSXTexture>(), "PSX: cannot decompress the image.");
	}
	image->clear_mipmaps();
	image->convert(Image::FORMAT_RGBA8);

	psx::TextureOptions opt;
	opt.max_size = CLAMP(p_max_size > 0 ? p_max_size : (int)GLOBAL_GET("rendering/psx/textures/max_size"), 8, 256);
	opt.depth = (uint8_t)CLAMP(p_depth >= 0 ? p_depth : (int)GLOBAL_GET("rendering/psx/textures/depth"), 0, 3);
	opt.max_colors = CLAMP(p_max_colors >= 0 ? p_max_colors : (int)GLOBAL_GET("rendering/psx/textures/max_colors"), 0, 256);
	const float cut = CLAMP(p_alpha_cut, 0.0f, 1.0f);

	// One variant per way a material can use alpha (only opaque when the
	// image has none).
	const bool has_alpha = image->detect_alpha() != Image::ALPHA_NONE;
	const uint8_t modes[3] = { psx::ALPHA_IGNORE, psx::ALPHA_CUT, psx::ALPHA_BLEND };
	const Vector<uint8_t> rgba = image->get_data();
	std::vector<psx::BakedTexture> baked;
	for (int i = 0; i < (has_alpha ? 3 : 1); i++) {
		psx::BakedTexture b;
		b.alpha_mode = modes[i];
		b.alpha_cut = cut;
		psx::TextureOptions o = opt;
		o.alpha_mode = modes[i];
		o.alpha_cut = cut;
		ERR_FAIL_COND_V(!psx::convert_texture(rgba.ptr(), image->get_width(), image->get_height(), o, b.data), Ref<PSXTexture>());
		baked.push_back(std::move(b));
	}
	std::vector<uint8_t> bytes;
	psx::write_baked_textures(baked, bytes);
	PackedByteArray packed;
	packed.resize((int64_t)bytes.size());
	memcpy(packed.ptrw(), bytes.data(), bytes.size());

	Ref<Image> shown = image;
	if (p_preview) {
		// The last variant carries the alpha a cut-out or blended material
		// would see (0, half or full); without alpha it is the opaque one.
		const psx::TextureData &t = baked.back().data;
		std::vector<uint8_t> decoded;
		psx::decode_texture(t, true, decoded);
		Vector<uint8_t> data;
		data.resize((int64_t)decoded.size());
		memcpy(data.ptrw(), decoded.data(), decoded.size());
		shown = Image::create_from_data(t.width, t.height, false, Image::FORMAT_RGBA8, data);
	}

	Ref<PSXTexture> tex;
	tex.instantiate();
	tex->set_image(shown);
	tex->set_baked_data(packed);
	return tex;
}

void PSXTexture::set_baked_data(const PackedByteArray &p_bytes) {
	baked_bytes = p_bytes;
	_register();
	emit_changed();
}

PackedByteArray PSXTexture::get_baked_data() const {
	return baked_bytes;
}

String PSXTexture::get_baked_summary() const {
	std::vector<psx::BakedTexture> textures;
	if (baked_bytes.is_empty() || !psx::read_baked_textures(baked_bytes.ptr(), (size_t)baked_bytes.size(), textures) || textures.empty()) {
		return "Not baked";
	}
	const psx::TextureData &t = textures[0].data;
	const char *depth = t.depth == psx::TEXTURE_4BIT ? "4-bit" : (t.depth == psx::TEXTURE_8BIT ? "8-bit" : "15-bit");
	return vformat("%dx%d %s, %d colours, %d alpha variants", t.width, t.height, depth, t.colors, (int)textures.size());
}

static const psx::BakedTexture *find_variant(const BakedEntry &p_entry, uint8_t p_alpha_mode, float p_alpha_cut) {
	for (const psx::BakedTexture &b : p_entry.textures) {
		if (b.alpha_mode != p_alpha_mode) {
			continue;
		}
		if (p_alpha_mode == psx::ALPHA_CUT && std::abs(b.alpha_cut - p_alpha_cut) > 1.0f / 512.0f) {
			continue;
		}
		return &b;
	}
	return nullptr;
}

bool PSXTexture::find_baked(RID p_texture, uint8_t p_alpha_mode, float p_alpha_cut, psx::TextureData &r_data, uint32_t &r_version) {
	BakedRegistry &r = registry();
	MutexLock lock(r.mutex);
	const BakedEntry *e = r.entries.getptr(p_texture);
	const psx::BakedTexture *b = e ? find_variant(*e, p_alpha_mode, p_alpha_cut) : nullptr;
	if (!b) {
		return false;
	}
	r_data = b->data;
	r_version = e->version;
	return true;
}

uint32_t PSXTexture::get_baked_version(RID p_texture, uint8_t p_alpha_mode, float p_alpha_cut) {
	BakedRegistry &r = registry();
	MutexLock lock(r.mutex);
	const BakedEntry *e = r.entries.getptr(p_texture);
	return (e && find_variant(*e, p_alpha_mode, p_alpha_cut)) ? e->version : 0;
}

void PSXTexture::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_baked_data", "bytes"), &PSXTexture::set_baked_data);
	ClassDB::bind_method(D_METHOD("get_baked_data"), &PSXTexture::get_baked_data);
	ClassDB::bind_method(D_METHOD("get_baked_summary"), &PSXTexture::get_baked_summary);
	ClassDB::bind_static_method("PSXTexture", D_METHOD("bake", "image", "max_size", "depth", "max_colors", "alpha_cut", "preview"), &PSXTexture::bake, DEFVAL(0), DEFVAL(-1), DEFVAL(-1), DEFVAL(0.5f), DEFVAL(true));
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "baked_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_INTERNAL), "set_baked_data", "get_baked_data");
}

PSXTexture::~PSXTexture() {
	_unregister();
}
