def can_build(env, platform):
    return True


def configure(env):
    pass


def get_doc_classes():
    return [
        "PSX",
        "PSXMaterial3D",
        "PSXSettings",
        "PSXTexture",
        "ResourceImporterPSXTexture",
    ]


def get_doc_path():
    return "doc_classes"
