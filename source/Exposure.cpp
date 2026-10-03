#include "Exposure.h"

#include "Controls.h"
#include "Shaders.h"

#include <cmath>

namespace gate::exposure
{
namespace
{
constexpr double kLog2Ten = 3.32192809488736234787;
} // namespace

Settings FromHost( const HostValues& host, int perturb )
{
	Settings s;
	s.fps       = controls::Fps( controls::OptionIndex( host.fps, controls::kFpsCount ) );
	s.blades    = controls::Blades( controls::OptionIndex( host.blades, controls::kBladesCount ) );
	s.open      = controls::ShutterOpen( host.shutter );
	s.lamp      = controls::OptionIndex( host.lamp, controls::kLampCount );
	s.framing   = controls::FramingHeights( host.framing );
	s.law       = model::Weave( controls::Amount( host.weave ), controls::Amount( host.shrinkage ), perturb );
	s.hair      = controls::Amount( host.hair );
	s.scratches = controls::Amount( host.scratches );
	s.dust      = controls::Amount( host.dust );
	s.splices   = controls::SplicesPerMinute( host.splices );
	s.age       = controls::Amount( host.age );
	s.vignette  = controls::VignetteDegrees( host.vignette );
	s.mix       = controls::Amount( host.mix );
	s.perturb   = perturb;
	return s;
}

void Build( uint32_t seed, const Settings& settings, const model::Segment* segments, int count, double aspect,
            int64_t cueStart, const std::vector< model::Scratch >& scratchOverride, Frame& out )
{
	using namespace model;

	out       = Frame();
	out.count = count;
	out.data.assign( static_cast< size_t >( shaders::kPrintDataWidth ) * 4, 0.0f );
	for( int i = 0; i < count; ++i )
		out.segments[ i ] = segments[ i ];

	//---------------------------------------------------------------------
	// The print, for every projector frame this display frame shows. Newest
	// first, so the particle cap drops the oldest frame's dust and the hair
	// shape is the newest frame's.
	//---------------------------------------------------------------------
	int hairFrom = -1;
	for( int i = count - 1; i >= 0; --i )
	{
		const int64_t k = segments[ i ].frame;
		double x = 0.0, y = 0.0;
		WeaveOffset( seed, k, settings.law, x, y );
		if( IsSplice( seed, k, settings.splices, settings.fps ) )
		{
			double jx = 0.0, jy = 0.0;
			SpliceJump( seed, k, jx, jy );
			x += jx;
			y += jy;
			out.flags[ i ] |= 1;
		}
		if( IsCue( cueStart, k ) )
			out.flags[ i ] |= 2;
		out.weights[ i ]         = static_cast< float >( segments[ i ].weight );
		out.offsets[ 2 * i ]     = static_cast< float >( x );
		out.offsets[ 2 * i + 1 ] = static_cast< float >( y );

		for( const Particle& p : Dust( seed, k, settings.dust, aspect ) )
		{
			if( out.particleCount >= kMaxParticles )
				break;
			float* t0 = out.data.data() + 8 * out.particleCount;
			t0[ 0 ]   = static_cast< float >( p.x );
			t0[ 1 ]   = static_cast< float >( p.y );
			t0[ 2 ]   = static_cast< float >( p.a );
			t0[ 3 ]   = static_cast< float >( p.b );
			t0[ 4 ]   = static_cast< float >( std::cos( p.angle ) );
			t0[ 5 ]   = static_cast< float >( std::sin( p.angle ) );
			t0[ 6 ]   = static_cast< float >( p.strength );
			t0[ 7 ]   = static_cast< float >( i + 8 * p.fibre );
			++out.particleCount;
		}

		const Hair h = HairAt( seed, k, settings.hair, aspect );
		if( h.present )
		{
			out.hairMask |= 1 << i;
			if( hairFrom < 0 )
			{
				hairFrom      = i;
				out.hairShape = h;
			}
		}
	}

	const int64_t latest = segments[ count - 1 ].frame;
	out.scratches = scratchOverride.empty() ? Scratches( seed, segments[ 0 ].frame, latest, settings.scratches, aspect ) : scratchOverride;
	for( const Scratch& s : out.scratches )
	{
		if( out.scratchCount >= kMaxScratches )
			break;
		int mask = 0;
		for( int i = 0; i < count; ++i )
			if( !scratchOverride.empty() || ( segments[ i ].frame >= s.born && segments[ i ].frame < s.dies ) )
				mask |= 1 << i;
		float* t = out.data.data() + 4 * ( shaders::kPrintScratchFirst + 2 * out.scratchCount );
		t[ 0 ]   = static_cast< float >( s.left );
		t[ 1 ]   = static_cast< float >( s.right );
		t[ 2 ]   = static_cast< float >( s.strength );
		t[ 3 ]   = static_cast< float >( s.side );
		t[ 4 ]   = static_cast< float >( mask );
		++out.scratchCount;
	}

	if( out.hairShape.present )
		for( int i = 0; i < kHairPoints; ++i )
		{
			float* t = out.data.data() + 4 * ( shaders::kPrintHairFirst + i );
			t[ 0 ]   = static_cast< float >( out.hairShape.x[ i ] );
			t[ 1 ]   = static_cast< float >( out.hairShape.y[ i ] );
		}

	//---------------------------------------------------------------------
	// The constants, in the precision the shader receives them.
	//---------------------------------------------------------------------
	double lampRgb[ 3 ], retention[ 3 ];
	LampRgb( controls::LampKelvin( settings.lamp ), lampRgb );
	Retention( settings.age, settings.perturb, retention );

	out.framing    = static_cast< float >( settings.framing );
	out.pitch      = static_cast< float >( kPitch );
	out.margin     = static_cast< float >( kMargin );
	out.maxDensity = static_cast< float >( kMaxDensity * kLog2Ten );
	for( int c = 0; c < 3; ++c )
	{
		out.retention[ c ] = static_cast< float >( retention[ c ] );
		out.lamp[ c ]      = static_cast< float >( lampRgb[ c ] );
	}
	out.spliceWash    = kSpliceWash;
	out.hairHalfWidth = static_cast< float >( out.hairShape.halfWidth );
	out.hairOpacity   = static_cast< float >( out.hairShape.opacity );
	out.cueCentre[ 0 ] = static_cast< float >( aspect - 0.12 );
	out.cueCentre[ 1 ] = 0.11f;
	out.cueRadius     = 0.028f;
	out.vignetteTan   = static_cast< float >( std::tan( settings.vignette * kPi / 180.0 ) );
	out.aspect        = static_cast< float >( aspect );
	out.mix           = settings.mix;
}

} // namespace gate::exposure
