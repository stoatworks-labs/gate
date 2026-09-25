/**
	gatest -- render Gate offline, and measure the projector out of it.

	Every check here drives the REAL plugin class through a headless GL
	context on a synthetic clock and measures the property it claims from the
	picture the plugin renders, against a prediction derived here by a
	different route from the one the plugin takes:

		gatest --out /tmp/frame.png     a picture, on the moving test card
		gatest --list                   every parameter, its kind and default
		gatest --flicker                a flat grey's brightness series has the
		                                spectrum the shutter's Fourier series
		                                predicts: its strongest line at the beat
		                                of blades x fps against 60 Hz
		gatest --hold                   the picture changes only at pull-downs,
		                                FPS distinct pictures a second, and every
		                                pull-down happens in the dark
		gatest --scratch                a scratch stays put while the picture
		                                moves under it; its side sets its polarity
		gatest --weave                  the frame offsets have the stated sigma
		                                and lag-1 correlation, with and without
		                                shrinkage
		gatest --fade                   a neutral grey goes magenta through Age,
		                                each dye keeping the density the law says
		gatest --resize                 the print and the held pictures survive a
		                                resize
		gatest --negative               every check above can FAIL
		gatest --names                  nothing the host will truncate
		gatest --bench                  the render cost and the state held
		gatest --dump-shaders DIR       the exact GLSL the plugin compiles
		gatest --pipe                   raw frames in, raw frames out

	AGENTS.md has one line per check on where each tolerance comes from.
*/

#include "Controls.h"
#include "Gate.h"
#include "Model.h"
#include "Shaders.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
namespace model    = gate::model;
namespace controls = gate::controls;

int g_checks   = 0;
int g_failures = 0;

