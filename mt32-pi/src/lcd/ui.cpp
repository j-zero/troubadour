//
// ui.cpp
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

#include <circle/timer.h>
#include <circle/util.h>

#include <cstdio>

#include "lcd/drivers/gc9a01.h"
#include "lcd/ui.h"
#include "synth/soundfontsynth.h"
#include "utility.h"

using TColor = CCanvas::TColor;
using TAlign = CCanvas::TAlign;

// Colours
constexpr TColor ColorBackground   = CCanvas::RGB(0, 0, 0);
constexpr TColor ColorText         = CCanvas::RGB(255, 255, 255);
constexpr TColor ColorTextDim      = CCanvas::RGB(170, 170, 170);
constexpr TColor ColorTextFaint    = CCanvas::RGB(100, 100, 100);
constexpr TColor ColorAccent       = CCanvas::RGB(0, 170, 255);
constexpr TColor ColorAccentText   = CCanvas::RGB(110, 210, 255);
constexpr TColor ColorSelection    = CCanvas::RGB(0, 105, 200);
constexpr TColor ColorTrack        = CCanvas::RGB(40, 40, 48);
constexpr TColor ColorWarning      = CCanvas::RGB(255, 170, 0);
constexpr TColor ColorError        = CCanvas::RGB(255, 70, 60);
constexpr TColor ColorLevelLow     = CCanvas::RGB(40, 210, 100);
constexpr TColor ColorLevelMid     = CCanvas::RGB(250, 210, 40);
constexpr TColor ColorLevelHigh    = CCanvas::RGB(250, 70, 50);
constexpr TColor ColorPeak         = CCanvas::RGB(255, 255, 255);
constexpr TColor ColorPanic        = CCanvas::RGB(90, 0, 0);

// Gauges cover 270 degrees, open at the bottom
constexpr float GaugeStartAngle = 225.0f;
constexpr float GaugeSweep      = 270.0f;

constexpr unsigned SpinnerPeriodMicros = 1000000;

// Text that is too long scrolls after a pause
constexpr unsigned ScrollPauseMillis = 2000;
constexpr unsigned ScrollMillisPerPixel = 25;
constexpr int ScrollGap = 40;
constexpr float SpinnerArc = 70.0f;

constexpr u8 MIDIChannels = 16;

CUserInterface::CUserInterface()
	: m_pCanvas(new CCanvas),

	  m_State(TState::None),
	  m_nStateTime(0),
	  m_CurrentImage(TImage::None),
	  m_nVolume(100),

	  m_Lock(TASK_LEVEL),
	  m_SystemMessageTextBuffer{'\0'},
	  m_SystemMessageType(TMessageType::Notice),
	  m_SysExDisplayMessageType(TSysExDisplayMessage::Roland),
	  m_SysExTextBuffer{'\0'},
	  m_SysExPixelBuffer{0},
	  m_Menu{}
{
}

CUserInterface::~CUserInterface()
{
	delete m_pCanvas;
}

