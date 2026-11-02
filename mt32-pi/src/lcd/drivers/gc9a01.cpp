//
// gc9a01.cpp
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

#include <circle/logger.h>
#include <circle/machineinfo.h>
#include <circle/timer.h>
#include <circle/util.h>

#include "lcd/drivers/gc9a01.h"

LOGMODULE("gc9a01");

enum TGC9A01Command : u8
{
	SleepOut         = 0x11,
	DisplayInversion = 0x21,
	DisplayOff       = 0x28,
	DisplayOn        = 0x29,
	ColumnAddressSet = 0x2A,
	RowAddressSet    = 0x2B,
	MemoryWrite      = 0x2C,
	TearingEffectOn  = 0x35,
	MemoryAccessCtrl = 0x36,
	PixelFormat      = 0x3A,
};

// Memory access control bits
constexpr u8 MADCTLRowOrder      = 0x80;
constexpr u8 MADCTLColumnOrder   = 0x40;
constexpr u8 MADCTLRowColExchange = 0x20;
constexpr u8 MADCTLBGR           = 0x08;

// Largest transfer of the SPI driver, rounded down to whole pixels
constexpr size_t MaxTransferSize = 0xFFFE;

// Vendor initialization sequence, as used by the manufacturer's and most other drivers: command, number of
// parameters, parameters
constexpr u8 InitSequence[] =
{
	0xEF, 0,
	0xEB, 1, 0x14,
	0xFE, 0,
	0xEF, 0,
	0xEB, 1, 0x14,
	0x84, 1, 0x40,
	0x85, 1, 0xFF,
	0x86, 1, 0xFF,
	0x87, 1, 0xFF,
	0x88, 1, 0x0A,
	0x89, 1, 0x21,
	0x8A, 1, 0x00,
	0x8B, 1, 0x80,
	0x8C, 1, 0x01,
	0x8D, 1, 0x01,
	0x8E, 1, 0xFF,
	0x8F, 1, 0xFF,
	0xB6, 2, 0x00, 0x00,
	PixelFormat, 1, 0x05,	// 16 bits per pixel
	0x90, 4, 0x08, 0x08, 0x08, 0x08,
	0xBD, 1, 0x06,
	0xBC, 1, 0x00,
	0xFF, 3, 0x60, 0x01, 0x04,
	0xC3, 1, 0x13,
	0xC4, 1, 0x13,
	0xC9, 1, 0x22,
	0xBE, 1, 0x11,
	0xE1, 2, 0x10, 0x0E,
	0xDF, 3, 0x21, 0x0C, 0x02,
	0xF0, 6, 0x45, 0x09, 0x08, 0x08, 0x26, 0x2A,
	0xF1, 6, 0x43, 0x70, 0x72, 0x36, 0x37, 0x6F,
	0xF2, 6, 0x45, 0x09, 0x08, 0x08, 0x26, 0x2A,
	0xF3, 6, 0x43, 0x70, 0x72, 0x36, 0x37, 0x6F,
	0xED, 2, 0x1B, 0x0B,
	0xAE, 1, 0x77,
	0xCD, 1, 0x63,
	0xE8, 1, 0x34,
	0x62, 12, 0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70,
	0x63, 12, 0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70,
	0x64, 7, 0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07,
	0x66, 10, 0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00,
	0x67, 10, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98,
	0x74, 7, 0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00,
	0x98, 2, 0x3E, 0x07,
	TearingEffectOn, 0,
	DisplayInversion, 0,
};

CGC9A01::CGC9A01(CSPIMaster* pSPIMaster, const TPins& Pins, unsigned nClockSpeed, TRotation Rotation, TMirror Mirror)
	: m_pSPIMaster(pSPIMaster),
	  m_nClockSpeed(nClockSpeed),
	  m_Rotation(Rotation),
	  m_Mirror(Mirror),
	  m_bBacklightEnabled(true),

	  m_DCPin(Pins.nDC, GPIOModeOutput),
	  m_ResetPin(Pins.nReset, GPIOModeOutput),
	  m_BacklightPin(Pins.nBacklight, GPIOModeOutput),

	  m_pDisplayed(new CCanvas::TColor[Size * Size]()),
	  m_pTransferBuffer(new CCanvas::TColor[Size * Size])
{
}

CGC9A01::~CGC9A01()
{
	delete[] m_pDisplayed;
	delete[] m_pTransferBuffer;
}

