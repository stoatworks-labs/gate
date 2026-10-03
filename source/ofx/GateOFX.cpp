/// The OpenFX build of Gate, for DaVinci Resolve, Vegas, Nuke, Natron and other
/// OFX hosts.
///
/// ------------------------------------------------------- what is shared
///
/// **The projector and the print.** Not a port of them: `Model.cpp` (the
/// shutter's closed form, the weave's moving-average sum, the scratches, the
/// dust, the hair, the splices, the cue timing, the dyes and the lamps),
/// `Controls.cpp` (what every 0..1 control means) and `Exposure.cpp` (what one
/// display frame's exposure shows short of its pictures: the weights, the
/// offsets, the flags, the print-data row and every constant the output pass
/// is given) are the same C++ the FFGL plugin links. The print uses the same
/// seed (`model::kPrintSeed`), so the strip is the same strip.
///
/// What is mirrored is the output pass and the capture's sRGB decode, and only
/// those: `Projection.cpp` is `kOutputBody` line for line, in float, both
/// copies marked `//= mirrored`.
///
/// ---------------------------------------------- what is different, and why
///
/// **The clock is the timeline.** OFX renders frames in any order, alone and
/// concurrently, so nothing here accumulates. Output frame t is on screen for
/// one frame period and its exposure is that whole period (the FFGL build's
/// "exposure is the display period"), so the film position at its end is
///
///     p1 = FPS x ( t + 1 ) / rate,   p0 = p1 - FPS / rate
///
/// with `rate` the clip's frame rate. That is exactly the FFGL build's film
/// position on a host that renders every frame in order at `rate` from frame
/// 0 (its first frame is worth 1/60 s, which is one frame period at 60).
/// FPS is not animatable for that reason: the position is FPS x time, not an
/// integral, and a keyframed FPS would jump the strip.
///
/// **The hold is fetched, not remembered.** The FFGL build captures the host's
/// picture on the first display frame whose exposure reaches a new projector
/// frame and keeps two: the newest frame's and the one before. Here the same
/// two are recomputed from the timeline -- the output frame on which each was
/// first reached -- and fetched through temporal clip access. Which segment
/// reads which picture, and which picture is "the frame above" when Framing
/// shows the strip, follow the FFGL bookkeeping exactly (`holdAt` below). The
/// window is bounded: the older picture is the start of the projector frame
/// before the newest, so it is less than 2 / FPS seconds back, plus at most one
/// output frame when FPS is faster than the timeline -- 7 frames of a 60 fps
/// timeline at FPS 16, 2 of a 24 fps one -- and `getFramesNeeded` tells the
/// host exactly which.
/// Before the clip starts the host has nothing to give, and the older picture
/// falls back to the newest, as the FFGL build's first frame does.
///
/// **Cue Dots is a keyframed toggle, not a button.** A momentary press has no
/// meaning on a timeline. The marks start on the projector frame in the gate
/// at the first output frame on which the toggle reads on after reading off,
/// which is exactly where an FFGL press on that frame puts them, and come back
/// 168 projector frames later. The most recent such switch wins, as a later
/// press does in the FFGL build. Finding it reads the toggle back through at
/// most the 172 projector frames a cue can still be on screen for, and not at
/// all when the toggle has no keyframes (a constant value never switches).
///
/// Consequences an editor will see, all of them the machine's: on a 24 fps
/// timeline at FPS 24 every output frame integrates exactly one projector
/// frame, so there is no beat and no double image -- the print, the weave and
/// the marks remain. The flicker and the hold appear when the rates differ.
///
/// ------------------------------------------------------------- and tiles
///
/// The weave, Framing and the neighbour frames read the held pictures away
/// from the pixel being written, so no tiles.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.h"

// After the OFX Support headers, which is where the OFX types come from.
#include "StoatworksAboutOFX.h"

#include "../Controls.h"
#include "../Exposure.h"
#include "../Model.h"
#include "../Projection.h"