void CUserInterface::Update(CGC9A01& Display, CSoundFontSynth& Synth, unsigned int nTicks)
{
	const unsigned int nDeltaTicks = nTicks - m_nStateTime;
	const TState State = m_State;

	// Timeouts
	if ((State == TState::DisplayingMessage || State == TState::DisplayingImage) && nDeltaTicks >= Utility::MillisToTicks(SystemMessageDisplayTimeMillis))
		m_State = TState::None;
	else if ((State == TState::DisplayingSysExText || State == TState::DisplayingSysExBitmap) && nDeltaTicks >= Utility::MillisToTicks(SC55DisplayTimeMillis))
		m_State = TState::None;
	else if (State == TState::EnteringPowerSavingMode && nDeltaTicks >= Utility::MillisToTicks(SystemMessageDisplayTimeMillis))
	{
		Display.SetBacklightState(false);
		m_State = TState::InPowerSavingMode;
	}

	if (m_State == TState::InPowerSavingMode)
		return;

	if (!Display.GetBacklightState())
		Display.SetBacklightState(true);

	m_pCanvas->Clear(ColorBackground);

	char Text[SystemMessageTextBufferSize];
	TMessageType MessageType;

	switch (m_State)
	{
		case TState::DisplayingMessage:
		case TState::DisplayingSpinnerMessage:
		case TState::EnteringPowerSavingMode:
		{
			m_Lock.Acquire();
			memcpy(Text, m_SystemMessageTextBuffer, sizeof(Text));
			MessageType = m_SystemMessageType;
			m_Lock.Release();

			const TColor Color = MessageType == TMessageType::Error ? ColorError : MessageType == TMessageType::Warning ? ColorWarning : ColorText;
			DrawMessage(Text, Color, m_State == TState::DisplayingSpinnerMessage, nTicks);
			break;
		}

		case TState::DisplayingImage:
		{
			const u8* pImage = m_CurrentImage == TImage::MisterLogo ? MisterLogo128x32 : MT32PiLogo128x32;
			m_pCanvas->DrawBitmap(pImage, 128, 32, CCanvas::Center - 96, CCanvas::Center - 24, 3, 2, ColorText);
			break;
		}

		case TState::DisplayingSysExText:
			m_Lock.Acquire();
			memcpy(Text, m_SysExTextBuffer, sizeof(m_SysExTextBuffer));
			m_Lock.Release();
			DrawWrappedText(FontMedium, Text, CCanvas::Center, ColorAccentText, 3);
			break;

		case TState::DisplayingSysExBitmap:
			DrawSysExBitmap();
			break;

		case TState::DisplayingMenu:
		{
			TMenu Menu;
			m_Lock.Acquire();
			Menu = m_Menu;
			m_Lock.Release();
			DrawMenu(Menu, nTicks);
			break;
		}

		default:
			DrawHome(Synth, nTicks);
			break;
	}

	Display.Update(*m_pCanvas);
}

void CUserInterface::ShowStartup(CGC9A01& Display, const char* pVersion, const char* pStatus)
{
	m_pCanvas->Clear(ColorBackground);
	m_pCanvas->DrawBitmap(MT32PiLogo128x32, 128, 32, CCanvas::Center - 96, 64, 3, 2, ColorText);
	m_pCanvas->DrawCenteredText(FontSmall, pVersion, 118, ColorTextDim, 116);

	if (pStatus && *pStatus)
		DrawWrappedText(FontMedium, pStatus, 162, ColorAccentText, 2, 116);

	Display.Update(*m_pCanvas);
}

void CUserInterface::ShowPanic(CGC9A01& Display, const char* pMessage)
{
	m_pCanvas->Clear(ColorPanic);
	DrawGauge(110, 118, 1.0f, ColorError);
	m_pCanvas->DrawCenteredText(FontMedium, "Guru Meditation", 66, ColorError, 106);
	DrawWrappedText(FontSmall, pMessage, 136, ColorText, 4);
	m_pCanvas->DrawCenteredText(FontSmall, "Please reboot", 186, ColorTextDim);

	Display.SetBacklightState(true);
	Display.Update(*m_pCanvas, true);
}

void CUserInterface::ShowSystemMessage(const char* pMessage, bool bSpinner, TMessageType Type)
{
	m_Lock.Acquire();
	snprintf(m_SystemMessageTextBuffer, sizeof(m_SystemMessageTextBuffer), "%s", pMessage);
	m_SystemMessageType = Type;
	m_Lock.Release();

	m_nStateTime = CTimer::GetClockTicks();
	m_State = bSpinner ? TState::DisplayingSpinnerMessage : TState::DisplayingMessage;
}

void CUserInterface::ClearSpinnerMessage()
{
	m_State = TState::None;
}

void CUserInterface::DisplayImage(TImage Image)
{
	m_CurrentImage = Image;
	m_nStateTime = CTimer::GetClockTicks();
	m_State = TState::DisplayingImage;
}

