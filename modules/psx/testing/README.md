# PSX testing

One script, `psx_test.ps1`, builds the engine, runs the `psx_core` unit tests,
runs test scenes with PSX on or off, and compares frames against golden images.
Run it from the repo root in PowerShell.

```powershell
.\modules\psx\testing\psx_test.ps1 all          # build, unit tests, golden check
.\modules\psx\testing\psx_test.ps1 unit         # psx_core only, seconds, no Godot build
.\modules\psx\testing\psx_test.ps1 run -Mode both -Stats
.\modules\psx\testing\psx_test.ps1 review       # side-by-side sheet: raw | composite | PSX off
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
| `demo` | The built-in cube (`demo.override.cfg` turns on `debug/demo_scene`) |
| `lighting` | Sun, omni and spot lights, ambient; default, coloured, alpha, emissive, metallic, unshaded and additive materials |
| `lighting_godot` | The same scene with `lighting_mode` = Godot Match |
| `fog_multimesh` | Depth fog, sky fog, a MultiMesh grid and a large subdivided ground |
| `ortho` | An orthographic camera |
| `skinned` | A two-bone mesh built in code, bent every frame (CPU skinning) |
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

## Unit tests

`unit` compiles `unit/test_psx_core.cpp` with every `psx_core/*.cpp` (MSVC,
`/W4 /WX`) and runs it: trig, the UNR divide, RTPS/RTPT/NCLIP/AVSZ and FLAG,
GTE lighting (NCS/NCCS/NCDS/DPCS), mesh conversion, subdivision, alpha
classes, `draw_mesh` (culling, near/far, lighting modes, emission, fog,
point lights), coverage and fill rule, size limits and scaled VRAM, dither, blend modes,
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
| `rendering/psx/vram_scale` | VRAM is (1024 N) x (512 N); 1 is the hardware (restart needed) |
| `rendering/psx/resolution_width` / `_height` | Display size, default 320 x 240 (restart needed) |
| `rendering/psx/world_scale` | GTE units per metre, default 512: far limit 65535 / scale metres, near limit H / 2 units |
| `rendering/psx/subdivide_size` | Split polygons until no edge is longer than this (metres, default 1, 0 = off) |
| `rendering/psx/lighting_mode` | PS1 (display colour times light, as the GTE does) or Godot Match (linear, matches Godot's brightness) |
| `rendering/psx/default_ambient` | Ambient light without a WorldEnvironment |
| `rendering/psx/debug/demo_scene` | Draw the built-in test cube instead of the scene |
| `rendering/psx/debug/dump_dir` | Folder to save raw PSX frames to |
| `rendering/psx/debug/dump_frames` | How many frames to save, from frame 0 |
| `rendering/psx/debug/print_stats` | One `PSX_STATS` and one `PSX_SCENE` line per frame: timings, instances, triangles in and culled by reason |

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
- Particles, blend shapes and textures are not drawn yet.