namespace
{
using namespace gate;

constexpr const char* kPluginIdentifier = "com.stoatworks.gate";
constexpr const char* kPluginName       = "Gate";
constexpr const char* kPluginGrouping   = "Stoatworks";
constexpr const char* kPluginDescription =
	"A film projector's gate, shutter and print.\n\n"
	"The clip is held at the projector's rate and pulled down with the shutter "
	"closed; the light the blades let through is integrated over each frame, so "
	"a projector whose rate differs from the timeline's flickers at the beat of "
	"the two; each frame lands in the gate with its own weave, more on a shrunk "
	"print; grit in the gate cuts scratches at a fixed x, dark on the base side "
	"and light on the emulsion; dust rides the print, a hair hangs in the gate "
	"until it shakes loose, splices jump and flash, cue dots mark the reel, and "
	"the dyes fade cyan first.\n\n"
	"In OpenFX the projector runs on the timeline: the film position is FPS x "
	"time and each output frame's exposure is one frame of the timeline, so on "
	"a 24 fps timeline at FPS 24 there is no beat. The held pictures are fetched "
	"from the clip where each projector frame was pulled down, at most two "
	"projector frames back. FPS does not animate. Cue Dots is a toggle: "
	"keyframe it from off to on and the marks start on the frame in the gate "
	"then, and return 168 projector frames later.\n\n"
	"The flicker is a whole-frame pulse; the user guide has a photosensitivity "
	"note.\n\n"
	"https://stoatworks-labs.com";

constexpr const char* kParamFps       = "fps";
constexpr const char* kParamBlades    = "blades";
constexpr const char* kParamShutter   = "shutterAngle";
constexpr const char* kParamLamp      = "lamp";
constexpr const char* kParamFraming   = "framing";
constexpr const char* kParamWeave     = "weave";
constexpr const char* kParamShrinkage = "shrinkage";
constexpr const char* kParamHair      = "hair";
constexpr const char* kParamScratches = "scratches";
constexpr const char* kParamDust      = "dust";
constexpr const char* kParamSplices   = "splices";
constexpr const char* kParamCueDots   = "cueDots";
constexpr const char* kParamAge       = "age";
constexpr const char* kParamVignette  = "vignette";
constexpr const char* kParamMix       = "mix";

/// The FFGL defaults (Gate::Gate()), in the same 0..1 host space.
struct Defaults
{
	int fps         = 2;//24
	int blades      = 2;//3
	double shutter  = controls::ShutterParam( 270.0 );
	int lamp        = 1;//Xenon
	double framing  = 0.5;
	double weave    = 0.25;
	double shrink   = 0.25;
	double hair     = 0.3;
	double scratch  = 0.35;
	double dust     = 0.3;
	double splices  = 0.05;
	double age      = 0.1;
	double vignette = 0.4;
	double mix      = 1.0;
};

//---------------------------------------------------------------------------
// The clock: a pure function of the timeline.
//---------------------------------------------------------------------------
struct Clock
{
	double projector = 24.0;///< FPS, projector frames a second
	double rate      = 24.0;///< the clip's frames a second

	double p1( double t ) const
	{
		return projector * ( t + 1.0 ) / rate;
	}
	double p0( double t ) const
	{
		return p1( t ) - projector / rate;
	}

	/// The newest projector frame output frame t's exposure overlaps, computed
	/// exactly as model::Segments numbers its last segment, so the two can
	/// never disagree at a pull-down that lands on an exposure's end.
	int64_t latest( double t ) const
	{
		const double a    = p0( t );
		const double base = std::floor( a );
		const double q1   = p1( t ) - base;
		return static_cast< int64_t >( base ) + static_cast< int64_t >( std::ceil( q1 ) ) - 1;
	}