void CUserInterface::ShowSysExText(TSysExDisplayMessage Type, const u8* pMessage, size_t nSize, u8 nOffset)
{
	if (nOffset >= SyxExTextBufferSize - 1)
		return;
	if (nOffset + nSize > SyxExTextBufferSize - 1)
		nSize = SyxExTextBufferSize - 1 - nOffset;

	m_Lock.Acquire();
	memset(m_SysExTextBuffer, ' ', nOffset);
	memcpy(m_SysExTextBuffer + nOffset, pMessage, nSize);
	m_SysExTextBuffer[nOffset + nSize] = '\0';
	m_SysExDisplayMessageType = Type;
	m_Lock.Release();

	m_nStateTime = CTimer::GetClockTicks();
	m_State = TState::DisplayingSysExText;
}

void CUserInterface::ShowSysExBitmap(TSysExDisplayMessage Type, const u8* pData, size_t nSize)
{
	if (nSize == 0)
		return;
	if (Type == TSysExDisplayMessage::Roland && nSize > 64)
		nSize = 64;
	else if (Type == TSysExDisplayMessage::Yamaha && nSize > 48)
		nSize = 48;

	m_Lock.Acquire();
	m_SysExDisplayMessageType = Type;
	memcpy(m_SysExPixelBuffer, pData, nSize);
	m_Lock.Release();

	m_nStateTime = CTimer::GetClockTicks();
	m_State = TState::DisplayingSysExBitmap;
}

void CUserInterface::EnterPowerSavingMode()
{
	m_Lock.Acquire();
	snprintf(m_SystemMessageTextBuffer, sizeof(m_SystemMessageTextBuffer), "Power saving mode");
	m_SystemMessageType = TMessageType::Notice;
	m_Lock.Release();

	m_nStateTime = CTimer::GetClockTicks();
	m_State = TState::EnteringPowerSavingMode;
}

void CUserInterface::ExitPowerSavingMode()
{
	m_State = TState::None;
}

void CUserInterface::ShowMenu(const TMenu& Menu)
{
	m_Lock.Acquire();
	m_Menu = Menu;
	m_Lock.Release();

	m_nStateTime = CTimer::GetClockTicks();
	m_State = TState::DisplayingMenu;
}

void CUserInterface::HideMenu()
{
	if (m_State == TState::DisplayingMenu)
		m_State = TState::None;
}

void CUserInterface::DrawHome(CSoundFontSynth& Synth, unsigned int nTicks)
{
	CSoundFontSynth::TDisplayInfo Info;
	Synth.GetDisplayInfo(Info);

	// Volume around the edge
	const u8 nVolume = m_nVolume;
	DrawGauge(113, 119, nVolume / 100.0f, ColorAccent);

	// SoundFont
	DrawScrollingText(FontSmall, Info.SoundFontName, 28, ColorTextDim, CCanvas::TextRadius, nTicks);

	// Instrument of the first channels: number, then the name as large as it fits
	if (Info.ProgramName[0])
	{
		char Number[32];
		if (Info.nBank)
			snprintf(Number, sizeof(Number), "%03d  Bank %d", Info.nProgram + 1, Info.nBank);
		else
			snprintf(Number, sizeof(Number), "%03d", Info.nProgram + 1);
		m_pCanvas->DrawCenteredText(FontSmall, Number, 50, ColorAccentText);

		const int nNameY = 76;
		if (m_pCanvas->GetTextWidth(FontLarge, Info.ProgramName) <= CCanvas::GetVisibleWidth(nNameY, nNameY + FontLarge.nHeight))
			m_pCanvas->DrawCenteredText(FontLarge, Info.ProgramName, nNameY, ColorText);
		else
			DrawWrappedText(FontMedium, Info.ProgramName, nNameY + FontLarge.nHeight / 2, ColorText, 2);
	}
	else
		m_pCanvas->DrawCenteredText(FontMedium, "No instrument", 82, ColorTextFaint);

	// Channel levels
	float Levels[MIDIChannels], Peaks[MIDIChannels];
	Synth.GetChannelLevels(nTicks, Levels, Peaks);
	DrawChannelLevels(Levels, Peaks, 128, 180);

	// Volume as a number
	char Volume[16];
	snprintf(Volume, sizeof(Volume), "Vol %d", nVolume);
	m_pCanvas->DrawCenteredText(FontSmall, Volume, 190, ColorTextDim);
}

