# AGENTS.md — Gate

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the short
command reference; this is the *why*. Read "What is actually verified" before you tell
anybody this works.

---

## What the plugin is

A film projector's gate, shutter and print, as an FFGL 2.1 effect (`GA01`, shown as
`SW Gate`) for Resolume Arena and Avenue. C++17 + GLSL 4.10, CMake, universal macOS
`.bundle` and (in CI, never yet run) a Windows `.dll`. MIT; intended home
`github.com/stoatworks-labs/gate`. Local v0.1.0, unreleased.

Built 2026-09-25 in one session (tranche five, an idea Allan picked) from
`specs/SPEC-gate.md` and the fleet's templates: filament (built the same day) for the
plugin shape, the harness, `--pipe` with SIGPIPE ignored, the software pass and verify;
readout for the clock voting and the idea of a shutter in time; standards and cadence for
frame rates beating against display rates; rebate as the neighbour to stay distinct from
(rebate is the negative and its scan; this is the print going through a projector, and
rebate's own AGENTS lists gate weave and dust as things it does not model); tinsel for
`PassBuffer`, the sweep and CI; photofinish for the resize-mid-run trap.

---

## The one idea

**Model the machine, not the look.** A projector pulls the film down one frame at a time
with a claw, holds it in the gate, and lets the lamp through only while a shutter blade
is out of the way. The print is a physical strip. So:

| the machine | what comes out |
| --- | --- |
| the claw pulls down at the projector's rate, with the shutter closed | **the hold**: the clip changes only at pull-downs, FPS times a second (a 3:2-like judder on a 30 fps clip at 24) |
| B blades, each opening `open` of its cycle, integrated over each display frame's exposure | **flicker at the beat**: 48 Hz against 60 is a 12 Hz pulse of amplitude 0.293 at 2 blades and 180°, 0.195 at 3 blades; nothing is dimmed on purpose |
| an exposure that straddles a pull-down gets light from both frames | **the in-between frames**: two frames blended in the shutter's proportions, with their two weaves |
| clearance at the claw and the sprocket teeth, correlated by the perforations | **weave**: an AR(1) offset per frame; a shrunk print weaves more and less like the last frame |
| grit in the gate cuts the strip as it runs | **scratches that stay put**: a straight line at a fixed x on screen while the picture weaves and moves under it; base side dark, emulsion side light (green first, since magenta is the top layer) |
| dust sits on the print and rides it | **specks that weave with the picture**, for one to four frames |
| a hair caught at the aperture's edge | a silhouette **fixed in the gate**, trembling at each pull-down, until it slides out |
| a cement splice | **a jump and a flash** on one frame |
| the reel's changeover marks | **cue dots** top right: four frames, then 168 frames on, four more |
| the aperture is smaller than the printed frame | Framing off by more than 2.3% of the height shows **the frame line**, and past it the neighbouring frame |
| each dye fades at its own rate | **the magenta print**: cyan first, magenta last; the frame line and the blacks go red |
| a black-body lamp and a lens | Carbon Arc 5000 K, Xenon 6200 K, Tungsten 3200 K against a 6504 K display white; **cos⁴** falloff to the corners |

### The pipeline

1. **Clock and film** (CPU, double). `dt` is the host's real seconds since the last frame
   (readout's unit voting; clamped to [0, 0.25 s], **no floor** — filament's trap: a
   wetplate-style 1/240 s floor would integrate the wrong exposure). The film position
   `p` (projector frames) advances by `dt × FPS`. The exposure is `[p − dt × FPS, p]`; a
   frame on which the clock did not move reuses the last exposure's length.
2. **Shutter** (CPU, closed form). For each projector frame the exposure overlaps, the
   open time in the overlap, `CumulativeOpen(b) − CumulativeOpen(a)`, divided by
   `open × (p1 − p0)` so a steady picture averages to itself. At most 8 frames (0.25 s at
   25 fps).
3. **Hold** (GPU capture). On a display frame whose exposure reaches a new projector
   frame, the host's picture is sRGB-decoded into whichever of two RGBA16F held pictures
   does not hold the previous frame. Otherwise nothing is captured.
4. **Gate** (CPU). Per projector frame: the weave (a stationary AR(1), as the
   moving-average sum of its last ~70 innovations, so a pure function of the frame), a
   splice's jump, Framing.
5. **Print** (CPU). Scratches alive on any frame of the exposure (a birth/death process
   scanned back 480 frames), the dust on each frame, the hair, the cue flags. One row of
   RGBA32F texels.
6. **Output** (GPU, to the host). Per projector frame: the held picture at its offset (a
   hand-written bilinear of `texelFetch`es), or the frame line, or the neighbour frame;
   density `−log2 T`, times the dyes' retention, times a splice's wash; emulsion
   scratches take density off layer by layer; `2^−D`; base scratches, dust, cue dots and
   the hair take light off. The weighted sum, the lamp, cos⁴, the sRGB encode, Mix.

---

## The shape of the code

| File | What it is |
| --- | --- |
| `source/Model.{h,cpp}` | The projector and the print in double: the shutter, the segments, the weave law and offsets, scratches, dust, the hair, splices, cues, the dyes' retention, the lamps' colours, the `Perturb` bits and the `Probe`. Every constant with its reason. |
| `source/Controls.{h,cpp}` | What a 0..1 slider means, with inverses; the option lists. |
| `source/Shaders.{h,cpp}` | Capture, resample and output. |
| `source/PassBuffer.*` | tinsel's FFGLFBO with the leak fixed, wetplate's Swap. |
| `source/Gate.{h,cpp}` | The plugin: parameters, the clock, the film position, the hold, the print data, the passes, the test hooks. |
| `tools/gatest/` | The harness: renders, measures, negative controls, benchmarks, pipes, dumps shaders. |
| `tools/sweep.py`, `tools/verify.sh` | No dead control; all of it. |

---

## Traps

In the order they bit, or would have.

### ☠️ The held picture is half-float, and a prediction from the input is not what is stored

The first `--fade` failed by 4e-5 against a float tolerance of 7e-6, and `--hold` could
not name 83 of 111 frames: both predicted the picture's density from the double-precision
sRGB decode of the input, but the capture stores it in RGBA16F, a relative 2^-11 away.
The fix is to measure the stored value, not to loosen the tolerance: `--fade` puts white
(which half stores exactly, density 0) beside the grey and reads the grey's stored
density as their ratio at Age 0, then passes to 1e-7 against a derived 2–3e-6. `--hold`
matches levels to 2.5e-3 (2^-10 and float noise), with neighbouring levels 1.1% apart,
and its lit-exposure check feeds white. Every other check is a ratio of two renders of the
same stored picture.

### ☠️ The spec's flicker frequency test cannot fail on its own negative control

Sampling the shutter at one instant per display frame, the spec's negative control, puts
the strongest line at **12 Hz too**: a 48 Hz square wave sampled at 60 Hz aliases to
exactly the beat an integrating display shows. What separates them is the amplitude: 0.293
integrated against 1.618 sampled. So `--flicker` predicts the whole spectrum from the
shutter's Fourier series, each harmonic through the exposure's sinc and folded to its
alias, and compares every DFT bin (worst 2e-8 against a 1.5e-6 bound). The frequency line
is still reported; it is not what fails.

### ☠️ A box's pixel coverage does not centre on the box

The first `--scratch` read an emulsion scratch's x as the coverage-weighted mean of its
columns' centres and missed by 0.041 px against a bound of 7e-5. A partly covered pixel's
light sits at the covered end, not the pixel centre, so that centroid is not the box's
centre. The check now reads the edges: the scratch is wider than a pixel, a column wholly
under it gives the picture's density, and each edge column's coverage places its edge.
For the print's own sub-pixel scratches, x within the pixel is not recoverable from
coverage at all; what is checked is that each one's profile across the columns is the same
on every frame (to 0.04 of the ratio tolerance) and confined to the columns its edges touch.

### ☠️ A check whose bound goes to infinity passes

Under the picture-space negative control the scratch moves off the column assumed wholly
under it, its density reads 0, and the edge bound became `x / 0`: two lines of the
negative run printed `ok` with `bound inf`. A density under 0.5 (log2 units) is now a miss
and fails the line.

### ☠️ A straddling exposure can be dark on one side

The resize check's held-picture half landed on a frame whose exposure straddled a
pull-down, as intended, but the older side of it was entirely inside the shutter's dark
stretch (±0.125 frames of a pull-down at 2 blades and 180°), so its weight was 0 and
clearing the held pictures changed nothing: the negative control passed. The frame is now
chosen so that both sides of the pull-down are longer than the dark stretch.

### ☠️ "Between the two pictures" is not "the two pictures blended"

`--hold` first asked that a straddling exposure's picture lie between its two frames'. The
GLSL mutation below (the sum replaced by the last term) passed that at 24 fps: showing
only one of the frames is "between" them, at an end. The check now predicts the blend's
proportions from the shutter's openings, computed as interval intersections (not the
plugin's cumulative function), and the mutation fails 36 of 73.

### ☠️ Every exposure must stay clear of a pull-down, or the hold is ambiguous

At 60 fps against 24 the exposure ends at p = 0.4(m + 1): every fifth one ends exactly on a
pull-down, where the plugin's accumulated double can fall either side. The harness clocks
frame 1 on 7.1 ms late (`lead`), which moves every end at least 0.03 frames (0.011 at 25
fps) off a pull-down; frame 0's exposure starts at p = 0 exactly and is never judged.

### The worktree guard judges by the session's directory

A bare `git checkout` after `cd ~/dev/gate` was blocked as if it were in the shared
`stoatworks-backend` checkout (the session's cwd). `git -C /Users/allansargeant/dev/gate`
is the rule on this machine anyway.

### Inherited from the fleet, and all still true here

`ScopedFBOBinding` does not restore the viewport (the host's is captured first and put back
before the output pass); every `ffglex::Scoped*` clears to 0 on exit, so every `Ensure()`
and the print-data upload happen before the passes bind anything; `FFGLFBO::Release()`
leaks the colour texture (`PassBuffer::Destroy()` deletes it first); a reallocated buffer
is a cleared buffer, so the held pictures are resampled across a resize; `SetParamInfo`
clamps a STANDARD default into 0..1; the core is an **OBJECT** library; `SetTextParameter`
must return `FF_SUCCESS` for the About block; the harness drives a synthetic clock; an
option's range reads back 0..1; Resolume's clock overflows a float, so the film position
is double and the shader sees only weights and offsets; GLSL 4.10's reserved words
(`packed` included) are not identifiers (verify greps); no `M_PI`, no `far`, no `near`; a
hardware linear filter may weight with 8 bits, so the gate's bilinear is written out;
randomness is PCG integer hashing on the CPU; the demo clips carry alpha.

---

## Would this hold on another rasteriser, at another raster?

One line per check. Every tolerance is derived, not fitted; every check ran at 320x180
and 1280x720 and on Apple's software renderer at 320x180 in `verify.sh`. `kU` = 2^-24, a
float's relative rounding. The numbers below are the 1280x720 run; 320x180 gives the same
(the weave's in pixels scale with the raster; its statistics are the same seed's).

| check | what it measures | tolerance and where it comes from | raster / renderer dependence |
| --- | --- | --- | --- |
| `--flicker` bins | every DFT bin of a flat grey's brightness over 300 frames at 60 Hz, divided by its mean, against the Fourier series of the stated shutter through the exposure's sinc, folded to the aliases | **2 × 8 kU × max y** (8 roundings a sample: float weight, product and sum for ≤ 2 segments, the lamp; doubled for the mean) **+ the series' tail past 2e6 terms**, `2 / (π² f B D N)`: 1.2–1.5e-6; measured ≤ 2.3e-8 | none: one pixel, no edges; 300 frames hold whole periods of every harmonic, so no leakage and the mean is exactly 1 |
| `--flicker` line | the strongest bin's frequency | half a bin (0.1 Hz) of the alias of B × FPS | none |
| `--hold` one-frame | which input frame a one-frame exposure shows, read from the ratio of a per-frame level to a fixed reference | **2.5e-3**: 2^-10 for two half-float stores, plus float noise; levels are ≥ 1.1% apart | none; the lead keeps every exposure ≥ 0.011 frames from a pull-down |
| `--hold` straddle | a two-frame exposure's picture against the two frames blended in the shutter's proportions (interval intersections) | 2.5e-3, the same | none |
| `--hold` per second | pictures captured in each whole second, read from their levels | exact: FPS | none |
| `--hold` dark | exposures containing a pull-down, at 2400 Hz | **exactly 0.0**: every weight is exactly 0 and 0 × T is 0 | none |
| `--hold` lit | exposures inside an opening, on white | 32 × 2 kU: white stores exactly, `2^0` is 1, the weight 2 is exact; 4 roundings allowed 32 | none |
| `--scratch` whole pixel | columns 5/16 W to 5/16 W + W/64 lose exactly 50%, their neighbours nothing, on 46 frames of a moving, weaving picture | **32 × 2 kU** on each ratio of the scratched to the clean render: ≤ 7 roundings in one, 6 in the other, two `exp2` at 3 ULP, the division: 26 ULP, rounded up | whole only where W is a multiple of 64 (320 and 1280 are); otherwise only the fractional checks apply, and verify says so |
| `--scratch` fractional | an emulsion scratch's centre from its read-back edges | **2 × (ratio tolerance / ln 2) / D + 2 W kU**: each edge's coverage is log2 q / D; plus an ULP of x W. 1.6e-4 px; measured 1.8e-5 | none: coverage is closed-form box overlap on `gl_FragCoord`, no filtering |
| `--scratch` stays put | the same centre across 43 frames; the print's own scratches' profiles frame to frame | 2 × the above; profiles to the ratio tolerance (base) or 4× it relative to their sum (emulsion) | none |
| `--scratch` polarity | the sign of every channel's ratio under each scratch | beyond the ratio tolerance | none |
| `--weave` sigma | the sample sd of 1000 consecutive frames' offsets, read from a step's row sum (a tent filter conserves the step's area) | **5 standard errors** of an AR(1) series' sample sd (Bartlett): `5 σ √((1 + ρ²) / (2n(1 − ρ²)))`, + 1e-3 px for the float row sums | the sd in px scales with H; the statistic does not |
| `--weave` rho | the lag-1 autocorrelation | **5 × √((1 − ρ²) / n)** (Bartlett) | none |
| `--fade` | each dye's retention from the stored density (grey against white) and the aged ratio | **e / (ln2 D) + \|r − 1\| e / (ln2 D)**, e = 24 × 2 kU (an exp2, a log2, the products, per sample, two samples a ratio): 2.3–3.1e-6; measured ≤ 1e-7 | none |
| `--fade` direction | R and B rise past G; cyan < yellow < magenta | strict inequalities | none |
| `--resize` print | every pixel of frames 40–59 after a resize to 1.5x and back | **4 × 2 kU** relative, for a software renderer that is not bit-repeatable; measured 0 differ | the print is not at any raster |
| `--resize` held | a straddling frame's centre pixel on the resize frame | 4 × 2 kU relative: the bilinear resample of a flat picture, `mix( v, v, t )` | the resize is to 1.5x whatever the raster |

Deliberately NOT relied on: a hardware-filtered read (the bilinear is by hand), GLSL `sin`
or `cos` in anything checked (the dust's rotation is a CPU cos/sin), exact cancellation
between two formulas, and `pow( 1, x ) == 1`. The half-float store is measured, never
predicted.

What might still differ on another rasteriser: the dust, the hair and the cue dots are
drawn with 1-pixel ramps and are only swept, not measured; an FFGL host that hands an
input texture larger than its picture (MaxUV < 1) is handled by `texelFetch` at the
picture's own coordinates and has not met a real host.

### The negative controls

`gatest --negative` runs eight, and `--perturb BITS` runs any check verbosely against one.
Each perturbs the *plugin's* model — a `Perturb` bit the shipped plugin carries at zero —
never the harness's expectation.

| perturbation | what fails, at 320x180, 1280x720 and on the software renderer |
| --- | --- |
| the shutter sampled at one instant per display frame | `--flicker`: 4 of 12 — every spectrum (beat amplitude 1.618 against 0.293); the frequency lines pass (the trap above) |
| the clip captured on every display frame (no hold) | `--hold`: 6 of 8 — 110 of 110 one-frame exposures show the wrong input, 36 pictures a second against 24 |
| scratches in picture space (riding the weave) | `--scratch`: 6 of 8 — 546 samples off the whole-pixel columns, the emulsion centre 2.5 px adrift, 3 of 4 print scratches moved |
| weave with no correlation (ρ = 0) | `--weave`: 4 of 12 — lag-1 −0.002 against 0.750 |
| shrinkage left out of sigma | `--weave`: 2 of 12 — sd 0.48 px against 1.41 at 2% |
| every dye fading at the mean rate | `--fade`: 9 of 10 — no magenta, retention 0.782 for all three against 0.607 / 0.963 / 0.819 |
| a resize makes a new print | `--resize`: 1 of 3 — 35 986 pixels differ |
| a resize clears the held pictures | `--resize`: 1 of 3 — 0.049 against 0.160 |

### The mutation

One character of the shipped GLSL, on a committed `source/Shaders.cpp`: in the output
pass, `light += SegWeight[ i ] * T;` → `light = SegWeight[ i ] * T;` (the shutter's sum
replaced by its last term). Caught by `--flicker` (7 of 12: the strongest line moves to 24
Hz and the beat's amplitude reads 0.770 against 0.293) and, once strengthened, `--hold` (2
of 8: 37 of 73 straddling exposures at 24 fps, 15 of 76 at 25, blend wrong). Before the
straddle check predicted proportions, `--hold` caught it at 25 fps only — the trap above.
`--scratch`, `--weave`, `--fade` and `--resize` passed, which is right: each compares
renders that carry the same mutation, or reads one-frame exposures. Reverted with
`git -C ~/dev/gate checkout -- source/Shaders.cpp`, rebuilt, `--hold` 8 of 8 and
`--flicker` 12 of 12 again.

---

## The browser demo

`demo/` is the page at **gate-demo.stoatworks-labs.com** (2026-09-25), on the fleet's
kit (`stoatworks-backend/resolume-demo`, vendored by its `sync.sh`).

**What is the plugin's.** Every GLSL string of `Shaders.cpp` — the version line, the
vertex body and the capture, resample and output bodies — is spliced into
`demo/plugin.js` by `demo/tools/sync_shaders.py`, tabs and comments included, and
assembled as `assemble` does. So the hold, the gate's bilinear, the frame line and the
neighbour frames, the density, retention and splice wash, the scratches, dust, cue dots
and hair, the shutter's weighted sum, the lamp, cos⁴ and the encode run on the GPU as
they do here, over two RGBA16F held pictures (WebGL2 needs `EXT_color_buffer_float` to
render into them, and the page refuses to start without it). The same script copies
Shaders.h's print-data layout, every Model.h constant (a `float` through `Math.fround`,
an expression such as `kMargin` evaluated in double) and the Controls option lists, rates
and lamp temperatures. `demo/tools/check_shaders.py` holds all of it to the C++ character
for character and `tools/verify.sh` runs it; a shader change here means re-running the
sync script, never an edit of the page. The one-character mutation above (`light +=` →
`light =`) fails it.

**What is a hand port, checked by nobody but a reader:** Model.cpp whole — `Hash` and
`Key` (uint32 through `Math.imul` and `>>> 0`, an int64 index split into its two's-
complement halves), `Normal`, `ShutterOpen`, `CumulativeOpen`, `Segments`, `Weave`,
`WeaveOffset`, `Scratches`, `Dust`, `HairAt`, `IsSplice`, `SpliceJump`, `IsCue`,
`Retention` and `LampRgb` — Controls.cpp's laws, the `Stream` enum, the seed and every
default from `Gate::Gate()`, and from `ProcessOpenGL` the clock (dt clamped to [0, 0.25 s],
the nominal first frame, the exposure reused on a frame the clock did not move), the film
position, the hold's bookkeeping, the resize resample, the print-data upload and every
uniform. Change one of those here and change the page by hand. The page says so in its
banner and disclosure.

**What differs, each said on the page:** the host's frame is the browser's
requestAnimationFrame, so **the flicker beats against the viewer's display rate** (12 Hz
at 60, 48 Hz at 120, none at 72 or 144) and the line under the picture reports the rate
and the beat; the clock is the kit's (no unit vote; a paused page renders dt = 0 frames;
Restart is a backward clock, which the clamp reads as 0 s, so the strip carries on); Cue
Dots is a toggle the renderer releases on the frame it acts (the kit has no event type);
no About block; `Perturb` and `Probe` are 0; the kit caps one frame at 0.1 s. A
photosensitivity note sits above the picture. Clips are the kit's generated ones (moving
scene first), never Resolume's.

