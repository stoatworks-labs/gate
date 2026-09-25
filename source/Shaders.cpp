#include "Shaders.h"

namespace gate::shaders
{
namespace
{
const char* const kVersion = "#version 410 core\n";

//---------------------------------------------------------------------------
// The vertex shader every pass shares.
//---------------------------------------------------------------------------
const char* const kVertexBody = R"(
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// capture: the host's picture, decoded to linear light. texelFetch, so an
// interior pixel is an exact copy of the host's (before the decode).
//---------------------------------------------------------------------------
const char* const kCaptureBody = R"(
uniform sampler2D Source;

in vec2 uv;
out vec4 fragColor;

vec3 decodeSrgb( vec3 c )
{
	c = max( c, vec3( 0.0 ) );
	vec3 lo = c / 12.92;
	vec3 hi = pow( ( c + 0.055 ) / 1.055, vec3( 2.4 ) );
	return mix( lo, hi, step( vec3( 0.04045 ), c ) );
}

void main()
{
	vec4 c = texelFetch( Source, ivec2( gl_FragCoord.xy ), 0 );
	fragColor = vec4( decodeSrgb( c.rgb ), c.a );
}
)";

//---------------------------------------------------------------------------
// resample: a held picture onto a new raster.
//---------------------------------------------------------------------------
const char* const kResampleBody = R"(
uniform sampler2D Old;

in vec2 uv;
out vec4 fragColor;

void main()
{
	fragColor = texture( Old, uv );
}
)";

//---------------------------------------------------------------------------
// output: the gate, the print, the shutter's sum, the lamp and the lens.
//---------------------------------------------------------------------------
const char* const kOutputBody = R"(
uniform sampler2D Held0;
uniform sampler2D Held1;
uniform ivec2 HeldSize;
uniform sampler2D Source;      //the live input, for Mix and alpha
uniform vec2 MaxUV;
uniform sampler2D PrintData;
uniform vec2 OutSize;

uniform int SegCount;
uniform float SegWeight[ 8 ];  //light over the display frame, / the mean
uniform vec2 SegOffset[ 8 ];   //the gate's offset of this frame, h-units, y down
uniform int SegPicture[ 8 ];   //which held picture is this frame
uniform int SegAbove[ 8 ];     //and which is the frame above it on the strip
uniform int SegFlags[ 8 ];     //1 a splice, 2 a cue dot

uniform float Framing;         //picture heights, + is down
uniform float Pitch;           //frame pitch in picture heights
uniform float Margin;          //printed picture beyond the aperture, each end
uniform float MaxDensity;      //log2 units
uniform vec3 Retention;        //what each dye keeps: ( cyan, magenta, yellow )
uniform float SpliceWash;
uniform int ParticleCount;
uniform int ScratchCount;
uniform int HairPoints;
uniform int HairMask;
uniform float HairHalfWidth;
uniform float HairOpacity;
uniform vec2 CueCentre;
uniform float CueRadius;
uniform vec3 Lamp;
uniform float VignetteTan;
uniform float Aspect;
uniform float MixAmount;
uniform int Probe;
uniform int Perturb;

in vec2 uv;
out vec4 fragColor;

const int kScratchFirst = 128;
const int kHairFirst = 160;

vec4 fetchHeld( int which, ivec2 p )
{
	p = clamp( p, ivec2( 0 ), HeldSize - 1 );
	if( which == 0 )
		return texelFetch( Held0, p, 0 );
	return texelFetch( Held1, p, 0 );
}

//Bilinear by hand, in float: a hardware filter may weight with 8 bits. At a
//whole-texel position t is exactly 0 and the texel comes back unchanged.
vec3 tapHeld( int which, vec2 s )
{
	vec2 f0 = floor( s );
	vec2 t = s - f0;
	ivec2 i0 = ivec2( f0 );
	vec3 a = fetchHeld( which, i0 ).rgb;
	vec3 b = fetchHeld( which, i0 + ivec2( 1, 0 ) ).rgb;
	vec3 c = fetchHeld( which, i0 + ivec2( 0, 1 ) ).rgb;
	vec3 d = fetchHeld( which, i0 + ivec2( 1, 1 ) ).rgb;
	return mix( mix( a, b, t.x ), mix( c, d, t.x ), t.y );
}

