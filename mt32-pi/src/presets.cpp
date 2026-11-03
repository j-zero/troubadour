//
// presets.cpp
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
#include <circle/string.h>
#include <circle/util.h>
#include <fatfs/ff.h>
#include <ini.h>

#include <cstdio>
#include <cstdlib>

#include "config.h"
#include "presets.h"

LOGMODULE("presets");

const char PresetSection[] = "preset";
const char FilePrefix[] = "preset";
const char FileSuffix[] = ".cfg";

// In the order of CSoundFontSynth::TEffect, as in mt32-pi.cfg
static const char* const EffectKeys[CPresets::EffectCount] =
{
	"reverb_level", "reverb_room_size", "reverb_damping", "reverb_width",
	"chorus_level", "chorus_depth", "chorus_speed", "chorus_voices",
};

CPresets::CPresets(const char* pDirectory)
	: m_pDirectory(pDirectory),
	  m_pEntries(new TEntry[MaxPresets]),
	  m_nCount(0),
	  m_nNextNumber(1)
{
}

CPresets::~CPresets()
{
	delete[] m_pEntries;
}

bool CPresets::Load()
{
	DIR Directory;
	FILINFO FileInfo;
	if (f_opendir(&Directory, m_pDirectory) != FR_OK)
	{
		// Created for the first preset
		f_mkdir(m_pDirectory);
		return false;
	}

	// The preset files ("preset<number>.cfg"), in the order of their names
	const size_t nPrefixLength = strlen(FilePrefix);
	while (m_nCount < MaxPresets && f_readdir(&Directory, &FileInfo) == FR_OK && FileInfo.fname[0])
	{
		const char* pName = FileInfo.fname;
		const size_t nLength = strlen(pName);
		if ((FileInfo.fattrib & AM_DIR) || nLength >= FileNameLength || nLength <= nPrefixLength + strlen(FileSuffix)
		    || strncasecmp(pName, FilePrefix, nPrefixLength) || strcasecmp(pName + nLength - strlen(FileSuffix), FileSuffix))
			continue;

		size_t i = m_nCount;
		while (i > 0 && strcasecmp(m_pEntries[i - 1].FileName, pName) > 0)
		{
			m_pEntries[i] = m_pEntries[i - 1];
			--i;
		}
		strcpy(m_pEntries[i].FileName, pName);
		++m_nCount;

		const unsigned nNumber = strtoul(pName + nPrefixLength, nullptr, 10);
		if (nNumber >= m_nNextNumber)
			m_nNextNumber = nNumber + 1;
	}
	f_closedir(&Directory);

	// Files that can't be read are left out
	size_t nLoaded = 0;
	for (size_t i = 0; i < m_nCount; ++i)
	{
		if (LoadFile(m_pEntries[i]))
			m_pEntries[nLoaded++] = m_pEntries[i];
	}
	m_nCount = nLoaded;

	LOGNOTE("%u presets", static_cast<unsigned>(m_nCount));
	return true;
}

bool CPresets::Add(const TPreset& Preset)
{
	if (m_nCount >= MaxPresets)
		return false;

	TEntry& Entry = m_pEntries[m_nCount++];
	snprintf(Entry.FileName, sizeof(Entry.FileName), "%s%02u%s", FilePrefix, m_nNextNumber++, FileSuffix);
	Entry.Preset = Preset;
	Entry.bChanged = true;
	return true;
}

void CPresets::Replace(size_t nIndex, const TPreset& Preset)
{
	if (nIndex >= m_nCount)
		return;

	m_pEntries[nIndex].Preset = Preset;
	m_pEntries[nIndex].bChanged = true;
}

bool CPresets::HasChanges() const
{
	for (size_t i = 0; i < m_nCount; ++i)
		if (m_pEntries[i].bChanged)
			return true;

	return false;
}

void CPresets::SaveChanged()
{
	for (size_t i = 0; i < m_nCount; ++i)
	{
		TEntry& Entry = m_pEntries[i];
		if (!Entry.bChanged)
			continue;

		// Cleared first, so that a change while the file is being written is saved the next time
		Entry.bChanged = false;
		if (!SaveFile(Entry))
			Entry.bChanged = true;
	}
}

void CPresets::Delete(size_t nIndex)
{
	if (nIndex >= m_nCount)
		return;

	char Path[PathLength];
	MakePath(m_pEntries[nIndex], Path, sizeof(Path));
	if (f_unlink(Path) != FR_OK)
		LOGWARN("Couldn't delete '%s'", Path);

	for (size_t i = nIndex + 1; i < m_nCount; ++i)
		m_pEntries[i - 1] = m_pEntries[i];
	--m_nCount;
}

