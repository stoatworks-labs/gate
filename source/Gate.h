#pragma once

#include "Model.h"
#include "PassBuffer.h"
#include "StoatworksAboutParams.h"

#include <FFGLSDK.h>

#include <string>
#include <vector>

/**
	Gate -- a film projector's gate, shutter and print, as an FFGL effect.

	**The one idea.** Model the machine, not the look. The projector pulls the
	film down a frame at a time with the shutter closed, holds it in the gate
	against the clearance of its claw, and lets the lamp through only while a
	blade is out of the way; the print is a physical strip with grit, dust
	and a splice in it, and dyes that fade. So the clip is held at the
	projector's rate, the light the shutter passes is integrated over each
	display frame (which is where the flicker's beat against 60 Hz comes
	from), each frame lands with its own weave, and every mark on the picture
	belongs to the print or the gate.

	Time is frame-relative: the film position is kept in double, in
	projector frames, and advanced by each frame's real elapsed seconds; the
	shader sees weights and offsets, never a clock. The print is a pure
	function of the film position and a seed (Model.h), held on the CPU, so a
	resize cannot touch it; the two held pictures are resampled, not
	reallocated, when the raster changes. See AGENTS.md for the traps.
*/
class Gate : public CFFGLPlugin
{
public:
	Gate();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// Clock test hook: the harness DECLARES its unit rather than leaving the
	/// voting to infer one.
	void SetClockScaleForTest( double scale );

	/// Negative-control hooks, a bitmask of `model::Perturb`. Always 0.
	void SetPerturbForTest( int bits );

	/// Probe hook: `model::Probe`.
	void SetProbeForTest( int probe );

	/// Replace the print's scratches with these (edges in width fractions),
	/// alive on every frame. For the whole-pixel scratch check. Empty: the
	/// model's own.
	void SetScratchOverrideForTest( const std::vector< gate::model::Scratch >& list );

	/// What the last frame used: its exposure in film frames, and the
	/// scratches alive in it.
	void LastWindowForTest( double& p0, double& p1 ) const;
	std::vector< gate::model::Scratch > LastScratchesForTest() const;

	/// Bytes of GPU state held across frames.
	size_t StateBytesForTest() const;

	/// Everything the operator can reach, in the order Resolume shows them.
	enum ParamID : FFUInt32
	{
		//Projector
		PT_FPS,
		PT_BLADES,
		PT_SHUTTER,
		PT_LAMP,
		PT_FRAMING,

		//Gate
		PT_WEAVE,
		PT_SHRINKAGE,
		PT_HAIR,

		//Print
		PT_SCRATCHES,
		PT_DUST,
		PT_SPLICES,
		PT_CUE_DOTS,
		PT_AGE,

		//Output
		PT_VIGNETTE,
		PT_MIX,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. Last, so no saved composition's ids shift.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

private:
	/// The host's clock in seconds, whatever unit it arrived in.
	double nowSeconds();

	ffglex::FFGLShader captureShader;
	ffglex::FFGLShader resampleShader;
	ffglex::FFGLShader outputShader;
	ffglex::FFGLScreenQuad quad;

	gate::PassBuffer held[ 2 ];  ///< the last two projector frames' pictures, linear
	gate::PassBuffer spare;      ///< a resize's scratch buffer
	int current = 0;             ///< which of `held` is the newest
	int64_t heldFrame = -1;      ///< the projector frame in held[ current ]
	bool havePrevious = false;
	GLuint printTexture = 0;

	int lastWidth  = 0;
	int lastHeight = 0;

	//--- the projector ------------------------------------------------------
	bool running         = false;
	double filmPosition  = 0.0; ///< projector frames, at the end of the last exposure
	double lastExposure  = gate::model::kNominalFrame;///< seconds, for a frame the clock did not move
	double windowP0 = 0.0, windowP1 = 0.0;
	uint32_t seed        = 0x6a7e5eedu;
	int64_t cueStart     = -1;
	bool cueHeld         = false;
	std::vector< gate::model::Scratch > lastScratches;
	std::vector< gate::model::Scratch > scratchOverride;

	//--- the clock (readout's unit voting) -----------------------------------
	bool hostTimeSeen   = false;
	double clockScale   = 0.0;///< 0 until decided; then 1.0 or 0.001
	double wallStart    = -1.0;
	double lastWallTime = -1.0;
	double lastRawTime  = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	double lastNow      = -1.0;
	int clockFrames     = 0;

	int perturb = 0;
	int probe   = 0;

	/// Zero-initialised: the About block's ids are never stored to, so
	/// without this GetFloatParameter hands the host whatever was on the
	/// stack for them.
	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
