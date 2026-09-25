/**
 * Gate — browser demo.
 *
 * A film projector's gate, shutter and print. The one idea, from `AGENTS.md`:
 * model the machine, not the look. The projector pulls the film down one frame
 * at a time with a claw, holds it in the gate against the clearance of the claw,
 * and lets the lamp through only while a shutter blade is out of the way; the
 * print is a physical strip with grit, dust, a hair and splices in it, and dyes
 * that fade. So the clip is held at the projector's rate, the light the blades
 * pass is integrated over each display frame (which is where the flicker's
 * beat against the display comes from), each frame lands in the gate with its
 * own weave, and every mark on the picture belongs to the print or the gate.
 *
 * The two halves of this page are not equally faithful, and the split is worth
 * being exact about:
 *
 *   The SHADERS are the plugin's. The five GLSL strings below -- the version
 *   line, the vertex body, and the capture, resample and output bodies -- are
 *   `source/Shaders.cpp`'s, spliced in by `demo/tools/sync_shaders.py` with
 *   their tabs and comments, and assembled the way the plugin assembles them.
 *   So the hold (the clip sRGB-decoded into one of two RGBA16F held pictures,
 *   on a pull-down only), the gate (each frame's weave offset through a
 *   bilinear written out in float, the frame line and the neighbouring frames
 *   when framed off), the print (density, the dyes' retention, a splice's
 *   wash, emulsion and base scratches at a fixed x, dust, the cue dots, the
 *   hair), the shutter's weighted sum, the lamp, cos^4 and the sRGB encode all
 *   run on the GPU here as there. The numbers they are handed -- Shaders.h's
 *   print-data layout, every constant of Model.h, the option lists, rates and
 *   lamp temperatures of Controls -- are copied by the same script.
 *   `demo/tools/check_shaders.py` holds all of it to the C++ character for
 *   character, and `tools/verify.sh` runs it.
 *
 *   The CPU half is a PORT, by hand, and nothing checks it but a reader:
 *   Model.cpp (the PCG hash and its keys, the shutter's closed form
 *   `CumulativeOpen` and the per-projector-frame weights of `Segments`, the
 *   weave law and its AR(1) as a moving-average sum, the scratches' birth and
 *   death scan, the dust's Poisson births, the hair, the splices, the cue dots,
 *   the dyes' retention and the lamps' black bodies through Wyman, Sloan and
 *   Shirley's fit), Controls.cpp (every slider to its unit) and
 *   Gate::ProcessOpenGL (the clock, the film position in double, the hold's
 *   bookkeeping, the resize resample, the print-data upload and every uniform).
 *   In JavaScript doubles as the C++ keeps them in double, with Math.imul and
 *   >>> 0 for the hash's uint32 arithmetic, rounded to float (Math.fround)
 *   where the plugin holds or hands over a float. `gatest --flicker`, `--hold`,
 *   `--scratch`, `--weave`, `--fade` and `--resize` check the C++ originals and
 *   have no idea this page exists.
 *
 * ------------------------------------------------------------- the clock
 *
 * The plugin reads the host's clock and votes on its unit (readout's scheme).
 * Here the clock is the kit's: `time` in declared seconds, accumulated from the
 * browser's requestAnimationFrame deltas while playing (the kit caps one delta
 * at 0.1 s), +1/60 on Step. No vote runs. The plugin's rules then apply
 * unchanged: dt = now - last, clamped to [0, 0.25 s], a nominal 1/60 on the
 * first frame; the exposure is the whole display period; a frame on which the
 * clock did not move reuses the last exposure's length.
 *
 * So the browser's frame rate IS the display rate the shutter beats against,
 * exactly as Resolume's is in the plugin: 72 Hz of blade (3 blades at 24 fps)
 * against a 60 Hz display pulses at 12 Hz, against a 120 Hz one at 48 Hz, and a
 * browser that drops frames changes the beat. The line under the picture says
 * what this browser's rate is and what beat it gives.
 *
 * A paused page renders only when a control moves, and each such frame is worth
 * 0 s: no pull-down, the last exposure's length again, the same frame. Restart
 * sends the kit's clock back to 0; the plugin has no rule for a clock that runs
 * backwards, and its clamp makes that frame worth 0 s, so the strip carries on
 * from where it was. That is the plugin's behaviour, not a reset.
 *
 * ------------------------------------------------------------- what is missing
 *
 * Cue Dots is FF_TYPE_EVENT in the plugin; the kit has no event type, so it is
 * a toggle here that the renderer releases on the frame it acts, which is what
 * a host does with an event anyway (the plugin fires on the rising edge). The
 * About block is absent, as on every page in this suite. The harness-only
 * `Perturb` and `Probe` uniforms are set to what the shipped plugin sets them
 * to: 0.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';

//=== BEGIN GENERATED by demo/tools/sync_shaders.py from source/ -- do not edit by hand.

// source/Shaders.cpp, verbatim. `assemble` (below the block) is Shaders.cpp's.
const K_VERSION = "#version 410 core\n";

const VERTEX_BODY = `
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
`;

const CAPTURE_BODY = `
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
`;

const RESAMPLE_BODY = `
uniform sampler2D Old;

in vec2 uv;
out vec4 fragColor;

void main()
{
	fragColor = texture( Old, uv );
}
`;

const OUTPUT_BODY = `
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
`;

// source/Shaders.h: the print data's layout, one row of RGBA32F texels.
const SHADERS_H = {};
SHADERS_H.kPrintDataWidth = 256;
SHADERS_H.kPrintScratchFirst = 128;
SHADERS_H.kPrintHairFirst = 160;

// source/Model.h: the film, the shutter, the weave, the print, the dyes.
const MODEL_H = {};
MODEL_H.kPi = 3.14159265358979323846;
MODEL_H.kNominalFrame = 1.0 / 60.0;
MODEL_H.kMaxFrameDelta = 0.25;
MODEL_H.kMaxSegments = 8;
MODEL_H.kApertureHeightMm = 15.29;
MODEL_H.kPrintedHeightMm = 16.0;
MODEL_H.kFramePitchMm = 19.0;
MODEL_H.kMargin = 0.5 * ( MODEL_H.kPrintedHeightMm - MODEL_H.kApertureHeightMm ) / MODEL_H.kApertureHeightMm;
MODEL_H.kPitch = MODEL_H.kFramePitchMm / MODEL_H.kApertureHeightMm;
MODEL_H.kMaxDensity = Math.fround(3.2);
MODEL_H.kWeaveMm = 0.080;
MODEL_H.kWeaveVertical = 0.6;
MODEL_H.kWeaveRho = 0.75;
MODEL_H.kShrinkMaxPercent = 2.0;
MODEL_H.kShrinkGain = 1.0;
MODEL_H.kShrinkDecorrelate = 4.0;
MODEL_H.kScratchBirths = 6.0 / 96.0;
MODEL_H.kScratchLifeMin = 24;
MODEL_H.kScratchLifeMean = 96.0;
MODEL_H.kScratchLifeMax = 480;
MODEL_H.kMaxScratches = 16;
MODEL_H.kScratchHalfMmMin = 0.004;
MODEL_H.kScratchHalfMmMax = 0.020;
MODEL_H.kDustBirths = 8.0;
MODEL_H.kDustBirthsMax = 16;
MODEL_H.kMaxParticles = 64;
MODEL_H.kHairSlot = 96;
MODEL_H.kHairPoints = 13;
MODEL_H.kSpliceWash = Math.fround(0.35);
MODEL_H.kCueFrames = 4;
MODEL_H.kCueGap = 168;
MODEL_H.kFadeCyan = 2.0;
MODEL_H.kFadeMagenta = 0.15;
MODEL_H.kFadeYellow = 0.8;

// source/Controls.h and Controls.cpp: the option lists, the rates, the lamps.
const CONTROLS_H = {};
CONTROLS_H.kFpsCount = 4;
CONTROLS_H.kBladesCount = 3;
CONTROLS_H.kLampCount = 3;
const FPS_NAMES = ['16', '18', '24', '25'];
const BLADES_NAMES = ['1', '2', '3'];
const LAMP_NAMES = ['Carbon Arc', 'Xenon', 'Tungsten'];
const FPS_RATES = [16.0, 18.0, 24.0, 25.0];
const LAMP_KELVIN = [5000.0, 6200.0, 3200.0];

//=== END GENERATED

//---------------------------------------------------------------------------
// Shaders.cpp's `assemble`: the version line, then the body.
//---------------------------------------------------------------------------
const assemble = (body) => K_VERSION + body;
const VERTEX = assemble(VERTEX_BODY);
const CAPTURE = assemble(CAPTURE_BODY);
const RESAMPLE = assemble(RESAMPLE_BODY);
const OUTPUT = assemble(OUTPUT_BODY);

/// What the shipped plugin sets its test hooks to.
const PERTURB = 0;
const PROBE = 0;

/// Gate.h's print seed, and Gate.cpp's log2( 10 ).
const SEED = 0x6a7e5eed;
const LOG2_TEN = 3.32192809488736234787;

//===========================================================================
// The port. Controls.cpp, then Model.cpp, then Gate::ProcessOpenGL, by hand.
// JavaScript numbers are doubles, as the C++ model is; Math.fround stands in
// for every float the plugin stores or computes in.
//===========================================================================

const f = Math.fround;
const clampTo = (v, lo, hi) => Math.min(Math.max(v, lo), hi);
const clamp01f = (value) => clampTo(f(value), 0.0, 1.0);
/// std::lround: halves away from zero (Math.round sends -0.5 up).
const lround = (value) => (value < 0 ? -Math.round(-value) : Math.round(value));

//--- Controls.cpp ----------------------------------------------------------
const controls = {
  fps: (index) => FPS_RATES[clampTo(index, 0, CONTROLS_H.kFpsCount - 1)],
  blades: (index) => clampTo(index, 0, CONTROLS_H.kBladesCount - 1) + 1,
  shutterDegrees: (value) => 45.0 + 270.0 * clamp01f(value),
  shutterParam: (degrees) => clampTo(f((degrees - 45.0) / 270.0), 0.0, 1.0),
  shutterOpen: (value) => controls.shutterDegrees(value) / 360.0,
  lampKelvin: (index) => LAMP_KELVIN[clampTo(index, 0, CONTROLS_H.kLampCount - 1)],
  framingHeights: (value) => clamp01f(value) - 0.5,
  framingParam: (heights) => clampTo(f(heights + 0.5), 0.0, 1.0),
  splicesPerMinute: (value) => 60.0 * clamp01f(value),
  vignetteDegrees: (value) => 35.0 * clamp01f(value),
  amount: (value) => clamp01f(value),
  optionIndex: (value, count) => clampTo(lround(f(value)), 0, count - 1),
};

//--- Model.cpp: the hash ---------------------------------------------------
const M = MODEL_H;

/// Model.cpp's anonymous Stream enum, in its order.
const STREAM = {
  weaveX: 1, weaveY: 2, scratch: 3, dust: 4, hair: 5, hairTremble: 6, splice: 7,
};

/// PCG's output permutation, in uint32: Math.imul for the wrapping multiply,
/// >>> 0 to read the bits back as unsigned.
function hash(v) {
  const state = (Math.imul(v, 747796405) + 2891336453) >>> 0;
  const word = Math.imul(((state >>> ((state >>> 28) + 4)) ^ state) >>> 0, 277803737) >>> 0;
  return ((word >>> 22) ^ word) >>> 0;
}

/// Key( seed, stream, index, sub ): an int64 index is hashed as its low then
/// its high 32 bits, two's complement, as the C++'s uint64_t cast gives them.
function key(seed, stream, index, sub = 0) {
  const low = index >>> 0;
  const high = Math.floor(index / 4294967296) >>> 0;
  let h = hash((seed ^ hash((stream + 0x9e3779b9) >>> 0)) >>> 0);
  h = hash((h ^ low) >>> 0);
  h = hash((h ^ high) >>> 0);
  return hash((h ^ Math.imul(sub, 0x85ebca6b)) >>> 0);
}

const uniform = (k) => (k + 0.5) / 4294967296.0;
const lerp = (a, b, t) => a + (b - a) * t;

function normal(seed, stream, index) {
  const u1 = uniform(key(seed, stream, index, 0));
  const u2 = uniform(key(seed, stream, index, 1));
  return Math.sqrt(-2.0 * Math.log(u1)) * Math.cos(2.0 * M.kPi * u2);
}

//--- Model.cpp: the shutter ------------------------------------------------
function shutterOpenAt(p, blades, open) {
  const c = p * blades;
  const r = c - Math.floor(c);
  return (r >= 0.5 * (1.0 - open) && r < 0.5 * (1.0 + open)) ? 1.0 : 0.0;
}

function cumulativeOpen(p, blades, open) {
  const c = p * blades;
  const n = Math.floor(c);
  const r = c - n;
  return (n * open + clampTo(r - 0.5 * (1.0 - open), 0.0, open)) / blades;
}

/// The projector frames the exposure [p0, p1] overlaps, oldest first, each
/// with the light the shutter passed during the overlap over open x (p1 - p0).
function segments(p0, p1, blades, open, instant) {
  const out = [];
  if (instant || !(p1 > p0)) {
    out.push({ frame: Math.floor(p1), weight: shutterOpenAt(p1, blades, open) / open });
    return out;
  }
  const base = Math.floor(p0);
  const q0 = p0 - base;
  const q1 = p1 - base;
  const last = Math.trunc(Math.ceil(q1)) - 1;
  const scale = 1.0 / (open * (q1 - q0));
  for (let j = 0; j <= last; j += 1) {
    const a = Math.max(q0, j);
    const b = Math.min(q1, j + 1);
    const w = (cumulativeOpen(b, blades, open) - cumulativeOpen(a, blades, open)) * scale;
    const k = base + j;
    if (out.length < M.kMaxSegments) out.push({ frame: k, weight: w });
    else {
      // Cannot happen inside kMaxFrameDelta at 25 fps; if it did, the oldest
      // keeps the light rather than the light being lost.
      out.shift();
      out[0].weight += w;
      out.push({ frame: k, weight: w });
    }
  }
  return out;
}

//--- Model.cpp: the weave --------------------------------------------------
function weaveLaw(weave, shrinkage, perturb) {
  const percent = M.kShrinkMaxPercent * clampTo(shrinkage, 0.0, 1.0);
  const gain = (perturb & (1 << 4)) ? 1.0 : 1.0 + M.kShrinkGain * percent;
  const sigmaX = M.kWeaveMm * clampTo(weave, 0.0, 1.0) * gain / M.kApertureHeightMm;
  return {
    sigmaX,
    sigmaY: M.kWeaveVertical * sigmaX,
    rho: (perturb & (1 << 3)) ? 0.0 : M.kWeaveRho * (1.0 - percent / M.kShrinkDecorrelate),
  };
}

function weaveOffset(seed, frame, law) {
  let x = 0.0;
  let y = 0.0;
  if (law.sigmaX <= 0.0) return [0.0, 0.0];
  const terms = law.rho > 0.0 ? Math.trunc(Math.ceil(Math.log(1e-9) / Math.log(law.rho))) : 0;
  let weight = 1.0;
  for (let j = 0; j <= terms; j += 1) {
    x += weight * normal(seed, STREAM.weaveX, frame - j);
    y += weight * normal(seed, STREAM.weaveY, frame - j);
    weight *= law.rho;
  }
  const innovation = Math.sqrt(1.0 - law.rho * law.rho);
  return [x * (innovation * law.sigmaX), y * (innovation * law.sigmaY)];
}

//--- Model.cpp: scratches --------------------------------------------------
const BASE = 0;
const EMULSION = 1;

function scratchesAlive(seed, first, last, amount, aspect) {
  const list = [];
  const p = clampTo(amount, 0.0, 1.0) * M.kScratchBirths;
  if (p <= 0.0) return list;
  // Newest first, so the cap drops the oldest grit.
  for (let b = last; b > first - M.kScratchLifeMax && list.length < M.kMaxScratches; b -= 1) {
    if (uniform(key(seed, STREAM.scratch, b)) >= p) continue;
    const life = Math.min(M.kScratchLifeMax,
      M.kScratchLifeMin - M.kScratchLifeMean * Math.log(uniform(key(seed, STREAM.scratch, b, 1))));
    const s = { born: b, dies: b + Math.trunc(life) };
    if (s.dies <= first) continue;
    const centre = lerp(0.03, 0.97, uniform(key(seed, STREAM.scratch, b, 2)));
    const halfMm = lerp(M.kScratchHalfMmMin, M.kScratchHalfMmMax, uniform(key(seed, STREAM.scratch, b, 3)));
    const half = halfMm / M.kApertureHeightMm / aspect;
    s.left = centre - half;
    s.right = centre + half;
    const u = uniform(key(seed, STREAM.scratch, b, 5));
    s.side = uniform(key(seed, STREAM.scratch, b, 4)) < 0.55 ? BASE : EMULSION;
    s.strength = s.side === BASE ? lerp(0.25, 0.65, u) : lerp(0.15, 1.0, u);
    list.push(s);
  }
  return list;
}

//--- Model.cpp: dust -------------------------------------------------------
function dustOn(seed, frame, amount, aspect) {
  const list = [];
  const mean = M.kDustBirths * clampTo(amount, 0.0, 1.0);
  if (mean <= 0.0) return list;
  for (let b = frame - 3; b <= frame; b += 1) {
    // Poisson by its inverse CDF, one uniform per frame.
    const u = uniform(key(seed, STREAM.dust, b));
    let n = 0;
    let term = Math.exp(-mean);
    let cdf = term;
    while (u > cdf && n < M.kDustBirthsMax) {
      n += 1;
      term *= mean / n;
      cdf += term;
    }
    for (let j = 0; j < n; j += 1) {
      const draw = (field) => uniform(key(seed, STREAM.dust, b, (1 + j * 16 + field) >>> 0));
      const l = draw(0);
      const life = l < 0.70 ? 1 : l < 0.85 ? 2 : l < 0.95 ? 3 : 4;
      const age = frame - b;
      if (age >= life) continue;
      const p = {};
      p.x = draw(1) * aspect + (draw(2) - 0.5) * 0.008 * age;
      p.y = draw(3) + (draw(4) - 0.5) * 0.008 * age;
      p.angle = draw(5) * M.kPi;
      p.fibre = draw(6) < 0.25 ? 1 : 0;
      if (p.fibre) {
        p.a = lerp(0.08, 0.45, draw(7)) / M.kApertureHeightMm;
        p.b = lerp(0.008, 0.016, draw(8)) / M.kApertureHeightMm;
      } else {
        const r = Math.min(0.15, 0.015 * Math.pow(draw(7), -1.0 / 2.0)) / M.kApertureHeightMm;
        p.a = r;
        p.b = r * lerp(0.5, 1.0, draw(8));
      }
      const s = draw(10);
      p.strength = draw(9) < 0.85 ? lerp(0.7, 1.0, s) : -lerp(0.6, 1.0, s);
      if (list.length < M.kMaxParticles) list.push(p);
    }
  }
  return list;
}

//--- Model.cpp: the hair ---------------------------------------------------
function hairAt(seed, frame, amount, aspect) {
  const hair = { present: false, x: [], y: [], halfWidth: 0.0, opacity: 0.0 };
  if (amount <= 0.0) return hair;
  // C++ integer division truncates; the negative branch makes it a floor.
  const slot = frame >= 0 ? Math.trunc(frame / M.kHairSlot) : Math.trunc((frame - M.kHairSlot + 1) / M.kHairSlot);
  const draw = (field) => uniform(key(seed, STREAM.hair, slot, field));
  if (draw(0) >= amount) return hair;
  const start = Math.trunc(draw(1) * 48.0);
  const end = Math.min(M.kHairSlot - 1, start + 36 + Math.trunc(draw(2) * 57.0));
  const t = frame - slot * M.kHairSlot;
  if (t < start || t >= end) return hair;
  hair.present = true;

  // Where it hangs: an edge of the aperture, a place along it, a direction
  // in, a length, a curl.
  const e = draw(3);
  const along = lerp(0.1, 0.9, draw(4));
  let ax; let ay; let dx; let dy;
  if (e < 0.40) { ax = along * aspect; ay = 1.0; dx = 0.0; dy = -1.0; } // bottom
  else if (e < 0.65) { ax = along * aspect; ay = 0.0; dx = 0.0; dy = 1.0; } // top
  else if (e < 0.85) { ax = 0.0; ay = along; dx = 1.0; dy = 0.0; } // left
  else { ax = aspect; ay = along; dx = -1.0; dy = 0.0; } // right
  const tilt = (draw(5) - 0.5) * 1.8;
  const c = Math.cos(tilt);
  const s = Math.sin(tilt);
  const ux = c * dx - s * dy;
  const uy = s * dx + c * dy; // in
  const vx = -uy;
  const vy = ux; // across
  const length = lerp(0.06, 0.18, draw(6));
  const curl = (draw(7) - 0.5) * 0.8;
  const waves = lerp(0.6, 2.2, draw(8));

  // It shakes loose: in its last four frames it slides back out of the gate.
  const left = end - t;
  const out = left <= 4 ? length * (1.0 - left / 5.0) : 0.0;
  const tremble = 0.0012 * (left <= 4 ? 3.0 : 1.0);
  for (let i = 0; i < M.kHairPoints; i += 1) {
    const u = i / (M.kHairPoints - 1);
    const bend = curl * length * Math.sin(M.kPi * u * waves) * u;
    // The pull-down shakes it every frame, the tip most.
    const jitter = tremble * u * normal(seed, STREAM.hairTremble, frame * M.kHairPoints + Math.trunc(i / 4));
    const along2 = u * length - 0.01 - out;
    hair.x.push(ax + ux * along2 + vx * (bend + jitter));
    hair.y.push(ay + uy * along2 + vy * (bend + jitter));
  }
  hair.halfWidth = lerp(0.020, 0.035, draw(9)) / M.kApertureHeightMm;
  hair.opacity = 0.92;
  return hair;
}

//--- Model.cpp: splices and cues -------------------------------------------
function isSplice(seed, frame, perMinute, fps) {
  if (perMinute <= 0.0 || fps <= 0.0) return false;
  return uniform(key(seed, STREAM.splice, frame)) < perMinute / (60.0 * fps);
}

function spliceJump(seed, frame) {
  const size = lerp(0.04, 0.12, uniform(key(seed, STREAM.splice, frame, 1)));
  const y = uniform(key(seed, STREAM.splice, frame, 2)) < 0.5 ? -size : size;
  const x = (uniform(key(seed, STREAM.splice, frame, 3)) - 0.5) * 0.02;
  return [x, y];
}

function isCue(cueStart, frame) {
  if (cueStart < 0) return false;
  const d = frame - cueStart;
  return (d >= 0 && d < M.kCueFrames) || (d >= M.kCueGap && d < M.kCueGap + M.kCueFrames);
}

//--- Model.cpp: dyes and lamps ---------------------------------------------
function retention(age, perturb) {
  let k = [M.kFadeCyan, M.kFadeMagenta, M.kFadeYellow];
  if (perturb & (1 << 5)) {
    const mean = (M.kFadeCyan + M.kFadeMagenta + M.kFadeYellow) / 3.0;
    k = [mean, mean, mean];
  }
  return k.map((ki) => Math.exp(-ki * Math.max(age, 0.0)));
}

function lobe(lambda, mu, s1, s2) {
  const t = (lambda - mu) / (lambda < mu ? s1 : s2);
  return Math.exp(-0.5 * t * t);
}

function blackBodyRgb(kelvin) {
  let X = 0.0;
  let Y = 0.0;
  let Z = 0.0;
  for (let nm = 380; nm <= 780; nm += 1) {
    const l = nm;
    const m = l * 1e-9;
    const B = 1.0 / (Math.pow(m, 5.0) * (Math.exp(1.438776877e-2 / (m * kelvin)) - 1.0));
    X += B * (1.056 * lobe(l, 599.8, 37.9, 31.0) + 0.362 * lobe(l, 442.0, 16.0, 26.7) - 0.065 * lobe(l, 501.1, 20.4, 26.2));
    Y += B * (0.821 * lobe(l, 568.8, 46.9, 40.5) + 0.286 * lobe(l, 530.9, 16.3, 31.1));
    Z += B * (1.217 * lobe(l, 437.0, 11.8, 36.0) + 0.681 * lobe(l, 459.0, 26.0, 13.8));
  }
  return [
    3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z,
    -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z,
    0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z,
  ];
}

function lampRgb(kelvin) {
  const white = blackBodyRgb(6504.0);
  const lamp = blackBodyRgb(kelvin);
  const rgb = lamp.map((v, i) => Math.max(0.0, v / white[i]));
  const Y = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
  return rgb.map((v) => v / Y);
}

//===========================================================================
// The renderer: Gate's members and Gate::ProcessOpenGL.
//===========================================================================

/// What the line under the picture reports, and what a driven check reads.
const telemetry = {
  ticked: false,
  dt: 0,
  p0: 0,
  p1: 0,
  segments: [],
  captured: false,
  fps: 24,
  blades: 3,
  degrees: 270,
  displayRate: 0,
  captures: 0,
};

/// For a driven check (AGENTS.md, "The browser demo"). `fresh()` puts the
/// renderer back to a newly instantiated plugin; `afterRender(gl, input)` runs
/// inside the frame, before the canvas is composited, so it can read it back.
const hooks = { afterRender: null, fresh: null };

function createRenderer(gl, quad) {
  const program = (fragment, label) => new Program(gl, VERTEX, fragment, label);
  const captureShader = program(CAPTURE, 'capture');
  const resampleShader = program(RESAMPLE, 'resample');
  const outputShader = program(OUTPUT, 'output');

  // The print's particles, scratches and hair: one row, texelFetch only.
  const printTexture = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, printTexture);
  gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA32F, SHADERS_H.kPrintDataWidth, 1);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.bindTexture(gl.TEXTURE_2D, null);

  // The two held pictures, RGBA16F with linear sampling, as PassBuffer::Ensure(
  // w, h, GL_RGBA16F, Sampling::Linear ); and a resize's spare.
  const linear = { filter: 'linear' };
  let held = [new PassBuffer(gl, linear), new PassBuffer(gl, linear)];
  let spare = new PassBuffer(gl, linear);
  const isValid = (buffer) => buffer.texture !== null;

  //--- Gate's members --------------------------------------------------------
  let current = 0;
  let heldFrame = -1;
  let havePrevious = false;
  let lastWidth = 0;
  let lastHeight = 0;
  let running = false;
  let filmPosition = 0.0;
  let lastExposure = M.kNominalFrame;
  let seed = SEED;
  let cueStart = -1;
  let cueHeld = false;
  let lastNow = -1.0;

  const lampCache = new Map();
  const lampOf = (kelvin) => {
    if (!lampCache.has(kelvin)) lampCache.set(kelvin, lampRgb(kelvin));
    return lampCache.get(kelvin);
  };

  const unbind = (...units) => { for (const u of units) bindTexture(gl, u, null); };
  const location = (shader, name) => shader.location(`${name}[0]`) ?? shader.location(name);

  // The page's own measurement of the display rate: the median of the last
  // 60 frame deltas. Reported, never fed to the plugin's maths.
  const deltas = [];

  hooks.fresh = () => {
    for (const b of held) b.dispose();
    spare.dispose();
    current = 0;
    heldFrame = -1;
    havePrevious = false;
    lastWidth = 0;
    lastHeight = 0;
    running = false;
    filmPosition = 0.0;
    lastExposure = M.kNominalFrame;
    seed = SEED;
    cueStart = -1;
    cueHeld = false;
    lastNow = -1.0;
    telemetry.captures = 0;
  };

  return {
    render({ input, params, width: outWidth, height: outHeight, time }) {
      const width = input.width;
      const height = input.height;
      const aspect = outWidth / outHeight;
      const p = (id) => params.get(id);

      //------------------------------------------------------------------
      // What the controls say.
      //------------------------------------------------------------------
      const fps = controls.fps(controls.optionIndex(p('fps'), CONTROLS_H.kFpsCount));
      const blades = controls.blades(controls.optionIndex(p('blades'), CONTROLS_H.kBladesCount));
      const open = controls.shutterOpen(p('shutter'));
      const lamp = controls.optionIndex(p('lamp'), CONTROLS_H.kLampCount);
      const framing = controls.framingHeights(p('framing'));
      const law = weaveLaw(controls.amount(p('weave')), controls.amount(p('shrinkage')), PERTURB);
      const hair = controls.amount(p('hair'));
      const scratches = controls.amount(p('scratches'));
      const dust = controls.amount(p('dust'));
      const splices = controls.splicesPerMinute(p('splices'));
      const age = controls.amount(p('age'));
      const vignette = controls.vignetteDegrees(p('vignette'));
      const mixAmount = controls.amount(p('mix'));

      //------------------------------------------------------------------
      // The clock and the film, frame-relative, in double. The exposure is
      // the whole display period [ p0, p1 ]; a frame the clock did not move
      // reuses the last exposure's length.
      //------------------------------------------------------------------
      const now = time;
      let dt = M.kNominalFrame;
      if (lastNow >= 0.0) dt = clampTo(now - lastNow, 0.0, M.kMaxFrameDelta);
      lastNow = now;

      const exposure = dt > 0.0 ? dt : lastExposure;
      if (dt > 0.0) lastExposure = dt;
      const p1 = (running ? filmPosition : 0.0) + dt * fps;
      const p0 = p1 - exposure * fps;
      running = true;
      filmPosition = p1;

      const segs = segments(p0, p1, blades, open, (PERTURB & 1) !== 0);
      const count = segs.length;
      const latest = segs[count - 1].frame;

      // The cue marks are on the print from the frame in the gate when fired.
      // An event in the plugin; a toggle here, released below once it acts.
      const cuePressed = f(p('cueDots')) >= 0.5;
      if (cuePressed && !cueHeld) cueStart = latest;
      cueHeld = cuePressed;

      //------------------------------------------------------------------
      // Buffers. A raster change RESAMPLES the held pictures: the previous
      // projector frame is still on screen through the shutter.
      //------------------------------------------------------------------
      const rasterChanged = lastWidth !== 0 && (lastWidth !== width || lastHeight !== height);
      lastWidth = width;
      lastHeight = height;
      if (rasterChanged) {
        for (let i = 0; i < 2; i += 1) {
          const b = held[i];
          if (!isValid(b)) continue;
          spare.ensure(width, height, gl.RGBA16F);
          spare.bind();
          resampleShader.use();
          bindTexture(gl, 0, b.texture);
          resampleShader.setSampler('Old', 0);
          gl.disable(gl.BLEND);
          quad.draw();
          unbind(0);
          held[i] = spare;
          spare = b;
        }
        spare.dispose(); // the old raster's picture, now in `spare`
      }

      const noHold = (PERTURB & 4) !== 0;
      const newFrame = noHold || latest > heldFrame || !isValid(held[current]);
      let previousFrame = -1;
      if (newFrame) {
        if (isValid(held[current]) && heldFrame >= 0 && !noHold) {
          previousFrame = heldFrame;
          current = 1 - current;
          havePrevious = true;
        }
        held[current].ensure(width, height, gl.RGBA16F);
        heldFrame = latest;
      } else if (havePrevious) {
        previousFrame = heldFrame - 1; // any earlier frame is in held[ 1 - current ]
      }

      //------------------------------------------------------------------
      // The print, for every projector frame this display frame shows.
      //------------------------------------------------------------------
      const weights = new Float32Array(count);
      const offsets = new Float32Array(2 * count);
      const pictures = new Int32Array(count);
      const above = new Int32Array(count);
      const flags = new Int32Array(count);
      const data = new Float32Array(SHADERS_H.kPrintDataWidth * 4);
      let particleCount = 0;
      let hairFrom = -1;
      let hairMask = 0;
      let hairShape = { present: false };
      for (let i = count - 1; i >= 0; i -= 1) {
        const k = segs[i].frame;
        let [x, y] = weaveOffset(seed, k, law);
        if (isSplice(seed, k, splices, fps)) {
          const [jx, jy] = spliceJump(seed, k);
          x += jx;
          y += jy;
          flags[i] |= 1;
        }
        if (isCue(cueStart, k)) flags[i] |= 2;
        weights[i] = segs[i].weight;
        offsets[2 * i] = x;
        offsets[2 * i + 1] = y;
        const older = !noHold && havePrevious && k <= previousFrame;
        pictures[i] = older ? 1 - current : current;
        above[i] = havePrevious && !noHold ? 1 - current : current;

        for (const q of dustOn(seed, k, dust, aspect)) {
          if (particleCount >= M.kMaxParticles) break;
          const t0 = 8 * particleCount;
          data[t0 + 0] = q.x;
          data[t0 + 1] = q.y;
          data[t0 + 2] = q.a;
          data[t0 + 3] = q.b;
          data[t0 + 4] = Math.cos(q.angle);
          data[t0 + 5] = Math.sin(q.angle);
          data[t0 + 6] = q.strength;
          data[t0 + 7] = i + 8 * q.fibre;
          particleCount += 1;
        }

        const h = hairAt(seed, k, hair, aspect);
        if (h.present) {
          hairMask |= 1 << i;
          if (hairFrom < 0) {
            hairFrom = i;
            hairShape = h;
          }
        }
      }

      const alive = scratchesAlive(seed, segs[0].frame, latest, scratches, aspect);
      let scratchCount = 0;
      for (const s of alive) {
        if (scratchCount >= M.kMaxScratches) break;
        let mask = 0;
        for (let i = 0; i < count; i += 1) {
          if (segs[i].frame >= s.born && segs[i].frame < s.dies) mask |= 1 << i;
        }
        const t = 4 * (SHADERS_H.kPrintScratchFirst + 2 * scratchCount);
        data[t + 0] = s.left;
        data[t + 1] = s.right;
        data[t + 2] = s.strength;
        data[t + 3] = s.side;
        data[t + 4] = mask;
        scratchCount += 1;
      }

      if (hairShape.present) {
        for (let i = 0; i < M.kHairPoints; i += 1) {
          const t = 4 * (SHADERS_H.kPrintHairFirst + i);
          data[t + 0] = hairShape.x[i];
          data[t + 1] = hairShape.y[i];
        }
      }

      gl.bindTexture(gl.TEXTURE_2D, printTexture);
      gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
      gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, SHADERS_H.kPrintDataWidth, 1, gl.RGBA, gl.FLOAT, data);
      gl.bindTexture(gl.TEXTURE_2D, null);

      //------------------------------------------------------------------
      // 1. Capture, on a pull-down only: between pull-downs the gate holds.
      //------------------------------------------------------------------
      if (newFrame) {
        held[current].bind();
        captureShader.use();
        bindTexture(gl, 0, input.texture);
        captureShader.setSampler('Source', 0);
        gl.disable(gl.BLEND);
        quad.draw();
        unbind(0);
        telemetry.captures += 1;
      }

      //------------------------------------------------------------------
      // 2. The projection, straight to the host (here, the canvas).
      //------------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, outWidth, outHeight);

      const other = isValid(held[1 - current]) ? held[1 - current].texture : held[current].texture;
      outputShader.use();
      bindTexture(gl, 0, current === 0 ? held[0].texture : other);
      bindTexture(gl, 1, current === 1 ? held[1].texture : other);
      bindTexture(gl, 2, input.texture);
      bindTexture(gl, 3, printTexture);

      const lampColour = lampOf(controls.lampKelvin(lamp));
      const kept = retention(age, PERTURB);

      outputShader.setSampler('Held0', 0);
      outputShader.setSampler('Held1', 1);
      outputShader.setSampler('Source', 2);
      outputShader.setSampler('PrintData', 3);
      gl.uniform2i(outputShader.location('HeldSize'), width, height);
      // GetMaxGLTexCoords: the kit's input texture is exactly its picture.
      outputShader.set('MaxUV', 1.0, 1.0);
      outputShader.set('OutSize', outWidth, outHeight);
      outputShader.setInt('SegCount', count);
      gl.uniform1fv(location(outputShader, 'SegWeight'), weights);
      gl.uniform2fv(location(outputShader, 'SegOffset'), offsets);
      gl.uniform1iv(location(outputShader, 'SegPicture'), pictures);
      gl.uniform1iv(location(outputShader, 'SegAbove'), above);
      gl.uniform1iv(location(outputShader, 'SegFlags'), flags);
      outputShader.set('Framing', framing);
      outputShader.set('Pitch', M.kPitch);
      outputShader.set('Margin', M.kMargin);
      outputShader.set('MaxDensity', M.kMaxDensity * LOG2_TEN);
      outputShader.set('Retention', kept[0], kept[1], kept[2]);
      outputShader.set('SpliceWash', M.kSpliceWash);
      outputShader.setInt('ParticleCount', particleCount);
      outputShader.setInt('ScratchCount', scratchCount);
      outputShader.setInt('HairPoints', hairShape.present ? M.kHairPoints : 0);
      outputShader.setInt('HairMask', hairMask);
      outputShader.set('HairHalfWidth', hairShape.present ? hairShape.halfWidth : 0.0);
      outputShader.set('HairOpacity', hairShape.present ? hairShape.opacity : 0.0);
      outputShader.set('CueCentre', aspect - 0.12, 0.11);
      outputShader.set('CueRadius', 0.028);
      outputShader.set('Lamp', lampColour[0], lampColour[1], lampColour[2]);
      outputShader.set('VignetteTan', Math.tan(vignette * M.kPi / 180.0));
      outputShader.set('Aspect', aspect);
      outputShader.set('MixAmount', mixAmount);
      outputShader.setInt('Probe', PROBE);
      outputShader.setInt('Perturb', PERTURB);
      gl.disable(gl.BLEND);
      quad.draw();

      // The event, released on the frame it acted (a host's press is short).
      if (cuePressed) params.set('cueDots', 0);

      if (dt > 0.0) {
        deltas.push(dt);
        if (deltas.length > 60) deltas.shift();
      }
      const sorted = [...deltas].sort((a, b) => a - b);
      telemetry.displayRate = sorted.length ? 1.0 / sorted[Math.floor(sorted.length / 2)] : 0;
      telemetry.ticked = true;
      telemetry.dt = dt;
      telemetry.p0 = p0;
      telemetry.p1 = p1;
      telemetry.segments = segs;
      telemetry.captured = newFrame;
      telemetry.fps = fps;
      telemetry.blades = blades;
      telemetry.degrees = open * 360.0;
      telemetry.scratches = scratchCount;
      telemetry.particles = particleCount;
      telemetry.hair = hairShape.present;
      // A uniform name the shader does not have is a dead control (glUniform
      // on -1 is a no-op); the kit counts them. Empty is right.
      telemetry.missing = [...captureShader.missing, ...resampleShader.missing, ...outputShader.missing];

      if (hooks.afterRender) hooks.afterRender(gl, input);
      unbind(3, 2, 1, 0);
    },
  };
}

//===========================================================================
// The controls, read out of Gate::Gate(). Same names, same groups, same
// order, same defaults, same dropdown elements. Absent: the About block.
//===========================================================================

const std = (id, name, def, group, extra = {}) => ({ id, name, type: 'standard', default: f(def), group, ...extra });
const opt = (id, name, elements, def, group, hint) => ({ id, name, type: 'option', elements, default: def, group, hint });
const button = (id, name, group, hint) => ({ id, name, type: 'boolean', default: 0, group, hint });
const signed = (x, digits) => `${x >= 0 ? '+' : '−'}${Math.abs(x).toFixed(digits)}`;

const demo = mountDemo({
  name: 'Gate',
  pluginId: 'GA01',
  kind: 'effect',
  tagline:
    'A film projector’s gate, shutter and print. The clip is held at the projector’s rate and pulled down with the shutter closed; the light the blades let through is integrated over each display frame, so the shutter beats against the display; each frame lands in the gate with its own weave; grit in the gate cuts scratches that stay put while the picture moves under them; dust rides the print, a hair hangs in the gate until it shakes loose, splices jump and flash, cue dots mark the reel, and the dyes fade cyan first.',
  repo: 'https://github.com/stoatworks-labs/gate',

  // The stock sentence says "same maths", which is most of the truth: the
  // gate, the print and the shutter's sum run in the plugin's shaders, but the
  // projector and the print's marks are computed on the CPU, and that half is
  // a hand port.
  blurb:
    'It is Gate’s own GLSL — the capture, resample and output passes — ported from the repository to WebGL2, so the hold, the weave through the gate, the frame line, the dyes’ density and fading, the scratches, dust, hair and cue dots and the shutter’s weighted sum run on your GPU as they do in the plugin, over the same two RGBA16F held pictures. The CPU half — the shutter’s closed-form light per projector frame, the AR(1) weave, every scratch, speck, hair, splice and cue dot as a function of the film frame and the print’s seed, the dyes’ retention, the lamps’ black-body colours and every control’s law — is ported to JavaScript by hand, and nothing checks that port but a reader. The flicker beats against this browser’s own frame rate, so it depends on your display. It runs on generated clips in this page, with the plugin’s own parameters and no install.',

  // The held pictures are RGBA16F render targets, as in the plugin.
  needFloat: true,

  params: [
    opt('fps', 'FPS', FPS_NAMES, 2, 'Projector',
      'The projector’s rate, 16, 18, 24 or 25 frames a second. The clip changes only at pull-downs, this many times a second, whatever the clip’s own rate.'),
    opt('blades', 'Blades', BLADES_NAMES, 2, 'Projector',
      'Openings per frame. The shutter passes light Blades × FPS times a second, and that beats against the display: 2 blades at 24 fps against 60 Hz is a 12 Hz pulse of ±29%, 3 blades ±20%. The plugin’s default is 3.'),
    std('shutter', 'Shutter Angle', controls.shutterParam(270.0), 'Projector', {
      display: (v) => `${controls.shutterDegrees(v).toFixed(1)}° open`,
      hint: 'Each blade cycle’s opening, 45° to 315°, linear (0.5 is 180°). The closed stretch is centred on the pull-down, so the claw always moves in the dark. Narrower is a deeper flicker; the picture is normalised to its mean, so it is not darker, it pulses more. The plugin’s default is 270°.',
    }),
    opt('lamp', 'Lamp', LAMP_NAMES, 1, 'Projector',
      'The lamp as a black body against a 6504 K display white: Carbon Arc 5000 K, Xenon 6200 K, Tungsten 3200 K (warm). A carbon arc and a xenon lamp are not really black bodies.'),
    std('framing', 'Framing', 0.5, 'Projector', {
      display: (v) => `${signed(controls.framingHeights(v), 3)} h`,
      hint: 'The racking knob, −0.5 to +0.5 picture heights (h; 0.5 is centred). The printed picture is a little bigger than the aperture; past 2.3% of the height you see the frame line, and past that the neighbouring frame.',
    }),

    std('weave', 'Weave', 0.25, 'Gate', {
      display: (v) => `σ ${(M.kWeaveMm * controls.amount(v)).toFixed(3)} mm`,
      hint: 'The claw’s clearance: each frame lands with an AR(1) offset, correlation 0.75 frame to frame, 0.08 mm standard deviation at 1 on an unshrunk print (the readout; Shrinkage adds to it); vertical is 0.6 of horizontal.',
    }),
    std('shrinkage', 'Shrinkage', 0.25, 'Gate', {
      display: (v) => `${(M.kShrinkMaxPercent * controls.amount(v)).toFixed(2)}%`,
      hint: 'How much the print has shrunk, 0 to 2%. A shrunk print’s perforations no longer seat on the teeth: it weaves more, and each frame less like the last.',
    }),
    std('hair', 'Hair', 0.3, 'Gate', {
      hint: 'The chance, in each 96-frame slot, that a hair catches at an edge of the aperture. It stays fixed in the gate, trembles at each pull-down, and slides out after its dwell.',
    }),

    std('scratches', 'Scratches', 0.35, 'Print', {
      hint: 'How often grit arrives in the gate. A scratch is a straight line at a fixed x on screen while the picture weaves under it: base side dark, emulsion side light (green first, since magenta is the top layer).',
    }),
    std('dust', 'Dust', 0.3, 'Print', {
      hint: 'Specks and fibres on the print, which weave with the picture and stay one to four frames. Most are dark; some are printed-in white.',
    }),
    std('splices', 'Splices', 0.05, 'Print', {
      display: (v) => `${controls.splicesPerMinute(v).toFixed(1)} /min`,
      hint: 'Cement splices, 0 to 60 a minute at the running speed: a jump of 4–12% of the height and a flash on one frame.',
    }),
    button('cueDots', 'Cue Dots', 'Print',
      'The reel’s changeover marks, top right: four frames from the frame in the gate, then 168 frames on, four more. An event button in the plugin; here a toggle the renderer releases on the frame it acts, which is why it blinks.'),
    std('age', 'Age', 0.1, 'Print', {
      hint: 'The dyes fade at their own rates, cyan first and magenta last, so an old print goes magenta; the frame line and the blacks go red too. An invented law chosen to show the documented order, not fitted to a stock.',
    }),

    std('vignette', 'Vignette', 0.4, 'Output', {
      display: (v) => `${controls.vignetteDegrees(v).toFixed(1)}°`,
      hint: 'The lens’s half-angle to the corner, 0° to 35°; the light there is cos⁴ of it.',
    }),
    std('mix', 'Mix', 1.0, 'Output', {
      hint: 'The projection against the input. Alpha is mix(source alpha, 1, Mix): the projection paints the whole frame.',
    }),
  ],

  // A moving picture first: the hold, the judder and the weave need motion to
  // be seen. The geometry card makes the weave and the frame line readable.
  sources: ['scene', 'grid', 'bars', 'ramp', 'spot', 'alpha'],

  // The plugin ships no factory presets. These are the page's own, expressed
  // entirely in the plugin's parameters and reachable with the controls.
  presets: {
    'Classic 2 blades at 180° (strong 12 Hz flicker)': { blades: 1, shutter: controls.shutterParam(180.0) },
    'Silent era: 16 fps, 3 blades': { fps: 0 },
    'Clean print': { weave: 0.05, shrinkage: 0, hair: 0, scratches: 0, dust: 0, splices: 0, age: 0 },
    'Worn print': { weave: 0.6, shrinkage: 0.7, hair: 0.8, scratches: 0.9, dust: 0.8, splices: 0.3 },
    'Faded to magenta': { age: 0.8 },
    'Framed off (the frame line)': { framing: controls.framingParam(0.2) },
    'Tungsten lamp': { lamp: 2 },
  },

  differences: [
    'The gate and the print run in the plugin’s own shaders, and those are not a port. The capture, resample and output bodies are the plugin’s GLSL, assembled as Shaders.cpp assembles them; the numbers they are handed (the print-data layout, the film’s geometry, the density cap, the splice wash, the weave, scratch, dust, hair, cue and fade constants, the option lists, rates and lamp temperatures) are copied by a script. demo/tools/check_shaders.py fails the repository’s verify script if a character of any of it drifts.',
    'The CPU half is a PORT, not the plugin’s own code: Model.cpp’s PCG hash and its keys, the shutter’s closed form (CumulativeOpen) and the per-projector-frame weights (Segments), the weave law and its AR(1) moving-average sum, the scratches’ birth-and-death scan, the dust’s Poisson births, the hair, the splices and cue dots, the dyes’ retention and the lamps’ black bodies; Controls.cpp’s laws; and Gate::ProcessOpenGL’s clock, film position, hold bookkeeping, resize resample and print-data upload. All of it is ported by hand, in JavaScript doubles as the plugin keeps them, with the hash in uint32 through Math.imul, rounded to float where the plugin holds a float. Nothing checks a port but a reader; the repository’s gatest --flicker, --hold, --scratch, --weave, --fade and --resize check the C++ and have never heard of this page.',
    'The flicker depends on your display. The plugin integrates the shutter over each of the host’s frames, and here the host’s frame is the browser’s requestAnimationFrame: 72 Hz of blade (3 blades at 24 fps) beats at 12 Hz against a 60 Hz display, at 48 Hz against 120 Hz, and not at all against 72 or 144 Hz. A browser that drops frames changes the beat, as a host that drops frames would. The line under the picture gives this browser’s rate and the beat it makes.',
    'The weave, the scratches, the dust, the hair, the splices and the flicker’s law are the plugin’s stated model, not measurements of a projector: σ, ρ and the shrinkage law, the rates and sizes of the marks, and the three fade rates are chosen, and the look of the dust, the hair, the cue dots and the splice flash was judged by eye. The geometry (the pitch, the apertures, the cue timing) and the dye layer order are published figures.',
    'The held pictures are the plugin’s: two RGBA16F render targets, linear-sampled, resampled rather than cleared when the raster changes. WebGL2 renders into half-float textures only with EXT_color_buffer_float, and the page refuses to start without it rather than fall back to 8 bits, which would band the densities.',
    'The clock is the kit’s, in declared seconds; the plugin’s unit vote and wall-clock fallback never run. Everything downstream is the plugin’s rule: dt is the frame delta clamped to 0–0.25 s (the kit itself caps a delta at 0.1 s), a nominal 1/60 on the first frame, the exposure the whole display period. A paused page renders only when a control moves, and each such frame is worth 0 s: the same projector frame, the last exposure’s length again. Restart sends the clock back to 0, which the plugin’s clamp reads as a frame of 0 s, so the strip carries on where it was; Step adds exactly 1/60 s.',
    'Cue Dots is an FF_TYPE_EVENT button in the plugin. The kit has no event type, so it is a toggle here that the renderer releases on the frame it acts, which is what a host does with an event anyway, and why the button blinks.',
    'The plugin stores each host value as a float; the page’s sliders are doubles, so every value is rounded through Math.fround before its law is applied, and the defaults are the plugin’s float defaults (Shutter Angle 0.8333333, which is 270°).',
    'The print’s seed is the plugin’s fixed one, so this page shows the same print — the same scratches, specks, hair and splices at the same film frames — as every instance of the plugin. The film position starts at 0 when the page loads, as it does when the plugin is instantiated.',
    'The harness-only Perturb and Probe uniforms are set to what the shipped plugin sets them to, 0. The eight negative controls and the linear-light probe gatest reads through them are not on this page. The About block is absent, as on every page in this suite.',
  ],

  createRenderer,
});

// For a driven check (AGENTS.md, "The browser demo"): the kit's state and
// redraw, the telemetry, and the hooks, so a script can pause, set the clock
// to n / 60 and render one frame at a time, as `gatest --pipe --fps 60` does.
window.__gateDemo = { demo, telemetry, hooks, port: { hash, key, segments, weaveLaw, weaveOffset, scratchesAlive, dustOn, hairAt, lampRgb, retention } };

//---------------------------------------------------------------------------
// On the page: a photosensitivity note above the picture, and under it a line
// reporting the display rate and the beat it makes. Skipped in embed mode,
// where there is no reader.
//---------------------------------------------------------------------------
if (demo && !new URLSearchParams(window.location.search).has('embed')) {
  const stage = document.querySelector('.stage');
  if (stage) {
    const note = document.createElement('p');
    note.className = 'stage__status';
    note.setAttribute('role', 'note');
    note.textContent =
      'Photosensitivity: the shutter controls can make the whole picture flicker at about 12 Hz. '
      + 'The classic 2 blades at 180° pulse by about ±29% on a 60 Hz display; the plugin’s defaults, 3 blades at 270°, by a few per cent. '
      + 'Pause stops it.';
    stage.prepend(note);

    const line = document.createElement('p');
    line.className = 'stage__status';
    stage.append(line);
    setInterval(() => {
      if (!telemetry.ticked) return;
      const t = telemetry;
      const blade = t.blades * t.fps;
      const rate = t.displayRate;
      const beat = rate > 0 ? Math.abs(blade - Math.round(blade / rate) * rate) : 0;
      const shown = t.segments.map((s) => `${s.frame} ×${s.weight.toFixed(2)}`).join(', ');
      line.textContent =
        `Display: ${rate > 0 ? `${rate.toFixed(1)} Hz` : '—'} (this browser’s frame clock, which the plugin would take from the host). `
        + `Shutter: ${t.blades} blade${t.blades === 1 ? '' : 's'} × ${t.fps} fps = ${blade} Hz at ${t.degrees.toFixed(0)}°`
        + `${rate > 0 ? `, beating against this display at ${beat.toFixed(1)} Hz` : ''}. `
        + `This frame: film ${t.p0.toFixed(3)}–${t.p1.toFixed(3)}, projector frame${t.segments.length === 1 ? '' : 's'} ${shown}`
        + `${t.captured ? ' (a pull-down: the gate took a new picture)' : ''}; `
        + `${t.scratches} scratch${t.scratches === 1 ? '' : 'es'}, ${t.particles} speck${t.particles === 1 ? '' : 's'}${t.hair ? ', a hair' : ''}.`;
    }, 250);
  }
}
