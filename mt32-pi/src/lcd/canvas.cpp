//
// canvas.cpp
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

#include <circle/util.h>

#include <cmath>

#include "lcd/canvas.h"
#include "utility.h"

constexpr char Ellipsis[] = "...";
constexpr float DegreesPerRadian = 57.2957795f;

static inline u16 Unswap(CCanvas::TColor Color)
{
	return Color >> 8 | Color << 8;
}

static inline const TUIGlyph& GetGlyph(const TUIFont& Font, char chChar)
{
	u8 nChar = static_cast<u8>(chChar);
	if (nChar < Font.nFirstChar || nChar > Font.nLastChar)
		nChar = '?';

	return Font.pGlyphs[nChar - Font.nFirstChar];
}

CCanvas::CCanvas()
	: m_pPixels(new TColor[Size * Size]()),
	  m_nClipX1(0),
	  m_nClipX2(Size - 1)
{
}

CCanvas::~CCanvas()
{
	delete[] m_pPixels;
}

void CCanvas::Clear(TColor Color)
{
	for (int i = 0; i < Size * Size; ++i)
		m_pPixels[i] = Color;
}

void CCanvas::FillRect(int nX, int nY, int nWidth, int nHeight, TColor Color)
{
	const int nX1 = Utility::Max(nX, 0);
	const int nY1 = Utility::Max(nY, 0);
	const int nX2 = Utility::Min(nX + nWidth, Size);
	const int nY2 = Utility::Min(nY + nHeight, Size);

	for (int y = nY1; y < nY2; ++y)
		for (int x = nX1; x < nX2; ++x)
			m_pPixels[y * Size + x] = Color;
}

void CCanvas::FillRoundedRect(int nX, int nY, int nWidth, int nHeight, int nRadius, TColor Color)
{
	nRadius = Utility::Min(nRadius, Utility::Min(nWidth, nHeight) / 2);

	for (int nRow = 0; nRow < nHeight; ++nRow)
	{
		// Inset of the rounded corners on this row
		int nInset = 0;
		const int nCornerRow = nRow < nRadius ? nRadius - nRow : nRow >= nHeight - nRadius ? nRow - (nHeight - nRadius) + 1 : 0;
		if (nCornerRow)
		{
			const float nDY = nCornerRow - 0.5f;
			nInset = nRadius - static_cast<int>(sqrtf(nRadius * nRadius - nDY * nDY) + 0.5f);
		}

		FillRect(nX + nInset, nY + nRow, nWidth - 2 * nInset, 1, Color);
	}
}

void CCanvas::FillArc(int nCenterX, int nCenterY, int nInnerRadius, int nOuterRadius, float nStartAngle, float nEndAngle, TColor Color)
{
	const int nInner2 = nInnerRadius * nInnerRadius;
	const int nOuter2 = nOuterRadius * nOuterRadius;

	for (int y = Utility::Max(nCenterY - nOuterRadius, 0); y < Utility::Min(nCenterY + nOuterRadius, Size); ++y)
	{
		for (int x = Utility::Max(nCenterX - nOuterRadius, 0); x < Utility::Min(nCenterX + nOuterRadius, Size); ++x)
		{
			// Distances from the center, doubled to stay on pixel centers
			const int nDX = 2 * (x - nCenterX) + 1;
			const int nDY = 2 * (y - nCenterY) + 1;
			const int nDistance2 = nDX * nDX + nDY * nDY;
			if (nDistance2 < 4 * nInner2 || nDistance2 >= 4 * nOuter2)
				continue;

			// Clockwise from the top
			float nAngle = atan2f(nDX, -nDY) * DegreesPerRadian;
			if (nAngle < 0.0f)
				nAngle += 360.0f;
			if (nAngle < nStartAngle)
				nAngle += 360.0f;

			if (nAngle <= nEndAngle)
				m_pPixels[y * Size + x] = Color;
		}
	}
}

void CCanvas::DrawImage(const TUIImage& Image, int nX, int nY, TColor Color)
{
	const int nBytesPerRow = (Image.nWidth + 1) / 2;

	for (int y = 0; y < Image.nHeight; ++y)
	{
		for (int x = 0; x < Image.nWidth; ++x)
		{
			const u8 nByte = Image.pData[y * nBytesPerRow + x / 2];
			BlendPixel(nX + x, nY + y, Color, x & 1 ? nByte & 0x0F : nByte >> 4);
		}
	}
}

