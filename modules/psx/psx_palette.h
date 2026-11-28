// PSXPalette: a palette shared by several textures, as PS1 games shared
// CLUTs (design doc, "Shared palettes").
//
// A palette is up to 15 or 255 colours for a 16- or 256-entry CLUT: entry 0
// stays transparent, so textures with and without cut-outs use the same
// CLUT. Textures are baked with it (the PSX Texture importer's psx/palette,
// PSXTexture.bake) or converted to it when drawn (PSXMaterial3D
// psx_palette); either way their CLUTs are the same and share one slot in
// texture memory. A material whose palette differs from the one its baked
// texture was made with, but has the same size, swaps it in: the texels
// stay, only the colours change.
//
// Every palette registers its colours under its object ID, so the renderer
// (render thread) can read them; the version changes with every edit.

#pragma once

#include "core/io/resource.h"
#include "core/os/mutex.h"
#include "core/variant/typed_array.h"

#include <cstdint>
#include <vector>

class Image;
class Texture2D;

// What the renderer reads: colours as 15-bit keys.
struct PSXPaletteData {
	std::vector<uint16_t> keys; // At most entries - 1
	int32_t entries = 16; // CLUT size: 16 or 256
	uint32_t version = 0; // 0 = no palette
};

class PSXPalette : public Resource {
	GDCLASS(PSXPalette, Resource);

public:
	enum PaletteSize {
		SIZE_16, // 4-bit textures, up to 15 colours
		SIZE_256, // 8-bit textures, up to 255 colours
	};

private:
	PaletteSize size = SIZE_16;
	PackedColorArray colors;
	TypedArray<Texture2D> source_textures;

	void _register();

protected:
	static void _bind_methods();
	bool _get(const StringName &p_name, Variant &r_ret) const;
	void _get_property_list(List<PropertyInfo> *p_list) const;
	void _validate_property(PropertyInfo &p_property) const;

public:
	void set_size(PaletteSize p_size);
	PaletteSize get_size() const;
	// The CLUT's entries: 16 or 256.
	int get_entries() const;
	// The most colours the palette holds: 15 or 255.
	int get_max_colors() const;

	// sRGB; alpha is not used. Colours past get_max_colors() are dropped.
	void set_colors(const PackedColorArray &p_colors);
	PackedColorArray get_colors() const;

	// Textures generate_from_sources reads (kept, to generate again later).
	void set_source_textures(const TypedArray<Texture2D> &p_textures);
	TypedArray<Texture2D> get_source_textures() const;

	// Replaces the colours with the median cut over every opaque pixel of
	// the images together (alpha of at least 0.5). Returns false when they
	// have none.
	bool generate_from_images(const TypedArray<Image> &p_images);
	// The same over source_textures (the inspector's Generate button).
	bool generate_from_sources();

	// The colours as 15-bit keys, as the renderer and importer use them.
	void get_data(PSXPaletteData &r_data) const;
	// Thread-safe lookup by object ID for the renderer.
	static bool find(ObjectID p_id, PSXPaletteData &r_data);

	PSXPalette();
	~PSXPalette();
};

VARIANT_ENUM_CAST(PSXPalette::PaletteSize);
