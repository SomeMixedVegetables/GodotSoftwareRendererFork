// "PSX Texture" importer: converts an image for the PS1 core at import time
// and saves a PSXTexture (psx_texture_resource.h). Chosen per file under
// Import As in the Import dock; the standard texture importer stays the
// default for images.

#pragma once

#ifdef TOOLS_ENABLED

#include "core/io/resource_importer.h"

class ResourceImporterPSXTexture : public ResourceImporter {
	GDCLASS(ResourceImporterPSXTexture, ResourceImporter);

public:
	virtual String get_importer_name() const override { return "psx_texture"; }
	virtual String get_visible_name() const override { return "PSX Texture"; }
	virtual void get_recognized_extensions(List<String> *p_extensions) const override;
	virtual String get_save_extension() const override { return "res"; }
	virtual String get_resource_type() const override { return "PSXTexture"; }
	// Below the standard texture importer, so it is never the default.
	virtual float get_priority() const override { return 0.1f; }
	// 2: the preview keeps the source alpha (cut-outs at any threshold).
	virtual int get_format_version() const override { return 2; }

	virtual int get_preset_count() const override { return 0; }
	virtual String get_preset_name(int p_idx) const override { return String(); }

	virtual void get_import_options(const String &p_path, List<ImportOption> *r_options, int p_preset = 0) const override;
	virtual bool get_option_visibility(const String &p_path, const String &p_option, const HashMap<StringName, Variant> &p_options) const override { return true; }

	virtual Error import(ResourceUID::ID p_source_id, const String &p_source_file, const String &p_save_path, const HashMap<StringName, Variant> &p_options, List<String> *r_platform_variants, List<String> *r_gen_files = nullptr, Variant *r_metadata = nullptr) override;
};

#endif // TOOLS_ENABLED
