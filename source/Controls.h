#pragma once

/**
	Host parameters are 0..1; these are what they mean.

	`SetParamInfo` clamps a STANDARD default into 0..1 *before* returning, and
	`SetParamRange` can only be called afterwards, so every slider here is a
	plain 0..1 float and the conversions live in this one file, which the
	plugin and the harness both use. Options are mapped by INDEX: an option
	parameter's range reads back 0..1 from the SDK whatever its element count.

	Positions a check needs to land on exactly do: Shutter Angle 0.5 is
	exactly 180 degrees, Framing 0.5 is exactly centred.
*/
namespace gate::controls
{

constexpr int kFpsCount = 4;
const char* FpsName( int index );
double Fps( int index );

constexpr int kBladesCount = 3;
const char* BladesName( int index );
int Blades( int index );

/// Shutter Angle: each blade cycle's opening, 45 to 315 degrees. 0.5 is 180.
double ShutterDegrees( float value );
float ShutterParam( double degrees );
/// The open fraction of a blade cycle.
double ShutterOpen( float value );

constexpr int kLampCount = 3;
const char* LampName( int index );
double LampKelvin( int index );

/// Framing: -0.5 to +0.5 picture heights. 0.5 is 0.
double FramingHeights( float value );
float FramingParam( double heights );

/// Splices: 0 to 60 a minute.
double SplicesPerMinute( float value );

/// Vignette: the lens's half-angle to the corner, 0 to 35 degrees; the
/// light there is cos^4 of it.
double VignetteDegrees( float value );

/// Weave, Shrinkage, Hair, Scratches, Dust, Age and Mix are 0..1 amounts.
float Amount( float value );

int OptionIndex( float value, int count );

} // namespace gate::controls
