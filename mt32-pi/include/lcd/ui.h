//
// ui.h
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

#ifndef _ui_h
#define _ui_h

#include <circle/spinlock.h>
#include <circle/types.h>

#include "lcd/canvas.h"
#include "lcd/images.h"

class CGC9A01;
class CSoundFontSynth;

// User interface on the round colour display. Messages and menus are set from the main task on core 0; the
// screen is drawn by the UI task on core 1.
class CUserInterface
{
public:
	enum class TSysExDisplayMessage
	{
		Roland,
		Yamaha,
	};

	enum class TMessageType
	{
		Notice,
		Warning,
		Error,
	};

	struct TMenu
	{
		enum class TType
		{
			List,
			Parameter,
		};

		// Lists show the selected item in the middle row
		static constexpr size_t ListRows = 5;
		static constexpr size_t SelectedRow = ListRows / 2;
		static constexpr size_t TextLength = 48;

		TType Type;
		char Title[TextLength];

		// List: items around the selection; empty rows are left blank
		char Rows[ListRows][TextLength];
		int nCurrentRow;	// Row of the item that is in use, which is highlighted; -1 for none

		// Parameter: value with a gauge
		char Value[TextLength];
		float nFraction;
		u8 nPage;
		u8 nPages;
	};

	CUserInterface();
	~CUserInterface();

	CUserInterface(const CUserInterface&) = delete;
	CUserInterface& operator=(const CUserInterface&) = delete;

	// Drawn by the UI task
	void Update(CGC9A01& Display, CSoundFontSynth& Synth, unsigned int nTicks);

	// Drawn immediately; only before the UI task has started, or after it has stopped
	void ShowStartup(CGC9A01& Display, const char* pVersion, const char* pStatus);
	void ShowStartupProgress(CGC9A01& Display, const char* pVersion, u64 nLoadedBytes, u64 nTotalBytes);
	void ShowPanic(CGC9A01& Display, const char* pMessage);

	void ShowSystemMessage(const char* pMessage, bool bSpinner = false, TMessageType Type = TMessageType::Notice);
	void ShowLoadProgress(u64 nLoadedBytes, u64 nTotalBytes);
	void ClearSpinnerMessage();
	void DisplayImage(TImage Image);
	void ShowSysExText(TSysExDisplayMessage Type, const u8* pMessage, size_t nSize, u8 nOffset);
	void ShowSysExBitmap(TSysExDisplayMessage Type, const u8* pData, size_t nSize);
	void EnterPowerSavingMode();
	void ExitPowerSavingMode();

	// Shown until hidden or replaced by a message
	void ShowMenu(const TMenu& Menu);
	void HideMenu();

	void SetVolume(u8 nVolume) { m_nVolume = nVolume; }

private:
	enum class TState
	{
		None,
		DisplayingMessage,
		DisplayingSpinnerMessage,
		DisplayingLoadProgress,
		DisplayingImage,
		DisplayingSysExText,
		DisplayingSysExBitmap,
		DisplayingMenu,
		EnteringPowerSavingMode,
		InPowerSavingMode
	};

	void DrawHome(CSoundFontSynth& Synth, unsigned int nTicks);
	void DrawChannelLevels(const float* pLevels, const float* pPeaks, int nY1, int nY2);
	void DrawGauge(int nInnerRadius, int nOuterRadius, float nFraction, CCanvas::TColor Color);
	void DrawMessage(const char* pMessage, CCanvas::TColor Color, bool bSpinner, unsigned int nTicks);
	void DrawLoadProgress(u64 nLoadedBytes, u64 nTotalBytes, int nInnerRadius, int nOuterRadius, int nPercentY);
	void DrawWrappedText(const TUIFont& Font, const char* pText, int nCenterY, CCanvas::TColor Color, size_t nMaxLines, int nRadius = CCanvas::TextRadius);
	void DrawMenu(const TMenu& Menu, unsigned int nTicks);
	void DrawScrollingText(const TUIFont& Font, const char* pText, int nY, CCanvas::TColor Color, int nRadius, unsigned int nTicks, int nMaxWidth = -1);
	void DrawSysExBitmap();

	static constexpr size_t SystemMessageTextBufferSize = 256;
	static constexpr size_t SyxExTextBufferSize = 32 + 1;
	static constexpr size_t SysExPixelBufferSize = 64;

	static constexpr unsigned SystemMessageDisplayTimeMillis = 3000;
	static constexpr unsigned SC55DisplayTimeMillis = 3000;

	CCanvas* m_pCanvas;

	// UI state
	volatile TState m_State;
	unsigned m_nStateTime;
	TImage m_CurrentImage;
	volatile u8 m_nVolume;

	// Text set from another core
	CSpinLock m_Lock;
	char m_SystemMessageTextBuffer[SystemMessageTextBufferSize];
	TMessageType m_SystemMessageType;
	u64 m_nLoadedBytes;
	u64 m_nTotalBytes;
	TSysExDisplayMessage m_SysExDisplayMessageType;
	char m_SysExTextBuffer[SyxExTextBufferSize];
	u8 m_SysExPixelBuffer[SysExPixelBufferSize];
	TMenu m_Menu;
};

#endif
