# PSX testing

One script, `psx_test.ps1`, builds the engine, runs the `psx_core` unit tests,
runs test scenes with PSX on or off, and compares frames against golden images.
Run it from the repo root in PowerShell.

```powershell
.\modules\psx\testing\psx_test.ps1 all          # build, unit tests, golden check
.\modules\psx\testing\psx_test.ps1 unit         # psx_core only, seconds, no Godot build
.\modules\psx\testing\psx_test.ps1 run -Mode both -Stats
.\modules\psx\testing\psx_test.ps1 review       # side-by-side sheet: raw | composite | PSX off
.\modules\psx\testing\psx_test.ps1 check -Renderer mobile   # the same goldens on the Mobile renderer
.\modules\psx\testing\psx_test.ps1 check        # compare against golden/
.\modules\psx\testing\psx_test.ps1 bless        # accept current output as golden
.\modules\psx\testing\psx_test.ps1 build -Dev   # dev build with symbols, for crash traces
```

Options: `-Scene <name>` (default: every scene in `project/scenes/`), `-Frames 40`,
`-GoldenFrame 30`, `-Tolerance 0` for raw frames, `-CompositeTolerance 2` for
window frames, `-Stats`, `-Dev`, `-ModelsDir <folder>`, `-ModelsPerCategory 2`.

## Scenes

