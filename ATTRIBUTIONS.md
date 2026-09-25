# Attributions

Gate is built on other people's work. This file lists what that work is, who did it, and
what it is doing here.

PROVISIONAL, hand-written in the shape the backend's `scripts/sync-attributions.py`
generates (graticule's recipe). Gate is not yet registered in the website's lists, so
the sync cannot produce this file; once it is, the generated copy replaces this one and
the entries below move to the master lists in `stoatworks-backend`.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Plugin shape, harness, --pipe contract and verify — Stoatworks filament, wetplate, toner

<https://github.com/stoatworks-labs/filament>  
Licence: MIT  
Copyright: Stoatworks Labs

The plugin's shape (the OBJECT core, the clock-unit voting, the About block, the Diag logger), the harness shape, the --pipe contract with SIGPIPE ignored and cues that step for options and events, the software-renderer pass, the verify script and the negative-control pattern are filament's, which had them from wetplate and toner, and they from rebate and pitch; the host clock-unit voting is readout's by way of all of them.

### PassBuffer — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

PassBuffer is tinsel's, with wetplate's Swap.

### The resize-mid-run guard — Stoatworks photofinish

<https://github.com/stoatworks-labs/photofinish>  
Licence: MIT  
Copyright: Stoatworks Labs

The trap that a reallocated buffer is a cleared buffer, and the check that guards it, are photofinish's.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl, pinned to b1afaf9 like the fleet.

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately. Resolves OpenGL entry points on Windows.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample. Part of the upstream SDK tree rather than something this plugin calls.

## Published figures the model uses

- **35 mm 4-perforation film geometry** — the 4.75 mm perforation pitch (19.0 mm a frame), the Academy camera aperture (0.864 × 0.630 in) and the projector aperture (0.825 × 0.602 in), as SMPTE's 35 mm standards and every projectionist's manual give them. They set the frame pitch, the frame line and how much printed picture lies beyond the aperture.
- **Reel-change cues** — a motor cue 8 s and a changeover cue 1 s before the end of a reel at 24 fps, each four frames, top right: the convention as projection manuals describe it.
- **Eastman colour print dye stability** — that in dark storage the cyan dye of the 1950s–80s Eastmancolor prints fades first and the magenta last, which is why faded prints go magenta: widely documented (for example by the Image Permanence Institute and in Wilhelm and Brower, *The Permanence and Care of Color Photographs*, 1993). The three rates here are chosen to show that order, not fitted to any stock.
- **The layer order of colour print film** (magenta on top, then cyan, then yellow nearest the base), which is why a shallow emulsion scratch on a print shows green.

## Standards and published specifications

- **IEC 61966-2-1:1999** — the sRGB primaries, white and transfer function.
- **CIE 1931 2° observer**, as the multi-lobe Gaussian fit of C. Wyman, P.-P. Sloan and P. Shirley, "Simple Analytic Approximations to the CIE XYZ Color Matching Functions", *Journal of Computer Graphics Techniques* 2(2), 2013 — the lamps' colours are black bodies through it.
- **Planck's law**, with the 2019 SI exact second radiation constant.
- **M. S. Bartlett (1946)** — the large-sample variance of an autocorrelation estimate, which sets `gatest --weave`'s tolerances.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### The 35 mm projection booth

Simplex, Century and Kinoton projectors, their two- and three-blade shutters, carbon arcs and xenon lamps, and the release prints that went through them until the scratches, the dust and the splices were part of the picture.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
