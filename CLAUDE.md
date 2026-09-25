# gate

A film projector's gate, shutter and print — the clip held at the projector's rate, its
light integrated through the blades, each frame weaving in the gate, and a print with
grit, dust, a hair, splices, cue dots and fading dyes — as an FFGL **effect** for
Resolume Arena/Avenue. C++/GLSL, CMake MODULE → universal `.bundle` (macOS) + Windows
`.dll`. MIT.

Read `AGENTS.md` before changing the shutter, the hold, the print or a check's tolerance.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build --parallel`
- Install into Arena: `cmake --install build` — **not run from a session**, it writes
  into `~/Documents/Resolume Arena/Extra Effects`
- Render a frame offline: `./build/gatest --out /tmp/f.png --size 1920x1080`
  (90 frames of the moving card at a synthetic 60 fps, then the last one;
  `--average` writes the mean of every frame instead)
- Set anything by name: `--set "FPS=3" --set "Blades=2" --set "Shutter Angle=0.5"`
  (0..1 for sliders, the element index for options, 1 to press Cue Dots)
- List parameters, kinds, defaults and ranges: `./build/gatest --list`
- Other sources: `--source flat --level 0.5`, `--source white`, `--source black`
- The exact GLSL the plugin compiles: `./build/gatest --dump-shaders DIR`
- Footage through the real shaders — **`--pipe`**, raw RGBA frames in, raw RGBA frames
  out, with `--size WxH`, `--fps N` (frame n is clocked at n / fps) and an optional
  `--script` of `frame Parameter Name value` cues. A slider ramps linearly between
  cues; an option, a boolean and an integer STEP (they hold the last cue at or before
  the frame); an event fires on its cue frame only. A cue naming no parameter is refused
  with exit 2, a partial frame at the end of stdin ends the stream cleanly, a failed
  render or a closed stdout exits 1 (SIGPIPE is ignored, so never 141):
  `ffmpeg -i clip.mov -vf fps=60 -f rawvideo -pix_fmt rgba - | ./build/gatest --pipe --size 1280x720 [--script cues.txt] | ffmpeg -f rawvideo -pix_fmt rgba -s 1280x720 -r 60 -i - out.mov`
  (Resolume's demo clips are 30 fps: `fps=60` shows each twice, as a 60 Hz host does)

## Verify
- Everything: `tools/verify.sh` (fresh universal build + glslc + the reserved-word grep
  + every check at 320x180 AND 1280x720 AND on the software renderer + --pipe + the
  sweep + the bundle; ~6 min on this Mac, most of it the software renderer)
- The flicker's spectrum is the shutter's Fourier series: `./build/gatest --flicker`
- The hold, the pictures per second, the pull-down in the dark: `./build/gatest --hold`
- Scratches stay put while the picture moves; side sets polarity: `./build/gatest --scratch`
- Weave sigma and lag-1 correlation, with and without shrinkage: `./build/gatest --weave`
- Fading toward magenta, each dye on its law: `./build/gatest --fade`
- The print and the held pictures survive a resize: `./build/gatest --resize`
- The checks can fail: `./build/gatest --negative`; one perturbation verbosely:
  `./build/gatest --perturb BITS --scratch` (bits in `Model.h`)
- Every check takes `--size WxH`; CI runs them at 320x180
- CI's renderer, on this Mac: `GATEST_RENDERER=software ./build/gatest --weave --size 320x180`
  (Apple's software renderer, not repeatable at the last bit; verify.sh runs every check on it)
- No name over 16 characters, none duplicated: `./build/gatest --names`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Render cost and the state held: `./build/gatest --bench`
- What a host sees: `~/Projects/resolume/oxbow/build/oxbow probe build-universal/Gate.bundle`

## Notes
- **The machine, not the look.** `Model.{h,cpp}` is the projector and the print in
  double on the CPU: the shutter's light per projector frame (closed form), the weave
  (a stationary AR(1) as a moving-average sum), the scratches, dust, hair, splices, cue
  dots, the dyes' retention and the lamps. `Shaders.cpp`'s output pass samples, prints
  and sums. A wrong projector is a fix in `Model.cpp`; a wrong print on screen, in GLSL.
- **Time is the film position, in double.** Projector frames, advanced by each display
  frame's real seconds × FPS. The exposure is the whole display period; a frame on
  which the host clock did not move reuses the last exposure's length. The shader sees
  weights and offsets, never a time.
- **The print is a pure function of the film position and a seed.** Nothing about it
  lives in a GPU buffer, so a resize cannot touch it. The cue schedule is the one piece
  of state (the frame Cue Dots was pressed on).
- **The held pictures are RGBA16F.** A value predicted from the input is off by up to
  2^-11 of it once stored; checks measure the stored picture (a white reference half,
  ratios of two renders) instead. White stores exactly.
- **Held pictures are resampled on a resize, never reallocated** — the previous frame is
  still lit through the shutter on a straddling exposure. `--resize` part 2.
- **Scratches are in the gate** (fixed on screen, the picture weaves under them); dust
  and cue dots are on the print (they weave with it); the hair is in the gate.
- **Output alpha is 1 at Mix 1** (`mix( src.a, 1, Mix )`): the projection paints the
  whole frame. Resolume's demo clips carry alpha; a transparent pixel prints as black.
- **`Perturb` bits, `Probe` and the scratch override are test hooks**, inert in the plugin.
- **Parameter names must be unique** — `--set` and the sweep find them by name.
- `SetParamInfo` clamps a STANDARD default into 0..1 before `SetParamRange` can widen
  it, so every slider is 0..1 and `Controls.cpp` holds the units, with inverses. Options
  map by index; their range reads back 0..1.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no host can
  instantiate the plugin at all.
- `gate_core` is an OBJECT library, not STATIC — the plugin registers itself from a
  file-scope constructor nothing references by name.
- `FFGLScopedFBOBinding.h` is not in the umbrella header; include it by hand.
- macOS build must be universal. Verify with `lipo`, never the build log.
- GLSL 4.10 reserved words are not identifiers: `patch sample input output filter
  common active half layout flat packed` and the rest of s3.6; `verify.sh` greps the
  dumped shaders for them. MSVC has no `M_PI` (use `model::kPi`), `far`/`near` are
  macros there, and `<cmath>` is included wherever `std::lround` is used.
- FFGL id is `GA01`, display name `SW Gate`.

## Not done yet
- **Never loaded into Resolume on macOS.** Everything numeric is measured offline against
  the real plugin class in a headless CGL context, plus an `oxbow` load. On Windows it passes
  the fleet's Arena gate (Arena 7.27.1, llvmpipe), 9/9 (see AGENTS.md).
- No OpenFX port, no factory presets, no Stock or seed control.
- The default shutter is 3 blades at 270° (photosensitivity; AGENTS.md "The three questions").
- `StoatworksAbout.h`, `ATTRIBUTIONS.md`, `.github/ISSUE_TEMPLATE/` and `.github/FUNDING.yml`
  are GENERATED by the backend's sync scripts; the user guide is `docs/USER-GUIDE.md` (PDF
  generated).

## Browser demo

`demo/` is the page at **gate-demo.stoatworks-labs.com**, deployed from
`wrangler.toml` (a Worker route over a proxied `AAAA 100::` DNS record, not a
custom domain) with `cf-run npx wrangler deploy` or by any push to main — no build
step; what is committed is what is served. `demo/vendor/` is copied in by
`~/Projects/infrastructure/stoatworks-backend/resolume-demo/sync.sh gate` and is not
a place to edit.
- **A shader or constant change in the plugin: `python3 demo/tools/sync_shaders.py`**
  (splices Shaders.cpp's strings, Shaders.h's layout, Model.h's constants and the
  Controls lists into `demo/plugin.js`), then `python3 demo/tools/check_shaders.py`
  (in verify.sh). Never hand-edit the generated block.
- The page's CPU half (Model.cpp's shutter, weave, print and lamps, Controls.cpp's
  laws, the clock, hold and print upload of `ProcessOpenGL`) is a **hand port**;
  change it by hand with the C++. Only a reader checks it. The defaults are
  hand-copied from `Gate::Gate()` too.
- Verify a deploy **by content**:
  `curl -s 'https://gate-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'`.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/gate/gate.YYYY-MM-DD.log        (macOS)
    %LOCALAPPDATA%\gate\logs\gate.YYYY-MM-DD.log   (Windows)

Frame 60 logs the host clock's raw value and the unit the voting settled on.
