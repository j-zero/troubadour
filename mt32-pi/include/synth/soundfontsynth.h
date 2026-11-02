//
// soundfontsynth.h
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

#ifndef _soundfontsynth_h
#define _soundfontsynth_h

#include <circle/sched/mutex.h>
#include <circle/spinlock.h>
#include <circle/types.h>

#include <fluidsynth.h>

#include "soundfontmanager.h"
#include "synth/fxprofile.h"
#include "synth/synthbase.h"

class CSoundFontSynth : public CSynthBase
{
public:
	// Effect parameters that can be changed at runtime
	enum class TEffect : u8
	{
		ReverbLevel,
		ReverbRoomSize,
		ReverbDamping,
		ReverbWidth,
		ChorusLevel,
		ChorusDepth,
		ChorusSpeed,
		ChorusVoices,
		Count
	};

	struct TEffectInfo
	{
		const char* pName;
		float nMin;
		float nMax;
		float nStep;
		u8 nDecimals;
	};

	// Called with the number of bytes read so far while a SoundFont or instrument takes long to load, from the
	// task that is loading it; the total is the amount of sample data being loaded, or 0 if unknown
	using TLoadProgressHandler = void (*)(u64 nLoadedBytes, u64 nTotalBytes, void* pContext);

	CSoundFontSynth(unsigned nSampleRate);
	virtual ~CSoundFontSynth() override;

	// CSynthBase
	virtual bool Initialize() override;
	virtual void HandleMIDIShortMessage(u32 nMessage) override;
	virtual void HandleMIDISysExMessage(const u8* pData, size_t nSize) override;
	virtual bool IsActive() override;
	virtual void AllSoundOff() override;
	virtual void SetMasterVolume(u8 nVolume) override;
	virtual size_t Render(s16* pOutBuffer, size_t nFrames) override;
	virtual size_t Render(float* pOutBuffer, size_t nFrames) override;
	virtual void ReportStatus() const override;
	virtual void GetChannelLevels(unsigned int nTicks, float* pLevels, float* pPeaks) override;

	// What the display shows; may be called from another core
	struct TDisplayInfo
	{
		char SoundFontName[64];
		char ProgramName[32];	// Instrument of MIDI channel 1; empty if there is none
		int nBank;
		int nProgram;
	};
	void GetDisplayInfo(TDisplayInfo& Info);

	static void SetLoadProgressHandler(TLoadProgressHandler pHandler, void* pContext);

	// SoundFont to load by Initialize() instead of the one from the config file, if available
	void SetInitialSoundFont(const char* pPath) { m_InitialSoundFontPath = pPath; }

	bool SwitchSoundFont(size_t nIndex);
	size_t GetSoundFontIndex() const { return m_nCurrentSoundFontIndex; }
	CSoundFontManager& GetSoundFontManager() { return m_SoundFontManager; }

	// Melodic presets of the current SoundFont, for selecting an instrument on several channels at once
	size_t GetPresetCount() const { return m_nPresets; }
	size_t GetChannelPresetIndex(u8 nChannel);
	bool GetPreset(size_t nIndex, int* pBank, int* pProgram);
	bool FindPreset(int nBank, int nProgram, size_t* pIndex);
	void FormatPresetName(size_t nIndex, char* pBuffer, size_t nSize);
	// Returns false if the instrument didn't fit into memory; the channels have no instrument then
	bool SetChannelsPreset(size_t nIndex, u8 nFirstChannel, u8 nChannels, bool* pSamplesLoaded = nullptr);

	// Effect values set here take precedence over the SoundFont's effects profile and the config file, also
	// after switching SoundFonts. Setting the reverb or chorus level to 0 turns the effect off.
	static const TEffectInfo& GetEffectInfo(TEffect Effect);
	float GetEffect(TEffect Effect);
	void SetEffect(TEffect Effect, float nValue);
	const TFXProfile& GetEffectOverrides() const { return m_EffectOverrides; }
	void SetEffectOverrides(const TFXProfile& Overrides) { m_EffectOverrides = Overrides; }

private:
	struct TPreset
	{
		u16 nBank;
		u8 nProgram;
	};

	static constexpr u8 MIDIChannels = 16;
	static constexpr int DrumBank = 128;

	// Must be called with the synth lock held
	void BuildPresetList();
	void LoadDefaultProgram(u8 nChannel);
	void UpdateDefaultPrograms();
	void UpdateProgramName();
	void UpdateSoundFontName();

	bool Reinitialize(const char* pSoundFontPath, const TFXProfile* pFXProfile);
	void ResetMIDIMonitor();
#ifndef NDEBUG
	void DumpFXSettings() const;
#endif
	bool ParseGMSysEx(const u8* pData, size_t nSize);
	bool ParseRolandSysEx(const u8* pData, size_t nSize);
	bool ParseYamahaSysEx(const u8* pData, size_t nSize);

	fluid_settings_t* m_pSettings;
	fluid_synth_t* m_pSynth;
	CMutex m_TaskLock;

	u8 m_nVolume;
	float m_nInitialGain;

	TFXProfile m_EffectOverrides;
	bool m_bReverbActive;
	bool m_bChorusActive;

	u16 m_nPercussionMask;
	size_t m_nCurrentSoundFontIndex;

	CSoundFontManager m_SoundFontManager;
	CString m_InitialSoundFontPath;

	int m_nSoundFontID;
	TPreset* m_pPresets;
	size_t m_nPresets;
	size_t m_nOutOfMemoryPresetIndex;

	// Channels whose default program hasn't been loaded yet
	int m_DefaultBanks[MIDIChannels];
	int m_DefaultPrograms[MIDIChannels];
	u16 m_nDefaultProgramMask;

	// For the display, which is updated from another core
	CSpinLock m_DisplayInfoLock;
	TDisplayInfo m_DisplayInfo;

	static void FluidSynthLogCallback(int nLevel, const char* pMessage, void* pUser);
};

#endif
