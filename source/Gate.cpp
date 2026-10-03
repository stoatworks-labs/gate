#include "Gate.h"

#include "Controls.h"
#include "Diag.h"
#include "Exposure.h"
#include "Model.h"
#include "Shaders.h"

#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

using namespace ffglex;
using namespace gate;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Gate >,                                       // Create method
	"GA01",                                                      // Plugin unique ID of maximum length 4.
	"SW Gate",                                                   // Plugin name
	2,                                                           // API major version number
	1,                                                           // API minor version number
	0,                                                           // Plugin major version number
	1,                                                           // Plugin minor version number
	FF_EFFECT,                                                   // Plugin type
	"A film projector's gate, shutter and print.\n\nThe clip is held at the projector's rate and pulled down with the shutter closed; the light the blades let through is integrated over each display frame, so a 2-blade projector at 24 fps pulses against 60 Hz at 12 Hz; each frame lands in the gate with its own weave, more on a shrunk print; grit in the gate cuts scratches at a fixed x, dark on the base side and light on the emulsion; dust rides the print, a hair hangs in the gate until it shakes loose, splices jump and flash, cue dots mark the reel, and the dyes fade cyan first.",// Plugin description
	"Gate FFGL effect"                                           // About
);

namespace
{
/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

} // namespace

//---------------------------------------------------------------------------
Gate::Gate()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The projector runs on host time.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults, chosen on Resolume's demo clips (AGENTS.md, "Decisions").
	//---------------------------------------------------------------------
	params[ PT_FPS ]     = 2.0f;//24
	// 3 blades at 270 degrees, not the classic 2 at 180: at a 60 Hz display the
	// classic shutter beats at 12 Hz by +-25-29% of the whole frame, which is the
	// 3-30 Hz band broadcast flash guidelines restrict. This is +-7-9% on the demo
	// clips (the release survey, AGENTS.md "Decisions").
	params[ PT_BLADES ]  = 2.0f;//3
	params[ PT_SHUTTER ] = controls::ShutterParam( 270.0 );
	params[ PT_LAMP ]    = 1.0f;//Xenon
	params[ PT_FRAMING ] = 0.5f;//centred

	params[ PT_WEAVE ]     = 0.25f;
	params[ PT_SHRINKAGE ] = 0.25f;
	params[ PT_HAIR ]      = 0.3f;

	params[ PT_SCRATCHES ] = 0.35f;
	params[ PT_DUST ]      = 0.3f;
	params[ PT_SPLICES ]   = 0.05f;
	params[ PT_CUE_DOTS ]  = 0.0f;
	params[ PT_AGE ]       = 0.1f;

	params[ PT_VIGNETTE ] = 0.4f;
	params[ PT_MIX ]      = 1.0f;

	auto declareOptions = [ this ]( unsigned int id, const char* name, int count, const char* ( *nameAt )( int ) ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( count ), params[ id ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( id, static_cast< unsigned int >( i ), nameAt( i ), static_cast< float >( i ) );
	};

	declareOptions( PT_FPS, "FPS", controls::kFpsCount, controls::FpsName );
	declareOptions( PT_BLADES, "Blades", controls::kBladesCount, controls::BladesName );
	SetParamInfof( PT_SHUTTER, "Shutter Angle", FF_TYPE_STANDARD );
	declareOptions( PT_LAMP, "Lamp", controls::kLampCount, controls::LampName );
	SetParamInfof( PT_FRAMING, "Framing", FF_TYPE_STANDARD );

	SetParamInfof( PT_WEAVE, "Weave", FF_TYPE_STANDARD );
	SetParamInfof( PT_SHRINKAGE, "Shrinkage", FF_TYPE_STANDARD );
	SetParamInfof( PT_HAIR, "Hair", FF_TYPE_STANDARD );

	SetParamInfof( PT_SCRATCHES, "Scratches", FF_TYPE_STANDARD );
	SetParamInfof( PT_DUST, "Dust", FF_TYPE_STANDARD );
	SetParamInfof( PT_SPLICES, "Splices", FF_TYPE_STANDARD );
	SetParamInfo( PT_CUE_DOTS, "Cue Dots", FF_TYPE_EVENT, false );
	SetParamInfof( PT_AGE, "Age", FF_TYPE_STANDARD );

	SetParamInfof( PT_VIGNETTE, "Vignette", FF_TYPE_STANDARD );
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	for( FFUInt32 i = PT_FPS; i <= PT_FRAMING; ++i )
		SetParamGroup( i, "Projector" );
	for( FFUInt32 i = PT_WEAVE; i <= PT_HAIR; ++i )
		SetParamGroup( i, "Gate" );
	for( FFUInt32 i = PT_SCRATCHES; i <= PT_AGE; ++i )
		SetParamGroup( i, "Print" );
	for( FFUInt32 i = PT_VIGNETTE; i <= PT_MIX; ++i )
		SetParamGroup( i, "Output" );

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Gate effect" );

	diag::init();
}