bool CGC9A01::Initialize()
{
	assert(m_pSPIMaster != nullptr);

	// The SPI clock divider must be even; the controller rounds odd ones down, which would make the clock faster
	// than requested, so round up to the next even divider instead
	const unsigned nCoreClockRate = CMachineInfo::Get()->GetClockRate(CLOCK_ID_CORE);
	unsigned nDivider = (nCoreClockRate + m_nClockSpeed - 1) / Utility::Max(m_nClockSpeed, 1u);
	nDivider = Utility::Max((nDivider + 1) & ~1u, 2u);
	const unsigned nClockSpeed = nCoreClockRate / nDivider;

	m_pSPIMaster->SetClock(nClockSpeed);
	m_pSPIMaster->SetMode(0, 0);

	LOGNOTE("SPI clock %u Hz (core clock %u Hz)", nClockSpeed, nCoreClockRate);

	// Hardware reset
	m_BacklightPin.Write(HIGH);
	m_DCPin.Write(HIGH);
	m_ResetPin.Write(HIGH);
	CTimer::SimpleMsDelay(10);
	m_ResetPin.Write(LOW);
	CTimer::SimpleMsDelay(10);
	m_ResetPin.Write(HIGH);
	CTimer::SimpleMsDelay(120);

	for (size_t i = 0; i < sizeof(InitSequence); i += 2 + InitSequence[i + 1])
		WriteCommand(InitSequence[i], &InitSequence[i + 2], InitSequence[i + 1]);

	// Orientation
	u8 nMADCTL;
	switch (m_Rotation)
	{
		case TRotation::Rotate90:	nMADCTL = MADCTLRowColExchange;					break;
		case TRotation::Inverted:	nMADCTL = MADCTLRowOrder;					break;
		case TRotation::Rotate270:	nMADCTL = MADCTLColumnOrder | MADCTLRowOrder | MADCTLRowColExchange;	break;
		default:			nMADCTL = MADCTLColumnOrder;					break;
	}
	if (m_Mirror == TMirror::Mirrored)
		nMADCTL ^= MADCTLColumnOrder;
	nMADCTL |= MADCTLBGR;
	WriteCommand(MemoryAccessCtrl, &nMADCTL, 1);

	WriteCommand(SleepOut);
	CTimer::SimpleMsDelay(120);

	// Start out black
	CCanvas Black;
	Update(Black, true);

	WriteCommand(DisplayOn);
	CTimer::SimpleMsDelay(20);

	return true;
}

void CGC9A01::Update(const CCanvas& Canvas, bool bForceFullUpdate)
{
	if (bForceFullUpdate)
	{
		SendRegion(Canvas, 0, 0, Size - 1, Size - 1);
		return;
	}

	const CCanvas::TColor* pPixels = Canvas.GetPixels();

	// Send runs of changed rows, each with the range of columns that changed in any of them
	int nRegionY1 = -1, nRegionX1 = 0, nRegionX2 = 0;

	for (int y = 0; y <= Size; ++y)
	{
		int nFirst = -1, nLast = -1;

		if (y < Size)
		{
			const CCanvas::TColor* pRow = pPixels + y * Size;
			const CCanvas::TColor* pDisplayedRow = m_pDisplayed + y * Size;

			for (int x = 0; x < Size; ++x)
			{
				if (pRow[x] != pDisplayedRow[x])
				{
					if (nFirst < 0)
						nFirst = x;
					nLast = x;
				}
			}
		}

		if (nFirst >= 0)
		{
			if (nRegionY1 < 0)
			{
				nRegionY1 = y;
				nRegionX1 = nFirst;
				nRegionX2 = nLast;
			}
			else
			{
				nRegionX1 = Utility::Min(nRegionX1, nFirst);
				nRegionX2 = Utility::Max(nRegionX2, nLast);
			}
		}
		else if (nRegionY1 >= 0)
		{
			SendRegion(Canvas, nRegionX1, nRegionY1, nRegionX2, y - 1);
			nRegionY1 = -1;
		}
	}
}

void CGC9A01::SendRegion(const CCanvas& Canvas, int nX1, int nY1, int nX2, int nY2)
{
	const CCanvas::TColor* pPixels = Canvas.GetPixels();
	const int nWidth = nX2 - nX1 + 1;
	CCanvas::TColor* pBuffer = m_pTransferBuffer;

	for (int y = nY1; y <= nY2; ++y)
	{
		const size_t nOffset = y * Size + nX1;
		memcpy(pBuffer, pPixels + nOffset, nWidth * sizeof(CCanvas::TColor));
		memcpy(m_pDisplayed + nOffset, pPixels + nOffset, nWidth * sizeof(CCanvas::TColor));
		pBuffer += nWidth;
	}

	SetWindow(nX1, nY1, nX2, nY2);
	WriteData(m_pTransferBuffer, (pBuffer - m_pTransferBuffer) * sizeof(CCanvas::TColor));
}

void CGC9A01::WriteCommand(u8 nCommand, const u8* pParameters, size_t nParameters)
{
	m_DCPin.Write(LOW);
	m_pSPIMaster->Write(ChipSelect, &nCommand, 1);
	m_DCPin.Write(HIGH);

	if (nParameters)
		WriteData(pParameters, nParameters);
}

void CGC9A01::WriteData(const void* pData, size_t nSize)
{
	const u8* pBytes = static_cast<const u8*>(pData);

	while (nSize)
	{
		const size_t nChunkSize = Utility::Min(nSize, MaxTransferSize);
		m_pSPIMaster->Write(ChipSelect, pBytes, nChunkSize);
		pBytes += nChunkSize;
		nSize -= nChunkSize;
	}
}

void CGC9A01::SetWindow(int nX1, int nY1, int nX2, int nY2)
{
	const u8 Columns[] = { 0, static_cast<u8>(nX1), 0, static_cast<u8>(nX2) };
	const u8 Rows[]    = { 0, static_cast<u8>(nY1), 0, static_cast<u8>(nY2) };

	WriteCommand(ColumnAddressSet, Columns, sizeof(Columns));
	WriteCommand(RowAddressSet, Rows, sizeof(Rows));
	WriteCommand(MemoryWrite);
}

void CGC9A01::SetBacklightState(bool bEnabled)
{
	m_bBacklightEnabled = bEnabled;

	m_BacklightPin.Write(bEnabled ? HIGH : LOW);
	WriteCommand(bEnabled ? DisplayOn : DisplayOff);
}