| Scene | Covers |
| --- | --- |
| `basic` | A box and a sun, with a 2D label over the 3D view |
| `blend_modes` | PSXMaterial3D `psx_blend` over black, grey and white bands: Material (opaque), Half, Add, Subtract and Add Quarter, on flat and textured quads. Godot draws them opaque (the setting is PS1-only) |
| `controls` | The controls: a PSXSettings node turns dither off; one sphere's PSXMaterial3D turns it back on; one box's turns subdivision off; the script sets `default_ambient` through `PSX.set_setting` and must print `PSX_CONTROLS result=pass` |
| `controls_off` | A PSXSettings node turns the PS1 view off: Godot's renderer draws. Checks that no PSX frames were made (no goldens) and the script's `PSX_CONTROLS` line |
| `demo` | The built-in cube (`demo.override.cfg` turns on `debug/demo_scene`) |
| `near_plane` | A low camera over a floor and beside a wall that both pass behind it: their triangles are clipped at the near plane, so they reach the screen edges with no holes or cracks. One floor has subdivision off (one big quad) |
| `lighting` | Sun, omni and spot lights, ambient; default, coloured, alpha, emissive, metallic, unshaded and additive materials |
| `lighting_godot` | The same scene with `lighting_mode` = Godot Match |
| `gltf_runtime` | `models/psx_quads_runtime.gltf`: three quads with embedded textures (opaque, alpha-scissor at 0.3, checker), converted when drawn |
| `gltf_baked` | The same model imported with `psx/bake_textures` on (checker set to Never). Checked against `gltf_runtime`'s goldens and must report exactly 2 baked textures |
| `gltf_shader_runtime`, `gltf_shader_baked` | The same model imported with ShaderMaterials (a `source_color` sampler; the `psx_test_gltf_shader` editor plugin's glTF extension makes them), converted when drawn and baked on import. Both are checked against `gltf_runtime`'s goldens; the baked one must report exactly 2 baked textures |
| `fog_multimesh` | Depth fog, sky fog, a MultiMesh grid and a large subdivided ground |
| `ortho` | An orthographic camera |
| `overlay` | A ring on layer 21 (where the editor puts gizmos and the grid): Godot's renderer draws it over the PS1 frame, in the composite only. Run with `-Renderer mobile` to check the overlay on Mobile |
| `particles` | Particle billboards on a MultiMesh with custom data (as CPUParticles3D makes them): each arrow turned by its custom data X, as in Godot |
| `palettes` | PSXPalette: three textures baked with one generated 16-entry palette (one with cut-out holes), the first with the palette swapped per material (same, warm, grey), and a runtime texture converted to the palette next to one with its own. The check also reads `PSX_TEXTURES`: 5 CLUTs and 5 shared slots (one CLUT for the palette, texels shared by the swaps) |
| `texture_dither` | Ordered texture dither: a gradient with an 8-colour palette, a dark gradient at 15-bit and a gradient with a fixed 4-colour PSXPalette, baked without (top) and with (middle) dither; runtime textures dithered by `rendering/psx/textures/dither` (bottom, set in its `.override.cfg`) |
| `gpu_particles` | GPUParticles3D built from nodes: billboards coloured by a ramp (local coordinates), spinning cubes (global coordinates) and an emitter with two draw passes, fixed seed. Also fails if the last `PSX_CONVERT` line reports no particles drawn (`particles_drawn`) |
| `particles_anim` | Particle animation frames (a 4 x 2 sheet, frames picked by custom data Z, clamped and looped) and per-instance MultiMesh colours on a material that uses vertex colour |
| `skinned` | A two-bone mesh built in code, bent every frame (CPU skinning). Also fails if the mesh is read back from Godot again after its first frame (`meshes_settled`) |
| `blend_shapes` | Meshes with two blend shapes built in code, weights changed every frame (CPU blend shapes): normalized (left), relative (middle), and on a skinned mesh (right). Also fails if a mesh is read back again after its first frame (`meshes_settled`) |
| `sprites` | Sprite3D, AnimatedSprite3D and Label3D, with and without billboarding, and a billboard material |
| `shaders` | ShaderMaterial: colour and texture uniforms, unshaded, alpha, vertex displacement, computed colour |
| `split` | Two SubViewports on one world (split screen) and a third with its own world |
| `texture_stress` | Two viewports on one world need 24 textures of 128 x 128 at 15-bit with one bank, more than VRAM holds. Fails unless the last frame of each viewport uploads nothing (no thrashing); the 4 that do not fit are drawn in their average colour |
| `texture_update` | Textures changed at frame 10: an ImageTexture by `update()`, one by `set_image()` with a bigger image, and a ViewportTexture whose SubViewport changes. All three must show their new images |
| `textured` | Procedural textures: repeating floor, a 256 x 256 wall split along tile edges, alpha scissor, alpha blend, a clamped decal, unshaded, fog and an omni light |
| `textured_baked` | `textured` with every texture baked by `PSXTexture.bake` (what the PSX Texture importer does). Checked against `textured`'s goldens: baking must not change a pixel. Also fails unless `PSX_TEXTURES` reports baked textures in use |
| `ext_models` | Third-party models from `-ModelsDir` (see below) |

`<scene>.override.cfg` next to a scene adds `[rendering]` settings for that
scene's runs only.

## Third-party test models

```powershell
.\modules\psx\testing\psx_test.ps1 review -Scene ext_models -ModelsDir "<folder of model folders>"
```

`-ModelsDir` copies the first `-ModelsPerCategory` `.glb` files (by name) from
each top-level folder into `project/external_models/` and re-imports the
project. The project's default scene import script
(`tools/vertex_color_albedo_import.gd`) turns on vertex colours as albedo for
glTF files that colour with `COLOR_0`. Scenes named `ext_*` need these models
and are skipped without them. The models, their import files and their golden
images (`golden_external/`) are git-ignored: **never commit third-party
models or renders of them.**

## What each check means

| Image | Source | Default tolerance | Catches |
| --- | --- | --- | --- |
| Raw | `psx/psx_NNNN.png`: the PSX framebuffer at native resolution, before upscaling | 0 (bit-exact) | Any change in the PS1 core's output |
| Composite | `movie/frameNNNNNNNN.png`: the window, 3D plus Godot's 2D | 2 per channel | Upload, upscale, clear and canvas problems |

Frame N of the raw dumps and frame N of the movie are the same moment.

Some scenes add checks: `baked_in_use` (the baked texture count),
`script` (a line the scene's script prints), `psx_off` (no PSX frames
when the scene turns the PS1 view off), `textures_settled` (no texture
uploads in each viewport's last frame), `meshes_settled` (no mesh read
back from Godot in the last two frames: `skinned`, `blend_shapes`) and
`particles_drawn` (the last frame drew GPUParticles3D particles: `gpu_particles`).

## Unit tests

`unit` compiles `unit/test_psx_core.cpp` with every `psx_core/*.cpp` (MSVC,
`/W4 /WX`) and runs it: trig, the UNR divide, RTPS/RTPT/NCLIP/AVSZ and FLAG,
GTE lighting (NCS/NCCS/NCDS/DPCS), mesh conversion, subdivision, alpha
classes, `draw_mesh` (culling, near/far, lighting modes, emission, fog,
point lights), texture conversion (resampling, 4/8-bit palettes, alpha, ordered dither),
baked textures (round trip, refusing bad data, decoding), the VRAM allocator, textured meshes (tile shift, texture window, texture
banks, the fog pass), coverage and fill rule, size limits and scaled VRAM, dither, blend modes,
modulation, transparency, CLUTs, texture window, mask bit, fills and copies,
lines, the ordering table, frame layout and the demo. It also saves demo
frames as PNG to `bin\psx_test_out\unit\demo\`, rendered without Godot or a
window.

## Output

Everything goes to `bin\psx_test_out\` (git-ignored), per scene and mode:
`log.txt`, `stderr.txt`, `summary.json`, `movie\`, `psx\`, and `diff_*.png`
when a check fails (differing pixels in red over the dimmed frame).

All result lines start with `PSX_` (`PSX_RUN`, `PSX_CHECK`, `PSX_DIFF`,
`PSX_STATS`, `PSX_DONE` ...), so they are easy to grep. Exit code 0 = pass.

## Debug settings

The runner writes these into `project/override.cfg` for each run; they also
work in any project:

| Setting | Effect |
| --- | --- |
| `rendering/psx/enabled` | Use the PSX renderer (restart needed) |
| `rendering/psx/active` | Draw 3D with it; off shows Godot's own 3D (live) |
| `rendering/psx/dither` | The GPU's 4x4 ordered dither (live) |
| `rendering/psx/vram_scale` | VRAM is (1024 N) x (512 N); 1 is the hardware (restart needed) |
| `rendering/psx/resolution_width` / `_height` | Display size, default 320 x 240 (restart needed) |
| `rendering/psx/world_scale` | GTE units per metre, default 512: far limit 65535 / scale metres, near limit H / 2 units |
| `rendering/psx/subdivide_size` | Split polygons until no edge is longer than this (metres, default 1, 0 = off) |
| `rendering/psx/lighting_mode` | PS1 (display colour times light, as the GTE does) or Godot Match (linear, matches Godot's brightness) |
| `rendering/psx/default_ambient` | Ambient light without a WorldEnvironment |
| `rendering/psx/textures/max_size` | Largest texture side, 8 to 256 (the PS1's texture page limit); bigger textures are scaled down |
| `rendering/psx/textures/depth` | Auto (4-bit when 15 colours or fewer, else 8-bit), or force 4-bit, 8-bit or 15-bit |
| `rendering/psx/textures/max_colors` | Palette limit for 4/8-bit textures (0 = no limit beyond the depth's) |
| `rendering/psx/textures/vram_banks` | Texture memory, in 1024 x 512 banks: 1 is the PS1, default Unlimited (see below) |
| `rendering/psx/debug/demo_scene` | Draw the built-in test cube instead of the scene |
| `rendering/psx/debug/dump_dir` | Folder to save raw PSX frames to |
| `rendering/psx/debug/dump_frames` | How many frames to save, from frame 0 |
| `rendering/psx/debug/print_stats` | One `PSX_STATS` and one `PSX_SCENE` line per frame: timings, instances, triangles in and culled by reason. `PSX_CONVERT`: time spent reading meshes and images back from Godot, converting them, and skinning, with counts |

## Texture memory

Textures are converted when first drawn (or at import, see Baked textures): scaled to a power of two, reduced to
a 4- or 8-bit palette (CLUT) and packed into VRAM next to the framebuffers.
Only the part of a texture a mesh's UVs use is kept, and identical textures
are stored once. When VRAM is full, the least recently used texture not drawn
this frame is removed; if nothing can be removed, the surface is drawn in the
texture's average colour, with a warning.

The PS1 has 1 MB of VRAM. With `vram_banks` above 1, extra 1024 x 512 texture
banks are added as needed. Bank 0 is VRAM; GP0 command E7h (an extension; the
PS1 does not have it) selects the bank that texture and CLUT reads and CPU
uploads use. Drawing always goes to VRAM. A bank holds the same layout rules
as VRAM, so a game that fits in 1 bank also fits on the hardware.

`PSX_TEXTURES` lines (with `print_stats`) report textured and windowed
triangles, textures resident (and how many of them were baked), uploads,
fallbacks, banks in use and free VRAM.

## Controls (on/off, dither and other overrides)

`rendering/psx/enabled` installs the PS1 renderer (restart). Once installed,
these settings change while running, resolved per viewport every frame,
later winning:

1. Project settings (`rendering/psx/*`), also in the PSX dock.
2. A PSXSettings node in the scene: `override_<name>` and `<name>` for each
   setting, for the World3D it is in (the editor's 3D views use it too).
3. Scripts: `PSX.set_setting("dither", false)`, `PSX.clear_setting(...)`,
   `PSX.set_active(false)`, `PSX.get_stats()`.

Then PSXMaterial3D (a StandardMaterial3D; Convert to PSXMaterial3D in the
material's menu) can set dither On or Off and subdivision Off per material.

| Setting | Cost of a change |
| --- | --- |
| `active` | None: off hands the frame to Godot's own renderer |
| `dither` | None |
| `lighting_mode`, `default_ambient` | None |
| `world_scale`, `subdivide_size` | The meshes drawn are converted again (one-frame hitch) |
| `texture_max_size`, `texture_depth`, `texture_max_colors`, `texture_dither` | The textures drawn are converted again (baked textures are not) |

Resolution, `vram_scale` and texture banks need a restart.

The PSX dock (right, next to the Inspector) shows whether the renderer is in
use, turns it on (with Restart Editor), switches the editor's views between
the PS1 picture and Godot's ("PS1 view in the editor", not saved), lists what
the open scene's PSXSettings node overrides (with a button to select it),
edits the settings above and shows the last frame's statistics. The classes
have reference pages in `doc_classes/` (and the settings in Project Settings
tooltips).

Dither is draw-mode state on the PS1 (GP0 E1h; a polygon's texpage does not
carry it): the frame's drawing environment sets the resolved value, and a
mesh whose material differs switches it around each of its triangles.

## Baked textures (PSX Texture importer)

Select an image in the FileSystem dock, set Import As to PSX Texture in the
Import dock and reimport. The texture is converted once, at import, and
saved as a `PSXTexture`: an ordinary texture to Godot, with the PS1 texels
and palettes alongside. The PSX renderer uses those directly, with no GPU
readback or conversion when the texture is first drawn.

| Option | Effect |
| --- | --- |
| `psx/max_size` | Largest side; Project Setting uses `rendering/psx/textures/max_size` |
| `psx/depth` | Auto, 4-bit, 8-bit or 15-bit; Project Setting uses `rendering/psx/textures/depth` |
| `psx/max_colors` | Palette limit (0 = the depth's); -1 uses `rendering/psx/textures/max_colors` |
| `alpha/cut_threshold` | The alpha scissor threshold the cut-out version is made for |
| `preview/ps1_look` | Godot (editor, other renderers) shows the PS1 result instead of the source image |

**Models (glTF, .blend, FBX ...).** Select the model, turn on **Psx > Bake
Textures** in the Import dock and reimport: every material's albedo texture
is baked as the scene imports, embedded or extracted, as a copy saved in the
imported scene. A ShaderMaterial the import makes (a glTF document extension,
say) bakes the sampler the renderer draws with: `texture_albedo`, else the
first `sampler2D` marked `source_color`, cut at its `alpha_scissor_threshold`
uniform. Materials swapped in after the import (`use_external`, import
scripts) are not baked; import their images as PSX Texture instead. The
dock's other Psx options are the image importer's; in
Advanced Import Settings each material has **Psx > Bake** (Scene Setting,
Always, Never) and its own size, depth and colour overrides. Cut-outs are
baked at the material's own alpha scissor threshold. An extracted image that
is itself imported as PSX Texture is left linked, with its own settings; any
other extracted image that changes later needs the model reimported.
`models/psx_quads_*.gltf` and `models/psx_shader_*.gltf` are written by `tools/make_test_models.gd` when missing (procedural textures,
no third-party content).

Images with alpha are baked three times: opaque, cut-out and blended, so any
material can use them. A material whose alpha scissor threshold differs from
`alpha/cut_threshold` (by more than 1/512) converts the texture when drawn,
as before, from the texture's image (the preview, when it is on). Scripts can
bake too: `PSXTexture.bake(image, max_size, depth, max_colors, alpha_cut,
preview)`.

## Limits

- Godot's `--headless` uses a dummy renderer, so PSX runs open a window for a
  few seconds and close it themselves (Movie Maker mode, fixed 30 fps).
  Image comparison and import do run headless.
- Golden images are tied to this machine's GPU and driver for the composite
  check. Raw frames come from the CPU path and should match anywhere.
- On 4.8 beta, `--import` exits with an access violation in Godot's editor
  code after importing successfully; the runner checks for `.godot/` instead.
- Adding a scene: put `<name>.tscn` in `project/scenes/`, run `bless -Scene <name>`,
  look at the images in `golden/<name>/`, then commit them (never for `ext_*`).

## Known differences from Godot

- No shadows, specular highlights or environment reflections (metallic
  surfaces only lose their direct diffuse light).
- Omni and spot lights are computed per vertex on the CPU, up to 4 per
  object; directional lights use the GTE's three slots. Spot cones are hard.
- Transparency is 50%, opaque or hidden per triangle (the PS1 has one alpha).
- Fog follows the GTE depth cue, a curve in 1/Z fitted to Godot's at two depths.
- Of the environment, only ambient light, the background colour and depth or
  exponential fog are read. Sky textures, glow, tonemapping, SSAO, volumetric
  fog and global illumination (SDFGI, VoxelGI, lightmaps) are ignored.
- Decals are not drawn. CPUParticles3D draws through its MultiMesh, with
  billboard spin, animation frames and instance colours. GPUParticles3D is
  simulated by Godot on the GPU and its particles are read back every frame
  (which waits for the GPU) and drawn the same way; their trails are not
  drawn.
- Triangles that cross the near plane are clipped there, and ones too big
  for the GPU are split, so the GTE and GPU limits do not drop them.
- A ViewportTexture is read back from the GPU and converted (at 15-bit) every
  frame it is drawn, which costs a GPU sync; other textures are read again
  only when they change.
- There is no depth buffer: polygons are sorted by average depth in the
  ordering table, as on the PS1, so overlapping or crossing polygons can sort
  wrongly. This is intended.
- Skinned meshes, and meshes with blend shape weights that are not zero, are
  deformed and converted again every frame on the CPU, which gets slow with
  many characters.
- Textures have no mipmaps or filtering (nearest texel, affine mapping, as
  on the PS1), so fine detail shimmers at a distance.
- Textures larger than `textures/max_size` (256 at most) are scaled down.
- Fog on textured surfaces is drawn as a second, additive pass: the texel
  fades to black and the fog colour is added on top.
- Emission is multiplied by the texel colour.
- Point lights on non-uniformly scaled meshes are approximate.
- Only VisualInstance3D layers 1-20 go through the PSX core. In the editor,
  the helpers on layers 21-32 (grid, origin axes, gizmos, selection boxes)
  are drawn by Godot's renderer over the PSX frame. There is no depth buffer
  under them, so they show through models, as if every gizmo were "on top".
- Subdivision stops once a surface reaches about 16,000 triangles, so a very
  large polygon keeps long edges.
- Billboards (BaseMaterial3D billboard modes, Sprite3D, AnimatedSprite3D,
  Label3D) are drawn by turning the instance to the camera; particle
  billboards ignore their spin.
- Material `render_priority` becomes a sort bias: each step moves a mesh one
  ordering-table bucket nearer, so it draws over coplanar meshes (Label3D's
  outline sits behind its text this way). It does not reorder meshes at
  different depths.
- ShaderMaterial: the PS1 path does not run shader code. It reads the cull,
  blend and unshaded render modes, ALPHA use, and the first `vec3`/`vec4`
  uniform and the first `sampler2D` uniform marked `source_color` as the
  albedo colour and texture (StandardMaterial3D's `albedo` and
  `texture_albedo` when present). Vertex displacement and colours computed
  in code are not drawn.
- Baked textures are used whole. Runtime conversion keeps only the part of a
  texture a mesh's UVs use (so a small crop gets more texels and its own
  palette); a baked texture keeps the palette it was imported with.
- Every viewport (split screen, SubViewports) shares one PS1 GPU and one
  texture memory; each is drawn at the PSX resolution and stretched to its
  own size.
