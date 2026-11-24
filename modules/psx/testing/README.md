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
window frames, `-Stats`, `-Dev`.

## What each check means

| Image | Source | Default tolerance | Catches |
| --- | --- | --- | --- |
| Raw | `psx/psx_NNNN.png`: the PSX framebuffer at native resolution, before upscaling | 0 (bit-exact) | Any change in the PS1 core's output |
| Composite | `movie/frameNNNNNNNN.png`: the window, 3D plus Godot's 2D | 2 per channel | Upload, upscale, clear and canvas problems |

Frame N of the raw dumps and frame N of the movie are the same moment.

## Unit tests

`unit` compiles `unit/test_psx_core.cpp` with every `psx_core/*.cpp` (MSVC,
`/W4 /WX`) and runs it: trig, the UNR divide, RTPS/RTPT/NCLIP/AVSZ and FLAG,
coverage and fill rule, size limits and scaled VRAM, dither, blend modes,
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
| `rendering/psx/debug/dump_dir` | Folder to save raw PSX frames to |
| `rendering/psx/debug/dump_frames` | How many frames to save, from frame 0 |
| `rendering/psx/debug/print_stats` | One `PSX_STATS` line per frame: raster and upload time, instance count |

## Limits

- Godot's `--headless` uses a dummy renderer, so PSX runs open a window for a
  few seconds and close it themselves (Movie Maker mode, fixed 30 fps).
  Image comparison and import do run headless.
- Golden images are tied to this machine's GPU and driver for the composite
  check. Raw frames come from the CPU path and should match anywhere.
- On 4.8 beta, `--import` exits with an access violation in Godot's editor
  code after importing successfully; the runner checks for `.godot/` instead.
- Adding a scene: put `<name>.tscn` in `project/scenes/`, run `bless -Scene <name>`,
  look at the images in `golden/<name>/`, then commit them.