void CCanvas::DrawBitmap(const u8* pBitmap, int nWidth, int nHeight, int nX, int nY, int nScale, int nDivisor, TColor Color)
{
	const int nBytesPerRow = (nWidth + 7) / 8;
	const int nScaledWidth = nWidth * nScale / nDivisor;
	const int nScaledHeight = nHeight * nScale / nDivisor;

	for (int y = 0; y < nScaledHeight; ++y)
	{
		const int nDestY = nY + y;
		if (nDestY < 0 || nDestY >= Size)
			continue;

		const u8* pRow = pBitmap + y * nDivisor / nScale * nBytesPerRow;

		for (int x = 0; x < nScaledWidth; ++x)
		{
			const int nDestX = nX + x;
			const int nSourceX = x * nDivisor / nScale;
			if (nDestX >= 0 && nDestX < Size && (pRow[nSourceX / 8] >> (7 - nSourceX % 8) & 1))
				m_pPixels[nDestY * Size + nDestX] = Color;
		}
	}
}

int CCanvas::GetTextWidth(const TUIFont& Font, const char* pText) const
{
	int nWidth = 0;
	while (*pText)
		nWidth += GetGlyph(Font, *pText++).nAdvance;

	return nWidth;
}

void CCanvas::DrawText(const TUIFont& Font, const char* pText, int nX, int nY, TColor Color, TAlign Align, int nMaxWidth)
{
	size_t nLength = strlen(pText);
	int nWidth = GetTextWidth(Font, pText);
	bool bShortened = false;

	// Shorten to fit, leaving room for an ellipsis
	if (nWidth > nMaxWidth)
	{
		const int nEllipsisWidth = GetTextWidth(Font, Ellipsis);
		while (nLength && nWidth + nEllipsisWidth > nMaxWidth)
			nWidth -= GetGlyph(Font, pText[--nLength]).nAdvance;

		// No trailing space before the ellipsis
		while (nLength && pText[nLength - 1] == ' ')
			nWidth -= GetGlyph(Font, pText[--nLength]).nAdvance;

		nWidth += nEllipsisWidth;
		bShortened = true;
	}

	if (Align == TAlign::Center)
		nX -= nWidth / 2;
	else if (Align == TAlign::Right)
		nX -= nWidth;

	nX = DrawString(Font, pText, nLength, nX, nY, Color);
	if (bShortened)
		DrawString(Font, Ellipsis, sizeof(Ellipsis) - 1, nX, nY, Color);
}

void CCanvas::DrawCenteredText(const TUIFont& Font, const char* pText, int nY, TColor Color, int nRadius)
{
	DrawText(Font, pText, Center, nY, Color, TAlign::Center, GetVisibleWidth(nY, nY + Font.nHeight, nRadius));
}

int CCanvas::GetVisibleWidth(int nY1, int nY2, int nRadius)
{
	// The row farthest from the center limits the width
	const float nDistance = Utility::Max(fabsf(nY1 - Center), fabsf(nY2 - Center));
	if (nDistance >= nRadius)
		return 0;

	return static_cast<int>(2.0f * sqrtf(nRadius * nRadius - nDistance * nDistance));
}

void CCanvas::BlendPixel(int nX, int nY, TColor Color, u8 nAlpha)
{
	if (nX < m_nClipX1 || nX > m_nClipX2 || nY < 0 || nY >= Size || !nAlpha)
		return;

	TColor& Pixel = m_pPixels[nY * Size + nX];
	if (nAlpha == 15)
	{
		Pixel = Color;
		return;
	}

	const u16 nSource = Unswap(Color);
	const u16 nDest = Unswap(Pixel);

	const u16 nRed   = ((nSource >> 11) * nAlpha + (nDest >> 11) * (15 - nAlpha)) / 15;
	const u16 nGreen = ((nSource >> 5 & 0x3F) * nAlpha + (nDest >> 5 & 0x3F) * (15 - nAlpha)) / 15;
	const u16 nBlue  = ((nSource & 0x1F) * nAlpha + (nDest & 0x1F) * (15 - nAlpha)) / 15;

	Pixel = Unswap(nRed << 11 | nGreen << 5 | nBlue);
}

int CCanvas::DrawString(const TUIFont& Font, const char* pText, size_t nLength, int nX, int nY, TColor Color)
{
	for (size_t i = 0; i < nLength; ++i)
	{
		const TUIGlyph& Glyph = GetGlyph(Font, pText[i]);
		const u8* pData = Font.pData + Glyph.nOffset;
		const int nBytesPerRow = (Glyph.nWidth + 1) / 2;

		for (int y = 0; y < Glyph.nHeight; ++y)
		{
			for (int x = 0; x < Glyph.nWidth; ++x)
			{
				const u8 nByte = pData[y * nBytesPerRow + x / 2];
				const u8 nAlpha = x & 1 ? nByte & 0x0F : nByte >> 4;
				BlendPixel(nX + Glyph.nOffsetX + x, nY + Glyph.nOffsetY + y, Color, nAlpha);
			}
		}

		nX += Glyph.nAdvance;
	}

	return nX;
}
