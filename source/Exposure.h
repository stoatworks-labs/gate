#pragma once

#include "Model.h"

#include <cstdint>
#include <vector>

/**
	What one display frame's exposure shows, short of the pictures themselves.

	Both builds call this. The FFGL plugin (Gate.cpp) turns the result into
	uniforms and the print-data texture row; the OpenFX plugin
	(source/ofx/GateOFX.cpp) hands it to the CPU copy of the output pass
	(Projection.cpp). So the weave, the splices, the cue flags, the dust, the
	hair, the scratches and every constant the output pass is given are
	computed by ONE piece of C++ for both, and only the per-pixel arithmetic
	is written twice.

	GL-free, and free of either plugin SDK: the OpenFX build on Linux has no
	GL loader to include.
*/
namespace gate::exposure
{

/// The controls as the host holds them: sliders 0..1, options as their
/// element index. Same space in both builds.
struct HostValues
{
	float fps       = 2.0f;
	float blades    = 2.0f;
	float shutter   = 0.0f;
	float lamp      = 1.0f;
	float framing   = 0.5f;
	float weave     = 0.0f;
	float shrinkage = 0.0f;
	float hair      = 0.0f;
	float scratches = 0.0f;
	float dust      = 0.0f;
	float splices   = 0.0f;
	float age       = 0.0f;
	float vignette  = 0.0f;
	float mix       = 1.0f;
};

/// The controls in the model's units (Controls.cpp's laws).
struct Settings
{
	double fps       = 24.0;
	int blades       = 3;
	double open      = 0.75;
	int lamp         = 1;
	double framing   = 0.0;
	model::WeaveLaw law;
	double hair      = 0.0;
	double scratches = 0.0;
	double dust      = 0.0;
	double splices   = 0.0;
	double age       = 0.0;
	double vignette  = 0.0;
	float mix        = 1.0f;
	int perturb      = 0;
};

Settings FromHost( const HostValues& host, int perturb );

/// Everything the output pass reads that is not a picture: per segment, the
/// print-data row, and the constants. Field for field what Gate.cpp sets as
/// uniforms, in the types the shader receives them in.
struct Frame
{
	int count = 0;
	model::Segment segments[ model::kMaxSegments ] = {};
	float weights[ model::kMaxSegments ]     = {};
	float offsets[ 2 * model::kMaxSegments ] = {};
	int flags[ model::kMaxSegments ]         = {};///< 1 a splice, 2 a cue dot

	/// One row of RGBA texels, Shaders.h's layout.
	std::vector< float > data;
	int particleCount = 0;
	int scratchCount  = 0;
	int hairMask      = 0;
	model::Hair hairShape;
	/// The scratches alive anywhere in the exposure (the test hook reads it).
	std::vector< model::Scratch > scratches;

	float framing       = 0.0f;
	float pitch         = 0.0f;
	float margin        = 0.0f;
	float maxDensity    = 0.0f;///< log2 units
	float retention[ 3 ] = {};
	float spliceWash    = 0.0f;
	float hairHalfWidth = 0.0f;
	float hairOpacity   = 0.0f;
	float cueCentre[ 2 ] = {};
	float cueRadius     = 0.0f;
	float lamp[ 3 ]      = {};
	float vignetteTan   = 0.0f;
	float aspect        = 1.0f;
	float mix           = 1.0f;
};

/**
	Build the frame for an exposure that overlaps `segments` (oldest first,
	`count` of them, from model::Segments). `cueStart` is the projector frame
	Cue Dots was fired on (-1: never). A non-empty `scratchOverride` replaces
	the print's scratches and is alive on every segment (the harness's
	whole-pixel scratch).
*/
void Build( uint32_t seed, const Settings& settings, const model::Segment* segments, int count, double aspect,
            int64_t cueStart, const std::vector< model::Scratch >& scratchOverride, Frame& out );

} // namespace gate::exposure