	/// The output frame on which projector frame k is captured: the first on
	/// t's lattice (t, t - 1, ...) whose exposure reaches k, which is the FFGL
	/// build's "first display frame whose exposure reaches a new frame". k
	/// must be reached by t itself.
	double captureTime( double t, int64_t k ) const
	{
		if( latest( t ) < k )
			return t;
		//p1( m ) > k  <=>  m > k rate / FPS - 1: start there and settle on the
		//exact lattice point with the same arithmetic as latest().
		const double m = static_cast< double >( k ) * rate / projector - 1.0;
		int64_t j      = std::max< int64_t >( 0, static_cast< int64_t >( std::ceil( t - m ) ) - 1 );
		while( j > 0 && latest( t - static_cast< double >( j ) ) < k )
			--j;
		while( latest( t - static_cast< double >( j + 1 ) ) >= k )
			++j;
		return t - static_cast< double >( j );
	}
};

/// What the FFGL build's two held pictures hold at output frame t, and which
/// segments read which (Gate.cpp, "newFrame" / "previousFrame").
struct Hold
{
	int64_t latest      = 0; ///< in held[ current ], captured at `currentTime`
	double currentTime  = 0.0;
	int64_t previous    = 0; ///< the frame held before it, captured at `previousTime`
	double previousTime = 0.0;
	/// Segments up to this frame read the older picture. On the frame a new
	/// picture is captured it is the frame held before; on the frames after,
	/// latest - 1 (Gate.cpp: "any earlier frame is in held[ 1 - current ]").
	int64_t olderUpTo = 0;
};

Hold holdAt( const Clock& clock, double t )
{
	Hold h;
	h.latest       = clock.latest( t );
	h.currentTime  = clock.captureTime( t, h.latest );
	h.previous     = clock.latest( h.currentTime - 1.0 );
	h.previousTime = clock.captureTime( t, h.previous );
	h.olderUpTo    = h.currentTime == t ? h.previous : h.latest - 1;
	return h;
}

/// The projector frame the most recent off-to-on switch of Cue Dots landed on,
/// at or before t; -1 when there is none a segment from `oldest` on could
/// still show (model::IsCue's "never"). `on( time )` reads the toggle.
template< typename On >
int64_t cueStartAt( const Clock& clock, double t, int64_t oldest, On on )
{
	//A press lights frame k only while k - start < kCueGap + kCueFrames, and a
	//newer press replaces an older one, so the search stops at the first
	//switch found going back, or where no switch could reach `oldest`.
	const int64_t reach = oldest - ( model::kCueGap + model::kCueFrames );
	bool later          = on( t );
	for( int64_t j = 0; j < 1000000; ++j )
	{
		const double m = t - static_cast< double >( j );
		const int64_t k = clock.latest( m );
		if( k <= reach )
			return -1;
		const bool earlier = on( m - 1.0 );
		if( later && !earlier )
			return k;
		later = earlier;
	}
	return -1;
}

//---------------------------------------------------------------------------
// Pixels in: a held picture is the capture pass, the sRGB decode into linear
// light. The clip is taken as premultiplied (an unpremultiplied one is
// multiplied up first) so a transparent pixel prints as black film, as it
// does in Resolume.
//---------------------------------------------------------------------------
class CaptureProcessor : public OFX::MultiThread::Processor
{
public:
	CaptureProcessor( const OFX::Image& image, bool premultiplied, projection::Picture& out ) :
		image( image ),
		premultiplied( premultiplied ),
		out( out )
	{
		const OfxRectI b = image.getBounds();
		out.resize( b.x2 - b.x1, b.y2 - b.y1 );
		depth      = image.getPixelDepth();
		components = image.getPixelComponents() == OFX::ePixelComponentRGBA ? 4 : 3;
		for( int i = 0; i < 256; ++i )
			byteTable[ i ] = projection::DecodeSrgb( static_cast< float >( i ) / 255.0f );
	}

	void multiThreadFunction( unsigned int threadID, unsigned int nThreads ) override
	{
		const int rows  = out.height;
		const int chunk = static_cast< int >( ( rows + nThreads - 1 ) / std::max( 1u, nThreads ) );
		const int y0    = static_cast< int >( threadID ) * chunk;
		const int y1    = std::min( rows, y0 + chunk );
		const OfxRectI b = image.getBounds();
		for( int y = y0; y < y1; ++y )
		{
			float* row = out.row( y );
			for( int x = 0; x < out.width; ++x )
			{
				float rgba[ 4 ] = { 0.0f, 0.0f, 0.0f, 0.0f };
				const bool byte = read( b.x1 + x, b.y1 + y, rgba );
				for( int c = 0; c < 3; ++c )
					row[ 3 * x + c ] = byte ? byteTable[ static_cast< int >( rgba[ c ] ) ] : projection::DecodeSrgb( rgba[ c ] );
			}
		}
	}

	/// One pixel, premultiplied. For a premultiplied (or opaque) 8-bit clip
	/// it hands back the raw byte values and true, for the exact table.
	bool read( int x, int y, float rgba[ 4 ] ) const
	{
		const void* p = image.getPixelAddress( x, y );
		if( p == nullptr )
			return false;
		switch( depth )
		{
			case OFX::eBitDepthUByte:
			{
				const unsigned char* v = static_cast< const unsigned char* >( p );
				if( components == 3 || premultiplied || v[ 3 ] == 255 )
				{
					for( int c = 0; c < 3; ++c )
						rgba[ c ] = static_cast< float >( v[ c ] );
					return true;
				}
				const float a = static_cast< float >( v[ 3 ] ) / 255.0f;
				for( int c = 0; c < 3; ++c )
					rgba[ c ] = static_cast< float >( v[ c ] ) / 255.0f * a;
				return false;
			}
			case OFX::eBitDepthUShort:
			{
				const unsigned short* v = static_cast< const unsigned short* >( p );
				const float a           = components == 4 ? static_cast< float >( v[ 3 ] ) / 65535.0f : 1.0f;
				for( int c = 0; c < 3; ++c )
					rgba[ c ] = static_cast< float >( v[ c ] ) / 65535.0f * ( premultiplied ? 1.0f : a );
				return false;
			}
			case OFX::eBitDepthFloat:
			{
				const float* v = static_cast< const float* >( p );
				const float a  = components == 4 ? v[ 3 ] : 1.0f;
				for( int c = 0; c < 3; ++c )
					rgba[ c ] = v[ c ] * ( premultiplied ? 1.0f : a );
				return false;
			}
			default:
				return false;
		}
	}

private:
	const OFX::Image& image;
	bool premultiplied;
	projection::Picture& out;
	OFX::BitDepthEnum depth = OFX::eBitDepthNone;
	int components          = 4;
	float byteTable[ 256 ]  = {};
};

//---------------------------------------------------------------------------
// Pixels out: the output pass, row by row across the host's threads.
//---------------------------------------------------------------------------
class OutputProcessorBase : public OFX::ImageProcessor
{
public:
	explicit OutputProcessorBase( OFX::ImageEffect& effect ) :
		OFX::ImageProcessor( effect )
	{
	}