void CPresets::Clear(TPreset& Preset)
{
	Preset.Name[0] = '\0';
	Preset.SoundFontPath[0] = '\0';
	Preset.nBank = Preset.nProgram = 0;
	Preset.bHasEffects = false;
	for (float& nValue : Preset.Effects)
		nValue = 0.0f;
	Preset.nTPTranspose = Preset.nTPBendMode = Preset.nTPBendRange = Preset.nTPChannelMode = Unknown;
	for (int& nLevel : Preset.TPSensitivity)
		nLevel = Unknown;
	for (TChannel& Channel : Preset.Channels)
		Clear(Channel);
}

void CPresets::Clear(TChannel& Channel)
{
	Channel.bEnabled = true;
	Channel.nBank = Channel.nProgram = Unknown;
	Channel.nSplitNote = NoSplit;
	Channel.nUpperBank = Channel.nUpperProgram = Unknown;
}

bool CPresets::IsDefault(const TChannel& Channel)
{
	return Channel.bEnabled && Channel.nProgram == Unknown && Channel.nSplitNote == NoSplit;
}

void CPresets::MakePath(const TEntry& Entry, char* pBuffer, size_t nSize) const
{
	snprintf(pBuffer, nSize, "%s/%s", m_pDirectory, Entry.FileName);
}

bool CPresets::LoadFile(TEntry& Entry)
{
	char Path[PathLength];
	MakePath(Entry, Path, sizeof(Path));

	FIL File;
	if (f_open(&File, Path, FA_READ) != FR_OK)
		return false;

	const UINT nSize = f_size(&File);
	char* pBuffer = new char[nSize + 1];
	UINT nRead;
	const FRESULT Result = f_read(&File, pBuffer, nSize, &nRead);
	f_close(&File);

	if (Result != FR_OK)
	{
		LOGERR("Error reading '%s'", Path);
		delete[] pBuffer;
		return false;
	}

	pBuffer[nRead] = '\0';
	Clear(Entry.Preset);
	Entry.bChanged = false;
	const int nResult = ini_parse_string(pBuffer, INIHandler, &Entry.Preset);
	delete[] pBuffer;

	if (nResult > 0)
		LOGWARN("'%s' parse error on line %d", Path, nResult);

	return nResult >= 0;
}