**Measured once (2026-09-25).** The page driven frame by frame at n / 60 from a fresh
instance (`window.__gateDemo.hooks`: `fresh()`, and `afterRender` to read the canvas and
the input inside the frame) on the Synthetic scene clip at 320x180, 120 frames, against
`gatest --pipe --fps 60` on the same input frames read back from the page, with the same
values `--set`: at the defaults; at 2 blades and 180°, 25 fps, Tungsten, Framing +0.12,
Weave 0.8, Shrinkage 0.9, every mark at 1, Age 0.5, Vignette 1 (a hair on projector
frames 40–49); at 16 fps, 1 blade at 99°, Carbon Arc, Framing −0.2, Age 1, Mix 0.7; and
the second of those with Cue Dots pressed at frame 10 (`--script`). Every pixel within 1/255 on all 120 frames of each.
Through ANGLE on Metal, 0 to 2 channel values of 27.6 million differ, by 1; through
SwiftShader, up to 415 000, by 1. It can fail: Weave 0.25 against 0.26 differs by up to
10/255 from the first frame on; Dust 1 against 0.99 by 41/255; the cue on the page only by
174/255 on frames 10–18. Age 0.1 → 0.9 and Lamp Xenon → Tungsten each move the paused
picture (mean |difference| 67 and 28 levels).