	void setup( const projection::Pass* p, const OFX::Image* source, bool srcPremultiplied, bool dstPremultiplied )
	{
		pass             = p;
		src              = source;
		srcPremult       = srcPremultiplied;
		dstPremult       = dstPremultiplied;
		srcComponents    = source->getPixelComponents() == OFX::ePixelComponentRGBA ? 4 : 3;
		srcDepth         = source->getPixelDepth();
	}

protected:
	/// The live input at (x, y), premultiplied RGBA floats; outside the
	/// source, transparent black.
	void source( int x, int y, float rgba[ 4 ] ) const
	{
		rgba[ 0 ] = rgba[ 1 ] = rgba[ 2 ] = rgba[ 3 ] = 0.0f;
		const void* p = src->getPixelAddress( x, y );
		if( p == nullptr )
			return;
		float scale = 1.0f;
		switch( srcDepth )
		{
			case OFX::eBitDepthUByte:
				scale = 1.0f / 255.0f;
				for( int c = 0; c < srcComponents; ++c )
					rgba[ c ] = static_cast< float >( static_cast< const unsigned char* >( p )[ c ] ) * scale;
				break;
			case OFX::eBitDepthUShort:
				scale = 1.0f / 65535.0f;
				for( int c = 0; c < srcComponents; ++c )
					rgba[ c ] = static_cast< float >( static_cast< const unsigned short* >( p )[ c ] ) * scale;
				break;
			case OFX::eBitDepthFloat:
				for( int c = 0; c < srcComponents; ++c )
					rgba[ c ] = static_cast< const float* >( p )[ c ];
				break;
			default:
				return;
		}
		if( srcComponents == 3 )
			rgba[ 3 ] = 1.0f;
		else if( !srcPremult )
			for( int c = 0; c < 3; ++c )
				rgba[ c ] *= rgba[ 3 ];
	}

	const projection::Pass* pass = nullptr;
	const OFX::Image* src        = nullptr;
	bool srcPremult              = true;
	bool dstPremult              = true;
	int srcComponents            = 4;
	OFX::BitDepthEnum srcDepth   = OFX::eBitDepthNone;
};

template< class PIX, int nComponents, int maxValue >
class OutputProcessor : public OutputProcessorBase
{
public:
	explicit OutputProcessor( OFX::ImageEffect& effect ) :
		OutputProcessorBase( effect )
	{
	}

