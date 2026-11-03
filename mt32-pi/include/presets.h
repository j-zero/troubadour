//
// presets.h
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

#ifndef _presets_h
#define _presets_h

#include <circle/sched/synchronizationevent.h>
#include <circle/sched/task.h>
#include <circle/types.h>

#include "synth/soundfontsynth.h"

// Presets that the footswitches switch between: SoundFont, instrument, effects and the TriplePlay's transpose, bend
// mode and range, channel mode and string sensitivities. Each preset is an INI file of its own in a directory; they are in the order of their file names.
class CPresets
{
public:
	static constexpr size_t MaxPresets = 64;
	static constexpr size_t PathLength = 128;
	static constexpr size_t NameLength = 40;
	static constexpr size_t FileNameLength = 16;
	static constexpr u8 EffectCount = static_cast<u8>(CSoundFontSynth::TEffect::Count);
	static constexpr int Unknown = -1000;
	static constexpr u8 TPStrings = 6;
	static constexpr u8 MIDIChannels = 16;
	static constexpr int NoSplit = -1;

	// What a MIDI channel plays in the preset
	struct TChannel
	{
		bool bEnabled;		// Muted otherwise
		int nBank;		// The channel's own instrument, or Unknown for the preset's (channels 1-6) or the
		int nProgram;		// SoundFont's default one (the others)
		int nSplitNote;		// From this note upwards, the upper instrument; NoSplit for none
		int nUpperBank;
		int nUpperProgram;
		int nTranspose;		// Semitones, below the split note (or all notes) and from it upwards
		int nUpperTranspose;
	};

	static constexpr int MaxChannelTranspose = 48;

	struct TPreset
	{
		char Name[NameLength];			// The instrument, for the display
		char SoundFontPath[PathLength];
		int nBank;
		int nProgram;
		bool bHasEffects;
		float Effects[EffectCount];		// In the order of CSoundFontSynth::TEffect
		// TriplePlay settings, each Unknown if not stored
		int nTPTranspose;			// Semitones
		int nTPBendMode;
		int nTPBendRange;
		int nTPChannelMode;
		int TPSensitivity[TPStrings];		// 1-16, high E first
		TChannel Channels[MIDIChannels];
	};

	CPresets(const char* pDirectory);
	~CPresets();

	bool Load();

	size_t GetCount() const { return m_nCount; }
	const TPreset& Get(size_t nIndex) const { return m_pEntries[nIndex].Preset; }

	// Changes are written to the files by SaveChanged(); returns false if there's no room for another preset
	bool Add(const TPreset& Preset);
	void Replace(size_t nIndex, const TPreset& Preset);
	bool HasChanges() const;
	void SaveChanged();

	// Deletes the file right away
	void Delete(size_t nIndex);

	static void Clear(TPreset& Preset);
	static void Clear(TChannel& Channel);
	static bool IsDefault(const TChannel& Channel);

private:
	struct TEntry
	{
		char FileName[FileNameLength];
		TPreset Preset;
		bool bChanged;		// Tasks only switch while a file is being read or written
	};

	bool LoadFile(TEntry& Entry);
	bool SaveFile(TEntry& Entry);
	void MakePath(const TEntry& Entry, char* pBuffer, size_t nSize) const;
	static int INIHandler(void* pUser, const char* pSection, const char* pName, const char* pValue);

	const char* m_pDirectory;
	TEntry* m_pEntries;
	size_t m_nCount;
	unsigned m_nNextNumber;		// For the file name of the next new preset
};

// Writes changed presets in a task of its own, so that the main task keeps handling MIDI meanwhile
class CPresetSaver : public CTask
{
public:
	CPresetSaver(CPresets& Presets) : m_Presets(Presets) {}

	void Trigger() { m_Event.Set(); }

	virtual void Run() override
	{
		while (true)
		{
			m_Event.Wait();
			m_Event.Clear();
			m_Presets.SaveChanged();
		}
	}

private:
	CPresets& m_Presets;
	CSynchronizationEvent m_Event;
};

#endif
