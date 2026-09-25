#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace gate::controls
{
namespace
{
float clamp01( float v )
{
	return std::clamp( v, 0.0f, 1.0f );
}
} // namespace

const char* FpsName( int index )
{
	static const char* const names[ kFpsCount ] = { "16", "18", "24", "25" };
	return names[ std::clamp( index, 0, kFpsCount - 1 ) ];
}

double Fps( int index )
{
	static const double rates[ kFpsCount ] = { 16.0, 18.0, 24.0, 25.0 };
	return rates[ std::clamp( index, 0, kFpsCount - 1 ) ];
}

const char* BladesName( int index )
{
	static const char* const names[ kBladesCount ] = { "1", "2", "3" };
	return names[ std::clamp( index, 0, kBladesCount - 1 ) ];
}

int Blades( int index )
{
	return std::clamp( index, 0, kBladesCount - 1 ) + 1;
}

double ShutterDegrees( float value )
{
	return 45.0 + 270.0 * static_cast< double >( clamp01( value ) );
}

float ShutterParam( double degrees )
{
	return clamp01( static_cast< float >( ( degrees - 45.0 ) / 270.0 ) );
}

double ShutterOpen( float value )
{
	return ShutterDegrees( value ) / 360.0;
}

const char* LampName( int index )
{
	static const char* const names[ kLampCount ] = { "Carbon Arc", "Xenon", "Tungsten" };
	return names[ std::clamp( index, 0, kLampCount - 1 ) ];
}

double LampKelvin( int index )
{
	static const double kelvin[ kLampCount ] = { 5000.0, 6200.0, 3200.0 };
	return kelvin[ std::clamp( index, 0, kLampCount - 1 ) ];
}

double FramingHeights( float value )
{
	return static_cast< double >( clamp01( value ) ) - 0.5;
}

float FramingParam( double heights )
{
	return clamp01( static_cast< float >( heights + 0.5 ) );
}

double SplicesPerMinute( float value )
{
	return 60.0 * static_cast< double >( clamp01( value ) );
}

double VignetteDegrees( float value )
{
	return 35.0 * static_cast< double >( clamp01( value ) );
}

float Amount( float value )
{
	return clamp01( value );
}

int OptionIndex( float value, int count )
{
	//Hosts send an option either as its element value (0, 1, 2 ...) or, some
	//of them, normalised; the element values here ARE the indices.
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

} // namespace gate::controls
