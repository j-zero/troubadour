//
// canvas.h
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

#ifndef _canvas_h
#define _canvas_h

#include <circle/types.h>

#include "lcd/fonts.h"
#include "lcd/logo.h"

// Colour framebuffer of the round 240x240 display
class CCanvas
{
public:
	static constexpr int Size   = 240;
	static constexpr int Center = Size / 2;

	// Text keeps clear of the edge, where gauges are drawn
	static constexpr int TextRadius = Center - 10;

	// RGB565, byte-swapped so that the framebuffer can be sent to the display as it is
	using TColor = u16;

	static constexpr TColor RGB(u8 nRed, u8 nGreen, u8 nBlue)
	{
		const u16 nValue = (nRed & 0xF8) << 8 | (nGreen & 0xFC) << 3 | nBlue >> 3;
		return static_cast<TColor>(nValue >> 8 | nValue << 8);
	}

	enum class TAlign
	{
		Left,
		Center,
		Right,
	};

	CCanvas();
	~CCanvas();

	CCanvas(const CCanvas&) = delete;
	CCanvas& operator=(const CCanvas&) = delete;

	const TColor* GetPixels() const { return m_pPixels; }

	void Clear(TColor Color);
	void FillRect(int nX, int nY, int nWidth, int nHeight, TColor Color);
	void FillRoundedRect(int nX, int nY, int nWidth, int nHeight, int nRadius, TColor Color);

	// Ring segment around the center (or the given point), clockwise from nStartAngle to nEndAngle (degrees; 0 = top)
	void FillArc(int nInnerRadius, int nOuterRadius, float nStartAngle, float nEndAngle, TColor Color)
	{
		FillArc(Center, Center, nInnerRadius, nOuterRadius, nStartAngle, nEndAngle, Color);
	}
	void FillArc(int nCenterX, int nCenterY, int nInnerRadius, int nOuterRadius, float nStartAngle, float nEndAngle, TColor Color);

	// Anti-aliased image, in the given colour
	void DrawImage(const TUIImage& Image, int nX, int nY, TColor Color);

	// 1 bit per pixel image (rows, most significant bit first), scaled by nScale/nDivisor
	void DrawBitmap(const u8* pBitmap, int nWidth, int nHeight, int nX, int nY, int nScale, int nDivisor, TColor Color);

	// Limit drawing to the columns from nX1 to nX2 (inclusive)
	void SetClip(int nX1, int nX2) { m_nClipX1 = nX1; m_nClipX2 = nX2; }
	void ResetClip() { SetClip(0, Size - 1); }

	// Text with its top at nY; text that is wider than nMaxWidth is shortened with "..."
	int GetTextWidth(const TUIFont& Font, const char* pText) const;
	void DrawText(const TUIFont& Font, const char* pText, int nX, int nY, TColor Color, TAlign Align = TAlign::Left, int nMaxWidth = Size);

	// Text centered horizontally that fits into the circle of the given radius on all of its rows
	void DrawCenteredText(const TUIFont& Font, const char* pText, int nY, TColor Color, int nRadius = TextRadius);

	// Width of a circle around the center that is available on all rows from nY1 to nY2
	static int GetVisibleWidth(int nY1, int nY2, int nRadius = TextRadius);

private:
	void BlendPixel(int nX, int nY, TColor Color, u8 nAlpha);
	int DrawString(const TUIFont& Font, const char* pText, size_t nLength, int nX, int nY, TColor Color);

	// On the heap, as it is too large for the stack the kernel object lives on
	TColor* m_pPixels;

	int m_nClipX1;
	int m_nClipX2;
};

#endif
