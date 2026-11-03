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

// Colours, after the logo: gold accents, and the red of its initial for the selection
constexpr TColor ColorBackground   = CCanvas::RGB(0, 0, 0);
constexpr TColor ColorText         = CCanvas::RGB(255, 255, 255);
constexpr TColor ColorTextDim      = CCanvas::RGB(170, 170, 170);
constexpr TColor ColorTextFaint    = CCanvas::RGB(100, 100, 100);
constexpr TColor ColorAccent       = CCanvas::RGB(230, 185, 85);
constexpr TColor ColorAccentText   = CCanvas::RGB(240, 205, 125);
constexpr TColor ColorSelection    = CCanvas::RGB(170, 32, 28);
constexpr TColor ColorTrack        = CCanvas::RGB(48, 42, 36);
constexpr TColor ColorWarning      = CCanvas::RGB(255, 170, 0);
constexpr TColor ColorError        = CCanvas::RGB(255, 70, 60);
constexpr TColor ColorPeak         = CCanvas::RGB(255, 255, 255);
constexpr TColor ColorPanic        = CCanvas::RGB(90, 0, 0);
constexpr TColor ColorLogoInitial  = CCanvas::RGB(205, 40, 35);
constexpr TColor ColorLogoText     = CCanvas::RGB(230, 185, 85);

// Top of the logo on the startup screen
constexpr int LogoY = 58;

// The largest of the fonts (largest first) in which the text fits into the width; the smallest one if none does
template <size_t N>
static const TUIFont& FitFont(const CCanvas& Canvas, const char* pText, int nWidth, const TUIFont* const (&Fonts)[N])
{
	for (const TUIFont* pFont : Fonts)
		if (Canvas.GetTextWidth(*pFont, pText) <= nWidth)
			return *pFont;

	return *Fonts[N - 1];
}

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
	  m_nDisplayChannels(MIDIChannels),
	  m_nStandbyTimeout(0),
	  m_nActivityCount(0),
	  m_nSeenActivityCount(0),
	  m_nLastActivityTime(0),
	  m_bStandby(false),

	  m_Lock(TASK_LEVEL),
	  m_SystemMessageTextBuffer{'\0'},
	  m_SystemMessageType(TMessageType::Notice),
	  m_nLoadedBytes(0),
	  m_nTotalBytes(0),
	  m_SysExDisplayMessageType(TSysExDisplayMessage::Roland),
	  m_SysExTextBuffer{'\0'},
	  m_SysExPixelBuffer{0},
	  m_Menu{},
	  m_LoadingText{'\0'},
	  m_PresetLabel{'\0'},
	  m_StatusMain{'\0'},
	  m_StatusSide{'\0'}
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

	// Display standby: after a while without activity on the home screen; it stays off until there is activity (the
	// clock wraps around after about 71 minutes) or something else is shown
	const unsigned nActivityCount = m_nActivityCount;
	if (nActivityCount != m_nSeenActivityCount || !m_nLastActivityTime)
	{
		m_nSeenActivityCount = nActivityCount;
		m_nLastActivityTime = nTicks | 1;
		m_bStandby = false;
	}

	const unsigned nStandbyTimeout = m_nStandbyTimeout;
	if (!m_bStandby && nStandbyTimeout && nTicks - m_nLastActivityTime >= nStandbyTimeout * 1000000u)
		m_bStandby = true;

	if (m_bStandby && m_State == TState::None)
	{
		if (Display.GetBacklightState())
			Display.SetBacklightState(false);
		return;
	}

	if (!Display.GetBacklightState())
		Display.SetBacklightState(true);

	m_pCanvas->Clear(ColorBackground);

	char Text[SystemMessageTextBufferSize];
	TMessageType MessageType;

	switch (m_State)
	{
		case TState::DisplayingMessage:
		case TState::DisplayingSpinnerMessage:
		{
			m_Lock.Acquire();
			memcpy(Text, m_SystemMessageTextBuffer, sizeof(Text));
			MessageType = m_SystemMessageType;
			m_Lock.Release();

			const TColor Color = MessageType == TMessageType::Error ? ColorError : MessageType == TMessageType::Warning ? ColorWarning : ColorText;
			DrawMessage(Text, Color, m_State == TState::DisplayingSpinnerMessage, nTicks);
			break;
		}

		case TState::DisplayingLoadProgress:
		{
			m_Lock.Acquire();
			const u64 nLoadedBytes = m_nLoadedBytes;
			const u64 nTotalBytes = m_nTotalBytes;
			memcpy(Text, m_LoadingText, sizeof(m_LoadingText));
			m_Lock.Release();

			// With the instrument that is loading, as large as it fits
			if (Text[0])
			{
				static const TUIFont* const NameFonts[] = { &FontMedium, &FontSmall, &FontTiny };
				const TUIFont& Font = FitFont(*m_pCanvas, Text, CCanvas::GetVisibleWidth(78, 78 + FontMedium.nHeight, 100), NameFonts);
				m_pCanvas->DrawCenteredText(FontSmall, "Loading samples", 56, ColorTextDim, 100);
				m_pCanvas->DrawCenteredText(Font, Text, 78 + (FontMedium.nHeight - Font.nHeight) / 2, ColorAccentText, 100);
				DrawLoadProgress(nLoadedBytes, nTotalBytes, 108, 118, 106);
			}
			else
			{
				m_pCanvas->DrawCenteredText(FontSmall, "Loading samples", 76, ColorTextDim, 100);
				DrawLoadProgress(nLoadedBytes, nTotalBytes, 108, 118, 102);
			}
			break;
		}

		case TState::DisplayingImage:
			if (m_CurrentImage == TImage::MisterLogo)
				m_pCanvas->DrawBitmap(MisterLogo128x32, 128, 32, CCanvas::Center - 96, CCanvas::Center - 24, 3, 2, ColorText);
			else
				DrawLogo(CCanvas::Center - LogoText.nHeight / 2);
			break;

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
	DrawLogo(LogoY);
	if (pVersion && *pVersion)
		m_pCanvas->DrawCenteredText(FontSmall, pVersion, 130, ColorTextDim, 116);

	if (pStatus && *pStatus)
		DrawWrappedText(FontMedium, pStatus, 172, ColorAccentText, 2, 116);

	Display.Update(*m_pCanvas);
}

