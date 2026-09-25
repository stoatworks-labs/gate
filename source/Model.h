#pragma once

#include <cstdint>
#include <vector>

/**
	The projector and the print, as numbers, in double, on the CPU.

	Everything here is a pure function of the film position (in projector
	frames) and the print's seed, except the cue schedule, which an operator
	fires. So the print does not live in a GPU buffer, it cannot be cleared by
	one, and a resize cannot touch it: the frame the gate shows at position k
	is the same frame whatever the raster.

	Geometry is in picture HEIGHTS ("h-units"), y down from the top of the
	frame, x across from the left to `aspect`. A physical length on the film
	is millimetres over kApertureHeightMm. Scratches are the exception: their
	edges are fractions of the output WIDTH, so a scratch can sit on whole
	pixels exactly (`gatest --scratch`).

	The shutter's light is integrated here in closed form, per projector frame
	that the display frame's exposure overlaps, and the shader only sums the
	weighted pictures. See AGENTS.md for which constants are measured and
	which are chosen.
*/
namespace gate::model
{

constexpr double kPi = 3.14159265358979323846;

//---------------------------------------------------------------------------
// Time.
//---------------------------------------------------------------------------

/// What a display frame is worth when the host's clock has not moved yet.
constexpr double kNominalFrame = 1.0 / 60.0;
/// A host stall is integrated for this long and no longer.
constexpr double kMaxFrameDelta = 0.25;
/// At most this many projector frames overlap one display frame: 0.25 s at
/// 25 fps is 6.25 frames, which can touch 8.
constexpr int kMaxSegments = 8;

//---------------------------------------------------------------------------
// The film (4-perf 35 mm, Academy). The clip is what the projector's
// aperture shows; the printed picture is a little bigger than the aperture,
// so a small weave shows more picture, not the frame line.
//---------------------------------------------------------------------------

/// The projector aperture's height, 0.602 in: the h-unit.
constexpr double kApertureHeightMm = 15.29;
/// The printed picture's height, 0.630 in (Academy camera aperture).
constexpr double kPrintedHeightMm = 16.0;
/// One frame's pitch along the strip: four perforations of 4.75 mm.
constexpr double kFramePitchMm = 19.0;
/// Printed picture beyond the aperture, top and bottom, in h-units: 0.023.
constexpr double kMargin = 0.5 * ( kPrintedHeightMm - kApertureHeightMm ) / kApertureHeightMm;
/// Frame pitch in h-units: 1.243. The frame line is 3.0 mm, 0.196 h.
constexpr double kPitch = kFramePitchMm / kApertureHeightMm;
/// A print's maximum density (all three dyes), and what an input of black
/// prints as. The frame line prints at it too.
constexpr float kMaxDensity = 3.2f;

//---------------------------------------------------------------------------
// The shutter.
//
// One revolution per frame, `blades` openings, each open for `open` of its
// blade cycle and centred on it, so every blade cycle is closed around its
// start. The pull-down happens at the start of the frame, which is the
// middle of a closed stretch: the claw moves in the dark.
//---------------------------------------------------------------------------

/// 1 if the shutter passes light at film position p (frames), else 0.
double ShutterOpen( double p, int blades, double open );

/// The open time from film position 0 to p, in frames; exact, piecewise
/// linear, periodic in 1 / blades.
double CumulativeOpen( double p, int blades, double open );

struct Segment
{
	int64_t frame; ///< the projector frame
	double weight; ///< its light over the display frame, as a fraction of the mean
};

/// The projector frames the display frame's exposure [p0, p1] overlaps, oldest
/// first, each weighted by the light the shutter passed during the overlap,
/// divided by ( open x ( p1 - p0 ) ): so a steady picture averages to itself.
/// `instant` is the negative control: the shutter sampled at p1 only.
int Segments( double p0, double p1, int blades, double open, bool instant, Segment out[ kMaxSegments ] );

//---------------------------------------------------------------------------
// The gate: weave.
//
// The claw and the sprocket teeth have clearance, so each frame lands with an
// offset. Stated law (an assumption, AGENTS.md): an AR(1) process in frames,
//   x_k = rho x_{k-1} + sqrt( 1 - rho^2 ) sigma e_k,  e_k ~ N( 0, 1 ),
// so its standard deviation is sigma and its lag-1 correlation rho. Sigma is
// kWeaveMm x Weave x ( 1 + kShrinkGain x shrinkage% ); rho is kWeaveRho x
// ( 1 - shrinkage% / kShrinkDecorrelate ). A shrunk print's perforations no
// longer seat on the teeth, so it weaves more and each frame less like the
// last. Vertical is kWeaveVertical of horizontal.
//---------------------------------------------------------------------------

constexpr double kWeaveMm           = 0.080;///< sigma at Weave 1, no shrinkage
constexpr double kWeaveVertical     = 0.6;
constexpr double kWeaveRho          = 0.75; ///< the perforator's correlation
constexpr double kShrinkMaxPercent  = 2.0;  ///< Shrinkage 1
constexpr double kShrinkGain        = 1.0;  ///< sigma x ( 1 + this x % )
constexpr double kShrinkDecorrelate = 4.0;  ///< rho x ( 1 - % / this )

struct WeaveLaw
{
	double sigmaX = 0.0; ///< h-units
	double sigmaY = 0.0; ///< h-units
	double rho    = 0.0;
};

WeaveLaw Weave( double weave, double shrinkage, int perturb );

/// The offset of frame k, h-units: the stationary AR(1) as its moving-average
/// sum over the last draws (truncated where rho^j < 1e-9), so it is a pure
/// function of k and needs no state.
void WeaveOffset( uint32_t seed, int64_t frame, const WeaveLaw& law, double& x, double& y );

/// One standard normal draw from the print's hash: Box-Muller in double.
double Normal( uint32_t seed, uint32_t stream, int64_t index );

//---------------------------------------------------------------------------
// The print.
//---------------------------------------------------------------------------

enum Side : int
{
	kBase     = 0,///< scatters light out of the lens: prints dark
	kEmulsion = 1,///< takes dye off, top layer first: prints light (green, then yellow, then white)
};

struct Scratch
{
	double left  = 0.0;///< fraction of the output width
	double right = 0.0;
	double strength = 0.0;///< base: fraction of light lost at full cover; emulsion: depth 0..1 through three layers
	int side = kBase;
	int64_t born = 0; ///< the first frame it is on
	int64_t dies = 0; ///< the first frame it is not
};

/// Grit arrives in the gate at a rate set by Scratches, sits at a fixed x for
/// its life, and leaves; the scratch it cuts is at that x on every frame in
/// between. Every scratch alive on any frame in [first, last].
constexpr double kScratchBirths   = 6.0 / 96.0;///< per frame at Scratches 1
constexpr int kScratchLifeMin     = 24;
constexpr double kScratchLifeMean = 96.0;
constexpr int kScratchLifeMax     = 480;
constexpr int kMaxScratches       = 16;
constexpr double kScratchHalfMmMin = 0.004;
constexpr double kScratchHalfMmMax = 0.020;
std::vector< Scratch > Scratches( uint32_t seed, int64_t first, int64_t last, double amount, double aspect );

struct Particle
{
	double x = 0.0, y = 0.0;///< h-units, film space (on the frame)
	double a = 0.0, b = 0.0;///< blob: the two radii; fibre: half length, half thickness
	double angle = 0.0;
	int fibre = 0;
	double strength = 0.0;///< > 0: dark (opacity); < 0: printed-in white
};

/// Dust on the print: births per frame are Poisson with mean kDustBirths x
/// Dust, and a speck rides with the print for one to four frames, drifting a
/// little as it goes. The particles on frame k.
constexpr double kDustBirths  = 8.0;
constexpr int kDustBirthsMax  = 16;
constexpr int kMaxParticles   = 64;
std::vector< Particle > Dust( uint32_t seed, int64_t frame, double amount, double aspect );

/// A hair caught in the gate: in each slot of kHairSlot frames one appears
/// with probability Hair, pokes in from an edge of the aperture, trembles at
/// every pull-down, and after its dwell shakes loose. Gate space.
constexpr int kHairSlot   = 96;
constexpr int kHairPoints = 13;
struct Hair
{
	bool present = false;
	double x[ kHairPoints ] = {}, y[ kHairPoints ] = {};///< h-units, gate space
	double halfWidth = 0.0;
	double opacity   = 0.0;
};
Hair HairAt( uint32_t seed, int64_t frame, double amount, double aspect );

/// Splices, as a rate per minute at the running speed. A splice frame jumps
/// and flashes.
bool IsSplice( uint32_t seed, int64_t frame, double perMinute, double fps );
void SpliceJump( uint32_t seed, int64_t frame, double& x, double& y );
constexpr float kSpliceWash = 0.35f;///< density x this on a splice frame

/// Cue dots: four frames of motor cue, then 168 frames on, four of
/// changeover (8 s and 1 s before the end of a reel at 24 fps).
constexpr int kCueFrames = 4;
constexpr int kCueGap    = 168;
bool IsCue( int64_t cueStart, int64_t frame );

//---------------------------------------------------------------------------
// The dyes. A print's dyes fade in the dark at different rates, cyan first,
// magenta last, so an old print goes magenta. Stated law: each dye keeps
// exp( -k x Age ) of its density, k = ( cyan, magenta, yellow ).
// Channel order in the picture is ( R, G, B ) = ( cyan, magenta, yellow ).
//---------------------------------------------------------------------------

constexpr double kFadeCyan    = 2.0;
constexpr double kFadeMagenta = 0.15;
constexpr double kFadeYellow  = 0.8;
void Retention( double age, int perturb, double rgb[ 3 ] );

//---------------------------------------------------------------------------
// The light.
//---------------------------------------------------------------------------

/// A black body at `kelvin` as linear sRGB, white-balanced against 6504 K
/// (the display's white), luminance 1. The CIE 1931 observer is Wyman, Sloan
/// and Shirley's multi-lobe fit (JCGT 2013), not the tabulated functions.
void LampRgb( double kelvin, double rgb[ 3 ] );

//---------------------------------------------------------------------------
// Test hooks. Always 0 in the plugin.
//---------------------------------------------------------------------------

enum Perturb : int
{
	kPerturbInstant        = 1 << 0,///< the shutter sampled at one instant per display frame
	kPerturbPictureSpace   = 1 << 1,///< scratches ride with the picture, not the gate
	kPerturbNoHold         = 1 << 2,///< the clip captured on every display frame
	kPerturbWhiteWeave     = 1 << 3,///< rho = 0
	kPerturbShrinkIgnored  = 1 << 4,///< Shrinkage does nothing to sigma
	kPerturbEqualFade      = 1 << 5,///< every dye fades at the mean rate
	kPerturbResizeReseeds  = 1 << 6,///< a resize makes a new print
	kPerturbResizeClears   = 1 << 7,///< a resize clears the held pictures
};

enum Probe : int
{
	kProbeNone   = 0,
	kProbeLinear = 1,///< linear light, before the encode and the Mix
};

/// PCG's output permutation: integer, exact on every platform.
uint32_t Hash( uint32_t v );
uint32_t Key( uint32_t seed, uint32_t stream, int64_t index, uint32_t sub = 0 );
double Uniform( uint32_t key );///< ( key + 0.5 ) / 2^32

} // namespace gate::model