bool CPresets::SaveFile(TEntry& Entry)
{
	const TPreset& Preset = Entry.Preset;
	CString Text;
	CString Line;

	// The text is made before writing; tasks only switch while the file is being written
	Line.Format("# Preset, saved automatically while it is current. Effects as in mt32-pi.cfg;\n"
		    "# tp_transpose in semitones, tp_sensitivity_1 (high E) to _6 from 1 to 16.\n"
		    "# channel_N (1-16): on/off; channel_N_instrument: bank:program (otherwise the\n"
		    "# preset's for channels 1-6, the SoundFont's default for the others);\n"
		    "# channel_N_split: MIDI note (60 = C4) from which channel_N_upper plays.\n"
		    "[%s]\nname = %s\nsoundfont = %s\nbank = %d\nprogram = %d\n",
		    PresetSection, Preset.Name, Preset.SoundFontPath, Preset.nBank, Preset.nProgram);
	Text.Append(Line);

	if (Preset.bHasEffects)
	{
		for (u8 nEffect = 0; nEffect < EffectCount; ++nEffect)
		{
			Line.Format("%s = %.2f\n", EffectKeys[nEffect], Preset.Effects[nEffect]);
			Text.Append(Line);
		}
	}

	if (Preset.nTPTranspose != Unknown)
	{
		Line.Format("tp_transpose = %d\n", Preset.nTPTranspose);
		Text.Append(Line);
	}

	if (Preset.nTPBendMode != Unknown)
	{
		Line.Format("tp_bend_mode = %d\n", Preset.nTPBendMode);
		Text.Append(Line);
	}

	if (Preset.nTPBendRange != Unknown)
	{
		Line.Format("tp_bend_range = %d\n", Preset.nTPBendRange);
		Text.Append(Line);
	}

	if (Preset.nTPChannelMode != Unknown)
	{
		Line.Format("tp_channel_mode = %d\n", Preset.nTPChannelMode);
		Text.Append(Line);
	}

	for (u8 nString = 0; nString < TPStrings; ++nString)
	{
		if (Preset.TPSensitivity[nString] != Unknown)
		{
			Line.Format("tp_sensitivity_%u = %d\n", nString + 1, Preset.TPSensitivity[nString]);
			Text.Append(Line);
		}
	}

	// Only what differs from the default
	for (u8 nChannel = 0; nChannel < MIDIChannels; ++nChannel)
	{
		const TChannel& Channel = Preset.Channels[nChannel];
		if (!Channel.bEnabled)
		{
			Line.Format("channel_%u = off\n", nChannel + 1);
			Text.Append(Line);
		}
		if (Channel.nProgram != Unknown)
		{
			Line.Format("channel_%u_instrument = %d:%d\n", nChannel + 1, Channel.nBank, Channel.nProgram);
			Text.Append(Line);
		}
		if (Channel.nSplitNote != NoSplit)
		{
			Line.Format("channel_%u_split = %d\nchannel_%u_upper = %d:%d\n", nChannel + 1, Channel.nSplitNote,
				    nChannel + 1, Channel.nUpperBank, Channel.nUpperProgram);
			Text.Append(Line);
		}
	}

	char Path[PathLength];
	MakePath(Entry, Path, sizeof(Path));

	FIL File;
	if (f_open(&File, Path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		// The directory may not exist yet
		f_mkdir(m_pDirectory);
		if (f_open(&File, Path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
		{
			LOGERR("Couldn't open '%s' for writing", Path);
			return false;
		}
	}

	UINT nWritten;
	const UINT nSize = Text.GetLength();
	const FRESULT Result = f_write(&File, static_cast<const char*>(Text), nSize, &nWritten);

	if (f_close(&File) != FR_OK || Result != FR_OK || nWritten != nSize)
	{
		LOGERR("Error writing '%s'", Path);
		return false;
	}

	LOGNOTE("Saved '%s'", Path);
	return true;
}

int CPresets::INIHandler(void* pUser, const char* pSection, const char* pName, const char* pValue)
{
	TPreset& Preset = *static_cast<TPreset*>(pUser);

	if (strcmp(pSection, PresetSection))
		return 0;

	if (!strcmp(pName, "name"))
	{
		snprintf(Preset.Name, sizeof(Preset.Name), "%s", pValue);
		return 1;
	}

	if (!strcmp(pName, "soundfont"))
	{
		snprintf(Preset.SoundFontPath, sizeof(Preset.SoundFontPath), "%s", pValue);
		return 1;
	}

	for (u8 nEffect = 0; nEffect < EffectCount; ++nEffect)
	{
		if (!strcmp(pName, EffectKeys[nEffect]))
		{
			Preset.bHasEffects = true;
			return CConfig::ParseOption(pValue, &Preset.Effects[nEffect]);
		}
	}

	// channel_N, channel_N_instrument, channel_N_split, channel_N_upper
	static constexpr char ChannelKey[] = "channel_";
	constexpr size_t nChannelKeyLength = sizeof(ChannelKey) - 1;
	if (!strncmp(pName, ChannelKey, nChannelKeyLength))
	{
		char* pEnd;
		const long nChannel = strtol(pName + nChannelKeyLength, &pEnd, 10);
		if (nChannel < 1 || nChannel > MIDIChannels || pEnd == pName + nChannelKeyLength)
			return 0;
		TChannel& Channel = Preset.Channels[nChannel - 1];

		// "bank:program"
		auto ParseInstrument = [pValue](int& nBank, int& nProgram)
		{
			char* pColon;
			const long nParsedBank = strtol(pValue, &pColon, 10);
			if (*pColon != ':')
				return 0;
			char* pProgramEnd;
			const long nParsedProgram = strtol(pColon + 1, &pProgramEnd, 10);
			if (*pProgramEnd || nParsedBank < 0 || nParsedBank > 0xFFFF || nParsedProgram < 0 || nParsedProgram > 127)
				return 0;
			nBank = nParsedBank;
			nProgram = nParsedProgram;
			return 1;
		};

		if (!*pEnd)
			return CConfig::ParseOption(pValue, &Channel.bEnabled);
		if (!strcmp(pEnd, "_instrument"))
			return ParseInstrument(Channel.nBank, Channel.nProgram);
		if (!strcmp(pEnd, "_upper"))
			return ParseInstrument(Channel.nUpperBank, Channel.nUpperProgram);
		if (!strcmp(pEnd, "_split"))
		{
			int nNote;
			if (!CConfig::ParseOption(pValue, &nNote) || nNote < 0 || nNote > 127)
				return 0;
			Channel.nSplitNote = nNote;
			return 1;
		}
		return 0;
	}

	int* pInt = !strcmp(pName, "bank") ? &Preset.nBank
		  : !strcmp(pName, "program") ? &Preset.nProgram
		  : !strcmp(pName, "tp_transpose") ? &Preset.nTPTranspose
		  : !strcmp(pName, "tp_bend_mode") ? &Preset.nTPBendMode
		  : !strcmp(pName, "tp_bend_range") ? &Preset.nTPBendRange
		  : !strcmp(pName, "tp_channel_mode") ? &Preset.nTPChannelMode
		  : nullptr;

	// tp_sensitivity_1 to tp_sensitivity_6
	static constexpr char SensitivityKey[] = "tp_sensitivity_";
	constexpr size_t nKeyLength = sizeof(SensitivityKey) - 1;
	if (!pInt && !strncmp(pName, SensitivityKey, nKeyLength) && pName[nKeyLength] >= '1'
	    && pName[nKeyLength] < '1' + TPStrings && !pName[nKeyLength + 1])
		pInt = &Preset.TPSensitivity[pName[nKeyLength] - '1'];

	return pInt ? CConfig::ParseOption(pValue, pInt) : 0;
}