Deploy: `cf-run npx wrangler deploy` from the repo root, or push to main
(`.github/workflows/deploy.yml`). The host is a Worker **route** over a proxied
`AAAA 100::` record made through the API on 2026-09-25, not a custom domain: the zone
is at Cloudflare's limit of 100. Delete that record and the page goes dark while deploys
stay green. Verify by content:
`curl -s 'https://gate-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'`.

## Decisions taken without asking

- **The exposure is the whole display period** (a 360° camera, or an eye). The spec's
  "integrated over each display frame's exposure" leaves the length open; the whole period
  is what makes the beat a property of the two rates alone.
- **Normalised to the mean.** Each weight is divided by `open`, so a steady picture
  averages to itself at any Shutter Angle: a projectionist sets the lamp for screen
  brightness. Peaks can reach `1 / open` of the picture (2x at 180°) and clip on white.
- **Shutter Angle is each blade cycle's opening**, 45–315°, the closed stretch centred on
  the cycle's start, so the pull-down is always in the dark. 180° at 2 blades is a
  classic 35 mm projector.
- **Straddling exposures blend two frames**, each at its own weave. It is what the machine
  does (and what a 360° camera filming a projector records); on fast motion it reads as a
  double image every 2–3 frames. Kept, and an open question below.
- **Hold captures the first display frame whose exposure reaches the new frame.** A host
  only has the current picture; the "true" frame at the pull-down instant is not available.
