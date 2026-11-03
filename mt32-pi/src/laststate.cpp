//
// laststate.cpp
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
#include <circle/util.h>
#include <fatfs/ff.h>
#include <ini.h>

#include "config.h"
#include "laststate.h"

LOGMODULE("laststate");

const char StateSection[] = "state";

CLastState::CLastState()
	: nBank(-1),
	  nProgram(-1),
	  nDrumBank(-1),
	  nDrumProgram(-1),
	  nVolume(-1),
	  nDisplayChannels(-1),
	  nDisplayStandby(-1),
	  nTriplePlay(-1),
	  nTriplePlaySoftwareMode(-1),
	  nTriplePlayProgramPresets(-1),
	  nPreset(-1)
{
}

bool CLastState::Load(const char* pPath)
{
	FIL File;
	if (f_open(&File, pPath, FA_READ) != FR_OK)
		return false;

	// +1 byte for null terminator
	const UINT nSize = f_size(&File);
	char Buffer[nSize + 1];
	UINT nRead;

	const FRESULT Result = f_read(&File, Buffer, nSize, &nRead);
	f_close(&File);

	if (Result != FR_OK)
	{
		LOGERR("Error reading '%s'", pPath);
		return false;
	}

	// Ensure null-terminated
	Buffer[nRead] = '\0';

	const int nResult = ini_parse_string(Buffer, INIHandler, this);
	if (nResult > 0)
		LOGWARN("'%s' parse error on line %d", pPath, nResult);

	// An instrument needs both values
	if (nBank < 0 || nProgram < 0 || nProgram > 127)
		nBank = nProgram = -1;
	if (nDrumBank < 0 || nDrumProgram < 0 || nDrumProgram > 127)
		nDrumBank = nDrumProgram = -1;
	if (nDisplayChannels < 1 || nDisplayChannels > 16)
		nDisplayChannels = -1;
	if (nDisplayStandby < 0 || nDisplayStandby > 3600)
		nDisplayStandby = -1;

	Format(m_SavedText);

	return nResult >= 0;
}