vec3 encodeSrgb( vec3 c )
{
	c = clamp( c, vec3( 0.0 ), vec3( 1.0 ) );
	vec3 lo = c * 12.92;
	vec3 hi = 1.055 * pow( c, vec3( 1.0 / 2.4 ) ) - 0.055;
	return mix( lo, hi, step( vec3( 0.0031308 ), c ) );
}

float segmentLine( vec2 p, vec2 a, vec2 b )
{
	vec2 ab = b - a;
	float t = clamp( dot( p - a, ab ) / max( dot( ab, ab ), 1e-12 ), 0.0, 1.0 );
	return length( p - a - ab * t );
}

void main()
{
	vec2 pix = gl_FragCoord.xy;
	float H = OutSize.y;
	float pxh = 1.0 / H;                        //one pixel, in h-units
	vec2 hpos = vec2( pix.x, H - pix.y ) / H;   //h-units, y down
	vec2 inScale = vec2( HeldSize ) / OutSize;

	//The hair is in the gate: one shape for every frame it is in.
	float hairCover = 0.0;
	if( HairPoints > 1 )
	{
		float dist = 1e9;
		vec2 prev = texelFetch( PrintData, ivec2( kHairFirst, 0 ), 0 ).xy;
		for( int k = 1; k < HairPoints; ++k )
		{
			vec2 next = texelFetch( PrintData, ivec2( kHairFirst + k, 0 ), 0 ).xy;
			dist = min( dist, segmentLine( hpos, prev, next ) );
			prev = next;
		}
		float w = max( HairHalfWidth, 0.5 * pxh );
		hairCover = clamp( ( w - dist ) / pxh + 0.5, 0.0, 1.0 ) * ( HairHalfWidth / w );
	}

	vec3 light = vec3( 0.0 );
	for( int i = 0; i < SegCount; ++i )
	{
		vec2 off = SegOffset[ i ];
		float down = Framing + off.y;
		float sx = pix.x - 0.5 - off.x * H;
		float sy = pix.y - 0.5 + down * H;
		float u = hpos.y - down;                  //picture heights below this frame's top
		vec2 film = vec2( hpos.x - off.x, u );    //where on the frame, h-units
		int which = SegPicture[ i ];
		bool own = true, picture = true;

		//Framed off: past the printed picture's own margin, the frame line,
		//then the next picture along the strip. Within the margin the picture
		//carries on (the clip's edge, repeated: the aperture never showed it).
		if( u < -Margin )
		{
			own = false;
			if( u >= 1.0 + Margin - Pitch )
				picture = false;
			else
			{
				sy -= Pitch * H;
				which = SegAbove[ i ];
			}
		}
		else if( u >= 1.0 + Margin )
		{
			own = false;
			if( u < Pitch - Margin )
				picture = false;
			else
				sy += Pitch * H;
		}

		//The print is dye: density, in log2 units, capped at the print's Dmax.
		vec3 D = vec3( MaxDensity );
		if( picture )
		{
			vec3 lin = tapHeld( which, ( vec2( sx, sy ) + 0.5 ) * inScale - 0.5 );
			D = clamp( -log2( max( lin, vec3( 1e-30 ) ) ), vec3( 0.0 ), vec3( MaxDensity ) );
		}
		D *= Retention;
		if( own && ( SegFlags[ i ] & 1 ) != 0 )
			D *= SpliceWash;

		//The gate's grit: straight lines at a fixed x. The base side scatters
		//light away; the emulsion side takes dye off, magenta first.
		vec3 baseLoss = vec3( 1.0 );
		float x0 = pix.x - 0.5;
		for( int j = 0; j < ScratchCount; ++j )
		{
			vec4 s0 = texelFetch( PrintData, ivec2( kScratchFirst + 2 * j, 0 ), 0 );
			vec4 s1 = texelFetch( PrintData, ivec2( kScratchFirst + 2 * j + 1, 0 ), 0 );
			if( ( ( int( s1.x ) >> i ) & 1 ) == 0 )
				continue;
			float l = s0.x * OutSize.x;
			float r = s0.y * OutSize.x;
			if( ( Perturb & 2 ) != 0 )
			{
				l += off.x * H;
				r += off.x * H;
			}
			float cover = clamp( min( r, x0 + 1.0 ) - max( l, x0 ), 0.0, 1.0 );
			if( cover <= 0.0 )
				continue;
			if( s0.w < 0.5 )
				baseLoss *= 1.0 - cover * s0.z;
			else
			{
				float d = 3.0 * s0.z;
				D *= 1.0 - cover * clamp( vec3( d - 1.0, d, d - 2.0 ), 0.0, 1.0 );
			}
		}

		vec3 T = exp2( -D ) * baseLoss;

		if( own )
		{
			//Dust on this frame of the print.
			for( int j = 0; j < ParticleCount; ++j )
			{
				vec4 p0 = texelFetch( PrintData, ivec2( 2 * j, 0 ), 0 );
				vec4 p1 = texelFetch( PrintData, ivec2( 2 * j + 1, 0 ), 0 );
				int code = int( p1.w );
				if( ( code & 7 ) != i )
					continue;
				vec2 d = film - p0.xy;
				if( abs( d.x ) > p0.z + 3.0 * pxh || abs( d.y ) > p0.z + 3.0 * pxh )
					continue;
				vec2 q = vec2( p1.x * d.x + p1.y * d.y, -p1.y * d.x + p1.x * d.y );
				float cover;
				if( code >= 8 )
				{
					float t = max( p0.w, 0.5 * pxh );
					float along = clamp( q.x, -p0.z, p0.z );
					float dist = length( vec2( q.x - along, q.y ) );
					cover = clamp( ( t - dist ) / pxh + 0.5, 0.0, 1.0 ) * ( p0.w / t );
				}
				else
				{
					vec2 rr = max( p0.zw, vec2( 0.5 * pxh ) );
					float dist = ( length( q / rr ) - 1.0 ) * min( rr.x, rr.y );
					cover = clamp( 0.5 - dist / pxh, 0.0, 1.0 ) * ( p0.z * p0.w ) / ( rr.x * rr.y );
				}
				if( p1.z > 0.0 )
					T *= 1.0 - cover * p1.z;
				else
					T = mix( T, vec3( 1.0 ), cover * -p1.z );
			}

			//A cue dot, scribed through the emulsion: clear, with a dark rim.
			if( ( SegFlags[ i ] & 2 ) != 0 )
			{
				float r = length( film - CueCentre );
				float disc = clamp( ( CueRadius - r ) / pxh + 0.5, 0.0, 1.0 );
				float rim = clamp( ( 0.12 * CueRadius - abs( r - CueRadius ) ) / pxh + 0.5, 0.0, 1.0 );
				T = mix( T, vec3( 0.92 ), 0.85 * disc );
				T *= 1.0 - 0.6 * rim;
			}
		}

		if( ( ( HairMask >> i ) & 1 ) != 0 )
			T *= 1.0 - HairOpacity * hairCover;

		light += SegWeight[ i ] * T;
	}

	//The lamp's colour, and the lens: cos^4 of the field angle.
	vec2 fromCentre = hpos - vec2( 0.5 * Aspect, 0.5 );
	float t = length( fromCentre ) / length( vec2( 0.5 * Aspect, 0.5 ) ) * VignetteTan;
	float falloff = 1.0 / ( ( 1.0 + t * t ) * ( 1.0 + t * t ) );
	vec3 lin = light * Lamp * falloff;

	if( Probe == 1 )
	{
		fragColor = vec4( lin, 1.0 );
		return;
	}

	vec4 src = texture( Source, uv * MaxUV );
	fragColor = vec4( mix( src.rgb, encodeSrgb( lin ), MixAmount ), mix( src.a, 1.0, MixAmount ) );
}
)";

std::string assemble( const char* body )
{
	return std::string( kVersion ) + body;
}
} // namespace

std::string Vertex()
{
	return assemble( kVertexBody );
}

std::string Capture()
{
	return assemble( kCaptureBody );
}

std::string Resample()
{
	return assemble( kResampleBody );
}

std::string Output()
{
	return assemble( kOutputBody );
}

} // namespace gate::shaders