void CUserInterface::DrawChannelLevels(const float* pLevels, const float* pPeaks, int nY1, int nY2)
{
	constexpr int nSpacing = 3;

	const int nAreaWidth = CCanvas::GetVisibleWidth(nY1, nY2, 106);
	const int nBarWidth = (nAreaWidth - (MIDIChannels - 1) * nSpacing) / MIDIChannels;
	const int nTotalWidth = nBarWidth * MIDIChannels + (MIDIChannels - 1) * nSpacing;
	const int nX0 = CCanvas::Center - nTotalWidth / 2;
	const int nHeight = nY2 - nY1;

	for (u8 nChannel = 0; nChannel < MIDIChannels; ++nChannel)
	{
		const int nX = nX0 + nChannel * (nBarWidth + nSpacing);
		const int nLevel = Utility::Clamp(static_cast<int>(pLevels[nChannel] * nHeight), 0, nHeight);

		m_pCanvas->FillRect(nX, nY1, nBarWidth, nHeight - nLevel, ColorTrack);

		// Coloured by height, like a VU meter
		for (int y = 0; y < nLevel; ++y)
		{
			const float nFraction = static_cast<float>(y) / nHeight;
			const TColor Color = nFraction < 0.6f ? ColorLevelLow : nFraction < 0.85f ? ColorLevelMid : ColorLevelHigh;
			m_pCanvas->FillRect(nX, nY2 - 1 - y, nBarWidth, 1, Color);
		}

		const int nPeak = Utility::Clamp(static_cast<int>(pPeaks[nChannel] * nHeight), 0, nHeight);
		if (nPeak > 1)
			m_pCanvas->FillRect(nX, nY2 - nPeak, nBarWidth, 2, ColorPeak);
	}
}

void CUserInterface::DrawGauge(int nInnerRadius, int nOuterRadius, float nFraction, TColor Color)
{
	nFraction = Utility::Clamp(nFraction, 0.0f, 1.0f);
	const float nEndAngle = GaugeStartAngle + GaugeSweep * nFraction;

	m_pCanvas->FillArc(nInnerRadius, nOuterRadius, GaugeStartAngle, GaugeStartAngle + GaugeSweep, ColorTrack);
	if (nFraction > 0.0f)
		m_pCanvas->FillArc(nInnerRadius, nOuterRadius, GaugeStartAngle, nEndAngle, Color);
}

void CUserInterface::DrawMessage(const char* pMessage, TColor Color, bool bSpinner, unsigned int nTicks)
{
	if (bSpinner)
	{
		// Arc going round once per period
		const float nAngle = (nTicks % SpinnerPeriodMicros) * 360.0f / SpinnerPeriodMicros;
		m_pCanvas->FillArc(108, 118, 0.0f, 360.0f, ColorTrack);
		m_pCanvas->FillArc(108, 118, nAngle, nAngle + SpinnerArc, ColorAccent);
	}

	// Inside the spinner's ring
	DrawWrappedText(FontMedium, pMessage, CCanvas::Center, Color, 3, 100);
}