- **The aperture is smaller than the printed frame** (0.602 in against 0.630 in), so the
  clip is the aperture's view, the picture carries on (edge-repeated) for 2.3% of the
  height beyond it, and only Framing past that shows the frame line. Without it a 1 px
  weave showed a black line at the top.
- **The neighbour frame below** (Framing up) is the current picture: the next frame has not
  arrived. The frame above is the previous projector frame, as the strip has it.
- **Scratches are in the gate** (the spec: grit in the gate, a fixed x while the picture
  moves under it), so they do not weave; **dust and cue dots are on the print** and weave
  with it; **the hair is in the gate**.
- **Emulsion scratches take layers off top first**: magenta, then cyan, then yellow (colour
  print film's order), so a shallow one is green, a deeper one yellow, the deepest clear.
  Base scratches lose a fraction of the light (scatter).
- **Dust rides the print for 1–4 frames** (70/15/10/5%) and drifts a little frame to frame:
  the spec's "persist a few frames". A real speck on a frame is there for that frame; the
  longer ones are loose particles dragged by the gate. 85% dark, 15% white (dirt printed in
  from the negative).
- **Splices are a rate per minute at the running speed**, a jump of 4–12% of the height and
  a wash (density × 0.35) on one frame.
- **Cue Dots is an event**: the marks start on the frame in the gate when it is pressed,
  four frames, then four more 168 frames later (the motor and changeover cues, 7 s apart at
  24 fps). Scribed: clear with a dark rim.
- **Fading is per-dye density retention** `exp( −k Age )`, k = (2.0, 0.15, 0.8) for cyan,
  magenta, yellow: an invented law chosen to show the documented order, not fitted to a
  stock. It fades the frame line and the blacks too (they go red), as real faded prints do.
- **Lamps are black bodies** (Carbon Arc 5000 K, Xenon 6200 K, Tungsten 3200 K) through
  Wyman, Sloan and Shirley's fit of the CIE observer, white-balanced to a 6504 K black body
  and normalised to luminance 1. A carbon arc and a xenon lamp are not black bodies.
- **The print is stateless**: every scratch, speck, hair, splice and weave offset is a pure
  function of the film frame and a fixed seed. So Scratches, Dust and Hair act at once (they
  change the rate the past was drawn at) and two instances show the same print.
- **Output alpha** is `mix( src.a, 1, Mix )`: the projection paints the whole frame. A
  transparent pixel of the clip (rgb 0 in Resolume's DXV demo clips) prints as black film.
- **Defaults**, chosen on Resolume's demo clips (Metalive 01, Trinity_09, Beat 001,
  IntoTheGlow_02, FogAndDust_3, Ethnik2_23) through `--pipe` at 60 Hz: 24 fps, 2 blades,
  180°, Xenon, centred, Weave 0.25, Shrinkage 0.25, Hair 0.3, Scratches 0.35, Dust 0.3,
  Splices 0.05 (3 a minute), Age 0.1, Vignette 0.4, Mix 1. Age 0.2 turned every black
  maroon; 0.1 is a mild pink. Dust was enlarged (0.015–0.15 mm) after it vanished on busy
  footage.
- **No boolean parameter exists**, so verify's step test covers an option and an event;
  the harness's step code covers booleans too.
- **Provisional About**: `StoatworksAbout.h` and `ATTRIBUTIONS.md` are hand copies in the
  generated shape, with `guide = ""` because there is no user guide yet.
- **Test hooks live in the shipped plugin** (`Perturb`, `Probe`, the scratch override,
  `LastWindowForTest`, `LastScratchesForTest`), inert in a host.

---

## What is actually verified, and what is assumed

### Verified by measurement, on an M4 Max running macOS 26.4 (2026-09-25)

Every number is `tools/verify.sh` on this machine against a fresh universal Release build,
at 320x180 and 1280x720 and on the software renderer.

- **Flicker.** 2 blades at 24 fps: the strongest line at 12.00 Hz, amplitude 0.29271, the
  Fourier series 0.29271; 3 blades: 12 Hz, 0.19514; 25 fps × 2: 10 Hz, 0.23094; 18 fps ×
  3 at 240°: 6 Hz, 0.08953. Every DFT bin within 2.3e-8 of the series (bound 1.2–1.5e-6).
- **Hold.** At 24 and 25 fps every one-frame exposure (110, 107) shows the input captured
  at its frame's pull-down; every lit straddling exposure (73, 76) blends the two in the
  shutter's proportions; 24, 24, 24 and 25, 25, 25 pictures captured a second. At 2400 Hz
  every exposure containing a pull-down is exactly black, every one inside an opening 2.0x.
- **Scratch.** A base-side scratch on whole pixels takes exactly 50% off exactly its
  columns (400–419 at 1280) on 46 frames of a moving, weaving picture; an emulsion scratch
  at 791.0834 px reads back to 1.8e-5 px (bound 1.6e-4) and moves 5e-8 px; four of the
  print's own keep their profiles to 0.03 of the tolerance; every polarity right.
- **Weave.** 1000 frames: sd 1.906 against σ 1.884 px, lag-1 0.742 against 0.750; at 2%
  shrinkage 5.779 against 5.651, 0.372 against 0.375 (1280x720); vertical 0.6 of it.
- **Fade.** At Age 0.25, 0.5 and 1 each dye keeps what the law says to 1e-7; red and blue
  rise past green (x4.43 and x2.58 against x1.27 at Age 1).
- **Resize.** After 1.5x and back, frames 40–59 identical to the unresized run's (scratches,
  dust, hair, weave); on the resize frame the previous frame is still lit (0.160053 both).