/// 2^-24: a float's relative rounding error, half an ULP at 1.
constexpr double kU = 5.9604644775390625e-8;

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );
	ihdr.push_back( 6 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Pictures, float RGBA, top-first, in sRGB code values (what a host hands an
// effect).
//---------------------------------------------------------------------------
using Picture = std::vector< float >;

Picture flat( int W, int H, double level )
{
	Picture p( static_cast< size_t >( W ) * H * 4 );
	for( size_t i = 0; i < p.size(); i += 4 )
	{
		p[ i ] = p[ i + 1 ] = p[ i + 2 ] = static_cast< float >( level );
		p[ i + 3 ]                       = 1.0f;
	}
	return p;
}

void put( Picture& p, int W, int x, int y, double level )
{
	float* px = p.data() + ( static_cast< size_t >( y ) * W + x ) * 4;
	px[ 0 ] = px[ 1 ] = px[ 2 ] = static_cast< float >( level );
	px[ 3 ]                     = 1.0f;
}

/// The sRGB decode, in double: what the capture pass computes in float.
double decode( double c )
{
	return c <= 0.04045 ? c / 12.92 : std::pow( ( c + 0.055 ) / 1.055, 2.4 );
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	//GATEST_RENDERER=software asks for Apple's software renderer by id, on a
	//Mac that has a GPU. It is what a GPU-less CI runner falls back to, and it
	//is not bit-repeatable frame to frame (repousse's resize check failed CI
	//by one ulp), so a check that would fail only in CI can be run here first.
	const CGLPixelFormatAttribute generic[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	const char* renderer     = std::getenv( "GATEST_RENDERER" );
	if( renderer != nullptr && std::strcmp( renderer, "software" ) == 0 )
	{
		if( CGLChoosePixelFormat( generic, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
		std::fprintf( stderr, "gatest: GATEST_RENDERER=software, Apple's software renderer\n" );
	}
	else if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, GLint internalFormat, GLenum type, const void* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, type, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

template< typename T >
std::vector< T > flipRows( const std::vector< T >& image, int width, int height )
{
	std::vector< T > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::copy( image.begin() + static_cast< long >( ( height - 1 - y ) * stride ),
		           image.begin() + static_cast< long >( ( height - y ) * stride ),
		           flipped.begin() + static_cast< long >( y * stride ) );
	return flipped;
}

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	unsigned int type;
	float value;
	float low;
	float high;
};

const char* kindName( const NamedParameter& p )
{
	if( p.index >= Gate::PT_ABOUT_FIRST )
		return "about";
	switch( p.type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_STANDARD: return "standard";
	default: return "other";
	}
}

std::vector< NamedParameter > listParameters( Gate& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < Gate::PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		NamedParameter p;
		p.name  = name ? name : "?";
		p.index = i;
		p.type  = plugin.GetParamType( i );
		p.value = plugin.GetFloatParameter( i );
		p.low   = 0.0f;
		p.high  = 1.0f;
		//An option's range reads back 0..1 whatever its element count, so
		//the element count is the range.
		if( p.type == FF_TYPE_OPTION )
			p.high = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( i ) ) - 1u );
		list.push_back( p );
	}
	return list;
}

int indexOfParameter( Gate& plugin, const std::string& name )
{
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name == name )
			return static_cast< int >( p.index );
	return -1;
}

bool applySetting( Gate& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.rfind( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name = assignment.substr( 0, equals );
	const int index        = indexOfParameter( plugin, name );
	if( index < 0 )
	{
		error = "no parameter called '" + name + "'";
		return false;
	}
	plugin.SetFloatParameter( static_cast< unsigned int >( index ), std::strtof( assignment.substr( equals + 1 ).c_str(), nullptr ) );
	return true;
}

bool set( Gate& plugin, const char* name, float value )
{
	std::string error;
	char buffer[ 64 ];
	std::snprintf( buffer, sizeof( buffer ), "%.9g", value );
	if( applySetting( plugin, std::string( name ) + "=" + buffer, error ) )
		return true;
	std::fprintf( stderr, "%s\n", error.c_str() );
	return false;
}

/// The projector a check runs: the controls it moves, everything else OFF
/// (no print, no weave, no lens falloff, centred), so each check sees only
/// what it measures.
struct Knobs
{
	int fps         = 2;//24
	int blades      = 1;//2
	double degrees  = 180.0;
	int lamp        = 1;//Xenon
	double weave    = 0.0;
	double shrink   = 0.0;
	double hair     = 0.0;
	double scratches = 0.0;
	double dust     = 0.0;
	double splices  = 0.0;
	double age      = 0.0;
};

void apply( Gate& p, const Knobs& k )
{
	set( p, "FPS", static_cast< float >( k.fps ) );
	set( p, "Blades", static_cast< float >( k.blades ) );
	set( p, "Shutter Angle", controls::ShutterParam( k.degrees ) );
	set( p, "Lamp", static_cast< float >( k.lamp ) );
	set( p, "Framing", 0.5f );
	set( p, "Weave", static_cast< float >( k.weave ) );
	set( p, "Shrinkage", static_cast< float >( k.shrink ) );
	set( p, "Hair", static_cast< float >( k.hair ) );
	set( p, "Scratches", static_cast< float >( k.scratches ) );
	set( p, "Dust", static_cast< float >( k.dust ) );
	set( p, "Splices", static_cast< float >( k.splices ) );
	set( p, "Age", static_cast< float >( k.age ) );
	set( p, "Vignette", 0.0f );
	set( p, "Mix", 1.0f );
}

double projectorFps( const Knobs& k )
{
	return controls::Fps( k.fps );
}

//---------------------------------------------------------------------------
// A session: the plugin, its input and output, and the clock that drives it.
//---------------------------------------------------------------------------
struct Session
{
	Gate plugin;
	int width  = 0;
	int height = 0;
	double fps = 60.0;
	/// Seconds added to every frame's clock from frame 1 on: moves the
	/// projector's phase against the display's, so no display frame's
	/// exposure ends exactly on a pull-down (where the plugin's accumulated
	/// double could fall either side).
	double lead = 0.0;
	/// Render into an RGBA32F framebuffer rather than RGBA8.
	bool floatOutput = true;

	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };
	ProcessOpenGLStruct process    = {};

	void makeTargets()
	{
		sourceTexture = makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr );
		outputTexture = floatOutput ? makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr )
		                            : makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, nullptr );
		outputFBO     = makeFramebuffer( outputTexture );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;
	}

	void dropTargets()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool begin( int w, int h )
	{
		width  = w;
		height = h;
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}
		makeTargets();
		return true;
	}

	/// What a host does when the clip or the composition changes size: hand
	/// the SAME instance a differently sized input. No DeInitGL.
	void resize( int w, int h )
	{
		dropTargets();
		width  = w;
		height = h;
		makeTargets();
	}

	void upload( const Picture& pixels )
	{
		const std::vector< float > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	void upload( const std::vector< unsigned char >& pixels )
	{
		const std::vector< unsigned char > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	double timeOf( long frame ) const
	{
		return static_cast< double >( frame ) / fps + ( frame > 0 ? lead : 0.0 );
	}

	/// A synthetic clock, and it has to be synthetic: frame n is clocked at
	/// n / fps (+ lead), the unit declared, not inferred.
	bool renderAt( long frame )
	{
		plugin.SetClockScaleForTest( 1.0 );
		plugin.SetTime( timeOf( frame ) );

		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		const bool ok = plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
		if( !ok )
			std::fprintf( stderr, "ProcessOpenGL failed on frame %ld\n", frame );
		return ok;
	}

	bool render( long frame, const std::vector< unsigned char >& pixels )
	{
		upload( pixels );
		return renderAt( frame );
	}

	bool render( long frame, const Picture& pixels )
	{
		upload( pixels );
		return renderAt( frame );
	}

	/// One pixel as floats; x from the left, y from the TOP.
	void readPixel( int x, int y, float out[ 4 ] )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( x, height - 1 - y, 1, 1, GL_RGBA, GL_FLOAT, out );
	}

	/// A row from the TOP, RGBA floats.
	std::vector< float > readRow( int y )
	{
		std::vector< float > row( static_cast< size_t >( width ) * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, height - 1 - y, width, 1, GL_RGBA, GL_FLOAT, row.data() );
		return row;
	}

	/// A column, top first, RGBA floats.
	std::vector< float > readColumn( int x )
	{
		std::vector< float > column( static_cast< size_t >( height ) * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( x, 0, 1, height, GL_RGBA, GL_FLOAT, column.data() );
		return flipRows( column, 1, height );
	}

	std::vector< float > readAll()
	{
		std::vector< float > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return pixels;
	}

	std::vector< unsigned char > readBack()
	{
		std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return flipRows( pixels, width, height );
	}

	void end()
	{
		plugin.DeInitGL();
		dropTargets();
	}
};

/// The harness's own model of the plugin's clock (the stated contract, in
/// Model.h): the first frame is worth kNominalFrame, every later one its
/// real seconds, and the film runs at the projector's rate from 0.
struct Clock
{
	double fps = 60.0, lead = 0.0, projector = 24.0;
	double t( long m ) const
	{
		return static_cast< double >( m ) / fps + ( m > 0 ? lead : 0.0 );
	}
	double p1( long m ) const
	{
		return projector * ( model::kNominalFrame + t( m ) - t( 0 ) );
	}
	double p0( long m ) const
	{
		return p1( m ) - projector * ( m == 0 ? model::kNominalFrame : t( m ) - t( m - 1 ) );
	}
	/// The newest projector frame the exposure overlaps.
	int64_t latest( long m ) const
	{
		return static_cast< int64_t >( std::ceil( p1( m ) ) ) - 1;
	}
	int64_t first( long m ) const
	{
		return static_cast< int64_t >( std::floor( p0( m ) ) );
	}
	/// How far the exposure's ends are from any pull-down, in frames.
	double margin( long m ) const
	{
		auto d = []( double p ) { return std::fabs( p - std::round( p ) ); };
		return std::min( d( p0( m ) ), d( p1( m ) ) );
	}
};

const char* verdict( bool ok )
{
	return ok ? "ok" : "FAIL";
}

int report( bool ok, bool quiet, const char* format, ... ) __attribute__( ( format( printf, 3, 4 ) ) );
int report( bool ok, bool quiet, const char* format, ... )
{
	++g_checks;
	if( !ok )
		++g_failures;
	//Quiet is a negative control's run: its failures are the point, and the
	//summary line says so. --perturb runs the same thing verbosely.
	if( quiet )
		return ok ? 0 : 1;
	va_list args;
	va_start( args, format );
	std::printf( "   %-4s ", verdict( ok ) );
	std::vprintf( format, args );
	std::printf( "\n" );
	va_end( args );
	return ok ? 0 : 1;
}

void note( bool quiet, const char* format, ... ) __attribute__( ( format( printf, 2, 3 ) ) );
void note( bool quiet, const char* format, ... )
{
	if( quiet )
		return;
	va_list args;
	va_start( args, format );
	std::printf( "        " );
	std::vprintf( format, args );
	std::printf( "\n" );
	va_end( args );
}

double sinc( double x )
{
	return std::fabs( x ) < 1e-300 ? 1.0 : std::sin( model::kPi * x ) / ( model::kPi * x );
}

//---------------------------------------------------------------------------
// --flicker
//
// A flat grey, the lens and the print off, read at the centre through the
// linear probe for N display frames at 60 fps. Each is divided by the series'
// mean. The prediction is NOT the plugin's piecewise integral: it is the
// shutter's Fourier series,
//   s( p ) = f + sum_n ( 2 sin( pi n f ) / ( pi n ) ) ( -1 )^n cos( 2 pi n B p ),
// averaged over each exposure (a sinc per harmonic) and sampled at 60 Hz, so
// harmonic n lands in the DFT bin of its alias with a known complex amplitude.
// N is a multiple of every period involved, so there is no leakage and the
// DC is exactly 1 (every harmonic that aliases to DC has an integral number
// of cycles in an exposure, and its sinc is 0).
//---------------------------------------------------------------------------
int runFlicker( int W, int H, int perturb, bool quiet = false )
{
	struct Case
	{
		int fps, blades;
		double degrees;
	};
	const Case cases[] = { { 2, 1, 180.0 }, { 2, 2, 180.0 }, { 3, 1, 180.0 }, { 1, 2, 240.0 } };
	const int N = 300, skip = 4;
	if( !quiet )
		std::printf( "flicker: a flat grey at 60 Hz, %d frames, its spectrum against the shutter's Fourier series, %dx%d\n", N, W, H );
	int failures = 0;
	for( const Case& c : cases )
	{
		Session s;
		Knobs k;
		k.fps     = c.fps;
		k.blades  = c.blades;
		k.degrees = c.degrees;
		apply( s.plugin, k );
		s.plugin.SetPerturbForTest( perturb );
		s.plugin.SetProbeForTest( model::kProbeLinear );
		if( !s.begin( W, H ) )
			return 1;
		const Picture grey = flat( W, H, 0.5 );
		s.upload( grey );
		std::vector< double > y;
		for( long m = 0; m < skip + N; ++m )
		{
			if( !s.renderAt( m ) )
				return 1;
			if( m < skip )
				continue;
			float px[ 4 ];
			s.readPixel( W / 2, H / 2, px );
			y.push_back( px[ 1 ] );
		}
		s.end();

		double mean = 0.0;
		for( double v : y )
			mean += v;
		mean /= N;
		for( double& v : y )
			v /= mean;

		//The measured spectrum.
		std::vector< std::complex< double > > X( N / 2 + 1 );
		for( int j = 0; j <= N / 2; ++j )
		{
			std::complex< double > sum = 0.0;
			for( int m = 0; m < N; ++m )
				sum += y[ m ] * std::polar( 1.0, -2.0 * model::kPi * j * m / N );
			X[ j ] = sum / static_cast< double >( N );
		}

		//The predicted one.
		Clock clock;
		clock.projector   = controls::Fps( c.fps );
		const double B    = controls::Blades( c.blades );
		const double f    = controls::ShutterOpen( controls::ShutterParam( c.degrees ) );
		const double D    = clock.projector / clock.fps;//an exposure, in frames
		const double pMid = clock.p1( skip ) - 0.5 * D;  //the first exposure's middle
		std::vector< std::complex< double > > P( N / 2 + 1, 0.0 );
		P[ 0 ]            = 1.0;
		const long terms  = 2000000;
		bool commensurate = true;
		for( long n = 1; n <= terms; ++n )
		{
			const double sign = ( n & 1 ) ? -1.0 : 1.0;
			const double cn   = sign * 2.0 * std::sin( model::kPi * n * f ) / ( model::kPi * n * f ) * sinc( n * B * D );
			const double nu   = n * B * D - std::floor( n * B * D );//cycles per display frame
			const double bin  = nu * N;
			const long j      = std::lround( bin );
			if( std::fabs( bin - j ) > 1e-6 )
				commensurate = false;
			const double arg = n * B * pMid;
			const double th  = 2.0 * model::kPi * ( arg - std::floor( arg ) );
			const long jj    = j % N;
			if( jj <= N / 2 )
				P[ jj ] += 0.5 * cn * std::polar( 1.0, th );
			if( N - jj <= N / 2 && jj != 0 )
				P[ N - jj ] += 0.5 * cn * std::polar( 1.0, -th );
		}
		//The series' tail past `terms`: |c_n| <= 2 / ( pi n f ) x 1 / ( pi n B D ).
		const double tail = 2.0 / ( model::kPi * model::kPi * f * B * D * terms );
		//Each sample is Sum w_k T x lamp through float: the float weights (one
		//rounding each), a product and a sum per segment (at most two here),
		//the lamp: 8 roundings of 2^-24, and the division by the mean doubles
		//it. Each DFT bin is a mean of the samples, so it carries the same.
		double peak = 0.0;
		for( double v : y )
			peak = std::max( peak, std::fabs( v ) );
		const double tolerance = 2.0 * 8.0 * kU * peak + tail;

		double worst = 0.0;
		int strongest = 1;
		for( int j = 1; j <= N / 2; ++j )
		{
			worst = std::max( worst, std::abs( X[ j ] - P[ j ] ) );
			if( std::abs( X[ j ] ) > std::abs( X[ strongest ] ) )
				strongest = j;
		}
		const double shutterHz = B * clock.projector;
		const double alias     = std::fabs( shutterHz - std::round( shutterHz / clock.fps ) * clock.fps );
		const double measuredHz = strongest * clock.fps / N;
		const double amplitude  = 2.0 * std::abs( X[ strongest ] );
		const double predicted  = 2.0 * std::abs( P[ std::lround( alias * N / clock.fps ) ] );

		char label[ 96 ];
		std::snprintf( label, sizeof( label ), "%g fps x %g blades at %g deg (%g Hz)", clock.projector, B, c.degrees, shutterHz );
		failures += report( commensurate, quiet, "%s: %d frames hold whole periods of every harmonic (no leakage)", label, N );
		failures += report( std::fabs( measuredHz - alias ) < 0.5 * clock.fps / N, quiet,
		                    "%s: strongest line at %.2f Hz, the beat |%g - 60| folds to %.2f Hz", label, measuredHz, shutterHz, alias );
		failures += report( worst <= tolerance, quiet, "%s: every DFT bin on the Fourier series, worst %.2e (tolerance %.2e); the beat's amplitude %.5f, predicted %.5f",
		                    label, worst, tolerance, amplitude, predicted );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --hold
//---------------------------------------------------------------------------
int runHold( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "hold: each display frame's picture against the frame the projector pulled down, %dx%d\n", W, H );
	int failures = 0;

	//Part 1: the input changes on every display frame: its left half is a
	//level unique to the frame, its right half a fixed reference. The ratio of
	//the two halves names the input frame the gate is showing, whatever the
	//shutter's gain.
	for( int fpsIndex : { 2, 3 } )
	{
		Session s;
		Knobs k;
		k.fps = fpsIndex;
		apply( s.plugin, k );
		s.plugin.SetPerturbForTest( perturb );
		s.plugin.SetProbeForTest( model::kProbeLinear );
		s.lead = 0.0071;
		if( !s.begin( W, H ) )
			return 1;
		Clock clock;
		clock.lead      = s.lead;
		clock.projector = controls::Fps( fpsIndex );
		const int M     = 184;
		auto level      = []( long m ) { return 0.12 + 0.0037 * static_cast< double >( m ); };
		const double reference = 0.5;
		std::vector< double > ratioOf( M );
		const double kHalfRatio = 2.5e-3;
		for( long m = 0; m < M; ++m )
			ratioOf[ m ] = decode( level( m ) ) / decode( reference );

		//The capture frame of projector frame k: the first display frame
		//whose exposure reaches it.
		std::map< int64_t, long > captured;
		for( long m = 0; m < M; ++m )
			for( int64_t kk = clock.first( m ); kk <= clock.latest( m ); ++kk )
				if( !captured.count( kk ) )
					captured[ kk ] = m;

		int single = 0, wrong = 0, blendsOk = 0, blends = 0, unmatched = 0;
		double worstMargin = 1.0;
		std::set< long > shownFirstSecond;
		std::vector< long > shown( M, -1 );
		for( long m = 0; m < M; ++m )
		{
			Picture p( static_cast< size_t >( W ) * H * 4 );
			for( int yy = 0; yy < H; ++yy )
				for( int xx = 0; xx < W; ++xx )
					put( p, W, xx, yy, xx < W / 2 ? level( m ) : reference );
			if( !s.render( m, p ) )
				return 1;
			//Frame 0's exposure starts at film position 0 exactly, a
			//pull-down: rendered, never judged.
			if( m == 0 )
				continue;
			float a[ 4 ], b[ 4 ];
			s.readPixel( W / 4, H / 2, a );
			s.readPixel( 3 * W / 4, H / 2, b );
			const double r = static_cast< double >( a[ 1 ] ) / b[ 1 ];
			worstMargin    = std::min( worstMargin, clock.margin( m ) );
			if( clock.first( m ) == clock.latest( m ) )
			{
				//One projector frame in the exposure: exactly one input frame.
				++single;
				long best = 0;
				for( long c = 0; c < M; ++c )
					if( std::fabs( ratioOf[ c ] - r ) < std::fabs( ratioOf[ best ] - r ) )
						best = c;
				//The held picture is RGBA16F: each half stores its level to a
				//relative 2^-11, so the ratio is good to 2^-10 (plus float
				//noise). Neighbouring levels differ by >= 1.1%, eleven times that.
				if( std::fabs( ratioOf[ best ] / r - 1.0 ) > kHalfRatio )
				{
					++unmatched;
					continue;
				}
				shown[ m ] = best;
				if( best != captured[ clock.latest( m ) ] )
					++wrong;
			}
			else
			{
				//Two frames in the exposure: a blend of exactly the two
				//pictures, so strictly between their ratios.
				++blends;
				const double lo = ratioOf[ captured[ clock.first( m ) ] ], hi = ratioOf[ captured[ clock.latest( m ) ] ];
				if( r > std::min( lo, hi ) * ( 1.0 - kHalfRatio ) && r < std::max( lo, hi ) * ( 1.0 + kHalfRatio ) )
					++blendsOk;
			}
		}
		s.end();

		//Each picture's level says which display frame captured it, so the
		//pictures CAPTURED in a whole second of display frames can be counted
		//out of the picture: a pull-down is captured by the display frame
		//whose exposure contains it, and a second of exposures contains
		//exactly fps pull-downs (none falls on an exposure's end).
		std::vector< int > perSecond;
		std::set< long > inputs;
		for( long m = 1; m < M; ++m )
			if( shown[ m ] >= 0 )
				inputs.insert( shown[ m ] );
		for( long start = 1; start + 60 + 3 <= M; start += 60 )
		{
			int n = 0;
			for( long c : inputs )
				if( c >= start && c < start + 60 )
					++n;
			perSecond.push_back( n );
		}
		char label[ 32 ];
		std::snprintf( label, sizeof( label ), "%g fps", clock.projector );
		failures += report( unmatched == 0 && wrong == 0, quiet, "%s: all %d one-frame exposures show the input captured at their projector frame's pull-down (%d wrong, %d unreadable)",
		                    label, single, wrong, unmatched );
		failures += report( blendsOk == blends, quiet, "%s: all %d exposures that straddle a pull-down blend exactly the two frames' pictures (%d do)", label, blends, blendsOk );
		bool rate = !perSecond.empty();
		std::string counts;
		for( int n : perSecond )
		{
			rate = rate && n == static_cast< int >( clock.projector );
			counts += ( counts.empty() ? "" : ", " ) + std::to_string( n );
		}
		failures += report( rate, quiet, "%s: distinct pictures captured in each whole second, read out of the picture: %s (want %g)", label, counts.c_str(), clock.projector );
		note( quiet, "no exposure ends within %.3f frames of a pull-down (the clock's lead is %.4f s)", worstMargin, s.lead );
	}

	//Part 2: the pull-down happens in the dark. At 2400 display frames a
	//second an exposure is a hundredth of a projector frame: every one that
	//contains a pull-down must be black to the last bit, and one in the
	//middle of an opening must show the picture at 1 / open.
	{
		Session s;
		Knobs k;
		apply( s.plugin, k );
		s.plugin.SetPerturbForTest( perturb );
		s.plugin.SetProbeForTest( model::kProbeLinear );
		s.fps  = 2400.0;
		s.lead = 0.0;
		if( !s.begin( W, H ) )
			return 1;
		Clock clock;
		clock.fps       = s.fps;
		clock.projector = 24.0;
		//White: the one level the RGBA16F held picture stores exactly (and
		//its density is exactly 0).
		s.upload( flat( W, H, 1.0 ) );
		const double f = controls::ShutterOpen( controls::ShutterParam( 180.0 ) );
		int dark = 0, darkOk = 0, lit = 0, litOk = 0;
		double lamp[ 3 ];
		model::LampRgb( controls::LampKelvin( 1 ), lamp );
		const double T = 1.0;
		for( long m = 0; m < 400; ++m )
		{
			if( !s.renderAt( m ) )
				return 1;
			if( m == 0 )
				continue;
			const double p0 = clock.p0( m ), p1 = clock.p1( m );
			float px[ 4 ];
			s.readPixel( W / 2, H / 2, px );
			if( std::floor( p0 ) != std::floor( p1 ) || std::fabs( p1 - std::round( p1 ) ) < 1e-9 )
			{
				++dark;
				if( px[ 0 ] == 0.0f && px[ 1 ] == 0.0f && px[ 2 ] == 0.0f )
					++darkOk;
			}
			//Wholly inside an opening, away from its edges by a whole exposure.
			const double c0 = p0 * 2.0 - std::floor( p0 * 2.0 ), c1 = p1 * 2.0 - std::floor( p1 * 2.0 );
			if( std::floor( p0 * 2.0 ) == std::floor( p1 * 2.0 ) && c0 > 0.25 + 0.03 && c1 < 0.75 - 0.03 )
			{
				++lit;
				//White over 1 / open, through the lamp: exp2( -0 ) is 1 exactly;
				//the weight (2, exact), the product and the lamp's float
				//rounding: 4 ULP, allowed 32.
				const double want = T / f * lamp[ 1 ];
				if( std::fabs( px[ 1 ] / want - 1.0 ) < 32.0 * 2.0 * kU )
					++litOk;
			}
		}
		s.end();
		failures += report( dark > 0 && darkOk == dark, quiet, "every one of %d exposures that contains a pull-down is exactly black (%d are): the claw moves in the dark", dark, darkOk );
		failures += report( lit > 0 && litOk == lit, quiet, "every one of %d exposures inside an opening shows the picture at 1 / open = %.1fx (%d do)", lit, 1.0 / f, litOk );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --scratch
//---------------------------------------------------------------------------
/// The moving picture a scratch has to hold still over: vertical bars an
/// eighth of the width, 0.35 and 0.6 alternately, sliding right 3.7 px a
/// frame (at 1280 wide; scaled), under a weave of Weave 1.
Picture bars( int W, int H, long m )
{
	Picture p( static_cast< size_t >( W ) * H * 4 );
	const double shift = 3.7 * W / 1280.0 * static_cast< double >( m );
	const double bar   = W / 8.0;
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
		{
			const long n = static_cast< long >( std::floor( ( x + 0.5 - shift ) / bar ) );
			put( p, W, x, y, ( n & 1 ) ? 0.6 : 0.35 );
		}
	return p;
}

int runScratch( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "scratch: the gate's scratches over a picture sliding under a weave, against the same run without them, %dx%d\n", W, H );
	int failures = 0;
	const int frames = 48, skip = 2;

	//Per-sample ratio error: at most 7 float roundings in the scratched sum
	//and 6 in the clean one, two exp2s of 3 ULP each, the division: 26
	//ULP, rounded up to 32 (2 x 2^-24 each).
	const double ratioTol = 32.0 * 2.0 * kU;
	const int row         = H / 2;

	//Two sessions in lockstep, one with the scratches and one without; the
	//print's hash streams are independent, so everything else is identical.
	auto lockstep = [ & ]( const std::vector< model::Scratch >& override, double amount,
	                       std::vector< std::vector< float > >& scratched, std::vector< std::vector< float > >& clean,
	                       std::vector< model::Scratch >& list ) -> bool {
		Session a, b;
		Knobs k;
		k.weave  = 1.0;
		k.shrink = 0.5;
		k.scratches = amount;
		apply( a.plugin, k );
		k.scratches = 0.0;
		apply( b.plugin, k );
		a.plugin.SetScratchOverrideForTest( override );
		for( Session* s : { &a, &b } )
		{
			s->plugin.SetPerturbForTest( perturb );
			s->plugin.SetProbeForTest( model::kProbeLinear );
			s->lead = 0.0071;
			if( !s->begin( W, H ) )
				return false;
		}
		scratched.clear();
		clean.clear();
		for( long m = 0; m < frames; ++m )
		{
			const Picture p = bars( W, H, m );
			if( !a.render( m, p ) )
				return false;
			if( m == skip )
				list = a.plugin.LastScratchesForTest();
			if( m >= skip )
				scratched.push_back( a.readRow( row ) );
			if( !b.render( m, p ) )
				return false;
			if( m >= skip )
				clean.push_back( b.readRow( row ) );
		}
		a.end();
		b.end();
		return true;
	};

	//Part 1: two scratches set by hand. A base-side one on whole pixels
	//( 5/16 to 5/16 + 1/64 of the width: whole at any width that is a
	//multiple of 64 ), and an emulsion-side one cut through all three layers
	//at a fractional x.
	{
		model::Scratch base, emulsion;
		base.left      = 5.0 / 16.0;
		base.right     = 5.0 / 16.0 + 1.0 / 64.0;
		base.strength  = 0.5;
		base.side      = model::kBase;
		//2.3 px wide at 320 (9.2 at 1280): at least one column wholly under
		//it, whatever its fractional x, so its edges can be read back.
		emulsion.left  = 0.6180339 - 1.15 / 320.0;
		emulsion.right = 0.6180339 + 1.15 / 320.0;
		emulsion.strength = 1.0;
		emulsion.side     = model::kEmulsion;
		std::vector< std::vector< float > > A, B;
		std::vector< model::Scratch > list;
		if( !lockstep( { base, emulsion }, 0.0, A, B, list ) )
			return 1;

		const bool whole = W % 64 == 0;
		const int L = static_cast< int >( std::lround( base.left * W ) ), R = static_cast< int >( std::lround( base.right * W ) );
		int wrongColumns = 0, darkWrong = 0;
		for( size_t f = 0; f < A.size(); ++f )
			for( int x = 0; x < W; ++x )
			{
				const bool inside = x >= L && x < R;
				bool allDarker    = true;
				for( int ch = 0; ch < 3; ++ch )
				{
					const double q    = static_cast< double >( A[ f ][ 4 * x + ch ] ) / B[ f ][ 4 * x + ch ];
					const double want = inside ? 1.0 - base.strength : 1.0;
					if( x >= L - 2 && x < R + 2 && std::fabs( q - want ) > ratioTol )
						++wrongColumns;
					if( inside && !( q < 1.0 ) )
						allDarker = false;
				}
				if( inside && !allDarker )
					++darkWrong;
			}
		if( whole )
			failures += report( wrongColumns == 0, quiet, "base side on whole pixels: exactly columns %d-%d lose %.0f%% of their light, their neighbours none, on all %zu frames (%d samples off by > %.1e)",
			                    L, R - 1, 100.0 * base.strength, A.size(), wrongColumns, ratioTol );
		else
			note( quiet, "width %d is not a multiple of 64: the base scratch is not on whole pixels, and only its centroid is checked", W );
		failures += report( darkWrong == 0, quiet, "base side prints dark: every channel darker at every covered column of every frame" );

		//The emulsion scratch, cut through all three layers: at a column it
		//covers by c, the density there is D ( 1 - c ), so log2 of the ratio
		//is c D. A column wholly under it gives D; its two edge columns' c
		//then place its edges, l = first + 1 - c_first and r = last + c_last,
		//and its centre is ( l + r ) / 2 -- read out of the picture, over a
		//stretch of the moving picture that is flat under it.
		const double centre = 0.5 * ( emulsion.left + emulsion.right ) * W;
		const int lo = static_cast< int >( std::floor( emulsion.left * W ) ), hi = static_cast< int >( std::ceil( emulsion.right * W ) ) - 1;
		int measured = 0, skipped = 0, lightWrong = 0, misses = 0;
		double worst = 0.0, bound = 0.0, first = -1.0, drift = 0.0;
		for( size_t f = 0; f < A.size(); ++f )
		{
			bool flatHere = true;
			for( int x = lo - 2; x <= hi + 2; ++x )
				flatHere = flatHere && std::fabs( B[ f ][ 4 * x + 1 ] / B[ f ][ 4 * ( lo - 2 ) + 1 ] - 1.0 ) < 1e-5;
			for( int x = lo - 2; x <= hi + 2; ++x )
				for( int ch = 0; ch < 3; ++ch )
					if( static_cast< double >( A[ f ][ 4 * x + ch ] ) / B[ f ][ 4 * x + ch ] < 1.0 - ratioTol )
						++lightWrong;
			if( !flatHere )
			{
				++skipped;
				continue;
			}
			auto cD = [ & ]( int x ) { return std::log2( static_cast< double >( A[ f ][ 4 * x + 1 ] ) / B[ f ][ 4 * x + 1 ] ); };
			const double D = cD( ( lo + hi ) / 2 );
			//The middle column must be wholly under the scratch, so it carries
			//the picture's whole density (> 0.5 in log2 on these bars). If it
			//does not, the scratch is not where it was put: a miss, not a bound
			//of infinity.
			if( !( D > 0.5 ) )
			{
				++misses;
				continue;
			}
			const double l = lo + 1 - cD( lo ) / D, r = hi + cD( hi ) / D;
			const double c = 0.5 * ( l + r );
			//Each log2 q is good to ratioTol / ln 2; c = cD / D carries that
			//from both, and the centre averages two edges. Plus an ULP of the
			//edges' own x W.
			const double e = ratioTol / std::log( 2.0 );
			bound          = std::max( bound, 2.0 * e / D + 2.0 * W * kU );
			worst          = std::max( worst, std::fabs( c - centre ) );
			if( first < 0.0 )
				first = c;
			drift = std::max( drift, std::fabs( c - first ) );
			++measured;
		}
		failures += report( measured > 0 && misses == 0 && worst <= bound, quiet,
		                    "emulsion side at a fractional x (%.4f px): its edges, read back, centre on it to %.1e px on %d frames (bound %.1e); %d frames skipped where a bar edge crossed it, %d with no column wholly under it",
		                    centre, worst, measured, bound, skipped, misses );
		failures += report( measured > 0 && drift <= 2.0 * bound, quiet, "emulsion side stays put: its centre moves %.1e px across the run (bound %.1e)", drift, 2.0 * bound );
		failures += report( lightWrong == 0, quiet, "emulsion side prints light: no channel darker anywhere under it (%d samples)", lightWrong );
	}

	//Part 2: the print's own scratches (Scratches 1), each at its own x,
	//side and strength, most of them narrower than a pixel at 320x180. A
	//sub-pixel line's x within its pixel cannot be read from coverage, but
	//"stays put" can be, exactly: its profile across the columns must be the
	//same on every frame, and confined to the columns its edges touch.
	{
		std::vector< std::vector< float > > A, B;
		std::vector< model::Scratch > list;
		if( !lockstep( {}, 1.0, A, B, list ) )
			return 1;
		int checked = 0, moved = 0, strayed = 0, polarity = 0, crowded = 0, bases = 0, emulsions = 0;
		double worstMove = 0.0;
		for( size_t i = 0; i < list.size(); ++i )
		{
			const model::Scratch& sc = list[ i ];
			//Alive for the whole run, and nothing else within 3 px.
			if( sc.born > 0 || sc.dies < 40 )
				continue;
			bool isolated = true;
			for( size_t j = 0; j < list.size(); ++j )
				if( j != i && list[ j ].right * W > sc.left * W - 3.0 && list[ j ].left * W < sc.right * W + 3.0 )
					isolated = false;
			if( !isolated )
			{
				++crowded;
				continue;
			}
			const int lo = static_cast< int >( std::floor( sc.left * W ) ), hi = static_cast< int >( std::ceil( sc.right * W ) ) - 1;
			if( lo < 2 || hi >= W - 2 )
				continue;
			//The profile: 1 - q on the base side (the light lost, s x cover),
			//log2 q on the emulsion side (cover x the dye it took, which
			//depends on the picture: compared only where the picture is flat).
			std::vector< double > firstProfile;
			bool ok = true;
			for( size_t f = 0; f < A.size(); ++f )
			{
				bool flatHere = true;
				for( int x = lo - 2; x <= hi + 2; ++x )
					flatHere = flatHere && std::fabs( B[ f ][ 4 * x + 1 ] / B[ f ][ 4 * ( lo - 2 ) + 1 ] - 1.0 ) < 1e-5;
				std::vector< double > profile;
				for( int x = lo - 2; x <= hi + 2; ++x )
				{
					const double q = static_cast< double >( A[ f ][ 4 * x + 1 ] ) / B[ f ][ 4 * x + 1 ];
					profile.push_back( sc.side == model::kBase ? 1.0 - q : std::log2( q ) );
					const bool touched = x >= lo && x <= hi;
					if( !touched && std::fabs( q - 1.0 ) > ratioTol )
						++strayed;
					for( int ch = 0; ch < 3; ++ch )
					{
						const double qc = static_cast< double >( A[ f ][ 4 * x + ch ] ) / B[ f ][ 4 * x + ch ];
						if( sc.side == model::kBase ? qc > 1.0 + ratioTol : qc < 1.0 - ratioTol )
							++polarity;
					}
				}
				if( sc.side == model::kEmulsion && !flatHere )
					continue;
				if( firstProfile.empty() )
				{
					firstProfile = profile;
					continue;
				}
				//The emulsion profile scales with the picture's density: compare
				//shapes, normalised by their sums.
				double sa = 0.0, sb = 0.0;
				for( size_t x = 0; x < profile.size(); ++x )
				{
					sa += profile[ x ];
					sb += firstProfile[ x ];
				}
				for( size_t x = 0; x < profile.size(); ++x )
				{
					const double d = sc.side == model::kBase ? std::fabs( profile[ x ] - firstProfile[ x ] ) / ratioTol
					                                          : std::fabs( profile[ x ] / sa - firstProfile[ x ] / sb ) / ( 4.0 * ratioTol / std::log( 2.0 ) / std::fabs( sa ) );
					worstMove = std::max( worstMove, d );
					if( d > 1.0 )
						ok = false;
				}
			}
			++checked;
			( sc.side == model::kBase ? bases : emulsions )++;
			if( !ok )
				++moved;
		}
		failures += report( checked >= 2 && moved == 0, quiet,
		                    "the print's own scratches: %d isolated ones alive all run (%d base, %d emulsion) keep the same profile across the columns on every frame, worst %.2f of the tolerance (%d moved; %d skipped as crowded)",
		                    checked, bases, emulsions, worstMove, moved, crowded );
		failures += report( strayed == 0, quiet, "the print's own scratches: nothing outside the columns their stated edges touch (%d samples)", strayed );
		failures += report( polarity == 0, quiet, "the print's own scratches: base side dark and emulsion side light in every channel (%d samples the wrong way)", polarity );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --weave
//
// The offsets are measured out of the picture: a step between two flat
// levels, bilinear-shifted, has a row sum of exactly L e + R ( W - e ), where
// e is the step's position -- a tent filter conserves the step's area. So the
// offset of every projector frame is read from one display frame that lies
// wholly inside it, and the stated law is tested statistically: the sample
// standard deviation and lag-1 autocorrelation against sigma and rho, at 5
// standard errors of their own sampling distributions for an AR(1) series of
// that length (Bartlett).
//---------------------------------------------------------------------------
Picture steps( int W, int H )
{
	Picture p( static_cast< size_t >( W ) * H * 4 );
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
		{
			//Top half: a vertical edge at W / 2 (horizontal weave). Column W / 4
			//is dark down to 3H / 4 and light below (vertical weave).
			double v = x < W / 2 ? 0.3 : 0.7;
			if( y >= H / 2 )
				v = y < 3 * H / 4 ? 0.3 : 0.7;
			put( p, W, x, y, v );
		}
	return p;
}

int runWeave( int W, int H, int perturb, bool quiet = false )
{
	const int wanted = 1000;
	if( !quiet )
		std::printf( "weave: %d projector frames' offsets read out of the picture, against the stated AR(1), %dx%d\n", wanted, W, H );
	int failures = 0;
	for( double shrink : { 0.0, 1.0 } )
	{
		Session s;
		Knobs k;
		k.weave  = 0.5;
		k.shrink = shrink;
		apply( s.plugin, k );
		s.plugin.SetPerturbForTest( perturb );
		s.plugin.SetProbeForTest( model::kProbeLinear );
		//48 display frames a second against 24: every exposure is half a
		//projector frame, and with the lead's phase (0.17 of a frame) every
		//other one lies wholly inside a frame, 0.17 clear of each pull-down.
		s.fps  = 48.0;
		s.lead = 0.0071;
		if( !s.begin( W, H ) )
			return 1;
		s.upload( steps( W, H ) );
		Clock clock;
		clock.fps  = s.fps;
		clock.lead = s.lead;
		std::map< int64_t, std::pair< double, double > > offsets;
		for( long m = 0; static_cast< int >( offsets.size() ) < wanted && m < 4 * wanted; ++m )
		{
			if( !s.renderAt( m ) )
				return 1;
			if( clock.first( m ) != clock.latest( m ) || clock.margin( m ) < 0.01 || offsets.count( clock.latest( m ) ) )
				continue;
			const std::vector< float > rowPx = s.readRow( H / 4 );
			const std::vector< float > colPx = s.readColumn( W / 4 );
			double S = 0.0, C = 0.0;
			for( int x = 0; x < W; ++x )
				S += rowPx[ 4 * x + 1 ];
			for( int y = 0; y < H; ++y )
				C += colPx[ 4 * y + 1 ];
			const double L = rowPx[ 4 * ( W / 16 ) + 1 ], R = rowPx[ 4 * ( W - 1 - W / 16 ) + 1 ];
			const double Tt = colPx[ 4 * ( H / 16 ) + 1 ], Bt = colPx[ 4 * ( H - 1 - H / 16 ) + 1 ];
			const double dx = ( R * W - S ) / ( R - L ) - W / 2;
			const double dy = ( Bt * H - C ) / ( Bt - Tt ) - 3 * H / 4;
			offsets[ clock.latest( m ) ] = { dx, dy };
		}
		s.end();

		const model::WeaveLaw law = model::Weave( 0.5, shrink, 0 );
		auto stats                = [ & ]( int axis, double& sd, double& r1 ) {
			std::vector< double > v;
			int64_t expect = offsets.begin()->first;
			for( const auto& e : offsets )
			{
				if( e.first != expect )
					break;//a gap: lag-1 needs consecutive frames
				v.push_back( axis == 0 ? e.second.first : e.second.second );
				++expect;
			}
			double mean = 0.0;
			for( double x : v )
				mean += x;
			mean /= v.size();
			double c0 = 0.0, c1 = 0.0;
			for( size_t i = 0; i < v.size(); ++i )
			{
				c0 += ( v[ i ] - mean ) * ( v[ i ] - mean );
				if( i + 1 < v.size() )
					c1 += ( v[ i ] - mean ) * ( v[ i + 1 ] - mean );
			}
			sd = std::sqrt( c0 / ( v.size() - 1 ) );
			r1 = c1 / c0;
			return static_cast< int >( v.size() );
		};
		for( int axis = 0; axis < 2; ++axis )
		{
			double sd = 0.0, r1 = 0.0;
			const int n        = stats( axis, sd, r1 );
			const double sigma = ( axis == 0 ? law.sigmaX : law.sigmaY ) * H;
			const double rho   = law.rho;
			//Bartlett: var( s^2 ) ~ 2 sigma^4 / n x ( 1 + rho^2 ) / ( 1 - rho^2 ),
			//so s is off by sigma x sqrt( ( 1 + rho^2 ) / ( 2 n ( 1 - rho^2 ) ) ) at
			//one standard error; var( r1 ) ~ ( 1 - rho^2 ) / n. Five of each. The
			//measurement itself is good to ~1e-3 px (float sums over a row).
			const double sdTol = 5.0 * sigma * std::sqrt( ( 1.0 + rho * rho ) / ( 2.0 * n * ( 1.0 - rho * rho ) ) ) + 1e-3;
			const double rTol  = 5.0 * std::sqrt( ( 1.0 - rho * rho ) / n );
			char label[ 64 ];
			std::snprintf( label, sizeof( label ), "%s, shrinkage %.0f%%", axis == 0 ? "horizontal" : "vertical", shrink * model::kShrinkMaxPercent );
			failures += report( n >= wanted * 9 / 10, quiet, "%s: %d consecutive projector frames measured", label, n );
			failures += report( std::fabs( sd - sigma ) <= sdTol, quiet, "%s: sd %.3f px against sigma %.3f px (+- %.3f)", label, sd, sigma, sdTol );
			failures += report( std::fabs( r1 - rho ) <= rTol, quiet, "%s: lag-1 correlation %.3f against rho %.3f (+- %.3f)", label, r1, rho, rTol );
		}
	}
	return failures;
}

//---------------------------------------------------------------------------
// --fade
//---------------------------------------------------------------------------
int runFade( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "fade: a neutral grey through Age, each dye's density read back against the stated law, %dx%d\n", W, H );
	//The left half is the grey, the right half white. The held picture is
	//RGBA16F, so the grey's stored density is NOT the double decode's: it is
	//quantised to 2^-11. White stores 1.0 exactly (density 0), so the two
	//halves' ratio at Age 0 IS the stored transmittance, read out of the
	//picture, whatever the shutter's gain and the lamp.
	const double grey = 0.46;
	auto centre       = [ & ]( double age, double out[ 3 ], double white[ 3 ] ) -> bool {
		Session s;
		Knobs k;
		k.age = age;
		apply( s.plugin, k );
		s.plugin.SetPerturbForTest( perturb );
		s.plugin.SetProbeForTest( model::kProbeLinear );
		if( !s.begin( W, H ) )
			return false;
		Picture p( static_cast< size_t >( W ) * H * 4 );
		for( int yy = 0; yy < H; ++yy )
			for( int xx = 0; xx < W; ++xx )
				put( p, W, xx, yy, xx < W / 2 ? grey : 1.0 );
		s.upload( p );
		for( long m = 0; m < 3; ++m )
			if( !s.renderAt( m ) )
				return false;
		float px[ 4 ], wh[ 4 ];
		s.readPixel( W / 4, H / 2, px );
		s.readPixel( 3 * W / 4, H / 2, wh );
		for( int i = 0; i < 3; ++i )
		{
			out[ i ]   = px[ i ];
			white[ i ] = wh[ i ];
		}
		s.end();
		return true;
	};
	double fresh[ 3 ], freshWhite[ 3 ];
	if( !centre( 0.0, fresh, freshWhite ) )
		return 1;
	//log2 units, as the shader: the stored picture's density, per channel.
	double Ds[ 3 ];
	for( int i = 0; i < 3; ++i )
		Ds[ i ] = -std::log2( fresh[ i ] / freshWhite[ i ] );
	int failures   = 0;
	double lastR[ 3 ] = { 1.0, 1.0, 1.0 };
	bool monotone     = true;
	const char* dye[ 3 ] = { "cyan", "magenta", "yellow" };
	for( double age : { 0.25, 0.5, 1.0 } )
	{
		double aged[ 3 ], agedWhite[ 3 ];
		if( !centre( age, aged, agedWhite ) )
			return 1;
		double want[ 3 ];
		model::Retention( age, 0, want );
		double r[ 3 ], q[ 3 ];
		std::string line;
		bool onLaw = true;
		double worst = 0.0, tol = 0.0;
		for( int i = 0; i < 3; ++i )
		{
			//T = 2^( -r D ) against 2^( -D ): r = 1 - log2( q ) / D, with D the
			//stored density measured above.
			const double D = Ds[ i ];
			q[ i ]         = aged[ i ] / fresh[ i ];
			r[ i ]         = 1.0 - std::log2( q[ i ] ) / D;
			//q and D are each a ratio of two float samples of the same path:
			//an exp2 (3 ULP), a log2 (3 ULP), the weight's and the lamp's
			//products (2), the density scale (1): 9 ULP a sample, 18 a ratio,
			//rounded up to 24 ULP (of 2^-23). r inherits q's error over D ln 2,
			//and D's scaled by |r - 1| / D.
			const double e    = 24.0 * 2.0 * kU;
			const double tolI = e / ( std::log( 2.0 ) * D ) + std::fabs( r[ i ] - 1.0 ) * ( e / std::log( 2.0 ) ) / D;
			tol               = std::max( tol, tolI );
			worst             = std::max( worst, std::fabs( r[ i ] - want[ i ] ) );
			onLaw             = onLaw && std::fabs( r[ i ] - want[ i ] ) <= tolI;
			monotone          = monotone && r[ i ] < lastR[ i ];
			lastR[ i ]        = r[ i ];
			char buf[ 64 ];
			std::snprintf( buf, sizeof( buf ), "%s%s %.5f (law %.5f)", i ? ", " : "", dye[ i ], r[ i ], want[ i ] );
			line += buf;
		}
		failures += report( onLaw, quiet, "Age %.2f: each dye keeps the density the law says: %s; worst %.1e (tolerance %.1e)", age, line.c_str(), worst, tol );
		failures += report( q[ 0 ] > q[ 1 ] && q[ 2 ] > q[ 1 ], quiet, "Age %.2f: toward magenta: red x%.3f and blue x%.3f rise past green x%.3f", age, q[ 0 ], q[ 2 ], q[ 1 ] );
		failures += report( r[ 0 ] < r[ 2 ] && r[ 2 ] < r[ 1 ], quiet, "Age %.2f: cyan loses the most density, then yellow, then magenta", age );
	}
	failures += report( monotone, quiet, "each dye's density falls with Age" );
	return failures;
}

//---------------------------------------------------------------------------
// --resize
//---------------------------------------------------------------------------
int runResize( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "resize: the print and the held pictures across a resize to 1.5x and back, %dx%d\n", W, H );
	int failures = 0;

	//Part 1: the print. Scratches, dust, a hair and weave, on a flat grey.
	//Run A is left alone; run B goes to 1.5x at frame 20 and back at 28.
	//From frame 40 every pixel of B must be A's.
	{
		auto run = [ & ]( bool resize, std::vector< std::vector< float > >& out, int& scratchesSeen ) -> bool {
			Session s;
			Knobs k;
			k.scratches = 1.0;
			k.dust      = 1.0;
			k.hair      = 1.0;
			k.weave     = 0.5;
			apply( s.plugin, k );
			s.plugin.SetPerturbForTest( perturb );
			s.plugin.SetProbeForTest( model::kProbeLinear );
			s.lead = 0.0071;
			if( !s.begin( W, H ) )
				return false;
			out.clear();
			for( long m = 0; m < 60; ++m )
			{
				if( resize && m == 20 )
					s.resize( W * 3 / 2, H * 3 / 2 );
				if( resize && m == 28 )
					s.resize( W, H );
				if( !s.render( m, flat( s.width, s.height, 0.5 ) ) )
					return false;
				if( m >= 40 )
					out.push_back( s.readAll() );
			}
			scratchesSeen = static_cast< int >( s.plugin.LastScratchesForTest().size() );
			s.end();
			return true;
		};
		std::vector< std::vector< float > > a, b;
		int na = 0, nb = 0;
		if( !run( false, a, na ) || !run( true, b, nb ) )
			return 1;
		//Identical float operations on identical numbers: equal on a
		//deterministic GPU. The software renderer is not bit-repeatable; four
		//ULP of the value is its allowance.
		size_t differ = 0;
		double marked = 0.0;
		for( size_t f = 0; f < a.size(); ++f )
			for( size_t i = 0; i < a[ f ].size(); i += 4 )
			{
				if( std::fabs( a[ f ][ i + 1 ] - b[ f ][ i + 1 ] ) > 4.0 * 2.0 * kU * std::fabs( a[ f ][ i + 1 ] ) + 1e-30 )
					++differ;
				marked += std::fabs( a[ f ][ i + 1 ] - a[ f ][ 1 ] ) > 1e-3 ? 1.0 : 0.0;
			}
		failures += report( differ == 0, quiet, "frames 40-59 after a resize to %dx%d and back: every pixel is the unresized run's (%zu differ)", W * 3 / 2, H * 3 / 2, differ );
		failures += report( na > 0 && marked > 0.0, quiet, "the print was marked when it was resized: %d scratches, %.0f marked pixels over the 20 frames compared", na, marked );
	}

	//Part 2: the previous frame. A flat grey, the print off; the resize
	//lands on a display frame whose exposure straddles a pull-down, so half
	//its light comes from the frame held BEFORE the resize.
	{
		Clock clock;
		clock.lead = 0.0071;
		//Both sides of the pull-down must pass light: the shutter is dark for
		//( 1 - open ) / ( 2 blades ) of a frame either side of it (0.125 here),
		//so each side of the exposure must be longer than that.
		const double dark = ( 1.0 - controls::ShutterOpen( controls::ShutterParam( 180.0 ) ) ) / ( 2.0 * 2.0 );
		long at           = 20;
		while( clock.first( at ) == clock.latest( at ) || clock.p1( at ) - clock.latest( at ) < dark + 0.02
		       || static_cast< double >( clock.latest( at ) ) - clock.p0( at ) < dark + 0.02 )
			++at;
		auto run = [ & ]( bool resize, double& value ) -> bool {
			Session s;
			Knobs k;
			apply( s.plugin, k );
			s.plugin.SetPerturbForTest( perturb );
			s.plugin.SetProbeForTest( model::kProbeLinear );
			s.lead = clock.lead;
			if( !s.begin( W, H ) )
				return false;
			for( long m = 0; m <= at; ++m )
			{
				if( resize && m == at )
					s.resize( W * 3 / 2, H * 3 / 2 );
				if( !s.render( m, flat( s.width, s.height, 0.5 ) ) )
					return false;
			}
			float px[ 4 ];
			s.readPixel( s.width / 2, s.height / 2, px );
			value = px[ 1 ];
			s.end();
			return true;
		};
		double a = 0.0, b = 0.0;
		if( !run( false, a ) || !run( true, b ) )
			return 1;
		//The bilinear resample of a flat picture is the picture to an ULP or
		//two ( mix( v, v, t ) ); four is the allowance.
		failures += report( std::fabs( b - a ) <= 4.0 * 2.0 * kU * a, quiet,
		                    "a resize on frame %ld, whose exposure spans frames %lld-%lld: the older frame is still lit (%.6f against %.6f unresized)",
		                    at, static_cast< long long >( clock.first( at ) ), static_cast< long long >( clock.latest( at ) ), b, a );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --negative
//---------------------------------------------------------------------------
int runNegative( int W, int H )
{
	std::printf( "negative controls: each perturbation of the plugin's model must FAIL its check, %dx%d\n", W, H );
	struct Control
	{
		int bits;
		const char* what;
		int ( *check )( int, int, int, bool );
		const char* name;
	};
	const Control list[] = {
		{ model::kPerturbInstant, "the shutter sampled at one instant", runFlicker, "--flicker" },
		{ model::kPerturbNoHold, "the clip captured every display frame", runHold, "--hold" },
		{ model::kPerturbPictureSpace, "scratches in picture space", runScratch, "--scratch" },
		{ model::kPerturbWhiteWeave, "weave with no correlation", runWeave, "--weave" },
		{ model::kPerturbShrinkIgnored, "shrinkage left out of sigma", runWeave, "--weave" },
		{ model::kPerturbEqualFade, "every dye fading alike", runFade, "--fade" },
		{ model::kPerturbResizeReseeds, "a resize makes a new print", runResize, "--resize" },
		{ model::kPerturbResizeClears, "a resize clears the held pictures", runResize, "--resize" },
	};
	int failures = 0;
	for( const Control& c : list )
	{
		const int before = g_failures;
		const int checks = g_checks;
		const int failed = c.check( W, H, c.bits, true );
		g_failures       = before;
		g_checks         = checks;
		failures += report( failed > 0, false, "%-38s -> %s fails (%d of its checks)", c.what, c.name, failed );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --names
//---------------------------------------------------------------------------
int runNames()
{
	std::printf( "names: nothing the host will silently truncate; every name unique\n" );
	Gate plugin;
	int failures = 0;
	std::set< std::string > seen;
	for( const NamedParameter& p : listParameters( plugin ) )
	{
		if( p.index >= Gate::PT_ABOUT_FIRST )
			continue;
		failures += report( p.name.size() <= 16, false, "%-16s %2zu characters", p.name.c_str(), p.name.size() );
		failures += report( seen.insert( p.name ).second, false, "%-16s unique", p.name.c_str() );
	}
	failures += report( std::string( "SW Gate" ).size() <= 16, false, "display name 'SW Gate' is %zu characters", std::string( "SW Gate" ).size() );
	return failures;
}

//---------------------------------------------------------------------------
// The moving card, for --out, the sweep and the bench: a row of colour
// patches, a grey ramp, a white disc on an orbit, a static black square, a
// drifting blue bar, and a flashing patch (on for 6 frames in 30).
//---------------------------------------------------------------------------
std::vector< unsigned char > buildCard( int width, int height, int64_t frame )
{
	std::vector< unsigned char > img( static_cast< size_t >( width ) * height * 4 );
	const double t  = static_cast< double >( frame ) / 60.0;
	const double cx = 0.72 + 0.14 * std::cos( 1.2 * t ), cy = 0.68 + 0.16 * std::sin( 1.2 * t );
	const double barX = std::fmod( 0.05 * t, 1.0 );
	const bool flash  = frame % 30 < 6;
	const double patches[ 8 ][ 3 ] = {
		{ 0.80, 0.10, 0.10 }, { 0.88, 0.67, 0.55 }, { 0.90, 0.85, 0.15 }, { 0.15, 0.60, 0.20 },
		{ 0.20, 0.80, 0.85 }, { 0.15, 0.25, 0.85 }, { 0.53, 0.81, 0.92 }, { 0.95, 0.95, 0.95 },
	};
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double fx = ( x + 0.5 ) / width, fy = ( y + 0.5 ) / height;
			double r = 0.40, g = 0.40, b = 0.40;
			if( fy > 0.06 && fy < 0.30 )
			{
				const int i = std::clamp( static_cast< int >( ( fx - 0.04 ) / 0.115 ), 0, 7 );
				if( fx > 0.04 && fx < 0.96 && std::fmod( fx - 0.04, 0.115 ) < 0.105 )
				{
					r = patches[ i ][ 0 ];
					g = patches[ i ][ 1 ];
					b = patches[ i ][ 2 ];
				}
			}
			if( fy > 0.36 && fy < 0.46 && fx > 0.04 && fx < 0.96 )
				r = g = b = ( fx - 0.04 ) / 0.92;
			if( fx > 0.08 && fx < 0.28 && fy > 0.56 && fy < 0.92 )
				r = g = b = flash ? 0.98 : 0.02;
			const double dx = ( fx - cx ) * width, dy = ( fy - cy ) * height;
			if( dx * dx + dy * dy < ( height * 0.08 ) * ( height * 0.08 ) )
				r = g = b = 0.98;
			if( std::fabs( fx - barX ) < 0.02 && fy > 0.5 )
			{
				r = 0.2;
				g = 0.3;
				b = 0.9;
			}
			unsigned char* px = img.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			px[ 0 ]           = static_cast< unsigned char >( std::lround( 255.0 * r ) );
			px[ 1 ]           = static_cast< unsigned char >( std::lround( 255.0 * g ) );
			px[ 2 ]           = static_cast< unsigned char >( std::lround( 255.0 * b ) );
			px[ 3 ]           = 255;
		}
	return img;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( Gate& plugin, int width, int height, int frames, double fps, size_t& stateBytes )
{
	Session session;
	session.floatOutput = false;
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.index < Gate::PT_ABOUT_FIRST && p.type != FF_TYPE_BUFFER && p.type != FF_TYPE_EVENT && p.type != FF_TYPE_TEXT )
			session.plugin.SetFloatParameter( p.index, p.value );
	session.fps = fps;
	if( !session.begin( width, height ) )
		return -1.0;

	//The card is uploaded once: a host's frame is already on the GPU, and
	//the plugin's cost does not depend on what the frame holds.
	const std::vector< unsigned char > card = buildCard( width, height, 0 );
	const int warmup                        = 20;
	for( int frame = 0; frame < warmup; ++frame )
		session.render( frame, card );
	glFinish();

	double best = 1e9;
	for( int run = 0; run < 3; ++run )
	{
		const auto start = std::chrono::steady_clock::now();
		for( int frame = 0; frame < frames; ++frame )
			session.renderAt( warmup + run * frames + frame );
		glFinish();
		const auto end       = std::chrono::steady_clock::now();
		const double seconds = std::chrono::duration< double >( end - start ).count();
		best                 = std::min( best, seconds * 1000.0 / static_cast< double >( frames ) );
	}
	stateBytes = session.plugin.StateBytesForTest();
	session.end();
	return best;
}

int runBench( Gate& plugin, int frames, double fps )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = {
		{ "1280x720  ", 1280, 720 },
		{ "1920x1080 ", 1920, 1080 },
		{ "3840x2160 ", 3840, 2160 },
	};
	std::printf( "%d frames each, best of three runs, after a 20-frame warm-up, glFinish both sides, the card uploaded once.\n\n", frames );
	std::printf( "resolution     ms/frame   equivalent fps   %% of a 60fps frame   state held\n" );
	for( const Size& size : sizes )
	{
		size_t bytes    = 0;
		const double ms = benchAt( plugin, size.width, size.height, frames, fps, bytes );
		std::printf( "%s    %7.3f       %8.0f            %5.1f%%          %6.2f MB\n", size.name, ms, ms > 0.0 ? 1000.0 / ms : 0.0,
		             ms / 16.667 * 100.0, static_cast< double >( bytes ) / 1048576.0 );
	}
	std::printf( "\nState is the two held pictures (RGBA16F at the input raster) and the print's\n"
	             "one-row data texture. Whatever the settings above were, they are what was\n"
	             "measured; --set measures another.\n" );
	return 0;
}

//---------------------------------------------------------------------------
// --dump-shaders
//---------------------------------------------------------------------------
int dumpShaders( const std::string& dir )
{
	namespace shaders = gate::shaders;
	const std::pair< const char*, std::string > files[] = {
		{ "vertex.vert", shaders::Vertex() },
		{ "capture.frag", shaders::Capture() },
		{ "resample.frag", shaders::Resample() },
		{ "output.frag", shaders::Output() },
	};
	for( const auto& f : files )
	{
		std::ofstream out( dir + "/" + f.first );
		if( !out )
		{
			std::fprintf( stderr, "cannot write %s/%s\n", dir.c_str(), f.first );
			return 1;
		}
		out << f.second;
	}
	std::printf( "wrote %zu shaders to %s\n", sizeof( files ) / sizeof( files[ 0 ] ), dir.c_str() );
	return 0;
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Name Value' per line, the fleet's format.
//
// A STANDARD parameter ramps linearly between cues. An option, a boolean
// and an integer STEP: they hold the last cue at or before the frame,
// because there is nothing between Xenon and Tungsten to ramp through. An
// event fires on its cue frame only.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}
	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );
		int frame = 0;
		if( !( in >> frame ) )
			continue;
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame, unsigned int type )
{
	if( track.empty() )
		return 0.0f;
	if( type == FF_TYPE_EVENT )
	{
		for( const auto& cue : track )
			if( cue.first == frame )
				return cue.second;
		return 0.0f;
	}
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 1; i < track.size(); ++i )
		if( frame <= track[ i ].first )
		{
			const auto& a = track[ i - 1 ];
			const auto& b = track[ i ];
			if( type != FF_TYPE_STANDARD )
				return frame == b.first ? b.second : a.second;
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? static_cast< float >( frame - a.first ) / span : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	return track.back().second;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"gatest -- render and measure the Gate projector\n"
		"\n"
		"  --out PATH          render the moving card through the plugin (default /tmp/gate.png)\n"
		"  --average           write the mean of every frame rendered, not the last\n"
		"  --size WxH          raster (default 1280x720)\n"
		"  --frames N          frames to render before reading back (default 90)\n"
		"  --fps N             synthetic display rate driving the clock (default 60)\n"
		"  --source card|flat|white|black   what to feed (card moves); --level V for flat\n"
		"  --set \"Name=V\"      set a parameter by its display name (element index for options).\n"
		"                      Repeatable.\n"
		"  --list              every parameter, its kind, default and range\n"
		"\n"
		"  checks that render, at --size:\n"
		"  --flicker           a flat grey's spectrum is the shutter's Fourier series, the beat on top\n"
		"  --hold              the picture changes at pull-downs only, which happen in the dark\n"
		"  --scratch           scratches stay at their x while the picture moves; side sets polarity\n"
		"  --weave             the offsets have the stated sigma and lag-1 correlation\n"
		"  --fade              a neutral grey goes magenta, each dye on the stated law\n"
		"  --resize            the print and the held pictures survive a resize\n"
		"  --negative          every check above can fail\n"
		"  --perturb BITS      run the checks verbosely against a perturbed model (bits in Model.h)\n"
		"\n"
		"  checks that need no GL:\n"
		"  --names             nothing the host will silently truncate\n"
		"\n"
		"  --bench             time ProcessOpenGL at 720p, 1080p and 4K, and the state held\n"
		"  --dump-shaders DIR  write the exact GLSL the plugin compiles\n"
		"  --pipe              raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH       parameter cues for --pipe: 'frame Name Value'\n"
		"  --help\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/gate.png";
	std::string scriptPath;
	std::string dumpDir;
	std::string source = "card";
	double level   = 0.5;
	int width      = 1280;
	int height     = 720;
	int frames     = 90;
	int failRender = -1;
	int perturb    = 0;
	double fps     = 60.0;
	bool wantList  = false;
	bool wantBench = false;
	bool wantPipe  = false;
	bool average   = false;
	bool allowNoGL = false;
	std::vector< std::string > settings;
	std::vector< std::string > checks;

	const std::set< std::string > rendered = { "--flicker", "--hold", "--scratch", "--weave", "--fade", "--resize", "--negative" };
	const std::set< std::string > offline  = { "--names" };

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--dump-shaders" && hasNext )
			dumpDir = argv[ ++i ];
		else if( argument == "--source" && hasNext )
			source = argv[ ++i ];
		else if( argument == "--level" && hasNext )
			level = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--perturb" && hasNext )
			perturb = std::atoi( argv[ ++i ] );
		else if( argument == "--fail-render-at" && hasNext )
			failRender = std::atoi( argv[ ++i ] );//test hook: verify.sh proves --pipe exits 1 on a failed render
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--average" )
			average = true;
		else if( argument == "--allow-no-gl" )
			allowNoGL = true;
		else if( rendered.count( argument ) || offline.count( argument ) )
			checks.push_back( argument );
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "width, height, frames and fps must all be positive\n" );
		return 2;
	}

	if( !dumpDir.empty() )
		return dumpShaders( dumpDir );

	if( wantList )
	{
		//No GL needed: answered before a context is made, so it works in CI.
		Gate plugin;
		std::printf( "%3s  %-16s  %-9s  %-8s  %s\n", "id", "name", "kind", "default", "range" );
		for( const NamedParameter& p : listParameters( plugin ) )
			std::printf( "%3u  %-16s  %-9s  %.4f    [%g..%g]\n", p.index, p.name.c_str(), kindName( p ), p.value, p.low, p.high );
		return 0;
	}

	if( !checks.empty() )
	{
		bool needGL = false;
		for( const std::string& check : checks )
		{
			if( check == "--names" )
			{
				runNames();
				std::printf( "\n" );
			}
			else
				needGL = true;
		}

		if( needGL )
		{
			CGLContextObj context = createContext();
			if( context == nullptr && allowNoGL )
				std::printf( "   SKIP  could not create an OpenGL 4.1 core context, accelerated or software.\n"
				             "         The rendering checks and their negative controls were NOT run.\n" );
			else if( context == nullptr )
			{
				std::printf( "   FAIL  could not create an OpenGL 4.1 core context\n" );
				++g_failures;
			}
			else
			{
				for( const std::string& check : checks )
				{
					if( check == "--flicker" )
						runFlicker( width, height, perturb );
					else if( check == "--hold" )
						runHold( width, height, perturb );
					else if( check == "--scratch" )
						runScratch( width, height, perturb );
					else if( check == "--weave" )
						runWeave( width, height, perturb );
					else if( check == "--fade" )
						runFade( width, height, perturb );
					else if( check == "--resize" )
						runResize( width, height, perturb );
					else if( check == "--negative" )
						runNegative( width, height );
					else
						continue;
					std::printf( "\n" );
				}
				CGLSetCurrentContext( nullptr );
				CGLDestroyContext( context );
			}
		}
		std::printf( "%d checks, %d failed\n", g_checks, g_failures );
		return g_failures == 0 ? 0 : 1;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}
	auto finish = [ & ]( int result ) {
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	};

	Session session;
	session.floatOutput = false;
	session.fps         = fps;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( applySetting( session.plugin, setting, error ) )
			continue;
		std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
		return finish( 2 );
	}
	session.plugin.SetPerturbForTest( perturb );

	if( wantBench )
		return finish( runBench( session.plugin, frames < 40 ? 60 : frames, fps ) );

	if( wantPipe )
	{
		//Everything but the video goes to stderr: one stray byte in stdout is
		//a torn frame for the rest of the reel.
		struct Automation
		{
			unsigned int index;
			unsigned int type;
			Track track;
		};
		std::vector< Automation > automation;
		if( !scriptPath.empty() )
		{
			std::string error;
			const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				return finish( 2 );
			}
			for( const auto& entry : tracks )
			{
				const int index = indexOfParameter( session.plugin, entry.first );
				if( index < 0 )
				{
					std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
					return finish( 2 );
				}
				automation.push_back( { static_cast< unsigned int >( index ), session.plugin.GetParamType( static_cast< unsigned int >( index ) ), entry.second } );
			}
		}

		//A closed stdout must be a failed write we can see, not a SIGPIPE
		//that kills the process with 141 before it can say so.
		std::signal( SIGPIPE, SIG_IGN );

		if( !session.begin( width, height ) )
			return finish( 1 );

		std::vector< unsigned char > frame( static_cast< size_t >( width ) * height * 4 );
		int status = 0;
		for( int index = 0;; ++index )
		{
			size_t got = 0;
			while( got < frame.size() )
			{
				const ssize_t n = read( STDIN_FILENO, frame.data() + got, frame.size() - got );
				if( n <= 0 )
					break;
				got += static_cast< size_t >( n );
			}
			//A partial frame is the end of the stream, never a frame.
			if( got < frame.size() )
			{
				if( got > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes, %dx%d): dropped\n", got, frame.size(), width, height );
				break;
			}

			//Through the plugin's own setter, so a cue moves what a slider
			//would, and an event is a press.
			for( const Automation& a : automation )
				session.plugin.SetFloatParameter( a.index, valueAt( a.track, index, a.type ) );

			const bool ok = index != failRender && session.render( index, frame );
			if( !ok )
			{
				std::fprintf( stderr, "render failed at frame %d\n", index );
				status = 1;
				break;
			}

			const std::vector< unsigned char > out = session.readBack();
			size_t written                         = 0;
			while( written < out.size() )
			{
				const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
				if( put <= 0 )
					break;
				written += static_cast< size_t >( put );
			}
			//The reader has gone: rendering on into a closed pipe is work
			//nobody will see, and a short frame is worse than none.
			if( written < out.size() )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				status = 1;
				break;
			}
		}
		session.end();
		return finish( status );
	}

	if( !session.begin( width, height ) )
		return finish( 1 );
	std::vector< double > sum;
	for( int frame = 0; frame < frames; ++frame )
	{
		bool ok = false;
		if( source == "card" )
			ok = session.render( frame, buildCard( width, height, frame ) );
		else if( source == "flat" )
			ok = session.render( frame, flat( width, height, level ) );
		else if( source == "white" )
			ok = session.render( frame, flat( width, height, 1.0 ) );
		else if( source == "black" )
			ok = session.render( frame, flat( width, height, 0.0 ) );
		else
		{
			std::fprintf( stderr, "unknown --source %s\n", source.c_str() );
			return finish( 2 );
		}
		if( !ok )
			return finish( 1 );
		if( average )
		{
			const std::vector< unsigned char > image = session.readBack();
			sum.resize( image.size(), 0.0 );
			for( size_t i = 0; i < image.size(); ++i )
				sum[ i ] += image[ i ];
		}
	}

	std::vector< unsigned char > image = session.readBack();
	if( average )
		for( size_t i = 0; i < image.size(); ++i )
			image[ i ] = static_cast< unsigned char >( std::lround( sum[ i ] / frames ) );
	session.end();
	if( !writePng( outPath, width, height, image ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return finish( 1 );
	}
	std::printf( "wrote %s (%dx%d, %d frames%s)\n", outPath.c_str(), width, height, frames, average ? ", averaged" : "" );
	return finish( 0 );
}
