#pragma once

#include "Exposure.h"

#include <vector>

/**
	The output pass on the CPU, for the OpenFX build.

	This is the ONE place the per-pixel arithmetic is written twice: here and
	in `kOutputBody` (and `kCaptureBody`'s decode) in Shaders.cpp. Everything
	upstream of it -- the shutter's weights, the weave, the splices, the cue
	flags, the dust, the hair, the scratches, the lamp, the dyes -- comes from
	Exposure.cpp, which both builds link. Each mirrored function is marked
	`//= mirrored`, and so is its GLSL twin: an edit to one copy is an edit to
	the other.

	The arithmetic is float, as the shader's is, and in the shader's order.
	What it does not mirror is the GPU's storage: the FFGL build keeps its two
	held pictures in RGBA16F, this keeps them in float, so a value differs by
	up to 2^-12 of itself before the print reads it (tools/verify.sh and
	AGENTS.md have the measured agreement).

	Coordinates are the shader's: x right and y UP from the bottom-left of the
	output, pixel centres at +0.5 -- which is also OpenFX's own orientation, so
	nothing flips.
*/
namespace gate::projection
{

/// A held picture: linear light, RGB, rows bottom-up, as the capture pass
/// stores it.
struct Picture
{
	int width  = 0;
	int height = 0;
	std::vector< float > rgb;

	void resize( int w, int h )
	{
		width  = w;
		height = h;
		rgb.assign( static_cast< size_t >( w ) * h * 3, 0.0f );
	}
	bool empty() const
	{
		return width <= 0 || height <= 0;
	}
	float* row( int y )
	{
		return rgb.data() + static_cast< size_t >( y ) * width * 3;
	}
};

/// kCaptureBody's decodeSrgb, one channel.
float DecodeSrgb( float c );

/// kOutputBody's encodeSrgb, one channel.
float EncodeSrgb( float c );

/// Everything one output frame's pass reads.
struct Pass
{
	const exposure::Frame* frame = nullptr;
	/// The two held pictures (Held0 and Held1). Both must be non-empty; when
	/// there is no previous frame, both point at the current one, as the
	/// FFGL build binds it.
	const Picture* held[ 2 ] = { nullptr, nullptr };
	int segPicture[ model::kMaxSegments ] = {};///< which held picture is this frame
	int segAbove[ model::kMaxSegments ]   = {};///< and which is the frame above it
	float outWidth  = 0.0f;
	float outHeight = 0.0f;
};

/// One output pixel, `x` and `y` in whole pixels from the bottom-left. `src`
/// is the live input at this pixel (for Mix and alpha), `out` gets RGBA.
void Shade( const Pass& pass, int x, int y, const float src[ 4 ], float out[ 4 ] );

} // namespace gate::projection