bool CLastState::Save(const char* pPath)
{
	CString Text;
	Format(Text);

	if (Text.Compare(m_SavedText) == 0)
		return true;

	FIL File;
	if (f_open(&File, pPath, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		LOGERR("Couldn't open '%s' for writing", pPath);
		return false;
	}

	UINT nWritten;
	const UINT nSize = Text.GetLength();
	FRESULT Result = f_write(&File, static_cast<const char*>(Text), nSize, &nWritten);

	if (f_close(&File) != FR_OK || Result != FR_OK || nWritten != nSize)
	{
		LOGERR("Error writing '%s'", pPath);
		return false;
	}

	m_SavedText = Text;
	LOGNOTE("State saved");

	return true;
}

void CLastState::Format(CString& Text) const
{
	CString Line;

	Text = "# Settings changed with the buttons and the encoder, saved automatically by\n"
	       "# mt32-pi and restored on startup. They take precedence over mt32-pi.cfg;\n"
	       "# delete this file to start with the settings from there.\n"
	       "#\n"
	       "# soundfont: path of the SoundFont\n"
	       "# bank:      bank of the instrument on MIDI channels 1-6\n"
	       "# program:   program number (0-127) of the instrument on MIDI channels 1-6\n"
	       "# drum_bank, drum_program: drum kit on MIDI channel 10\n"
	       "# volume:    master volume (0-100)\n"
	       "# display_channels: number of channels shown on the home screen (1-16)\n"
	       "# display_standby: seconds without activity before the display turns off (0: never)\n"
	       "# tripleplay, tripleplay_software_mode, tripleplay_program_change_presets: as\n"
	       "#   enabled/software_mode/program_change_presets in [tripleplay]\n"
	       "# preset: current preset (from 0)\n"
	       "# reverb_*, chorus_*: effect settings, as in mt32-pi.cfg\n";

	Line.Format("[%s]\n", StateSection);
	Text.Append(Line);

	if (SoundFontPath.GetLength())
	{
		Line.Format("soundfont = %s\n", static_cast<const char*>(SoundFontPath));
		Text.Append(Line);
	}

	if (nProgram >= 0)
	{
		Line.Format("bank = %d\nprogram = %d\n", nBank, nProgram);
		Text.Append(Line);
	}

	if (nDrumProgram >= 0)
	{
		Line.Format("drum_bank = %d\ndrum_program = %d\n", nDrumBank, nDrumProgram);
		Text.Append(Line);
	}

	if (nVolume >= 0)
	{
		Line.Format("volume = %d\n", nVolume);
		Text.Append(Line);
	}

	if (nDisplayChannels >= 0)
	{
		Line.Format("display_channels = %d\n", nDisplayChannels);
		Text.Append(Line);
	}

	if (nDisplayStandby >= 0)
	{
		Line.Format("display_standby = %d\n", nDisplayStandby);
		Text.Append(Line);
	}

	if (nTriplePlay >= 0)
	{
		Line.Format("tripleplay = %s\n", nTriplePlay ? "on" : "off");
		Text.Append(Line);
	}

	if (nTriplePlaySoftwareMode >= 0)
	{
		Line.Format("tripleplay_software_mode = %s\n", nTriplePlaySoftwareMode ? "on" : "off");
		Text.Append(Line);
	}

	if (nTriplePlayProgramPresets >= 0)
	{
		Line.Format("tripleplay_program_change_presets = %s\n", nTriplePlayProgramPresets ? "on" : "off");
		Text.Append(Line);
	}

	if (nPreset >= 0)
	{
		Line.Format("preset = %d\n", nPreset);
		Text.Append(Line);
	}

	#define FORMAT_BOOL(NAME, MEMBER)                                           \
		if (Effects.MEMBER)                                                 \
		{                                                                   \
			Line.Format(NAME " = %s\n", Effects.MEMBER.Value() ? "on" : "off"); \
			Text.Append(Line);                                          \
		}

	#define FORMAT_FLOAT(NAME, MEMBER)                                       \
		if (Effects.MEMBER)                                              \
		{                                                                \
			Line.Format(NAME " = %.2f\n", Effects.MEMBER.Value()); \
			Text.Append(Line);                                       \
		}

	FORMAT_BOOL("reverb", bReverbActive);
	FORMAT_FLOAT("reverb_level", nReverbLevel);
	FORMAT_FLOAT("reverb_room_size", nReverbRoomSize);
	FORMAT_FLOAT("reverb_damping", nReverbDamping);
	FORMAT_FLOAT("reverb_width", nReverbWidth);
	FORMAT_BOOL("chorus", bChorusActive);
	FORMAT_FLOAT("chorus_level", nChorusLevel);
	FORMAT_FLOAT("chorus_depth", nChorusDepth);
	FORMAT_FLOAT("chorus_speed", nChorusSpeed);

	if (Effects.nChorusVoices)
	{
		Line.Format("chorus_voices = %d\n", Effects.nChorusVoices.Value());
		Text.Append(Line);
	}

	#undef FORMAT_BOOL
	#undef FORMAT_FLOAT
}

CLastStateSaver::CLastStateSaver(CLastState& State, const char* pPath)
	: m_State(State),
	  m_pPath(pPath)
{
}

void CLastStateSaver::Run()
{
	while (true)
	{
		m_Event.Wait();
		m_Event.Clear();

		// Tasks only switch while the file is being written, after the text has been made from the settings
		m_State.Save(m_pPath);
	}
}

int CLastState::INIHandler(void* pUser, const char* pSection, const char* pName, const char* pValue)
{
	CLastState* const pState = static_cast<CLastState*>(pUser);

	if (strcmp(pSection, StateSection))
		return 0;

	if (!strcmp(pName, "soundfont"))
		return CConfig::ParseOption(pValue, &pState->SoundFontPath);
	if (!strcmp(pName, "bank"))
		return CConfig::ParseOption(pValue, &pState->nBank);
	if (!strcmp(pName, "program"))
		return CConfig::ParseOption(pValue, &pState->nProgram);
	if (!strcmp(pName, "drum_bank"))
		return CConfig::ParseOption(pValue, &pState->nDrumBank);
	if (!strcmp(pName, "drum_program"))
		return CConfig::ParseOption(pValue, &pState->nDrumProgram);
	if (!strcmp(pName, "volume"))
		return CConfig::ParseOption(pValue, &pState->nVolume);
	if (!strcmp(pName, "display_channels"))
		return CConfig::ParseOption(pValue, &pState->nDisplayChannels);
	if (!strcmp(pName, "display_standby"))
		return CConfig::ParseOption(pValue, &pState->nDisplayStandby);

	// Switches, stored as 0/1
	auto ParseSwitch = [pValue](int* pSwitch)
	{
		bool bValue;
		if (!CConfig::ParseOption(pValue, &bValue))
			return 0;
		*pSwitch = bValue;
		return 1;
	};

	if (!strcmp(pName, "tripleplay"))
		return ParseSwitch(&pState->nTriplePlay);
	if (!strcmp(pName, "tripleplay_software_mode"))
		return ParseSwitch(&pState->nTriplePlaySoftwareMode);
	if (!strcmp(pName, "tripleplay_program_change_presets"))
		return ParseSwitch(&pState->nTriplePlayProgramPresets);
	// From when presets could be switched off; they are always used now
	if (!strcmp(pName, "preset_mode"))
		return 1;
	if (!strcmp(pName, "preset"))
		return CConfig::ParseOption(pValue, &pState->nPreset);

	#define MATCH(NAME, TYPE, MEMBER)                          \
		if (!strcmp(NAME, pName))                          \
		{                                                  \
			TYPE Value;                                \
			if (!CConfig::ParseOption(pValue, &Value)) \
				return 0;                          \
			pState->Effects.MEMBER = Value;            \
			return 1;                                  \
		}

	MATCH("reverb", bool, bReverbActive);
	MATCH("reverb_level", float, nReverbLevel);
	MATCH("reverb_room_size", float, nReverbRoomSize);
	MATCH("reverb_damping", float, nReverbDamping);
	MATCH("reverb_width", float, nReverbWidth);
	MATCH("chorus", bool, bChorusActive);
	MATCH("chorus_level", float, nChorusLevel);
	MATCH("chorus_depth", float, nChorusDepth);
	MATCH("chorus_speed", float, nChorusSpeed);
	MATCH("chorus_voices", int, nChorusVoices);

	#undef MATCH

	return 0;
}
