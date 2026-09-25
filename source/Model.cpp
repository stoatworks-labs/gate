#include "Model.h"

#include <algorithm>
#include <cmath>

namespace gate::model
{
namespace
{
/// One independent hash stream per thing the print draws.
enum Stream : uint32_t
{
	kStreamWeaveX = 1,
	kStreamWeaveY,
	kStreamScratch,
	kStreamDust,
	kStreamHair,
	kStreamHairTremble,
	kStreamSplice,
};

double lerp( double a, double b, double t )
{
	return a + ( b - a ) * t;
}
} // namespace

//---------------------------------------------------------------------------
uint32_t Hash( uint32_t v )
{
	const uint32_t state = v * 747796405u + 2891336453u;
	const uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

uint32_t Key( uint32_t seed, uint32_t stream, int64_t index, uint32_t sub )
{
	const uint64_t bits = static_cast< uint64_t >( index );
	uint32_t h          = Hash( seed ^ Hash( stream + 0x9e3779b9u ) );
	h                   = Hash( h ^ static_cast< uint32_t >( bits ) );
	h                   = Hash( h ^ static_cast< uint32_t >( bits >> 32 ) );
	return Hash( h ^ ( sub * 0x85ebca6bu ) );
}

double Uniform( uint32_t key )
{
	return ( static_cast< double >( key ) + 0.5 ) / 4294967296.0;
}

double Normal( uint32_t seed, uint32_t stream, int64_t index )
{
	const double u1 = Uniform( Key( seed, stream, index, 0 ) );
	const double u2 = Uniform( Key( seed, stream, index, 1 ) );
	return std::sqrt( -2.0 * std::log( u1 ) ) * std::cos( 2.0 * kPi * u2 );
}

//---------------------------------------------------------------------------
// The shutter.
//---------------------------------------------------------------------------
double ShutterOpen( double p, int blades, double open )
{
	const double c = p * blades;
	const double r = c - std::floor( c );
	return ( r >= 0.5 * ( 1.0 - open ) && r < 0.5 * ( 1.0 + open ) ) ? 1.0 : 0.0;
}

double CumulativeOpen( double p, int blades, double open )
{
	const double c = p * blades;
	const double n = std::floor( c );
	const double r = c - n;
	return ( n * open + std::clamp( r - 0.5 * ( 1.0 - open ), 0.0, open ) ) / blades;
}

int Segments( double p0, double p1, int blades, double open, bool instant, Segment out[ kMaxSegments ] )
{
	if( instant || !( p1 > p0 ) )
	{
		out[ 0 ].frame  = static_cast< int64_t >( std::floor( p1 ) );
		out[ 0 ].weight = ShutterOpen( p1, blades, open ) / open;
		return 1;
	}

	//Work relative to the first frame: the shutter repeats every frame, so
	//shifting by a whole frame changes nothing but keeps the numbers small.
	const double base  = std::floor( p0 );
	const double q0    = p0 - base, q1 = p1 - base;
	const int last     = static_cast< int >( std::ceil( q1 ) ) - 1;
	const double scale = 1.0 / ( open * ( q1 - q0 ) );
	int count          = 0;
	for( int j = 0; j <= last; ++j )
	{
		const double a  = std::max( q0, static_cast< double >( j ) );
		const double b  = std::min( q1, static_cast< double >( j + 1 ) );
		const double w  = ( CumulativeOpen( b, blades, open ) - CumulativeOpen( a, blades, open ) ) * scale;
		const int64_t k = static_cast< int64_t >( base ) + j;
		if( count < kMaxSegments )
			out[ count++ ] = { k, w };
		else
		{
			//Cannot happen inside kMaxFrameDelta at 25 fps; if it did, the
			//oldest keeps the light rather than the light being lost.
			for( int i = 0; i + 1 < kMaxSegments; ++i )
				out[ i ] = out[ i + 1 ];
			out[ 0 ].weight += w;
			out[ kMaxSegments - 1 ] = { k, w };
		}
	}
	return count;
}

//---------------------------------------------------------------------------
// Weave.
//---------------------------------------------------------------------------
WeaveLaw Weave( double weave, double shrinkage, int perturb )
{
	const double percent = kShrinkMaxPercent * std::clamp( shrinkage, 0.0, 1.0 );
	const double gain    = ( perturb & kPerturbShrinkIgnored ) ? 1.0 : 1.0 + kShrinkGain * percent;
	WeaveLaw law;
	law.sigmaX = kWeaveMm * std::clamp( weave, 0.0, 1.0 ) * gain / kApertureHeightMm;
	law.sigmaY = kWeaveVertical * law.sigmaX;
	law.rho    = ( perturb & kPerturbWhiteWeave ) ? 0.0 : kWeaveRho * ( 1.0 - percent / kShrinkDecorrelate );
	return law;
}

void WeaveOffset( uint32_t seed, int64_t frame, const WeaveLaw& law, double& x, double& y )
{
	x = y = 0.0;
	if( law.sigmaX <= 0.0 )
		return;
	const int terms = law.rho > 0.0 ? static_cast< int >( std::ceil( std::log( 1e-9 ) / std::log( law.rho ) ) ) : 0;
	double weight   = 1.0;
	for( int j = 0; j <= terms; ++j )
	{
		x += weight * Normal( seed, kStreamWeaveX, frame - j );
		y += weight * Normal( seed, kStreamWeaveY, frame - j );
		weight *= law.rho;
	}
	const double innovation = std::sqrt( 1.0 - law.rho * law.rho );
	x *= innovation * law.sigmaX;
	y *= innovation * law.sigmaY;
}

//---------------------------------------------------------------------------
// Scratches.
//---------------------------------------------------------------------------
std::vector< Scratch > Scratches( uint32_t seed, int64_t first, int64_t last, double amount, double aspect )
{
	std::vector< Scratch > list;
	const double p = std::clamp( amount, 0.0, 1.0 ) * kScratchBirths;
	if( p <= 0.0 )
		return list;
	//Newest first, so the cap drops the oldest grit.
	for( int64_t b = last; b > first - kScratchLifeMax && static_cast< int >( list.size() ) < kMaxScratches; --b )
	{
		if( Uniform( Key( seed, kStreamScratch, b ) ) >= p )
			continue;
		const double life = std::min( static_cast< double >( kScratchLifeMax ),
		                              kScratchLifeMin - kScratchLifeMean * std::log( Uniform( Key( seed, kStreamScratch, b, 1 ) ) ) );
		Scratch s;
		s.born = b;
		s.dies = b + static_cast< int64_t >( life );
		if( s.dies <= first )
			continue;
		const double centre = lerp( 0.03, 0.97, Uniform( Key( seed, kStreamScratch, b, 2 ) ) );
		const double halfMm = lerp( kScratchHalfMmMin, kScratchHalfMmMax, Uniform( Key( seed, kStreamScratch, b, 3 ) ) );
		const double half   = halfMm / kApertureHeightMm / aspect;
		s.left              = centre - half;
		s.right             = centre + half;
		const double u      = Uniform( Key( seed, kStreamScratch, b, 5 ) );
		s.side              = Uniform( Key( seed, kStreamScratch, b, 4 ) ) < 0.55 ? kBase : kEmulsion;
		s.strength          = s.side == kBase ? lerp( 0.25, 0.65, u ) : lerp( 0.15, 1.0, u );
		list.push_back( s );
	}
	return list;
}

//---------------------------------------------------------------------------
// Dust.
//---------------------------------------------------------------------------
std::vector< Particle > Dust( uint32_t seed, int64_t frame, double amount, double aspect )
{
	std::vector< Particle > list;
	const double mean = kDustBirths * std::clamp( amount, 0.0, 1.0 );
	if( mean <= 0.0 )
		return list;
	for( int64_t b = frame - 3; b <= frame; ++b )
	{
		//Poisson by its inverse CDF, one uniform per frame.
		const double u = Uniform( Key( seed, kStreamDust, b ) );
		int n          = 0;
		double term    = std::exp( -mean ), cdf = term;
		while( u > cdf && n < kDustBirthsMax )
		{
			++n;
			term *= mean / n;
			cdf += term;
		}
		for( int j = 0; j < n; ++j )
		{
			auto draw = [ & ]( uint32_t field ) { return Uniform( Key( seed, kStreamDust, b, 1 + static_cast< uint32_t >( j ) * 16u + field ) ); };
			const double l = draw( 0 );
			const int life = l < 0.70 ? 1 : l < 0.85 ? 2 : l < 0.95 ? 3 : 4;
			const int age  = static_cast< int >( frame - b );
			if( age >= life )
				continue;
			Particle p;
			p.x     = draw( 1 ) * aspect + ( draw( 2 ) - 0.5 ) * 0.008 * age;
			p.y     = draw( 3 ) + ( draw( 4 ) - 0.5 ) * 0.008 * age;
			p.angle = draw( 5 ) * kPi;
			p.fibre = draw( 6 ) < 0.25 ? 1 : 0;
			if( p.fibre )
			{
				p.a = lerp( 0.05, 0.30, draw( 7 ) ) / kApertureHeightMm;
				p.b = lerp( 0.004, 0.009, draw( 8 ) ) / kApertureHeightMm;
			}
			else
			{
				const double r = std::min( 0.08, 0.008 * std::pow( draw( 7 ), -1.0 / 2.2 ) ) / kApertureHeightMm;
				p.a            = r;
				p.b            = r * lerp( 0.5, 1.0, draw( 8 ) );
			}
			const double s = draw( 10 );
			p.strength     = draw( 9 ) < 0.85 ? lerp( 0.7, 1.0, s ) : -lerp( 0.6, 1.0, s );
			if( static_cast< int >( list.size() ) < kMaxParticles )
				list.push_back( p );
		}
	}
	return list;
}

//---------------------------------------------------------------------------
// The hair.
//---------------------------------------------------------------------------
Hair HairAt( uint32_t seed, int64_t frame, double amount, double aspect )
{
	Hair hair;
	if( amount <= 0.0 )
		return hair;
	const int64_t slot = frame >= 0 ? frame / kHairSlot : ( frame - kHairSlot + 1 ) / kHairSlot;
	auto draw          = [ & ]( uint32_t field ) { return Uniform( Key( seed, kStreamHair, slot, field ) ); };
	if( draw( 0 ) >= amount )
		return hair;
	const int start = static_cast< int >( draw( 1 ) * 48.0 );
	const int end   = std::min( kHairSlot - 1, start + 36 + static_cast< int >( draw( 2 ) * 57.0 ) );
	const int t     = static_cast< int >( frame - slot * kHairSlot );
	if( t < start || t >= end )
		return hair;
	hair.present = true;

	//Where it hangs: an edge of the aperture, a place along it, a direction
	//in, a length, a curl.
	const double e = draw( 3 ), along = lerp( 0.1, 0.9, draw( 4 ) );
	double ax, ay, dx, dy;
	if( e < 0.40 )
		ax = along * aspect, ay = 1.0, dx = 0.0, dy = -1.0;//bottom
	else if( e < 0.65 )
		ax = along * aspect, ay = 0.0, dx = 0.0, dy = 1.0;//top
	else if( e < 0.85 )
		ax = 0.0, ay = along, dx = 1.0, dy = 0.0;//left
	else
		ax = aspect, ay = along, dx = -1.0, dy = 0.0;//right
	const double tilt = ( draw( 5 ) - 0.5 ) * 1.8;
	const double c = std::cos( tilt ), s = std::sin( tilt );
	const double ux = c * dx - s * dy, uy = s * dx + c * dy;//in
	const double vx = -uy, vy = ux;                          //across
	const double length = lerp( 0.06, 0.18, draw( 6 ) );
	const double curl   = ( draw( 7 ) - 0.5 ) * 0.8;
	const double waves  = lerp( 0.6, 2.2, draw( 8 ) );

	//It shakes loose: in its last four frames it slides back out of the gate.
	const int left      = end - t;
	const double out    = left <= 4 ? length * ( 1.0 - left / 5.0 ) : 0.0;
	const double tremble = 0.0012 * ( left <= 4 ? 3.0 : 1.0 );
	for( int i = 0; i < kHairPoints; ++i )
	{
		const double u    = static_cast< double >( i ) / ( kHairPoints - 1 );
		const double bend = curl * length * std::sin( kPi * u * waves ) * u;
		//The pull-down shakes it every frame, the tip most.
		const double jitter = tremble * u * Normal( seed, kStreamHairTremble, frame * kHairPoints + i / 4 );
		const double along2 = u * length - 0.01 - out;
		hair.x[ i ]         = ax + ux * along2 + vx * ( bend + jitter );
		hair.y[ i ]         = ay + uy * along2 + vy * ( bend + jitter );
	}
	hair.halfWidth = lerp( 0.020, 0.035, draw( 9 ) ) / kApertureHeightMm;
	hair.opacity   = 0.92;
	return hair;
}

//---------------------------------------------------------------------------
// Splices and cues.
//---------------------------------------------------------------------------
bool IsSplice( uint32_t seed, int64_t frame, double perMinute, double fps )
{
	if( perMinute <= 0.0 || fps <= 0.0 )
		return false;
	return Uniform( Key( seed, kStreamSplice, frame ) ) < perMinute / ( 60.0 * fps );
}

void SpliceJump( uint32_t seed, int64_t frame, double& x, double& y )
{
	const double size = lerp( 0.04, 0.12, Uniform( Key( seed, kStreamSplice, frame, 1 ) ) );
	y                 = Uniform( Key( seed, kStreamSplice, frame, 2 ) ) < 0.5 ? -size : size;
	x                 = ( Uniform( Key( seed, kStreamSplice, frame, 3 ) ) - 0.5 ) * 0.02;
}

bool IsCue( int64_t cueStart, int64_t frame )
{
	if( cueStart < 0 )
		return false;
	const int64_t d = frame - cueStart;
	return ( d >= 0 && d < kCueFrames ) || ( d >= kCueGap && d < kCueGap + kCueFrames );
}

//---------------------------------------------------------------------------
// Dyes.
//---------------------------------------------------------------------------
void Retention( double age, int perturb, double rgb[ 3 ] )
{
	double k[ 3 ] = { kFadeCyan, kFadeMagenta, kFadeYellow };
	if( perturb & kPerturbEqualFade )
		k[ 0 ] = k[ 1 ] = k[ 2 ] = ( kFadeCyan + kFadeMagenta + kFadeYellow ) / 3.0;
	for( int i = 0; i < 3; ++i )
		rgb[ i ] = std::exp( -k[ i ] * std::max( age, 0.0 ) );
}

//---------------------------------------------------------------------------
// Lamps.
//---------------------------------------------------------------------------
namespace
{
double lobe( double lambda, double mu, double s1, double s2 )
{
	const double t = ( lambda - mu ) / ( lambda < mu ? s1 : s2 );
	return std::exp( -0.5 * t * t );
}

void blackBodyRgb( double kelvin, double rgb[ 3 ] )
{
	double X = 0.0, Y = 0.0, Z = 0.0;
	for( int nm = 380; nm <= 780; ++nm )
	{
		const double l = static_cast< double >( nm );
		const double m = l * 1e-9;
		const double B = 1.0 / ( std::pow( m, 5.0 ) * ( std::exp( 1.438776877e-2 / ( m * kelvin ) ) - 1.0 ) );
		X += B * ( 1.056 * lobe( l, 599.8, 37.9, 31.0 ) + 0.362 * lobe( l, 442.0, 16.0, 26.7 ) - 0.065 * lobe( l, 501.1, 20.4, 26.2 ) );
		Y += B * ( 0.821 * lobe( l, 568.8, 46.9, 40.5 ) + 0.286 * lobe( l, 530.9, 16.3, 31.1 ) );
		Z += B * ( 1.217 * lobe( l, 437.0, 11.8, 36.0 ) + 0.681 * lobe( l, 459.0, 26.0, 13.8 ) );
	}
	rgb[ 0 ] = 3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z;
	rgb[ 1 ] = -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z;
	rgb[ 2 ] = 0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z;
}
} // namespace

void LampRgb( double kelvin, double rgb[ 3 ] )
{
	double white[ 3 ], lamp[ 3 ];
	blackBodyRgb( 6504.0, white );
	blackBodyRgb( kelvin, lamp );
	for( int i = 0; i < 3; ++i )
		rgb[ i ] = std::max( 0.0, lamp[ i ] / white[ i ] );
	const double Y = 0.2126 * rgb[ 0 ] + 0.7152 * rgb[ 1 ] + 0.0722 * rgb[ 2 ];
	for( int i = 0; i < 3; ++i )
		rgb[ i ] /= Y;
}

} // namespace gate::model