void CUserInterface::ShowStartupProgress(CGC9A01& Display, const char* pVersion, u64 nLoadedBytes, u64 nTotalBytes)
{
	char Text[LoadingTextLength];
	m_Lock.Acquire();
	memcpy(Text, m_LoadingText, sizeof(Text));
	m_Lock.Release();

	// The instrument that is loading instead of the version
	m_pCanvas->Clear(ColorBackground);
	DrawLogo(LogoY);
	if (Text[0])
		m_pCanvas->DrawCenteredText(FontSmall, Text, 130, ColorAccentText, 116);
	else if (pVersion && *pVersion)
		m_pCanvas->DrawCenteredText(FontSmall, pVersion, 130, ColorTextDim, 116);
	DrawLoadProgress(nLoadedBytes, nTotalBytes, 112, 118, 152);

	Display.Update(*m_pCanvas);
}

void CUserInterface::DrawLogo(int nY)
{
	// Like an illuminated manuscript: a red initial, the rest in gold
	const int nX = CCanvas::Center - LogoText.nWidth / 2;
	m_pCanvas->DrawImage(LogoInitial, nX, nY, ColorLogoInitial);
	m_pCanvas->DrawImage(LogoText, nX, nY, ColorLogoText);
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

void CUserInterface::ShowLoadProgress(u64 nLoadedBytes, u64 nTotalBytes)
{
	m_Lock.Acquire();
	m_nLoadedBytes = nLoadedBytes;
	m_nTotalBytes = nTotalBytes;
	m_Lock.Release();

	m_nStateTime = CTimer::GetClockTicks();
	m_State = TState::DisplayingLoadProgress;
}

void CUserInterface::SetPresetLabel(const char* pLabel)
{
	m_Lock.Acquire();
	snprintf(m_PresetLabel, sizeof(m_PresetLabel), "%s", pLabel);
	m_Lock.Release();
}

void CUserInterface::SetLoadingText(const char* pText)
{
	m_Lock.Acquire();
	snprintf(m_LoadingText, sizeof(m_LoadingText), "%s", pText);
	m_Lock.Release();
}

void CUserInterface::SetStatusText(const char* pMain, const char* pSide)
{
	m_Lock.Acquire();
	snprintf(m_StatusMain, sizeof(m_StatusMain), "%s", pMain);
	snprintf(m_StatusSide, sizeof(m_StatusSide), "%s", pSide);
	m_Lock.Release();
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

	// Status, e.g. of the MIDI guitar: the main line below the channel levels, the other one next to the volume
	char StatusMain[StatusTextLength], StatusSide[StatusTextLength], PresetLabel[StatusTextLength];
	m_Lock.Acquire();
	memcpy(StatusMain, m_StatusMain, sizeof(StatusMain));
	memcpy(StatusSide, m_StatusSide, sizeof(StatusSide));
	memcpy(PresetLabel, m_PresetLabel, sizeof(PresetLabel));
	m_Lock.Release();
	if (StatusMain[0])
		m_pCanvas->DrawCenteredText(FontMedium, StatusMain, 151, ColorAccentText);

	// Instrument of the first channels: number, then the name as large as it fits
	if (Info.ProgramName[0])
	{
		// With the preset, if there is one
		char Number[64];
		const char* const pSeparator = PresetLabel[0] ? "  " : "";
		if (Info.nBank)
			snprintf(Number, sizeof(Number), "%s%s%03d  Bank %d", PresetLabel, pSeparator, Info.nProgram + 1, Info.nBank);
		else
			snprintf(Number, sizeof(Number), "%s%s%03d", PresetLabel, pSeparator, Info.nProgram + 1);
		m_pCanvas->DrawCenteredText(FontSmall, Number, 40, ColorAccentText);

		const int nNameY = 62;
		if (m_pCanvas->GetTextWidth(FontLarge, Info.ProgramName) <= CCanvas::GetVisibleWidth(nNameY, nNameY + FontLarge.nHeight))
			m_pCanvas->DrawCenteredText(FontLarge, Info.ProgramName, nNameY, ColorText);
		else
			DrawWrappedText(FontMedium, Info.ProgramName, nNameY + FontLarge.nHeight / 2, ColorText, 2);
	}
	else
		m_pCanvas->DrawCenteredText(FontMedium, "No instrument", 68, ColorTextFaint);

	// Channel levels
	float Levels[MIDIChannels], Peaks[MIDIChannels];
	Synth.GetChannelLevels(nTicks, Levels, Peaks);
	const u8 nChannels = m_nDisplayChannels;
	DrawChannelLevels(Levels, Peaks, Utility::Clamp<u8>(nChannels, 1, MIDIChannels), 104, 144);

	DrawVolume(nVolume, 180, StatusSide);
}

void CUserInterface::DrawVolume(u8 nVolume, int nY, const char* pSideText)
{
	// Speaker with sound waves (or a cross when muted), followed by the value and the side text
	constexpr int nIconWidth = 23;
	constexpr int nIconHeight = 15;
	constexpr int nGap = 6;
	constexpr int nSideGap = 16;
	const TColor Color = ColorTextDim;

	char Text[8];
	snprintf(Text, sizeof(Text), "%d", nVolume);
	const int nTextWidth = m_pCanvas->GetTextWidth(FontSmall, Text);
	const int nSideWidth = *pSideText ? nSideGap + m_pCanvas->GetTextWidth(FontSmall, pSideText) : 0;
	const int nX = CCanvas::Center - (nIconWidth + nGap + nTextWidth + nSideWidth) / 2;
	const int nMiddle = nY + nIconHeight / 2;

	if (*pSideText)
		m_pCanvas->DrawText(FontSmall, pSideText, nX + nIconWidth + nGap + nTextWidth + nSideGap, nY, ColorAccentText);

	// Body and cone
	m_pCanvas->FillRect(nX, nMiddle - 3, 4, 7, Color);
	for (int i = 0; i < 5; ++i)
		m_pCanvas->FillRect(nX + 4 + i, nMiddle - 3 - i, 1, 7 + 2 * i, Color);

	if (nVolume == 0)
	{
		for (int i = 0; i < 7; ++i)
		{
			m_pCanvas->FillRect(nX + 12 + i, nMiddle - 3 + i, 2, 1, Color);
			m_pCanvas->FillRect(nX + 12 + i, nMiddle + 3 - i, 2, 1, Color);
		}
	}
	else
	{
		m_pCanvas->FillArc(nX + 9, nMiddle + 1, 3, 5, 50.0f, 130.0f, Color);
		if (nVolume >= 34)
			m_pCanvas->FillArc(nX + 9, nMiddle + 1, 7, 9, 45.0f, 135.0f, Color);
		if (nVolume >= 67)
			m_pCanvas->FillArc(nX + 9, nMiddle + 1, 11, 13, 40.0f, 140.0f, Color);
	}

	m_pCanvas->DrawText(FontSmall, Text, nX + nIconWidth + nGap, nY, Color);
}

void CUserInterface::DrawChannelLevels(const float* pLevels, const float* pPeaks, u8 nChannels, int nY1, int nY2)
{
	constexpr int nSpacing = 3;
	constexpr int nMaxBarWidth = 24;

	const int nAreaWidth = CCanvas::GetVisibleWidth(nY1, nY2, 106);
	const int nBarWidth = Utility::Min((nAreaWidth - (nChannels - 1) * nSpacing) / nChannels, nMaxBarWidth);
	const int nTotalWidth = nBarWidth * nChannels + (nChannels - 1) * nSpacing;
	const int nX0 = CCanvas::Center - nTotalWidth / 2;
	const int nHeight = nY2 - nY1;

	for (u8 nChannel = 0; nChannel < nChannels; ++nChannel)
	{
		const int nX = nX0 + nChannel * (nBarWidth + nSpacing);
		const int nLevel = Utility::Clamp(static_cast<int>(pLevels[nChannel] * nHeight), 0, nHeight);

		// In the red of the selection, like the logo's initial
		m_pCanvas->FillRect(nX, nY1, nBarWidth, nHeight - nLevel, ColorTrack);
		m_pCanvas->FillRect(nX, nY2 - nLevel, nBarWidth, nLevel, ColorSelection);

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

void CUserInterface::DrawLoadProgress(u64 nLoadedBytes, u64 nTotalBytes, int nInnerRadius, int nOuterRadius, int nPercentY)
{
	constexpr u64 BytesPerMegabyte = 1024 * 1024;
	char Text[32];

	// Without a known total, only the amount loaded so far can be shown
	if (!nTotalBytes)
	{
		m_pCanvas->FillArc(nInnerRadius, nOuterRadius, 0.0f, 360.0f, ColorTrack);
		snprintf(Text, sizeof(Text), "%u MB", static_cast<unsigned>(nLoadedBytes / BytesPerMegabyte));
		m_pCanvas->DrawCenteredText(FontLarge, Text, nPercentY, ColorText);
		return;
	}

	nLoadedBytes = Utility::Min(nLoadedBytes, nTotalBytes);
	const unsigned nPercent = static_cast<unsigned>(nLoadedBytes * 100 / nTotalBytes);

	// Ring filling up clockwise from the top
	m_pCanvas->FillArc(nInnerRadius, nOuterRadius, 0.0f, 360.0f, ColorTrack);
	if (nLoadedBytes)
		m_pCanvas->FillArc(nInnerRadius, nOuterRadius, 0.0f, 360.0f * nLoadedBytes / nTotalBytes, ColorAccent);

	snprintf(Text, sizeof(Text), "%u%%", nPercent);
	m_pCanvas->DrawCenteredText(FontLarge, Text, nPercentY, ColorText);

	// Rounded up, so that a small total doesn't show as 0 MB
	snprintf(Text, sizeof(Text), "%u / %u MB", static_cast<unsigned>(nLoadedBytes / BytesPerMegabyte),
		 static_cast<unsigned>((nTotalBytes + BytesPerMegabyte - 1) / BytesPerMegabyte));
	m_pCanvas->DrawCenteredText(FontSmall, Text, nPercentY + FontLarge.nHeight + 6, ColorTextFaint);
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
	// Text is shown as large as it fits, so that all of it can be read; only if it doesn't fit in the smallest
	// font, it is shortened (or scrolls, if selected)
	static const TUIFont* const SelectedFonts[] = { &FontMedium, &FontSmall, &FontTiny };
	static const TUIFont* const RowFonts[] = { &FontSmall, &FontTiny };

	if (Menu.Type == TMenu::TType::Value)
	{
		static const TUIFont* const ValueFonts[] = { &FontLarge, &FontMedium, &FontSmall };

		if (Menu.nFraction >= 0.0f)
			DrawGauge(104, 116, Menu.nFraction, ColorAccent);

		// Its position in the list, then the name and the value, which keep their centres when they get smaller
		char Position[16];
		snprintf(Position, sizeof(Position), "%u / %u", static_cast<unsigned>(Menu.nItem), static_cast<unsigned>(Menu.nItems));
		m_pCanvas->DrawCenteredText(FontSmall, Position, 52, ColorTextFaint, 98);

		const int nTitleY = 84 + FontMedium.nHeight / 2;
		const TUIFont& TitleFont = FitFont(*m_pCanvas, Menu.Title, CCanvas::GetVisibleWidth(nTitleY - 12, nTitleY + 12, 98), SelectedFonts);
		DrawScrollingText(TitleFont, Menu.Title, nTitleY - TitleFont.nHeight / 2, ColorText, 98, nTicks);

		const int nValueY = 112 + FontLarge.nHeight / 2;
		const TUIFont& ValueFont = FitFont(*m_pCanvas, Menu.Value, CCanvas::GetVisibleWidth(nValueY - 16, nValueY + 16, 98), ValueFonts);
		DrawScrollingText(ValueFont, Menu.Value, nValueY - ValueFont.nHeight / 2, ColorAccentText, 98, nTicks);
		return;
	}

	// Rows of two lines, the selection in the middle
	constexpr int nRowHeight = 58;
	constexpr int nSelectionHeight = 54;
	constexpr int nMiddleY = 128;
	constexpr int nTitleY = 22;

	const TUIFont& TitleFont = FitFont(*m_pCanvas, Menu.Title, CCanvas::GetVisibleWidth(nTitleY, nTitleY + FontSmall.nHeight, 116), RowFonts);
	DrawScrollingText(TitleFont, Menu.Title, nTitleY + FontSmall.nHeight - TitleFont.nHeight, ColorAccentText, 116, nTicks);

	for (size_t nRow = 0; nRow < TMenu::ListRows; ++nRow)
	{
		const char* pName = Menu.Names[nRow];
		const char* pValue = Menu.Values[nRow];
		if (!*pName)
			continue;

		const int nDistance = static_cast<int>(nRow) - static_cast<int>(TMenu::SelectedRow);
		const int nCenterY = nMiddleY + nDistance * nRowHeight;

		if (nDistance == 0)
		{
			const int nY = nCenterY - nSelectionHeight / 2;
			const int nWidth = CCanvas::GetVisibleWidth(nY, nY + nSelectionHeight, 110);
			m_pCanvas->FillRoundedRect(CCanvas::Center - nWidth / 2, nY, nWidth, nSelectionHeight, 12, ColorSelection);
			const int nTextWidth = nWidth - 24;

			// Without a value, the name alone in the larger font
			if (!*pValue)
			{
				const TUIFont& Font = FitFont(*m_pCanvas, pName, nTextWidth, SelectedFonts);
				DrawScrollingText(Font, pName, nCenterY - Font.nHeight / 2, ColorText, nWidth / 2, nTicks, nTextWidth);
				continue;
			}

			// The name small above the value; a value that is still to be applied in the accent colour
			const TUIFont& NameFont = FitFont(*m_pCanvas, pName, nTextWidth, RowFonts);
			DrawScrollingText(NameFont, pName, nY + 6 + FontSmall.nHeight - NameFont.nHeight, ColorTextDim, nWidth / 2, nTicks, nTextWidth);

			const TUIFont& ValueFont = FitFont(*m_pCanvas, pValue, nTextWidth, SelectedFonts);
			const int nValueY = nY + 6 + FontSmall.nHeight + 2 + (FontMedium.nHeight - ValueFont.nHeight) / 2;
			DrawScrollingText(ValueFont, pValue, nValueY, Menu.bPending ? ColorAccentText : ColorText, nWidth / 2, nTicks, nTextWidth);
			continue;
		}

		const bool bCurrent = static_cast<int>(nRow) == Menu.nCurrentRow;
		const TColor NameColor = bCurrent ? ColorAccentText : ColorTextDim;
		const int nWidth = CCanvas::GetVisibleWidth(nCenterY - nRowHeight / 2 + 4, nCenterY + nRowHeight / 2 - 4, 108);

		if (!*pValue)
		{
			const TUIFont& Font = FitFont(*m_pCanvas, pName, nWidth, RowFonts);
			m_pCanvas->DrawText(Font, pName, CCanvas::Center, nCenterY - Font.nHeight / 2, NameColor, TAlign::Center, nWidth);
			continue;
		}

		const int nNameY = nCenterY - FontSmall.nHeight;
		const TUIFont& NameFont = FitFont(*m_pCanvas, pName, nWidth, RowFonts);
		m_pCanvas->DrawText(NameFont, pName, CCanvas::Center, nNameY + FontSmall.nHeight - NameFont.nHeight, NameColor, TAlign::Center, nWidth);
		const TUIFont& ValueFont = FitFont(*m_pCanvas, pValue, nWidth, RowFonts);
		m_pCanvas->DrawText(ValueFont, pValue, CCanvas::Center, nCenterY + 1, bCurrent ? ColorAccentText : ColorTextFaint, TAlign::Center, nWidth);
	}

	DrawScrollBar(Menu.nItem, Menu.nItems);
}

void CUserInterface::DrawScrollBar(size_t nItem, size_t nItems)
{
	// Along the right edge; its thumb is as long as the part of the list that is shown, and moves from the top to the
	// bottom while the selection goes from the first item to the last
	constexpr float nStartAngle = 42.0f;
	constexpr float nSweep = 96.0f;
	constexpr float nMinThumb = 10.0f;

	if (nItems <= 1)
		return;

	const float nThumb = Utility::Max(nSweep * Utility::Min(static_cast<float>(TMenu::ListRows) / nItems, 1.0f), nMinThumb);
	const float nPosition = static_cast<float>(Utility::Min(nItem, nItems - 1)) / (nItems - 1);
	const float nThumbStart = nStartAngle + (nSweep - nThumb) * nPosition;

	m_pCanvas->FillArc(114, 119, nStartAngle, nStartAngle + nSweep, ColorTrack);
	m_pCanvas->FillArc(114, 119, nThumbStart, nThumbStart + nThumb, ColorAccent);
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