void CUserInterface::DrawWrappedText(const TUIFont& Font, const char* pText, int nCenterY, TColor Color, size_t nMaxLines, int nRadius)
{
	constexpr size_t MaxLines = 6;
	constexpr size_t MaxLineLength = 128;

	nMaxLines = Utility::Min(nMaxLines, MaxLines);

	// Wrap at spaces to the width that is available for the whole block
	const int nBlockHeight = nMaxLines * Font.nHeight;
	const int nWidth = CCanvas::GetVisibleWidth(nCenterY - nBlockHeight / 2, nCenterY + nBlockHeight / 2, nRadius);

	char Lines[MaxLines][MaxLineLength];
	size_t nLines = 0;

	while (*pText == ' ')
		++pText;

	while (*pText && nLines < nMaxLines)
	{
		char* pLine = Lines[nLines++];
		size_t nLength = 0;

		// The last line takes the rest and is shortened if needed
		const bool bLastLine = nLines == nMaxLines;

		while (*pText && nLength < MaxLineLength - 1)
		{
			// Next word, with the space before it
			const char* pWordEnd = pText;
			while (*pWordEnd == ' ')
				++pWordEnd;
			while (*pWordEnd && *pWordEnd != ' ')
				++pWordEnd;

			const size_t nWordLength = Utility::Min(static_cast<size_t>(pWordEnd - pText), MaxLineLength - 1 - nLength);
			memcpy(pLine + nLength, pText, nWordLength);
			pLine[nLength + nWordLength] = '\0';

			// Keep a word that doesn't fit on its own line; it is shortened when drawn
			if (!bLastLine && nLength && m_pCanvas->GetTextWidth(Font, pLine) > nWidth)
			{
				pLine[nLength] = '\0';
				break;
			}

			nLength += nWordLength;
			pText += nWordLength;
		}

		pLine[nLength] = '\0';
		while (*pText == ' ')
			++pText;
	}

	int nY = nCenterY - static_cast<int>(nLines * Font.nHeight) / 2;
	for (size_t i = 0; i < nLines; ++i, nY += Font.nHeight)
	{
		// Leading space of wrapped words
		const char* pLine = Lines[i];
		while (*pLine == ' ')
			++pLine;

		m_pCanvas->DrawCenteredText(Font, pLine, nY, Color, nRadius);
	}
}

void CUserInterface::DrawMenu(const TMenu& Menu, unsigned int nTicks)
{
	if (Menu.Type == TMenu::TType::Parameter)
	{
		DrawGauge(104, 116, Menu.nFraction, ColorAccent);

		char Page[16];
		snprintf(Page, sizeof(Page), "%d / %d", Menu.nPage, Menu.nPages);
		// Inside the gauge
		m_pCanvas->DrawCenteredText(FontSmall, Page, 52, ColorTextFaint, 98);
		DrawScrollingText(FontMedium, Menu.Title, 84, ColorText, 98, nTicks);
		m_pCanvas->DrawCenteredText(FontLarge, Menu.Value, 110, ColorAccentText, 98);
		return;
	}

	// List with the selection in the middle
	constexpr int nRowHeight = 34;
	constexpr int nSelectionHeight = 32;
	constexpr int nMiddleY = 128;

	m_pCanvas->DrawCenteredText(FontSmall, Menu.Title, 30, ColorAccentText, 118);

	for (size_t nRow = 0; nRow < TMenu::ListRows; ++nRow)
	{
		const char* pText = Menu.Rows[nRow];
		if (!*pText)
			continue;

		const int nDistance = static_cast<int>(nRow) - static_cast<int>(TMenu::SelectedRow);
		const int nCenterY = nMiddleY + nDistance * nRowHeight;

		if (nDistance == 0)
		{
			const int nY = nCenterY - nSelectionHeight / 2;
			const int nWidth = CCanvas::GetVisibleWidth(nY, nY + nSelectionHeight, 112);
			m_pCanvas->FillRoundedRect(CCanvas::Center - nWidth / 2, nY, nWidth, nSelectionHeight, 10, ColorSelection);
			DrawScrollingText(FontMedium, pText, nCenterY - FontMedium.nHeight / 2, ColorText, nWidth / 2 - 10, nTicks, nWidth - 20);
		}
		else
		{
			const bool bCurrent = static_cast<int>(nRow) == Menu.nCurrentRow;
			const TColor Color = bCurrent ? ColorAccentText : (nDistance == 1 || nDistance == -1) ? ColorTextDim : ColorTextFaint;
			m_pCanvas->DrawCenteredText(FontSmall, pText, nCenterY - FontSmall.nHeight / 2, Color);
		}
	}
}