//---------------------------------------------------------------------------
FFResult Gate::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	const std::string vertex = shaders::Vertex();
	struct
	{
		FFGLShader* shader;
		std::string fragment;
		const char* name;
	} const stages[] = {
		{ &captureShader, shaders::Capture(), "capture" },
		{ &resampleShader, shaders::Resample(), "resample" },
		{ &outputShader, shaders::Output(), "output" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( vertex, stage.fragment ) )
			continue;

		//Returning FF_FAIL here is invisible to the operator: the effect
		//simply does nothing in Resolume, with no message anywhere. These two
		//lines are the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Gate: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Gate: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	//The print's particles, scratches and hair: one row, texelFetch only.
	glGenTextures( 1, &printTexture );
	glBindTexture( GL_TEXTURE_2D, printTexture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, shaders::kPrintDataWidth, 1, 0, GL_RGBA, GL_FLOAT, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );

	current      = 0;
	heldFrame    = -1;
	havePrevious = false;
	lastWidth = lastHeight = 0;

	diag::info( "initialised" );

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
FFResult Gate::SetTime( double time )
{
	hostTimeSeen = true;
	return CFFGLPlugin::SetTime( time );
}

//The unit voting is readout's, unchanged: the ratio of the host's clock
//delta to a steady clock's names the unit outright, and nothing plausible
//sits between 1 and 1000.
double Gate::nowSeconds()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	if( !hostTimeSeen || hostTime < 0.0 )
		return wallNow - wallStart;

	const double raw = hostTime;

	if( clockScale == 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		//A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}
	lastRawTime  = raw;
	lastWallTime = wallNow;

	//Until the unit is settled, run on the real clock rather than assume one:
	//wrong in origin but right in rate, where assuming seconds would be a
	//thousand times fast on Resolume.
	return clockScale != 0.0 ? raw * clockScale : wallNow - wallStart;
}

//---------------------------------------------------------------------------
FFResult Gate::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	using namespace model;

	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& input = *pGL->inputTextures[ 0 ];
	if( input.Width == 0 || input.Height == 0 )
		return FF_FAIL;

	const int width  = static_cast< int >( input.Width );
	const int height = static_cast< int >( input.Height );

	//The host's viewport, read before anything of ours changes it.
	//ScopedFBOBinding restores the framebuffer binding and only that.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );
	const double outWidth  = hostViewport[ 2 ] > 0 ? hostViewport[ 2 ] : width;
	const double outHeight = hostViewport[ 3 ] > 0 ? hostViewport[ 3 ] : height;
	const double aspect    = outWidth / outHeight;

	//---------------------------------------------------------------------
	// What the controls say. The same conversion the OpenFX build makes
	// (Exposure.cpp), from the same 0..1 host space.
	//---------------------------------------------------------------------
	exposure::HostValues host;
	host.fps       = params[ PT_FPS ];
	host.blades    = params[ PT_BLADES ];
	host.shutter   = params[ PT_SHUTTER ];
	host.lamp      = params[ PT_LAMP ];
	host.framing   = params[ PT_FRAMING ];
	host.weave     = params[ PT_WEAVE ];
	host.shrinkage = params[ PT_SHRINKAGE ];
	host.hair      = params[ PT_HAIR ];
	host.scratches = params[ PT_SCRATCHES ];
	host.dust      = params[ PT_DUST ];
	host.splices   = params[ PT_SPLICES ];
	host.age       = params[ PT_AGE ];
	host.vignette  = params[ PT_VIGNETTE ];
	host.mix       = params[ PT_MIX ];
	const exposure::Settings settings = exposure::FromHost( host, perturb );
	const double fps                  = settings.fps;

	//---------------------------------------------------------------------
	// The clock and the film. Frame-relative: the film position is held in
	// double in projector frames and advanced by this frame's real seconds,
	// so an FPS change carries on from where the strip is, and nothing
	// absolute reaches the shader. The display frame's exposure is the whole
	// display period, [ p0, p1 ]; a frame the clock did not move reuses the
	// last exposure's length rather than dividing by zero.
	//---------------------------------------------------------------------
	const double now = nowSeconds();
	double dt        = kNominalFrame;
	if( lastNow >= 0.0 )
		dt = std::clamp( now - lastNow, 0.0, kMaxFrameDelta );
	lastNow = now;
	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( hostTime ) + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now ) );

	const double exposure = dt > 0.0 ? dt : lastExposure;
	if( dt > 0.0 )
		lastExposure = dt;
	const double p1 = ( running ? filmPosition : 0.0 ) + dt * fps;
	const double p0 = p1 - exposure * fps;
	running         = true;
	filmPosition    = p1;
	windowP0        = p0;
	windowP1        = p1;

	Segment segments[ kMaxSegments ];
	const int count      = Segments( p0, p1, settings.blades, settings.open, ( perturb & kPerturbInstant ) != 0, segments );
	const int64_t latest = segments[ count - 1 ].frame;

	//The cue marks are on the print from the frame in the gate when fired.
	const bool cuePressed = params[ PT_CUE_DOTS ] >= 0.5f;
	if( cuePressed && !cueHeld )
		cueStart = latest;
	cueHeld = cuePressed;

	//---------------------------------------------------------------------
	// Buffers, every allocation before anything binds a texture for the
	// frame's passes. A raster change RESAMPLES the held pictures: the
	// previous projector frame is still on screen through the shutter.
	//---------------------------------------------------------------------
	const bool rasterChanged = lastWidth != 0 && ( lastWidth != width || lastHeight != height );
	lastWidth  = width;
	lastHeight = height;
	if( rasterChanged )
	{
		if( perturb & kPerturbResizeReseeds )
			seed = Hash( seed );//the negative control: a new print
		for( PassBuffer& b : held )
		{
			if( !b.IsValid() )
				continue;
			if( perturb & kPerturbResizeClears )
			{
				//The negative control: the photofinish bug, on purpose.
				if( !b.Ensure( width, height, GL_RGBA16F, PassBuffer::Sampling::Linear ) )
					return FF_FAIL;
				continue;
			}
			if( !spare.Ensure( width, height, GL_RGBA16F, PassBuffer::Sampling::Linear ) )
			{
				diag::error( "could not allocate a resize buffer at " + std::to_string( width ) + "x" + std::to_string( height ) );
				return FF_FAIL;
			}
			{
				ScopedFBOBinding fbo( spare.GetGLID(), ScopedFBOBinding::RB_REVERT );
				spare.ResizeViewPort();
				ScopedShaderBinding shader( resampleShader.GetGLID() );
				ScopedSamplerActivation sampler( 0 );
				Scoped2DTextureBinding texture( b.TextureID() );
				resampleShader.Set( "Old", 0 );
				quad.Draw();
			}
			b.Swap( spare );
		}
		spare.Destroy();//the old raster's picture, now in `spare`
	}

	const bool noHold   = ( perturb & kPerturbNoHold ) != 0;
	const bool newFrame = noHold || latest > heldFrame || !held[ current ].IsValid();
	int64_t previousFrame = -1;
	if( newFrame )
	{
		if( held[ current ].IsValid() && heldFrame >= 0 && !noHold )
		{
			previousFrame = heldFrame;
			current       = 1 - current;
			havePrevious  = true;
		}
		if( !held[ current ].Ensure( width, height, GL_RGBA16F, PassBuffer::Sampling::Linear ) )
		{
			diag::error( "could not allocate the held picture at " + std::to_string( width ) + "x" + std::to_string( height ) );
			return FF_FAIL;
		}
		heldFrame = latest;
	}
	else if( havePrevious )
		previousFrame = heldFrame - 1;//any earlier frame is in held[ 1 - current ]

	//---------------------------------------------------------------------
	// The print, for every projector frame this display frame shows
	// (Exposure.cpp, shared with the OpenFX build), and which held picture
	// each frame and the frame above it on the strip are in.
	//---------------------------------------------------------------------
	exposure::Frame frame;
	exposure::Build( seed, settings, segments, count, aspect, cueStart, scratchOverride, frame );
	lastScratches = frame.scratches;

	int pictures[ kMaxSegments ] = {};
	int above[ kMaxSegments ]    = {};
	for( int i = 0; i < count; ++i )
	{
		const bool older = !noHold && havePrevious && segments[ i ].frame <= previousFrame;
		pictures[ i ]    = older ? 1 - current : current;
		above[ i ]       = havePrevious && !noHold ? 1 - current : current;
	}

	{
		Scoped2DTextureBinding texture( printTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, shaders::kPrintDataWidth, 1, GL_RGBA, GL_FLOAT, frame.data.data() );
	}

	//---------------------------------------------------------------------
	// 1. Capture, on a pull-down only: between pull-downs the gate holds.
	//---------------------------------------------------------------------
	if( newFrame )
	{
		ScopedFBOBinding fbo( held[ current ].GetGLID(), ScopedFBOBinding::RB_REVERT );
		held[ current ].ResizeViewPort();
		ScopedShaderBinding shader( captureShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( input.Handle );
		captureShader.Set( "Source", 0 );
		quad.Draw();
	}

	//---------------------------------------------------------------------
	// 2. The projection, straight to the host.
	//---------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		const GLuint other = held[ 1 - current ].IsValid() ? held[ 1 - current ].TextureID() : held[ current ].TextureID();
		ScopedShaderBinding shader( outputShader.GetGLID() );
		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding held0( current == 0 ? held[ 0 ].TextureID() : other );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding held1( current == 1 ? held[ 1 ].TextureID() : other );
		ScopedSamplerActivation sampler2( 2 );
		Scoped2DTextureBinding sourceTexture( input.Handle );
		ScopedSamplerActivation sampler3( 3 );
		Scoped2DTextureBinding printBinding( printTexture );

		const FFGLTexCoords maxCoords = GetMaxGLTexCoords( input );

		outputShader.Set( "Held0", 0 );
		outputShader.Set( "Held1", 1 );
		outputShader.Set( "Source", 2 );
		outputShader.Set( "PrintData", 3 );
		glUniform2i( outputShader.FindUniform( "HeldSize" ), width, height );
		outputShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		outputShader.Set( "OutSize", static_cast< float >( outWidth ), static_cast< float >( outHeight ) );
		outputShader.Set( "SegCount", count );
		glUniform1fv( outputShader.FindUniform( "SegWeight" ), count, frame.weights );
		glUniform2fv( outputShader.FindUniform( "SegOffset" ), count, frame.offsets );
		glUniform1iv( outputShader.FindUniform( "SegPicture" ), count, pictures );
		glUniform1iv( outputShader.FindUniform( "SegAbove" ), count, above );
		glUniform1iv( outputShader.FindUniform( "SegFlags" ), count, frame.flags );
		outputShader.Set( "Framing", frame.framing );
		outputShader.Set( "Pitch", frame.pitch );
		outputShader.Set( "Margin", frame.margin );
		outputShader.Set( "MaxDensity", frame.maxDensity );
		outputShader.Set( "Retention", frame.retention[ 0 ], frame.retention[ 1 ], frame.retention[ 2 ] );
		outputShader.Set( "SpliceWash", frame.spliceWash );
		outputShader.Set( "ParticleCount", frame.particleCount );
		outputShader.Set( "ScratchCount", frame.scratchCount );
		outputShader.Set( "HairPoints", frame.hairShape.present ? kHairPoints : 0 );
		outputShader.Set( "HairMask", frame.hairMask );
		outputShader.Set( "HairHalfWidth", frame.hairHalfWidth );
		outputShader.Set( "HairOpacity", frame.hairOpacity );
		outputShader.Set( "CueCentre", frame.cueCentre[ 0 ], frame.cueCentre[ 1 ] );
		outputShader.Set( "CueRadius", frame.cueRadius );
		outputShader.Set( "Lamp", frame.lamp[ 0 ], frame.lamp[ 1 ], frame.lamp[ 2 ] );
		outputShader.Set( "VignetteTan", frame.vignetteTan );
		outputShader.Set( "Aspect", frame.aspect );
		outputShader.Set( "MixAmount", frame.mix );
		outputShader.Set( "Probe", probe );
		outputShader.Set( "Perturb", perturb );
		quad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Gate::DeInitGL()
{
	captureShader.FreeGLResources();
	resampleShader.FreeGLResources();
	outputShader.FreeGLResources();
	quad.Release();
	for( PassBuffer& b : held )
		b.Destroy();
	spare.Destroy();
	if( printTexture != 0 )
	{
		glDeleteTextures( 1, &printTexture );
		printTexture = 0;
	}
	current      = 0;
	heldFrame    = -1;
	havePrevious = false;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Gate::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// An About button is a press, not a value to keep: it opens a browser and
	// nothing about the effect changes.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Gate::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

//---------------------------------------------------------------------------
char* Gate::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Gate::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

//---------------------------------------------------------------------------
void Gate::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}

void Gate::SetPerturbForTest( int bits )
{
	perturb = bits;
}

void Gate::SetProbeForTest( int p )
{
	probe = p;
}

void Gate::SetScratchOverrideForTest( const std::vector< model::Scratch >& list )
{
	scratchOverride = list;
}

void Gate::LastWindowForTest( double& p0, double& p1 ) const
{
	p0 = windowP0;
	p1 = windowP1;
}

std::vector< model::Scratch > Gate::LastScratchesForTest() const
{
	return lastScratches;
}

size_t Gate::StateBytesForTest() const
{
	size_t bytes = 0;
	auto count   = [ &bytes ]( const PassBuffer& b, size_t perTexel ) {
		if( b.IsValid() )
			bytes += static_cast< size_t >( b.GetWidth() ) * b.GetHeight() * perTexel;
	};
	count( held[ 0 ], 8 );
	count( held[ 1 ], 8 );
	count( spare, 8 );
	return bytes + static_cast< size_t >( shaders::kPrintDataWidth ) * 16;
}