	void multiThreadProcessImages( OfxRectI window ) override
	{
		const OfxRectI bounds = _dstImg->getBounds();
		for( int y = window.y1; y < window.y2; ++y )
		{
			if( _effect.abort() )
				break;
			PIX* dst = static_cast< PIX* >( _dstImg->getPixelAddress( window.x1, y ) );
			if( dst == nullptr )
				continue;
			for( int x = window.x1; x < window.x2; ++x, dst += nComponents )
			{
				float in[ 4 ], out[ 4 ];
				source( x, y, in );
				projection::Shade( *pass, x - bounds.x1, y - bounds.y1, in, out );

				//The pass works premultiplied, as the FFGL build's mix with the
				//host's picture does; a straight output is divided back out.
				const float a = out[ 3 ];
				if( !dstPremult && nComponents == 4 )
					for( int c = 0; c < 3; ++c )
						out[ c ] = a > 0.0f ? out[ c ] / a : 0.0f;

				for( int c = 0; c < nComponents; ++c )
					dst[ c ] = quantise( out[ c ] );
			}
		}
	}

private:
	/// Integer formats round to nearest, as a GL framebuffer does; float is
	/// left alone.
	static PIX quantise( float v )
	{
		if( maxValue == 1 )
			return static_cast< PIX >( v );
		v = std::min( std::max( v, 0.0f ), 1.0f );
		return static_cast< PIX >( std::lround( v * static_cast< float >( maxValue ) ) );
	}
};

class GatePlugin : public OFX::ImageEffect
{
public:
	explicit GatePlugin( OfxImageEffectHandle handle ) :
		OFX::ImageEffect( handle )
	{
		dstClip = fetchClip( kOfxImageEffectOutputClipName );
		srcClip = fetchClip( kOfxImageEffectSimpleSourceClipName );

		fps       = fetchChoiceParam( kParamFps );
		blades    = fetchChoiceParam( kParamBlades );
		shutter   = fetchDoubleParam( kParamShutter );
		lamp      = fetchChoiceParam( kParamLamp );
		framing   = fetchDoubleParam( kParamFraming );
		weave     = fetchDoubleParam( kParamWeave );
		shrinkage = fetchDoubleParam( kParamShrinkage );
		hair      = fetchDoubleParam( kParamHair );
		scratches = fetchDoubleParam( kParamScratches );
		dust      = fetchDoubleParam( kParamDust );
		splices   = fetchDoubleParam( kParamSplices );
		cueDots   = fetchBooleanParam( kParamCueDots );
		age       = fetchDoubleParam( kParamAge );
		vignette  = fetchDoubleParam( kParamVignette );
		mix       = fetchDoubleParam( kParamMix );
	}

	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& paramName ) override
	{
		// The About links open a browser and change nothing about the render.
		stoatworks::about::ofx::changedParam( args, paramName );
	}

	/// Every output frame is different even on a still: the weave, the dust
	/// and the shutter move on. A host that assumed otherwise would cache one
	/// frame of a still clip for its whole length.
	void getClipPreferences( OFX::ClipPreferencesSetter& preferences ) override
	{
		preferences.setOutputFrameVarying( true );
	}

	/// The two held pictures are earlier frames of the source; say which, or a
	/// host is entitled to refuse the fetches.
	void getFramesNeeded( const OFX::FramesNeededArguments& args, OFX::FramesNeededSetter& frames ) override
	{
		const Hold hold = holdAt( clockAt( args.time ), args.time );
		OfxRangeD range;
		range.min = std::min( hold.previousTime, args.time );
		range.max = args.time;
		frames.setFramesNeeded( *srcClip, range );
	}

	void render( const OFX::RenderArguments& args ) override
	{
		const double t = args.time;
		std::unique_ptr< OFX::Image > dst( dstClip->fetchImage( t ) );
		std::unique_ptr< OFX::Image > src( srcClip->fetchImage( t ) );
		if( dst == nullptr || src == nullptr )
			OFX::throwSuiteStatusException( kOfxStatFailed );

		const OFX::BitDepthEnum depth       = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();
		if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );

		const OfxRectI bounds = dst->getBounds();
		const int width       = bounds.x2 - bounds.x1;
		const int height      = bounds.y2 - bounds.y1;
		if( width <= 0 || height <= 0 )
			return;
		const double aspect = static_cast< double >( width ) / height;

		//---------------------------------------------------------------------
		// The exposure, and what the print shows on it: Exposure.cpp, the
		// FFGL build's own code.
		//---------------------------------------------------------------------
		const exposure::Settings settings = settingsAt( t );
		const Clock clock                 = clockFor( settings, t );

		model::Segment segments[ model::kMaxSegments ];
		const int count = model::Segments( clock.p0( t ), clock.p1( t ), settings.blades, settings.open, false, segments );
		const Hold hold = holdAt( clock, t );

		const int64_t cueStart = !cueAnimated() ? -1 : cueStartAt( clock, t, segments[ 0 ].frame, [ this ]( double time ) {
			return cueDots->getValueAtTime( time );
		} );

		exposure::Frame frame;
		exposure::Build( model::kPrintSeed, settings, segments, count, aspect, cueStart, {}, frame );

		//---------------------------------------------------------------------
		// The two held pictures, from where the FFGL build would have captured
		// them. Before the clip has a frame to give, the older one is the
		// newer one, as on the FFGL build's first frame.
		//---------------------------------------------------------------------
		const bool srcPremultiplied = comps != OFX::ePixelComponentRGBA
		                              || srcClip->getPreMultiplication() != OFX::eImageUnPreMultiplied;
		const bool dstPremultiplied = comps != OFX::ePixelComponentRGBA
		                              || dstClip->getPreMultiplication() != OFX::eImageUnPreMultiplied;