void CUserInterface::DrawScrollingText(const TUIFont& Font, const char* pText, int nY, TColor Color, int nRadius, unsigned int nTicks, int nMaxWidth)
{
	int nWidth = CCanvas::GetVisibleWidth(nY, nY + Font.nHeight, nRadius);
	if (nMaxWidth >= 0)
		nWidth = nMaxWidth;

	const int nTextWidth = m_pCanvas->GetTextWidth(Font, pText);
	if (nTextWidth <= nWidth)
	{
		m_pCanvas->DrawText(Font, pText, CCanvas::Center, nY, Color, TAlign::Center);
		return;
	}

	// Pause at the start of each pass, then scroll left until the text's start comes round again
	const int nPeriod = nTextWidth + ScrollGap;
	const unsigned int nPassMillis = ScrollPauseMillis + nPeriod * ScrollMillisPerPixel;
	const unsigned int nTime = (nTicks / 1000) % nPassMillis;
	const int nOffset = nTime < ScrollPauseMillis ? 0 : (nTime - ScrollPauseMillis) / ScrollMillisPerPixel;

	const int nX1 = CCanvas::Center - nWidth / 2;
	m_pCanvas->SetClip(nX1, nX1 + nWidth - 1);
	m_pCanvas->DrawText(Font, pText, nX1 - nOffset, nY, Color, TAlign::Left, nTextWidth);
	m_pCanvas->DrawText(Font, pText, nX1 - nOffset + nPeriod, nY, Color, TAlign::Left, nTextWidth);
	m_pCanvas->ResetClip();
}

void CUserInterface::DrawSysExBitmap()
{
	u8 Pixels[SysExPixelBufferSize];
	m_Lock.Acquire();
	memcpy(Pixels, m_SysExPixelBuffer, sizeof(Pixels));
	const TSysExDisplayMessage Type = m_SysExDisplayMessageType;
	m_Lock.Release();

	// 16x16 pixels, scaled to 128x128 and centered
	constexpr int nScale = 8;
	constexpr int nOffset = CCanvas::Center - 8 * nScale;

	u8 nHeadLength, nHeadPixels, nTailPixels;
	if (Type == TSysExDisplayMessage::Roland)
	{
		// SC-55: Max 64 bytes; each byte representing 5 pixels (see p78 of SC-55 manual)
		// First 48 bytes have 5 columns of pixels, last 16 bytes have only 1
		nHeadLength = 48;
		nHeadPixels = 5;
		nTailPixels = 1;
	}
	else
	{
		// Yamaha: Max 48 bytes; each byte representing 7 pixels (see p16 of MU80 Sound List & MIDI Data book)
		// First 32 bytes have 7 columns of pixels, last 16 bytes have only 2
		nHeadLength = 32;
		nHeadPixels = 7;
		nTailPixels = 2;
	}

	for (u8 nByte = 0; nByte < sizeof(Pixels); ++nByte)
	{
		const u8 nPixels = nByte < nHeadLength ? nHeadPixels : nTailPixels;

		for (u8 nPixel = 0; nPixel < nPixels; ++nPixel)
		{
			if (!((Pixels[nByte] >> (nHeadPixels - 1 - nPixel)) & 1))
				continue;

			const int nX = nByte / 16 * nHeadPixels + nPixel;
			const int nY = nByte % 16;
			m_pCanvas->FillRect(nOffset + nX * nScale, nOffset + nY * nScale, nScale - 1, nScale - 1, ColorAccentText);
		}
	}
}
