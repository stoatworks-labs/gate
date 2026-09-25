#pragma once

#include <string>

/**
	Three passes.

	1. **capture** -- input raster, RGBA16F. The host's picture, sRGB decoded
	   to linear light, into whichever of the two held pictures does not hold
	   the previous projector frame. It runs only on a display frame where a
	   pull-down happened: between pull-downs the gate holds the same frame.

	2. **resample** -- a held picture onto a new raster, bilinear, when the
	   host's raster changes. A reallocated buffer is a cleared buffer, and the
	   previous frame is still being shown through the shutter.

	3. **output** -- to the host. For each projector frame the display frame's
	   exposure overlaps (a "segment", weighted by the light the shutter let
	   through): the gate (weave, framing, the frame line and the neighbour
	   frames), the print (dye density, fading, a splice's flash, dust, cue
	   dots) and the gate's grit and hair; then the weighted sum, the lamp, the
	   lens's cos^4 falloff, the sRGB encode, Mix.

	Every picture read is `texelFetch` and a bilinear blend written out here,
	not a hardware filter: a weave offset of 0 reads the held picture exactly,
	and a fractional one interpolates in float on every driver. `gatest
	--dump-shaders DIR` writes exactly the strings the plugin compiles.

	`PrintData` is one row of RGBA32F texels the CPU writes each frame:
	  [ 0, 128 )   64 particles, two texels each: ( x, y, a, b ),
	               ( cos, sin, strength, segment + 8 x fibre )
	  [ 128, 160 ) 16 scratches, two texels each: ( left, right, strength,
	               side ), ( segment mask, 0, 0, 0 )
	  [ 160, 173 ) the hair's 13 points: ( x, y, 0, 0 )
*/
namespace gate::shaders
{

constexpr int kPrintDataWidth    = 256;
constexpr int kPrintScratchFirst = 128;
constexpr int kPrintHairFirst    = 160;

std::string Vertex();
std::string Capture();
std::string Resample();
std::string Output();

} // namespace gate::shaders
