//
// gc9a01.h
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

#ifndef _gc9a01_h
#define _gc9a01_h

#include <circle/gpiopin.h>
#include <circle/spimaster.h>
#include <circle/types.h>

#include "lcd/canvas.h"
#include "utility.h"

// Round 240x240 SPI TFT
class CGC9A01
{
public:
	#define ENUM_LCDROTATION(ENUM) \
		ENUM(Normal, normal)       \
		ENUM(Rotate90, 90)         \
		ENUM(Inverted, inverted)   \
		ENUM(Rotate270, 270)

	#define ENUM_LCDMIRROR(ENUM) \
		ENUM(Normal, normal)     \
		ENUM(Mirrored, mirrored)

	CONFIG_ENUM(TRotation, ENUM_LCDROTATION);
	CONFIG_ENUM(TMirror, ENUM_LCDMIRROR);

	struct TPins
	{
		u8 nDC;
		u8 nReset;
		u8 nBacklight;
	};

	CGC9A01(CSPIMaster* pSPIMaster, const TPins& Pins, unsigned nClockSpeed, TRotation Rotation = TRotation::Normal, TMirror Mirror = TMirror::Normal);
	~CGC9A01();

	bool Initialize();

	// Sends the parts of the canvas that have changed since the last update
	void Update(const CCanvas& Canvas, bool bForceFullUpdate = false);

	bool GetBacklightState() const { return m_bBacklightEnabled; }
	void SetBacklightState(bool bEnabled);

private:
	static constexpr int Size = CCanvas::Size;
	static constexpr unsigned ChipSelect = 0;

	void WriteCommand(u8 nCommand, const u8* pParameters = nullptr, size_t nParameters = 0);
	void WriteData(const void* pData, size_t nSize);
	void SetWindow(int nX1, int nY1, int nX2, int nY2);
	void SendRegion(const CCanvas& Canvas, int nX1, int nY1, int nX2, int nY2);

	CSPIMaster* m_pSPIMaster;
	unsigned m_nClockSpeed;
	TRotation m_Rotation;
	TMirror m_Mirror;
	bool m_bBacklightEnabled;

	CGPIOPin m_DCPin;
	CGPIOPin m_ResetPin;
	CGPIOPin m_BacklightPin;

	// Contents of the display, and pixels of a region to send
	CCanvas::TColor* m_pDisplayed;
	CCanvas::TColor* m_pTransferBuffer;
};

#endif