- **Negative controls.** All eight fail their check. **Mutation** caught (above).
- **No dead controls**, all 15, the four About buttons skipped.
- **Every shader compiles** through `glslc` as the plugin assembles it; no reserved word.
- **`--pipe`**: two frames for two and a half, exit 2 on an unknown cue, **exit 1 on a closed
  stdout (`| head -c 1`)**, exit 1 on a failed render, an option steps, an event fires on its
  frame only, a slider ramps.
- **The bundle** is universal (`x86_64 arm64`), exports `_plugMain`, carries
  `com.stoatworks.ffgl.gate`, ad-hoc signs, and `oxbow` reports `SW Gate` / `GA01` /
  `effect` and renders 120 frames through `plugMain`.
- **Alpha**: through `--pipe` on Trinity_09, whose alpha is below 255 over 93% of the
  frame: 255 everywhere at Mix 1, and the clip's own alpha byte for byte at Mix 0.
- **Render cost** (`gatest --bench`, best of three, `glFinish` both sides, the defaults, a
  shared GPU): 0.085 / 0.094 / 0.265 ms at 720p / 1080p / 4K (0.5 / 0.6 / 1.6% of a 60 fps
  frame), holding 14 / 32 / 127 MB of GPU state (two RGBA16F pictures).

### Assumed, or not done

