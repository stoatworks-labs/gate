#include "Projection.h"

#include "Shaders.h"

#include <algorithm>
#include <cmath>

namespace gate::projection
{
namespace
{
inline float clampf( float v, float lo, float hi )
{
	return std::min( std::max( v, lo ), hi );
}

/// GLSL's mix: x ( 1 - a ) + y a, in that form.
inline float mixf( float x, float y, float a )
{
	return x * ( 1.0f - a ) + y * a;
}

//= mirrored: segmentLine in kOutputBody (Shaders.cpp). Edit both.
float segmentLine( float px, float py, float ax, float ay, float bx, float by )
{
	const float abx = bx - ax, aby = by - ay;
	const float t   = clampf( ( ( px - ax ) * abx + ( py - ay ) * aby ) / std::max( abx * abx + aby * aby, 1e-12f ), 0.0f, 1.0f );
	const float dx  = ( px - ax ) - abx * t;
	const float dy  = ( py - ay ) - aby * t;
	return std::sqrt( dx * dx + dy * dy );
}

//= mirrored: fetchHeld in kOutputBody (Shaders.cpp). Edit both.
inline const float* fetchHeld( const Picture& p, int x, int y )
{
	x = std::clamp( x, 0, p.width - 1 );
	y = std::clamp( y, 0, p.height - 1 );
	return p.rgb.data() + ( static_cast< size_t >( y ) * p.width + x ) * 3;
}

//= mirrored: tapHeld in kOutputBody (Shaders.cpp). Edit both.
//Bilinear by hand, in float. At a whole-texel position t is exactly 0 and the
//texel comes back unchanged.
void tapHeld( const Picture& p, float sx, float sy, float out[ 3 ] )
{
	//The clamp only keeps the int conversion defined; any position this far
	//out reads the edge texel either way.
	const float fx = std::floor( clampf( sx, -1e8f, 1e8f ) );
	const float fy = std::floor( clampf( sy, -1e8f, 1e8f ) );
	const float tx = sx - fx;
	const float ty = sy - fy;
	const int ix   = static_cast< int >( fx );
	const int iy   = static_cast< int >( fy );
	const float* a = fetchHeld( p, ix, iy );
	const float* b = fetchHeld( p, ix + 1, iy );
	const float* c = fetchHeld( p, ix, iy + 1 );
	const float* d = fetchHeld( p, ix + 1, iy + 1 );
	for( int k = 0; k < 3; ++k )
		out[ k ] = mixf( mixf( a[ k ], b[ k ], tx ), mixf( c[ k ], d[ k ], tx ), ty );
}
} // namespace

//= mirrored: decodeSrgb in kCaptureBody (Shaders.cpp). Edit both.
float DecodeSrgb( float c )
{
	c              = std::max( c, 0.0f );
	const float lo = c / 12.92f;
	const float hi = std::pow( ( c + 0.055f ) / 1.055f, 2.4f );
	return c < 0.04045f ? lo : hi;
}

//= mirrored: encodeSrgb in kOutputBody (Shaders.cpp). Edit both.
float EncodeSrgb( float c )
{
	c              = clampf( c, 0.0f, 1.0f );
	const float lo = c * 12.92f;
	const float hi = 1.055f * std::pow( c, 1.0f / 2.4f ) - 0.055f;
	return c < 0.0031308f ? lo : hi;
}

//= mirrored: main() in kOutputBody (Shaders.cpp), line for line. Edit both.
void Shade( const Pass& pass, int x, int y, const float src[ 4 ], float out[ 4 ] )
{
	const exposure::Frame& f = *pass.frame;
	const float* data        = f.data.data();

	const float pixX = static_cast< float >( x ) + 0.5f;
	const float pixY = static_cast< float >( y ) + 0.5f;
	const float H    = pass.outHeight;
	const float pxh  = 1.0f / H;           //one pixel, in h-units
	const float hposX = pixX / H;          //h-units, y down
	const float hposY = ( H - pixY ) / H;

	//The hair is in the gate: one shape for every frame it is in.
	float hairCover      = 0.0f;
	const int hairPoints = f.hairShape.present ? model::kHairPoints : 0;
	if( hairPoints > 1 )
	{
		float dist        = 1e9f;
		const float* prev = data + 4 * shaders::kPrintHairFirst;
		for( int k = 1; k < hairPoints; ++k )
		{
			const float* next = data + 4 * ( shaders::kPrintHairFirst + k );
			dist              = std::min( dist, segmentLine( hposX, hposY, prev[ 0 ], prev[ 1 ], next[ 0 ], next[ 1 ] ) );
			prev              = next;
		}
		const float w = std::max( f.hairHalfWidth, 0.5f * pxh );
		hairCover     = clampf( ( w - dist ) / pxh + 0.5f, 0.0f, 1.0f ) * ( f.hairHalfWidth / w );
	}

	float light[ 3 ] = { 0.0f, 0.0f, 0.0f };
	for( int i = 0; i < f.count; ++i )
	{
		const float offX  = f.offsets[ 2 * i ];
		const float offY  = f.offsets[ 2 * i + 1 ];
		const float down  = f.framing + offY;
		const float sx    = ( pixX - 0.5f ) - offX * H;
		float sy          = ( pixY - 0.5f ) + down * H;
		const float u     = hposY - down;      //picture heights below this frame's top
		const float filmX = hposX - offX;      //where on the frame, h-units
		const float filmY = u;
		int which         = pass.segPicture[ i ];
		bool own = true, picture = true;

		//Framed off: past the printed picture's own margin, the frame line,
		//then the next picture along the strip.
		if( u < -f.margin )
		{
			own = false;
			if( u >= ( 1.0f + f.margin ) - f.pitch )
				picture = false;
			else
			{
				sy -= f.pitch * H;
				which = pass.segAbove[ i ];
			}
		}
		else if( u >= 1.0f + f.margin )
		{
			own = false;
			if( u < f.pitch - f.margin )
				picture = false;
			else
				sy += f.pitch * H;
		}

		//The print is dye: density, in log2 units, capped at the print's Dmax.
		float D[ 3 ] = { f.maxDensity, f.maxDensity, f.maxDensity };
		if( picture )
		{
			const Picture& held  = *pass.held[ which ];
			const float scaleX   = static_cast< float >( held.width ) / pass.outWidth;
			const float scaleY   = static_cast< float >( held.height ) / pass.outHeight;
			float lin[ 3 ];
			tapHeld( held, ( sx + 0.5f ) * scaleX - 0.5f, ( sy + 0.5f ) * scaleY - 0.5f, lin );
			for( int c = 0; c < 3; ++c )
				D[ c ] = clampf( -std::log2( std::max( lin[ c ], 1e-30f ) ), 0.0f, f.maxDensity );
		}
		for( int c = 0; c < 3; ++c )
			D[ c ] *= f.retention[ c ];
		if( own && ( f.flags[ i ] & 1 ) != 0 )
			for( int c = 0; c < 3; ++c )
				D[ c ] *= f.spliceWash;

		//The gate's grit: straight lines at a fixed x. The base side scatters
		//light away; the emulsion side takes dye off, magenta first.
		float baseLoss[ 3 ] = { 1.0f, 1.0f, 1.0f };
		const float x0      = pixX - 0.5f;
		for( int j = 0; j < f.scratchCount; ++j )
		{
			const float* s0 = data + 4 * ( shaders::kPrintScratchFirst + 2 * j );
			const float* s1 = s0 + 4;
			if( ( ( static_cast< int >( s1[ 0 ] ) >> i ) & 1 ) == 0 )
				continue;
			const float l     = s0[ 0 ] * pass.outWidth;
			const float r     = s0[ 1 ] * pass.outWidth;
			const float cover = clampf( std::min( r, x0 + 1.0f ) - std::max( l, x0 ), 0.0f, 1.0f );
			if( cover <= 0.0f )
				continue;
			if( s0[ 3 ] < 0.5f )
			{
				for( int c = 0; c < 3; ++c )
					baseLoss[ c ] *= 1.0f - cover * s0[ 2 ];
			}
			else
			{
				const float d        = 3.0f * s0[ 2 ];
				const float layer[ 3 ] = { d - 1.0f, d, d - 2.0f };
				for( int c = 0; c < 3; ++c )
					D[ c ] *= 1.0f - cover * clampf( layer[ c ], 0.0f, 1.0f );
			}
		}

		float T[ 3 ];
		for( int c = 0; c < 3; ++c )
			T[ c ] = std::exp2( -D[ c ] ) * baseLoss[ c ];

		if( own )
		{
			//Dust on this frame of the print.
			for( int j = 0; j < f.particleCount; ++j )
			{
				const float* p0 = data + 8 * j;
				const float* p1 = p0 + 4;
				const int code  = static_cast< int >( p1[ 3 ] );
				if( ( code & 7 ) != i )
					continue;
				const float dx = filmX - p0[ 0 ];
				const float dy = filmY - p0[ 1 ];
				if( std::fabs( dx ) > p0[ 2 ] + 3.0f * pxh || std::fabs( dy ) > p0[ 2 ] + 3.0f * pxh )
					continue;
				const float qx = p1[ 0 ] * dx + p1[ 1 ] * dy;
				const float qy = -p1[ 1 ] * dx + p1[ 0 ] * dy;
				float cover;
				if( code >= 8 )
				{
					const float t     = std::max( p0[ 3 ], 0.5f * pxh );
					const float along = clampf( qx, -p0[ 2 ], p0[ 2 ] );
					const float ex    = qx - along;
					const float dist  = std::sqrt( ex * ex + qy * qy );
					cover             = clampf( ( t - dist ) / pxh + 0.5f, 0.0f, 1.0f ) * ( p0[ 3 ] / t );
				}
				else
				{
					const float rrX  = std::max( p0[ 2 ], 0.5f * pxh );
					const float rrY  = std::max( p0[ 3 ], 0.5f * pxh );
					const float nx   = qx / rrX;
					const float ny   = qy / rrY;
					const float dist = ( std::sqrt( nx * nx + ny * ny ) - 1.0f ) * std::min( rrX, rrY );
					cover            = clampf( 0.5f - dist / pxh, 0.0f, 1.0f ) * ( p0[ 2 ] * p0[ 3 ] ) / ( rrX * rrY );
				}
				if( p1[ 2 ] > 0.0f )
				{
					for( int c = 0; c < 3; ++c )
						T[ c ] *= 1.0f - cover * p1[ 2 ];
				}
				else
				{
					for( int c = 0; c < 3; ++c )
						T[ c ] = mixf( T[ c ], 1.0f, cover * -p1[ 2 ] );
				}
			}

			//A cue dot, scribed through the emulsion: clear, with a dark rim.
			if( ( f.flags[ i ] & 2 ) != 0 )
			{
				const float cx   = filmX - f.cueCentre[ 0 ];
				const float cy   = filmY - f.cueCentre[ 1 ];
				const float r    = std::sqrt( cx * cx + cy * cy );
				const float disc = clampf( ( f.cueRadius - r ) / pxh + 0.5f, 0.0f, 1.0f );
				const float rim  = clampf( ( 0.12f * f.cueRadius - std::fabs( r - f.cueRadius ) ) / pxh + 0.5f, 0.0f, 1.0f );
				for( int c = 0; c < 3; ++c )
				{
					T[ c ] = mixf( T[ c ], 0.92f, 0.85f * disc );
					T[ c ] *= 1.0f - 0.6f * rim;
				}
			}
		}

		if( ( ( f.hairMask >> i ) & 1 ) != 0 )
			for( int c = 0; c < 3; ++c )
				T[ c ] *= 1.0f - f.hairOpacity * hairCover;

		for( int c = 0; c < 3; ++c )
			light[ c ] += f.weights[ i ] * T[ c ];
	}

	//The lamp's colour, and the lens: cos^4 of the field angle.
	const float fromX   = hposX - 0.5f * f.aspect;
	const float fromY   = hposY - 0.5f;
	const float cornerX = 0.5f * f.aspect;
	const float t       = std::sqrt( fromX * fromX + fromY * fromY ) / std::sqrt( cornerX * cornerX + 0.5f * 0.5f ) * f.vignetteTan;
	const float falloff = 1.0f / ( ( 1.0f + t * t ) * ( 1.0f + t * t ) );

	for( int c = 0; c < 3; ++c )
		out[ c ] = mixf( src[ c ], EncodeSrgb( light[ c ] * f.lamp[ c ] * falloff ), f.mix );
	out[ 3 ] = mixf( src[ 3 ], 1.0f, f.mix );
}

} // namespace gate::projection
