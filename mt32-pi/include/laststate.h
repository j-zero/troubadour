//
// laststate.h
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

#ifndef _laststate_h
#define _laststate_h

#include <circle/string.h>

#include "synth/fxprofile.h"

// Settings changed with the physical controls, which are restored on startup
class CLastState
{
public:
	CLastState();

	bool Load(const char* pPath);
	bool Save(const char* pPath);

	CString SoundFontPath;	// Empty if not set
	int nBank;		// Instrument for the button channels; -1 if not set
	int nProgram;
	int nVolume;		// -1 if not set
	TFXProfile Effects;	// Reverb and chorus settings that were changed

private:
	void Format(CString& Text) const;

	// Contents of the file, so that it is only written when something has changed
	CString m_SavedText;

	static int INIHandler(void* pUser, const char* pSection, const char* pName, const char* pValue);
};

#endif