- ☠️ **Never loaded into Resolume.** Everything numeric was compiled, rendered and measured
  offline against the real plugin class in a headless CGL context, plus an `oxbow` load.
- **Never built on Windows.** CI and release workflows are written and have not run.
- **The checks verify the stated model, not a projector.** The weave's σ, ρ and shrinkage
  law, the scratch and dust rates and sizes, the hair, the splice and the three fade rates
  are chosen, not measured from film; the geometry (pitch, apertures, cue timing) and the
  layer order are published figures.
- **The look is judged by eye**: dust shapes, the hair, the cue dots, the splice flash, the
  lamps' colours. The sweep proves each control moves the picture; nothing measures how.
- **Seen on Resolume's bundled demo clips** (CG loops) through `--pipe`; never on camera
  footage, never on film scans.
- **The clock-unit voting** is readout's, which has met Arena; this plugin has not.
- **Not verified at 4K**, only benchmarked there.
- **No user guide, OpenFX port, factory presets or seed control.**

---

## Open questions

- **The in-between frames.** Straddling exposures blend two frames at two weaves: the
  machine's truth, but a double image on fast motion. Should there be a display shutter
  (a camera angle under 360°) so an operator can choose clean frames?
- **Flicker strength.** At the classic 2 blades and 180° the beat is ±29% in linear light.
  Is that the right default for a show, or should the default be 3 blades or a wider angle?
- **A seed.** Two instances show the same print. A Seed control is one parameter.
- **Stock.** The spec's pipeline mentions a Stock setting the fade; the control list has
  Age only. A Stock option (Eastmancolor, which fades, against Technicolor dye transfer,
  which does not) would change the three rates.
- **Base scratches in density.** A base scratch loses a fraction of the light; a real one
  also refracts, and shows a bright core on some prints.
- **Scratches that wander.** Gate scratches are straight; scratches from an earlier
  projection, printed into the base, meander and weave with the picture. Neither the spec
  nor the model has them.
- **The frame below.** Framing up shows the current picture as the next frame; one frame of
  latency would show the real one.

---

## Siblings

- **filament** — the plugin shape, the harness, `--pipe` with SIGPIPE ignored, the
  software pass, verify, the negative controls.
- **readout** — a shutter in time; the clock-unit voting.
- **standards, cadence** — frame rates beating against display rates.
- **rebate** — the negative and its scan; the neighbour this is not.
- **tinsel** — `PassBuffer`, `sweep.py`, CI, the fleet's trap list.
- **photofinish** — the resize-mid-run trap.
- **oxbow** — `oxbow probe` and `oxbow selftest` load this bundle as a host.