		std::unique_ptr< OFX::Image > currentImage, previousImage;
		const OFX::Image* current = src.get();
		if( hold.currentTime != t )
		{
			currentImage.reset( srcClip->fetchImage( hold.currentTime ) );
			if( currentImage != nullptr && currentImage->getPixelDepth() == src->getPixelDepth() )
				current = currentImage.get();
		}
		//previousTime is always before currentTime: the frame held before was
		//first reached on an earlier output frame.
		const OFX::Image* previous = nullptr;
		previousImage.reset( srcClip->fetchImage( hold.previousTime ) );
		if( previousImage != nullptr && previousImage->getPixelDepth() == src->getPixelDepth() )
			previous = previousImage.get();

		projection::Picture pictures[ 2 ];
		capture( *current, srcPremultiplied, pictures[ 0 ] );
		const bool havePrevious = previous != nullptr;
		if( havePrevious )
			capture( *previous, srcPremultiplied, pictures[ 1 ] );
		if( pictures[ 0 ].empty() || ( havePrevious && pictures[ 1 ].empty() ) )
			OFX::throwSuiteStatusException( kOfxStatFailed );

		projection::Pass pass;
		pass.frame     = &frame;
		pass.held[ 0 ] = &pictures[ 0 ];
		pass.held[ 1 ] = havePrevious ? &pictures[ 1 ] : &pictures[ 0 ];
		for( int i = 0; i < count; ++i )
		{
			pass.segPicture[ i ] = havePrevious && segments[ i ].frame <= hold.olderUpTo ? 1 : 0;
			pass.segAbove[ i ]   = havePrevious ? 1 : 0;
		}
		pass.outWidth  = static_cast< float >( width );
		pass.outHeight = static_cast< float >( height );

		switch( depth )
		{
			case OFX::eBitDepthUByte:
				comps == OFX::ePixelComponentRGBA
					? run< OutputProcessor< unsigned char, 4, 255 > >( args, dst.get(), src.get(), pass, srcPremultiplied, dstPremultiplied )
					: run< OutputProcessor< unsigned char, 3, 255 > >( args, dst.get(), src.get(), pass, srcPremultiplied, dstPremultiplied );
				break;
			case OFX::eBitDepthUShort:
				comps == OFX::ePixelComponentRGBA
					? run< OutputProcessor< unsigned short, 4, 65535 > >( args, dst.get(), src.get(), pass, srcPremultiplied, dstPremultiplied )
					: run< OutputProcessor< unsigned short, 3, 65535 > >( args, dst.get(), src.get(), pass, srcPremultiplied, dstPremultiplied );
				break;
			case OFX::eBitDepthFloat:
				comps == OFX::ePixelComponentRGBA
					? run< OutputProcessor< float, 4, 1 > >( args, dst.get(), src.get(), pass, srcPremultiplied, dstPremultiplied )
					: run< OutputProcessor< float, 3, 1 > >( args, dst.get(), src.get(), pass, srcPremultiplied, dstPremultiplied );
				break;
			default:
				OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}
	}

