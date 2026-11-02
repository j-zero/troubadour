//
// config.h
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
// Copyright (C) 2020-2023 Dale Whinham <daleyo@gmail.com>
//
// This file is part of mt32-pi.
//
// mt32-pi is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// mt32-pi is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along with
// mt32-pi. If not, see <http://www.gnu.org/licenses/>.
//

#ifndef _config_h
#define _config_h

#include <circle/net/ipaddress.h>
#include <circle/types.h>

#include "control/rotaryencoder.h"
#include "lcd/drivers/gc9a01.h"
#include "utility.h"

class CConfig
{
public:
	#define ENUM_AUDIOOUTPUTDEVICE(ENUM) \
		ENUM(PWM, pwm)                   \
		ENUM(HDMI, hdmi)                 \
		ENUM(I2S, i2s)

	#define ENUM_CONTROLSCHEME(ENUM)        \
		ENUM(None, none)                    \
		ENUM(SimpleButtons, simple_buttons) \
		ENUM(SimpleEncoder, simple_encoder)

	using TEncoderType             = CRotaryEncoder::TEncoderType;

	using TLCDRotation             = CGC9A01::TRotation;
	using TLCDMirror               = CGC9A01::TMirror;

	#define ENUM_LCDTYPE(ENUM)             \
		ENUM(None, none)                   \
		ENUM(GC9A01SPI, gc9a01_spi)

	#define ENUM_NETWORKMODE(ENUM) \
		ENUM(Off, off)             \
		ENUM(Ethernet, ethernet)   \
		ENUM(WiFi, wifi)

	CONFIG_ENUM(TAudioOutputDevice, ENUM_AUDIOOUTPUTDEVICE);
	CONFIG_ENUM(TControlScheme, ENUM_CONTROLSCHEME);
	CONFIG_ENUM(TLCDType, ENUM_LCDTYPE);
	CONFIG_ENUM(TNetworkMode, ENUM_NETWORKMODE);

	CConfig();
	bool Initialize(const char* pPath);

	static CConfig* Get() { return s_pThis; }

	// Expand all config variables from definition file
	#define CFG(_1, TYPE, MEMBER_NAME, _2, _3...) TYPE MEMBER_NAME;
	#include "config.def"

	// Overloaded function to parse config options based on their types specified in the definition file
	static bool ParseOption(const char* pString, bool* pOut);
	static bool ParseOption(const char* pString, int* pOut, bool bHex = false);
	static bool ParseOption(const char* pString, float* pOutFloat);
	static bool ParseOption(const char *pString, CString* pOut);
	static bool ParseOption(const char *pString, CIPAddress* pOut);
	static bool ParseOption(const char* pString, TAudioOutputDevice* pOut);
	static bool ParseOption(const char* pString, TLCDType* pOut);
	static bool ParseOption(const char* pString, TControlScheme* pOut);
	static bool ParseOption(const char* pString, TEncoderType* pOut);
	static bool ParseOption(const char* pString, TLCDRotation* pOut);
	static bool ParseOption(const char* pString, TLCDMirror* pOut);
	static bool ParseOption(const char* pString, TNetworkMode* pOut);

private:
	static int INIHandler(void* pUser, const char* pSection, const char* pName, const char* pValue);

	static CConfig* s_pThis;
};

#endif