private:
	exposure::Settings settingsAt( double t ) const
	{
		//ChoiceParam answers through an out parameter, unlike every other
		//param type in the Support library.
		const auto choice = [ t ]( OFX::ChoiceParam* param ) {
			int value = 0;
			param->getValueAtTime( t, value );
			return static_cast< float >( value );
		};
		const auto slider = [ t ]( OFX::DoubleParam* param ) {
			return static_cast< float >( param->getValueAtTime( t ) );
		};

		exposure::HostValues host;
		host.fps       = choice( fps );
		host.blades    = choice( blades );
		host.shutter   = slider( shutter );
		host.lamp      = choice( lamp );
		host.framing   = slider( framing );
		host.weave     = slider( weave );
		host.shrinkage = slider( shrinkage );
		host.hair      = slider( hair );
		host.scratches = slider( scratches );
		host.dust      = slider( dust );
		host.splices   = slider( splices );
		host.age       = slider( age );
		host.vignette  = slider( vignette );
		host.mix       = slider( mix );
		return exposure::FromHost( host, 0 );
	}

	/// The clip's frame rate. A host that reports none -- some do with
	/// nothing connected -- would otherwise divide by it.
	double frameRate() const
	{
		double rate = dstClip->getFrameRate();
		if( !( rate > 0.0 ) )
			rate = srcClip->getFrameRate();
		if( !( rate > 0.0 ) )
			rate = 24.0;
		return rate;
	}

	Clock clockFor( const exposure::Settings& settings, double ) const
	{
		Clock clock;
		clock.projector = settings.fps;
		clock.rate      = frameRate();
		return clock;
	}

	Clock clockAt( double t ) const
	{
		int value = 0;
		fps->getValueAtTime( t, value );
		Clock clock;
		clock.projector = controls::Fps( value );
		clock.rate      = frameRate();
		return clock;
	}

	/// A toggle with no keyframes never switches, so the search for a switch
	/// is skipped outright. A host that cannot say is searched anyway.
	bool cueAnimated() const
	{
		try
		{
			return cueDots->getNumKeys() > 0;
		}
		catch( ... )
		{
			return true;
		}
	}

	void capture( const OFX::Image& image, bool premultiplied, projection::Picture& out )
	{
		CaptureProcessor processor( image, premultiplied, out );
		processor.multiThread();
	}

	template< class Processor >
	void run( const OFX::RenderArguments& args, OFX::Image* dst, const OFX::Image* src, const projection::Pass& pass,
	          bool srcPremultiplied, bool dstPremultiplied )
	{
		Processor processor( *this );
		processor.setDstImg( dst );
		processor.setup( &pass, src, srcPremultiplied, dstPremultiplied );
		processor.setRenderWindow( args.renderWindow );
		processor.process();
	}

	OFX::Clip* dstClip = nullptr;
	OFX::Clip* srcClip = nullptr;

	OFX::ChoiceParam* fps       = nullptr;
	OFX::ChoiceParam* blades    = nullptr;
	OFX::DoubleParam* shutter   = nullptr;
	OFX::ChoiceParam* lamp      = nullptr;
	OFX::DoubleParam* framing   = nullptr;
	OFX::DoubleParam* weave     = nullptr;
	OFX::DoubleParam* shrinkage = nullptr;
	OFX::DoubleParam* hair      = nullptr;
	OFX::DoubleParam* scratches = nullptr;
	OFX::DoubleParam* dust      = nullptr;
	OFX::DoubleParam* splices   = nullptr;
	OFX::BooleanParam* cueDots  = nullptr;
	OFX::DoubleParam* age       = nullptr;
	OFX::DoubleParam* vignette  = nullptr;
	OFX::DoubleParam* mix       = nullptr;
};

OFX::DoubleParamDescriptor* defineSlider( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                                          OFX::GroupParamDescriptor* group, const char* name, const char* label,
                                          const char* hint, double value )
{
	OFX::DoubleParamDescriptor* param = desc.defineDoubleParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	param->setRange( 0.0, 1.0 );
	param->setDisplayRange( 0.0, 1.0 );
	param->setDefault( value );
	param->setParent( *group );
	page->addChild( *param );
	return param;
}

OFX::ChoiceParamDescriptor* defineChoice( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                                          OFX::GroupParamDescriptor* group, const char* name, const char* label,
                                          const char* hint, int count, const char* ( *labelFor )( int ), int value )
{
	OFX::ChoiceParamDescriptor* param = desc.defineChoiceParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	for( int i = 0; i < count; ++i )
		param->appendOption( labelFor( i ) );
	param->setDefault( value );
	param->setParent( *group );
	page->addChild( *param );
	return param;
}

OFX::GroupParamDescriptor* defineGroup( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page, const char* name )
{
	OFX::GroupParamDescriptor* group = desc.defineGroupParam( name );
	group->setLabels( name, name, name );
	page->addChild( *group );
	return group;
}

mDeclarePluginFactory( GatePluginFactory, {}, {} );
} // namespace

void GatePluginFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	desc.setLabels( kPluginName, kPluginName, kPluginName );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	// The weave, Framing and the neighbour frames read the held pictures away
	// from the pixel being written, so there is no tile to render from. The
	// hold is temporal access: two earlier frames of the source, bounded (see
	// the note at the top of this file).
	desc.setSupportsTiles( false );
	desc.setTemporalClipAccess( true );
	desc.setRenderThreadSafety( OFX::eRenderFullySafe );
	desc.setSupportsMultiResolution( true );
	desc.setSupportsMultipleClipPARs( false );
	desc.setSupportsMultipleClipDepths( false );
}

void GatePluginFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* srcClip = desc.defineClip( kOfxImageEffectSimpleSourceClipName );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGB );
	srcClip->setSupportsTiles( false );
	srcClip->setTemporalClipAccess( true );

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	// Same parameters, same 0..1 ranges, same defaults and the same groups as
	// the FFGL build, so the two inspectors read alike and one guide covers
	// both. The one change is Cue Dots, a toggle here (see the file comment).
	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );
	const Defaults defaults;

	//-------------------------------------------------------------- Projector
	OFX::GroupParamDescriptor* projector = defineGroup( desc, page, "Projector" );

	defineChoice( desc, page, projector, kParamFps, "FPS",
	              "The projector's rate. Film runs at FPS x the timeline's time, so this does not animate. "
	              "Against a timeline at another rate it beats: the hold and the flicker.",
	              controls::kFpsCount, controls::FpsName, defaults.fps )
		->setAnimates( false );

	defineChoice( desc, page, projector, kParamBlades, "Blades",
	              "Openings per revolution of the shutter. More blades flicker faster and shallower.",
	              controls::kBladesCount, controls::BladesName, defaults.blades );

	defineSlider( desc, page, projector, kParamShutter, "Shutter Angle",
	              "Each blade's opening, 45 to 315 degrees across the slider (0.5 is 180). Wider is "
	              "less flicker; the classic 35 mm shutter, 2 blades at 180, beats hardest.",
	              defaults.shutter );

	defineChoice( desc, page, projector, kParamLamp, "Lamp",
	              "The lamp's colour: a carbon arc at 5000 K, xenon at 6200 K, tungsten at 3200 K, "
	              "against a 6504 K display white.",
	              controls::kLampCount, controls::LampName, defaults.lamp );

	defineSlider( desc, page, projector, kParamFraming, "Framing",
	              "Plus or minus half a frame (0.5 is centred). Past the aperture's margin the frame "
	              "line comes into view, and beyond it the neighbouring frame.",
	              defaults.framing );

	//------------------------------------------------------------------- Gate
	OFX::GroupParamDescriptor* gateGroup = defineGroup( desc, page, "Gate" );

	defineSlider( desc, page, gateGroup, kParamWeave, "Weave",
	              "How far each frame lands off in the gate: up to 0.08 mm of the film, each frame "
	              "like the last.",
	              defaults.weave );

	defineSlider( desc, page, gateGroup, kParamShrinkage, "Shrinkage",
	              "0 to 2% shrinkage. A shrunk print weaves more, and less like the last frame.",
	              defaults.shrink );

	defineSlider( desc, page, gateGroup, kParamHair, "Hair",
	              "How often a hair is caught at the aperture's edge, where it trembles at every "
	              "pull-down until it shakes loose.",
	              defaults.hair );

	//------------------------------------------------------------------ Print
	OFX::GroupParamDescriptor* print = defineGroup( desc, page, "Print" );

	defineSlider( desc, page, print, kParamScratches, "Scratches",
	              "Grit in the gate: straight lines at a fixed place on screen while the picture moves "
	              "under them. Dark on the base side, light on the emulsion.",
	              defaults.scratch );

	defineSlider( desc, page, print, kParamDust, "Dust",
	              "Specks and fibres riding the print for a frame or a few.", defaults.dust );

	defineSlider( desc, page, print, kParamSplices, "Splices",
	              "0 to 60 a minute: a splice frame jumps and flashes.", defaults.splices );

	OFX::BooleanParamDescriptor* cue = desc.defineBooleanParam( kParamCueDots );
	cue->setLabels( "Cue Dots", "Cue Dots", "Cue Dots" );
	cue->setHint( "The reel-change marks, top right. Keyframe this from off to on: the dots start on the "
	              "projector frame in the gate at that frame, for four frames, and come back 168 "
	              "projector frames later (7 s at 24 fps). Switching it off does nothing; the latest "
	              "switch on wins." );
	cue->setDefault( false );
	cue->setParent( *print );
	page->addChild( *cue );

	defineSlider( desc, page, print, kParamAge, "Age",
	              "The dyes fading, cyan first and magenta last, so an old print goes pink.",
	              defaults.age );

	//----------------------------------------------------------------- Output
	OFX::GroupParamDescriptor* output = defineGroup( desc, page, "Output" );

	defineSlider( desc, page, output, kParamVignette, "Vignette",
	              "The lens's cos^4 falloff to the corners: a half-angle to the corner of 0 to 35 "
	              "degrees.",
	              defaults.vignette );

	defineSlider( desc, page, output, kParamMix, "Mix",
	              "Wet/dry against the clip. At 1 the projection paints the whole frame, opaque.",
	              defaults.mix );

	// The Stoatworks About block: a read-only credit line and one push button per
	// link, in a group that starts folded. Last, so it sits under the effect's
	// own controls.
	stoatworks::about::ofx::describe( desc, page );
}

OFX::ImageEffect* GatePluginFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new GatePlugin( handle );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static GatePluginFactory* factory =
		new GatePluginFactory( kPluginIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( factory );
}
