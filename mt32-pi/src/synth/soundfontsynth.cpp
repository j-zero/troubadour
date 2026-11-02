//
// soundfontsynth.cpp
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

#include <fatfs/ff.h>
#include <circle/logger.h>
#include <circle/spinlock.h>
#include <circle/timer.h>

#include "config.h"
#include "lcd/ui.h"
#include "synth/gmsysex.h"
#include "synth/rolandsysex.h"
#include "synth/soundfontsynth.h"
#include "synth/yamahasysex.h"
#include "utility.h"
#include "zoneallocator.h"

LOGMODULE("soundfontsynth");
const char SoundFontPath[] = "soundfonts";

// Counts allocations that failed, so that an instrument that didn't fit into memory can be detected
static unsigned s_nFailedAllocs = 0;

extern "C"
{
	// Replacements for fluid_sys.c functions
	void* fluid_alloc(size_t len)
	{
		void* ptr = CZoneAllocator::Get()->Alloc(len, TZoneTag::FluidSynth);
		if (!ptr && len)
			++s_nFailedAllocs;
		return ptr;
	}

	void* fluid_realloc(void* ptr, size_t len)
	{
		void* new_ptr = CZoneAllocator::Get()->Realloc(ptr, len, TZoneTag::FluidSynth);
		if (!new_ptr && len)
			++s_nFailedAllocs;
		return new_ptr;
	}

	void fluid_free(void* ptr)
	{
		CZoneAllocator::Get()->Free(ptr);
	}
}

// Enough for roughly 500 file fragments; FatFs falls back on walking the FAT if a file needs more
constexpr size_t ClusterMapSize = 1024;

struct TSoundFontFile
{
	FIL File;
	DWORD ClusterMap[ClusterMapSize];
};

static CSpinLock* s_pSynthLock = nullptr;

// While set, the UI shows a spinner as soon as FluidSynth starts loading sample data
static CUserInterface* s_pLoadingUI = nullptr;
static bool s_bSamplesLoaded = false;

// Progress of reading the SoundFont file, for long loads
constexpr unsigned LoadProgressPeriodMicros = 250000;
constexpr u64 BytesPerMegabyte = 1024 * 1024;
static CSoundFontSynth::TLoadProgressHandler s_pLoadProgressHandler = nullptr;
static void* s_pLoadProgressContext = nullptr;
static u64 s_nLoadedBytes = 0;
static u64 s_nTotalBytes = 0;
static unsigned s_nLoadProgressTime = 0;

// Our addition to FluidSynth: called with the amount of sample data that is about to be loaded
extern "C" void (*fluid_defsfont_preset_load_hook)(fluid_long_long_t nBytes);

static void PresetLoadHook(fluid_long_long_t nBytes)
{
	s_nTotalBytes  = nBytes;
	s_nLoadedBytes = 0;
}

// With dynamic sample loading, FluidSynth reads from the SD card from within calls that are made with the
// synth lock held. The lock must be dropped for the duration of any file access; the audio task would stall
// otherwise, and the SD card driver sleeps in the scheduler, so another task could spin on the lock forever.
// This is safe because the audio task only touches voices and the sample data they already hold.
class CUnlockedFileAccess
{
public:
	CUnlockedFileAccess() { s_pSynthLock->Release(); }
	~CUnlockedFileAccess() { s_pSynthLock->Acquire(); }
};

// File callbacks for FluidSynth's SoundFont loader; must be called with the synth lock held
// These were found to be much faster than FluidSynth's default approach of going through libc
static void* SoundFontOpen(const char* pPath)
{
	CUnlockedFileAccess Unlocked;

	// FluidSynth opens the file whenever a preset needs sample data that isn't in memory
	if (s_pLoadingUI && !s_bSamplesLoaded)
		s_pLoadingUI->ShowSystemMessage("Loading samples", true);
	s_bSamplesLoaded = true;

	s_nLoadedBytes      = 0;
	s_nLoadProgressTime = CTimer::GetClockTicks();

	TSoundFontFile* pFile = new TSoundFontFile;
	if (f_open(&pFile->File, pPath, FA_READ) != FR_OK)
	{
		delete pFile;
		return nullptr;
	}

	// Seeks need to walk the FAT from the start of the file without a cluster map, which is far too slow for
	// picking individual samples out of a large SoundFont
	pFile->ClusterMap[0] = ClusterMapSize;
	pFile->File.cltbl = pFile->ClusterMap;
	if (f_lseek(&pFile->File, CREATE_LINKMAP) != FR_OK)
		pFile->File.cltbl = nullptr;

	return pFile;
}

static int SoundFontClose(void* pHandle)
{
	CUnlockedFileAccess Unlocked;

	TSoundFontFile* pFile = static_cast<TSoundFontFile*>(pHandle);
	const FRESULT Result = f_close(&pFile->File);
	delete pFile;

	// Loading is complete
	s_nTotalBytes = 0;

	return Result == FR_OK ? FLUID_OK : FLUID_FAILED;
}

static fluid_long_long_t SoundFontTell(void* pHandle)
{
	return f_tell(&static_cast<TSoundFontFile*>(pHandle)->File);
}

static int SoundFontRead(void* pBuffer, fluid_long_long_t nCount, void* pHandle)
{
	CUnlockedFileAccess Unlocked;

	UINT nRead;
	const FRESULT Result = f_read(&static_cast<TSoundFontFile*>(pHandle)->File, pBuffer, nCount, &nRead);

	// Only sample data adds up to megabytes; the SoundFont's structure is much smaller
	s_nLoadedBytes += nRead;
	const unsigned nTicks = CTimer::GetClockTicks();
	if (s_pLoadProgressHandler && s_nLoadedBytes >= BytesPerMegabyte && nTicks - s_nLoadProgressTime >= LoadProgressPeriodMicros)
	{
		s_nLoadProgressTime = nTicks;
		s_pLoadProgressHandler(s_nLoadedBytes, s_nTotalBytes, s_pLoadProgressContext);
	}

	return Result == FR_OK && nRead == nCount ? FLUID_OK : FLUID_FAILED;
}

static int SoundFontSeek(void* pHandle, fluid_long_long_t nOffset, int nWhence)
{
	CUnlockedFileAccess Unlocked;

	FIL* pFile = &static_cast<TSoundFontFile*>(pHandle)->File;

	switch (nWhence)
	{
	case SEEK_CUR:
		nOffset += f_tell(pFile);
		break;

	case SEEK_END:
		nOffset += f_size(pFile);
		break;

	default:
		break;
	}

	return f_lseek(pFile, nOffset) == FR_OK ? FLUID_OK : FLUID_FAILED;
}

// Serializes tasks using the synth, as a file access in one task can let another task run
class CTaskLockHolder
{
public:
	CTaskLockHolder(CMutex& Mutex) : m_Mutex(Mutex) { m_Mutex.Acquire(); }
	~CTaskLockHolder() { m_Mutex.Release(); }

private:
	CMutex& m_Mutex;
};

// Reports any sample loading caused by a MIDI message or instrument change for the duration of its scope
class CSampleLoadReporter
{
public:
	CSampleLoadReporter(CUserInterface* pUI)
	{
		s_pLoadingUI     = pUI;
		s_bSamplesLoaded = false;
	}

	~CSampleLoadReporter()
	{
		if (s_bSamplesLoaded && s_pLoadingUI)
			s_pLoadingUI->ClearSpinnerMessage();

		s_pLoadingUI = nullptr;
	}

	bool SamplesLoaded() const { return s_bSamplesLoaded; }
};

// Order of the instrument list: by bank, then program
static bool IsPresetBefore(int nBank, int nProgram, int nOtherBank, int nOtherProgram)
{
	return nBank < nOtherBank || (nBank == nOtherBank && nProgram < nOtherProgram);
}

static void FormatProgramName(char* pBuffer, size_t nSize, int nBank, int nProgram, const char* pName)
{
	if (nBank)
		snprintf(pBuffer, nSize, "%03d.%d %s", nProgram + 1, nBank, pName);
	else
		snprintf(pBuffer, nSize, "%03d %s", nProgram + 1, pName);
}

CSoundFontSynth::CSoundFontSynth(unsigned nSampleRate)
	: CSynthBase(nSampleRate),

	  m_pSettings(nullptr),
	  m_pSynth(nullptr),

	  m_nVolume(100),
	  m_nInitialGain(0.2f),

	  m_bReverbActive(false),
	  m_bChorusActive(false),

	  m_nPercussionMask(1 << 9),
	  m_nCurrentSoundFontIndex(0),

	  m_nSoundFontID(FLUID_FAILED),
	  m_PresetLists{},
	  m_DefaultBanks{0},
	  m_DefaultPrograms{0},
	  m_nDefaultProgramMask(0),

	  m_DisplayInfoLock(TASK_LEVEL),
	  m_DisplayInfo{}
{
}

CSoundFontSynth::~CSoundFontSynth()
{
	FreePresetLists();

	if (m_pSynth)
		delete_fluid_synth(m_pSynth);

	if (m_pSettings)
		delete_fluid_settings(m_pSettings);
}

void CSoundFontSynth::SetLoadProgressHandler(TLoadProgressHandler pHandler, void* pContext)
{
	s_pLoadProgressHandler = pHandler;
	s_pLoadProgressContext = pContext;
	fluid_defsfont_preset_load_hook = pHandler ? PresetLoadHook : nullptr;
}

void CSoundFontSynth::FluidSynthLogCallback(int nLevel, const char* pMessage, void* pUser)
{
	// FluidSynth reports running out of memory as a panic, but recovers from it; Circle's LogPanic would halt the system
	const TLogSeverity Severity = nLevel == FLUID_PANIC ? LogError : static_cast<TLogSeverity>(nLevel);
	CLogger::Get()->Write(From, Severity, pMessage);
}

bool CSoundFontSynth::Initialize()
{
	const CConfig* const pConfig = CConfig::Get();

	if (!m_SoundFontManager.ScanSoundFonts())
		return false;

	// Try to get preferred SoundFont
	size_t nInitialIndex;
	if (m_InitialSoundFontPath.GetLength() && m_SoundFontManager.FindSoundFont(m_InitialSoundFontPath, &nInitialIndex))
		m_nCurrentSoundFontIndex = nInitialIndex;
	else
		m_nCurrentSoundFontIndex = pConfig->FluidSynthSoundFont;
	const char* pSoundFontPath = m_SoundFontManager.GetSoundFontPath(m_nCurrentSoundFontIndex);

	// Fall back on first available SoundFont
	if (!pSoundFontPath)
	{
		pSoundFontPath = m_SoundFontManager.GetFirstValidSoundFontPath();
		m_nCurrentSoundFontIndex = 0;
	}

	// Give up
	if (!pSoundFontPath)
		return false;

	TFXProfile FXProfile = m_SoundFontManager.GetSoundFontFXProfile(m_nCurrentSoundFontIndex);

	// Install logging handlers
	fluid_set_log_function(FLUID_PANIC, FluidSynthLogCallback, this);
	fluid_set_log_function(FLUID_ERR, FluidSynthLogCallback, this);
	// fluid_set_log_function(FLUID_WARN, FluidSynthLogCallback, this);
	// fluid_set_log_function(FLUID_INFO, FluidSynthLogCallback, this);
	// fluid_set_log_function(FLUID_DBG, FluidSynthLogCallback, this);

	m_pSettings = new_fluid_settings();
	if (!m_pSettings)
	{
		LOGERR("Failed to create settings");
		return false;
	}

	// Set device ID to match the default Roland Sound Canvas ID so that it recognises some GS SysEx messages
	fluid_settings_setint(m_pSettings, "synth.device-id", static_cast<int>(TDeviceID::SoundCanvasDefault));
	fluid_settings_setnum(m_pSettings, "synth.sample-rate", static_cast<double>(m_nSampleRate));
	fluid_settings_setint(m_pSettings, "synth.threadsafe-api", false);
	fluid_settings_setint(m_pSettings, "synth.dynamic-sample-loading", pConfig->FluidSynthDynamicSamples);

	s_pSynthLock = &m_Lock;

	if (!Reinitialize(pSoundFontPath, &FXProfile))
		return false;

	UpdateSoundFontName();
	return true;
}

void CSoundFontSynth::HandleMIDIShortMessage(u32 nMessage)
{
	const u8 nStatus  = nMessage & 0xFF;
	const u8 nChannel = nMessage & 0x0F;
	const u8 nData1   = (nMessage >> 8) & 0xFF;
	const u8 nData2   = (nMessage >> 16) & 0xFF;

	CTaskLockHolder TaskLock(m_TaskLock);
	CSampleLoadReporter LoadReporter(m_pUI);

	// Handle system real-time messages
	if (nStatus == 0xFF)
	{
		m_Lock.Acquire();
		fluid_synth_system_reset(m_pSynth);
		UpdateDefaultPrograms();
		m_Lock.Release();
		return;
	}

	m_Lock.Acquire();

	// Handle channel messages
	switch (nStatus & 0xF0)
	{
		// Note off
		case 0x80:
			fluid_synth_noteoff(m_pSynth, nChannel, nData1);
			break;

		// Note on
		case 0x90:
			if (nData2 && (m_nDefaultProgramMask & (1 << nChannel)))
				LoadDefaultProgram(nChannel);
			fluid_synth_noteon(m_pSynth, nChannel, nData1, nData2);
			break;

		// Polyphonic key pressure/aftertouch
		case 0xA0:
			fluid_synth_key_pressure(m_pSynth, nChannel, nData1, nData2);
			break;

		// Control change
		case 0xB0:
			fluid_synth_cc(m_pSynth, nChannel, nData1, nData2);
			break;

		// Program change
		case 0xC0:
			fluid_synth_program_change(m_pSynth, nChannel, nData1);
			m_nDefaultProgramMask &= ~(1 << nChannel);
			if (nChannel == 0)
				UpdateProgramName();
			break;

		// Channel pressure/aftertouch
		case 0xD0:
			fluid_synth_channel_pressure(m_pSynth, nChannel, nData1);
			break;

		// Pitch bend
		case 0xE0:
			fluid_synth_pitch_bend(m_pSynth, nChannel, (nData2 << 7) | nData1);
			break;
	}

	m_Lock.Release();

	// Update MIDI monitor
	CSynthBase::HandleMIDIShortMessage(nMessage);
}

void CSoundFontSynth::HandleMIDISysExMessage(const u8* pData, size_t nSize)
{
	// Return early if it wasn't a GM Mode On/Off message and was consumed as a text/display dots message
	if (!ParseGMSysEx(pData, nSize) && (ParseRolandSysEx(pData, nSize) || ParseYamahaSysEx(pData, nSize)))
		return;

	// No special handling; forward to FluidSynth SysEx parser, excluding leading 0xF0 and trailing 0xF7
	CTaskLockHolder TaskLock(m_TaskLock);
	CSampleLoadReporter LoadReporter(m_pUI);
	m_Lock.Acquire();
	fluid_synth_sysex(m_pSynth, reinterpret_cast<const char*>(pData + 1), nSize - 2, nullptr, nullptr, nullptr, false);
	UpdateDefaultPrograms();
	m_Lock.Release();
}

bool CSoundFontSynth::IsActive()
{
	CTaskLockHolder TaskLock(m_TaskLock);
	m_Lock.Acquire();
	int nVoices = fluid_synth_get_active_voice_count(m_pSynth);
	m_Lock.Release();

	return nVoices > 0;
}

void CSoundFontSynth::AllSoundOff()
{
	{
		CTaskLockHolder TaskLock(m_TaskLock);
		m_Lock.Acquire();
		fluid_synth_all_sounds_off(m_pSynth, -1);
		m_Lock.Release();
	}

	// Reset MIDI monitor
	CSynthBase::AllSoundOff();
}

void CSoundFontSynth::SetMasterVolume(u8 nVolume)
{
	m_nVolume = nVolume;
	CTaskLockHolder TaskLock(m_TaskLock);
	m_Lock.Acquire();
	fluid_synth_set_gain(m_pSynth, m_nVolume / 100.0f * m_nInitialGain);
	m_Lock.Release();
}

size_t CSoundFontSynth::Render(float* pOutBuffer, size_t nFrames)
{
	m_Lock.Acquire();
	assert(fluid_synth_write_float(m_pSynth, nFrames, pOutBuffer, 0, 2, pOutBuffer, 1, 2) == FLUID_OK);
	m_Lock.Release();
	return nFrames;
}

size_t CSoundFontSynth::Render(s16* pOutBuffer, size_t nFrames)
{
	m_Lock.Acquire();
	assert(fluid_synth_write_s16(m_pSynth, nFrames, pOutBuffer, 0, 2, pOutBuffer, 1, 2) == FLUID_OK);
	m_Lock.Release();
	return nFrames;
}

void CSoundFontSynth::ReportStatus() const
{
	if (m_pUI)
		m_pUI->ShowSystemMessage(m_SoundFontManager.GetSoundFontName(m_nCurrentSoundFontIndex));
}

void CSoundFontSynth::GetChannelLevels(unsigned int nTicks, float* pLevels, float* pPeaks)
{
	m_MIDIMonitor.GetChannelLevels(nTicks, pLevels, pPeaks, m_nPercussionMask);
}

void CSoundFontSynth::GetDisplayInfo(TDisplayInfo& Info)
{
	m_DisplayInfoLock.Acquire();
	Info = m_DisplayInfo;
	m_DisplayInfoLock.Release();
}

void CSoundFontSynth::UpdateSoundFontName()
{
	const char* pName = m_SoundFontManager.GetSoundFontName(m_nCurrentSoundFontIndex);

	m_DisplayInfoLock.Acquire();
	snprintf(m_DisplayInfo.SoundFontName, sizeof(m_DisplayInfo.SoundFontName), "%s", pName ? pName : "");
	m_DisplayInfoLock.Release();
}

size_t CSoundFontSynth::GetChannelPresetIndex(u8 nChannel, TPresetList List)
{
	CTaskLockHolder TaskLock(m_TaskLock);
	const TPresets& Presets = m_PresetLists[static_cast<u8>(List)];

	int nSoundFontID, nBank, nProgram;
	m_Lock.Acquire();
	fluid_synth_get_program(m_pSynth, nChannel, &nSoundFontID, &nBank, &nProgram);
	const bool bUnset = !fluid_synth_get_channel_preset(m_pSynth, nChannel) && !(m_nDefaultProgramMask & (1 << nChannel));
	m_Lock.Release();

	// Continue browsing from an instrument that didn't fit into memory
	if (bUnset)
		return Presets.nOutOfMemoryIndex;

	// First preset at or after the current one
	for (size_t i = 0; i < Presets.nCount; ++i)
	{
		if (!IsPresetBefore(Presets.pPresets[i].nBank, Presets.pPresets[i].nProgram, nBank, nProgram))
			return i;
	}

	return 0;
}

bool CSoundFontSynth::GetPreset(size_t nIndex, int* pBank, int* pProgram, TPresetList List)
{
	CTaskLockHolder TaskLock(m_TaskLock);
	const TPresets& Presets = m_PresetLists[static_cast<u8>(List)];

	if (nIndex >= Presets.nCount)
		return false;

	*pBank    = Presets.pPresets[nIndex].nBank;
	*pProgram = Presets.pPresets[nIndex].nProgram;
	return true;
}

bool CSoundFontSynth::FindPreset(int nBank, int nProgram, size_t* pIndex, TPresetList List)
{
	CTaskLockHolder TaskLock(m_TaskLock);
	const TPresets& Presets = m_PresetLists[static_cast<u8>(List)];

	for (size_t i = 0; i < Presets.nCount; ++i)
	{
		if (Presets.pPresets[i].nBank == nBank && Presets.pPresets[i].nProgram == nProgram)
		{
			*pIndex = i;
			return true;
		}
	}

	return false;
}

void CSoundFontSynth::FormatPresetName(size_t nIndex, char* pBuffer, size_t nSize, TPresetList List)
{
	CTaskLockHolder TaskLock(m_TaskLock);
	const TPresets& Presets = m_PresetLists[static_cast<u8>(List)];

	if (nIndex >= Presets.nCount)
	{
		snprintf(pBuffer, nSize, "No instrument");
		return;
	}

	const TPreset& Preset = Presets.pPresets[nIndex];

	m_Lock.Acquire();
	fluid_sfont_t* pSoundFont = fluid_synth_get_sfont_by_id(m_pSynth, m_nSoundFontID);
	fluid_preset_t* pPreset = pSoundFont ? fluid_sfont_get_preset(pSoundFont, Preset.nBank, Preset.nProgram) : nullptr;
	FormatProgramName(pBuffer, nSize, Preset.nBank, Preset.nProgram, pPreset ? fluid_preset_get_name(pPreset) : "?");
	m_Lock.Release();
}

bool CSoundFontSynth::SetChannelsPreset(size_t nIndex, u8 nFirstChannel, u8 nChannels, bool* pSamplesLoaded, TPresetList List)
{
	CTaskLockHolder TaskLock(m_TaskLock);
	CSampleLoadReporter LoadReporter(m_pUI);
	TPresets& Presets = m_PresetLists[static_cast<u8>(List)];

	if (pSamplesLoaded)
		*pSamplesLoaded = false;

	if (nIndex >= Presets.nCount)
		return false;

	const TPreset& Preset = Presets.pPresets[nIndex];
	const u8 nLastChannel = nFirstChannel + nChannels;

	m_Lock.Acquire();

	// Nothing to do if all channels already have this instrument; reloading it could take seconds
	bool bAlreadySet = true;
	for (u8 nChannel = nFirstChannel; nChannel < nLastChannel && bAlreadySet; ++nChannel)
	{
		int nSoundFontID, nBank, nProgram;
		fluid_synth_get_program(m_pSynth, nChannel, &nSoundFontID, &nBank, &nProgram);
		bAlreadySet = fluid_synth_get_channel_preset(m_pSynth, nChannel) && nSoundFontID == m_nSoundFontID
			      && nBank == Preset.nBank && nProgram == Preset.nProgram;
	}

	if (bAlreadySet)
	{
		m_Lock.Release();
		return true;
	}

	// Release the samples of the current instrument first; otherwise, both instruments would have to fit into
	// memory at the same time, as the channels still holding the old one keep its samples loaded
	for (u8 nChannel = nFirstChannel; nChannel < nLastChannel; ++nChannel)
	{
		fluid_synth_all_sounds_off(m_pSynth, nChannel);
		fluid_synth_unset_program(m_pSynth, nChannel);
		m_nDefaultProgramMask &= ~(1 << nChannel);
	}

	// Channels share the sample data, so it is only loaded once
	const unsigned nFailedAllocs = s_nFailedAllocs;
	for (u8 nChannel = nFirstChannel; nChannel < nLastChannel; ++nChannel)
		fluid_synth_program_select(m_pSynth, nChannel, m_nSoundFontID, Preset.nBank, Preset.nProgram);

	// FluidSynth disables the samples that didn't fit; free the incomplete instrument rather than playing it
	const bool bOutOfMemory = s_nFailedAllocs != nFailedAllocs;
	if (bOutOfMemory)
	{
		LOGERR("Not enough memory for instrument %d:%d", Preset.nBank, Preset.nProgram);
		Presets.nOutOfMemoryIndex = nIndex;
		for (u8 nChannel = nFirstChannel; nChannel < nLastChannel; ++nChannel)
			fluid_synth_unset_program(m_pSynth, nChannel);
	}

	UpdateProgramName();
	m_Lock.Release();

	if (pSamplesLoaded)
		*pSamplesLoaded = LoadReporter.SamplesLoaded();

	return !bOutOfMemory;
}

const CSoundFontSynth::TEffectInfo& CSoundFontSynth::GetEffectInfo(TEffect Effect)
{
	static const TEffectInfo EffectInfo[] =
	{
		{ "Reverb level",     0.0f,  1.0f, 0.05f, 2 },
		{ "Reverb room",      0.0f,  1.0f, 0.05f, 2 },
		{ "Reverb damping",   0.0f,  1.0f, 0.05f, 2 },
		{ "Reverb width",     0.0f, 10.0f, 0.1f,  1 },
		{ "Chorus level",     0.0f, 10.0f, 0.1f,  1 },
		{ "Chorus depth",     0.0f, 64.0f, 0.5f,  1 },
		{ "Chorus speed",     0.1f,  5.0f, 0.05f, 2 },
		{ "Chorus voices",    0.0f, 99.0f, 1.0f,  0 },
	};
	static_assert(Utility::ArraySize(EffectInfo) == static_cast<size_t>(TEffect::Count), "Missing effect info");

	return EffectInfo[static_cast<size_t>(Effect)];
}

float CSoundFontSynth::GetEffect(TEffect Effect)
{
	CTaskLockHolder TaskLock(m_TaskLock);

	double nValue = 0.0;
	int nVoices = 0;

	m_Lock.Acquire();
	switch (Effect)
	{
		// An effect that is turned off is shown with a level of 0
		case TEffect::ReverbLevel:
			if (m_bReverbActive)
				fluid_synth_get_reverb_group_level(m_pSynth, -1, &nValue);
			break;

		case TEffect::ReverbRoomSize:	fluid_synth_get_reverb_group_roomsize(m_pSynth, -1, &nValue);	break;
		case TEffect::ReverbDamping:	fluid_synth_get_reverb_group_damp(m_pSynth, -1, &nValue);	break;
		case TEffect::ReverbWidth:	fluid_synth_get_reverb_group_width(m_pSynth, -1, &nValue);	break;

		case TEffect::ChorusLevel:
			if (m_bChorusActive)
				fluid_synth_get_chorus_group_level(m_pSynth, -1, &nValue);
			break;

		case TEffect::ChorusDepth:	fluid_synth_get_chorus_group_depth(m_pSynth, -1, &nValue);	break;
		case TEffect::ChorusSpeed:	fluid_synth_get_chorus_group_speed(m_pSynth, -1, &nValue);	break;

		case TEffect::ChorusVoices:
			fluid_synth_get_chorus_group_nr(m_pSynth, -1, &nVoices);
			nValue = nVoices;
			break;

		default:
			break;
	}
	m_Lock.Release();

	return nValue;
}

void CSoundFontSynth::SetEffect(TEffect Effect, float nValue)
{
	const TEffectInfo& Info = GetEffectInfo(Effect);
	nValue = Utility::Clamp(nValue, Info.nMin, Info.nMax);

	CTaskLockHolder TaskLock(m_TaskLock);
	m_Lock.Acquire();
	switch (Effect)
	{
		case TEffect::ReverbLevel:
			m_bReverbActive = nValue > 0.0f;
			m_EffectOverrides.bReverbActive = m_bReverbActive;
			m_EffectOverrides.nReverbLevel  = nValue;
			fluid_synth_reverb_on(m_pSynth, -1, m_bReverbActive);
			fluid_synth_set_reverb_group_level(m_pSynth, -1, nValue);
			break;

		case TEffect::ReverbRoomSize:
			m_EffectOverrides.nReverbRoomSize = nValue;
			fluid_synth_set_reverb_group_roomsize(m_pSynth, -1, nValue);
			break;

		case TEffect::ReverbDamping:
			m_EffectOverrides.nReverbDamping = nValue;
			fluid_synth_set_reverb_group_damp(m_pSynth, -1, nValue);
			break;

		case TEffect::ReverbWidth:
			m_EffectOverrides.nReverbWidth = nValue;
			fluid_synth_set_reverb_group_width(m_pSynth, -1, nValue);
			break;

		case TEffect::ChorusLevel:
			m_bChorusActive = nValue > 0.0f;
			m_EffectOverrides.bChorusActive = m_bChorusActive;
			m_EffectOverrides.nChorusLevel  = nValue;
			fluid_synth_chorus_on(m_pSynth, -1, m_bChorusActive);
			fluid_synth_set_chorus_group_level(m_pSynth, -1, nValue);
			break;

		case TEffect::ChorusDepth:
			m_EffectOverrides.nChorusDepth = nValue;
			fluid_synth_set_chorus_group_depth(m_pSynth, -1, nValue);
			break;

		case TEffect::ChorusSpeed:
			m_EffectOverrides.nChorusSpeed = nValue;
			fluid_synth_set_chorus_group_speed(m_pSynth, -1, nValue);
			break;

		case TEffect::ChorusVoices:
			m_EffectOverrides.nChorusVoices = static_cast<int>(nValue + 0.5f);
			fluid_synth_set_chorus_group_nr(m_pSynth, -1, m_EffectOverrides.nChorusVoices.Value());
			break;

		default:
			break;
	}
	m_Lock.Release();
}

bool CSoundFontSynth::SwitchSoundFont(size_t nIndex)
{
	// Is this SoundFont already active?
	if (m_nCurrentSoundFontIndex == nIndex)
	{
		if (m_pUI)
			m_pUI->ShowSystemMessage("Already selected!");
		return false;
	}

	// Get SoundFont if available
	const char* pSoundFontPath = m_SoundFontManager.GetSoundFontPath(nIndex);
	if (!pSoundFontPath)
	{
		if (m_pUI)
			m_pUI->ShowSystemMessage("SoundFont not avail!");
		return false;
	}

	if (m_pUI)
		m_pUI->ShowSystemMessage("Loading SoundFont", true);

	TFXProfile FXProfile = m_SoundFontManager.GetSoundFontFXProfile(nIndex);

	// We can't use fluid_synth_sfunload() as we don't support the lazy SoundFont unload timer, so trash the entire synth and create a new one
	if (!Reinitialize(pSoundFontPath, &FXProfile))
	{
		if (m_pUI)
			m_pUI->ShowSystemMessage("SF switch failed!");

		return false;
	}

	m_nCurrentSoundFontIndex = nIndex;
	UpdateSoundFontName();

	LOGNOTE("Loaded \"%s\"", m_SoundFontManager.GetSoundFontName(nIndex));
	if (m_pUI)
		m_pUI->ClearSpinnerMessage();

	return true;
}

bool CSoundFontSynth::Reinitialize(const char* pSoundFontPath, const TFXProfile* pFXProfile)
{
	const CConfig* const pConfig = CConfig::Get();

	CTaskLockHolder TaskLock(m_TaskLock);
	m_Lock.Acquire();

	if (m_pSynth)
		delete_fluid_synth(m_pSynth);

	FreePresetLists();
	m_nSoundFontID        = FLUID_FAILED;
	m_nDefaultProgramMask = 0;

	m_pSynth = new_fluid_synth(m_pSettings);

	if (!m_pSynth)
	{
		m_Lock.Release();
		LOGERR("Failed to create synth");
		return false;
	}

	fluid_sfloader_t* pLoader = new_fluid_defsfloader(m_pSettings);
	if (!pLoader)
	{
		m_Lock.Release();
		LOGERR("Failed to create SoundFont loader");
		return false;
	}

	fluid_sfloader_set_callbacks(pLoader, SoundFontOpen, SoundFontRead, SoundFontSeek, SoundFontTell, SoundFontClose);
	fluid_synth_add_sfloader(m_pSynth, pLoader);

	fluid_synth_set_polyphony(m_pSynth, pConfig->FluidSynthPolyphony);

	m_nInitialGain = pFXProfile->nGain.ValueOr(pConfig->FluidSynthDefaultGain);
	fluid_synth_set_gain(m_pSynth, m_nVolume / 100.0f * m_nInitialGain);

	// Use values changed at runtime if set, then values from effects profile if set, otherwise use defaults
	#define FX_VALUE(MEMBER, DEFAULT) (m_EffectOverrides.MEMBER ? m_EffectOverrides.MEMBER.Value() : pFXProfile->MEMBER.ValueOr(pConfig->DEFAULT))

	m_bReverbActive = FX_VALUE(bReverbActive, FluidSynthDefaultReverbActive);
	fluid_synth_reverb_on(m_pSynth, -1, m_bReverbActive);
	fluid_synth_set_reverb_group_damp(m_pSynth, -1, FX_VALUE(nReverbDamping, FluidSynthDefaultReverbDamping));
	fluid_synth_set_reverb_group_level(m_pSynth, -1, FX_VALUE(nReverbLevel, FluidSynthDefaultReverbLevel));
	fluid_synth_set_reverb_group_roomsize(m_pSynth, -1, FX_VALUE(nReverbRoomSize, FluidSynthDefaultReverbRoomSize));
	fluid_synth_set_reverb_group_width(m_pSynth, -1, FX_VALUE(nReverbWidth, FluidSynthDefaultReverbWidth));

	m_bChorusActive = FX_VALUE(bChorusActive, FluidSynthDefaultChorusActive);
	fluid_synth_chorus_on(m_pSynth, -1, m_bChorusActive);
	fluid_synth_set_chorus_group_depth(m_pSynth, -1, FX_VALUE(nChorusDepth, FluidSynthDefaultChorusDepth));
	fluid_synth_set_chorus_group_level(m_pSynth, -1, FX_VALUE(nChorusLevel, FluidSynthDefaultChorusLevel));
	fluid_synth_set_chorus_group_nr(m_pSynth, -1, FX_VALUE(nChorusVoices, FluidSynthDefaultChorusVoices));
	fluid_synth_set_chorus_group_speed(m_pSynth, -1, FX_VALUE(nChorusSpeed, FluidSynthDefaultChorusSpeed));

	#undef FX_VALUE

#ifndef NDEBUG
	DumpFXSettings();
#endif

	ResetMIDIMonitor();

	const unsigned int nLoadStart = CTimer::GetClockTicks();

	// Don't select the default programs yet; with dynamic sample loading, that would read the default instruments
	// of all 16 channels from the SD card (several hundred megabytes for a large piano). Each channel gets its
	// default program on its first note instead, unless a program is selected before.
	const int nSoundFontID = fluid_synth_sfload(m_pSynth, pSoundFontPath, false);

	if (nSoundFontID != FLUID_FAILED)
	{
		m_nSoundFontID = nSoundFontID;

		for (u8 nChannel = 0; nChannel < MIDIChannels; ++nChannel)
		{
			int nUnusedSoundFontID;
			fluid_synth_get_program(m_pSynth, nChannel, &nUnusedSoundFontID, &m_DefaultBanks[nChannel], &m_DefaultPrograms[nChannel]);
		}
		m_nDefaultProgramMask = (1 << MIDIChannels) - 1;

		BuildPresetLists();
		UpdateProgramName();
	}

	m_Lock.Release();

	if (nSoundFontID == FLUID_FAILED)
	{
		LOGERR("Failed to load SoundFont");
		return false;
	}

	const float nLoadTime = (CTimer::GetClockTicks() - nLoadStart) / 1000000.0f;
	LOGNOTE("\"%s\" loaded in %0.2f seconds", pSoundFontPath, nLoadTime);

	return true;
}

void CSoundFontSynth::BuildPresetLists()
{
	fluid_sfont_t* pSoundFont = fluid_synth_get_sfont_by_id(m_pSynth, m_nSoundFontID);
	if (!pSoundFont)
		return;

	auto GetListIndex = [](int nBank) {
		return static_cast<u8>(nBank >= DrumBank ? TPresetList::DrumKits : TPresetList::Instruments);
	};

	size_t Counts[static_cast<u8>(TPresetList::Count)] = {};
	fluid_sfont_iteration_start(pSoundFont);
	while (fluid_preset_t* pPreset = fluid_sfont_iteration_next(pSoundFont))
		++Counts[GetListIndex(fluid_preset_get_banknum(pPreset))];

	for (u8 nList = 0; nList < static_cast<u8>(TPresetList::Count); ++nList)
		m_PresetLists[nList] = { new TPreset[Counts[nList]], 0, 0 };

	// Sorted by bank, then program, so that each bank is listed as a whole
	fluid_sfont_iteration_start(pSoundFont);
	while (fluid_preset_t* pPreset = fluid_sfont_iteration_next(pSoundFont))
	{
		const int nBank = fluid_preset_get_banknum(pPreset);
		const TPreset Preset = { static_cast<u16>(nBank), static_cast<u8>(fluid_preset_get_num(pPreset)) };
		TPresets& List = m_PresetLists[GetListIndex(nBank)];

		size_t i = List.nCount++;
		while (i > 0 && IsPresetBefore(Preset.nBank, Preset.nProgram, List.pPresets[i - 1].nBank, List.pPresets[i - 1].nProgram))
		{
			List.pPresets[i] = List.pPresets[i - 1];
			--i;
		}
		List.pPresets[i] = Preset;
	}
}

void CSoundFontSynth::FreePresetLists()
{
	for (TPresets& List : m_PresetLists)
	{
		delete[] List.pPresets;
		List = { nullptr, 0, 0 };
	}
}

void CSoundFontSynth::LoadDefaultProgram(u8 nChannel)
{
	int nSoundFontID, nBank, nProgram;
	fluid_synth_get_program(m_pSynth, nChannel, &nSoundFontID, &nBank, &nProgram);

	if (nBank == m_DefaultBanks[nChannel])
		fluid_synth_program_change(m_pSynth, nChannel, nProgram);

	// A bank select without a program change doesn't change the instrument, but must apply to the next one
	else if (fluid_synth_program_select(m_pSynth, nChannel, m_nSoundFontID, m_DefaultBanks[nChannel], m_DefaultPrograms[nChannel]) == FLUID_OK)
		fluid_synth_bank_select(m_pSynth, nChannel, nBank);
	else
		fluid_synth_program_change(m_pSynth, nChannel, nProgram);

	m_nDefaultProgramMask &= ~(1 << nChannel);

	if (nChannel == 0)
		UpdateProgramName();
}

void CSoundFontSynth::UpdateDefaultPrograms()
{
	// Resets select the default programs on all channels
	for (u8 nChannel = 0; nChannel < MIDIChannels; ++nChannel)
		if ((m_nDefaultProgramMask & (1 << nChannel)) && fluid_synth_get_channel_preset(m_pSynth, nChannel))
			m_nDefaultProgramMask &= ~(1 << nChannel);

	UpdateProgramName();
}

void CSoundFontSynth::UpdateProgramName()
{
	char ProgramName[sizeof(m_DisplayInfo.ProgramName)];
	int nBank = 0, nProgram = 0;

	fluid_preset_t* pPreset = fluid_synth_get_channel_preset(m_pSynth, 0);
	if (pPreset)
	{
		nBank    = fluid_preset_get_banknum(pPreset);
		nProgram = fluid_preset_get_num(pPreset);
	}
	else if (m_nDefaultProgramMask & 1)
	{
		// Default program that hasn't been loaded yet
		nBank    = m_DefaultBanks[0];
		nProgram = m_DefaultPrograms[0];

		fluid_sfont_t* pSoundFont = fluid_synth_get_sfont_by_id(m_pSynth, m_nSoundFontID);
		pPreset = pSoundFont ? fluid_sfont_get_preset(pSoundFont, nBank, nProgram) : nullptr;
	}

	snprintf(ProgramName, sizeof(ProgramName), "%s", pPreset ? fluid_preset_get_name(pPreset) : "");

	m_DisplayInfoLock.Acquire();
	memcpy(m_DisplayInfo.ProgramName, ProgramName, sizeof(ProgramName));
	m_DisplayInfo.nBank    = nBank;
	m_DisplayInfo.nProgram = nProgram;
	m_DisplayInfoLock.Release();
}

void CSoundFontSynth::ResetMIDIMonitor()
{
	m_MIDIMonitor.AllNotesOff();
	m_MIDIMonitor.ResetControllers(false);
	m_nPercussionMask = 1 << 9;
}

#ifndef NDEBUG
void CSoundFontSynth::DumpFXSettings() const
{
	double nGain, nReverbDamping, nReverbLevel, nReverbRoomSize, nReverbWidth, nChorusDepth, nChorusLevel, nChorusSpeed;
	int nChorusVoices;

	nGain = fluid_synth_get_gain(m_pSynth);

	assert(fluid_synth_get_reverb_group_damp(m_pSynth, -1, &nReverbDamping) == FLUID_OK);
	assert(fluid_synth_get_reverb_group_level(m_pSynth, -1, &nReverbLevel) == FLUID_OK);
	assert(fluid_synth_get_reverb_group_roomsize(m_pSynth, -1, &nReverbRoomSize) == FLUID_OK);
	assert(fluid_synth_get_reverb_group_width(m_pSynth, -1, &nReverbWidth) == FLUID_OK);

	assert(fluid_synth_get_chorus_group_depth(m_pSynth, -1, &nChorusDepth) == FLUID_OK);
	assert(fluid_synth_get_chorus_group_level(m_pSynth, -1, &nChorusLevel) == FLUID_OK);
	assert(fluid_synth_get_chorus_group_nr(m_pSynth, -1, &nChorusVoices) == FLUID_OK);
	assert(fluid_synth_get_chorus_group_speed(m_pSynth, -1, &nChorusSpeed) == FLUID_OK);

	LOGNOTE("Gain: %.2f", nGain);

	LOGNOTE("Reverb: %.2f, %.2f, %.2f, %.2f",
		nReverbDamping,
		nReverbLevel,
		nReverbRoomSize,
		nReverbWidth
	);

	LOGNOTE("Chorus: %.2f, %.2f, %d, %.2f",
		nChorusDepth,
		nChorusLevel,
		nChorusVoices,
		nChorusSpeed
	);
}
#endif

bool CSoundFontSynth::ParseGMSysEx(const u8* pData, size_t nSize)
{
	// Must be at least size of header plus Start/End of Exclusive bytes
	if (nSize < sizeof(TGMSysExHeader) + 2)
		return false;

	const auto& Header = reinterpret_cast<const TGMSysExHeader&>(pData[1]);

	if (Header.ManufacturerID == TManufacturerID::UniversalNonRealTime &&
	    Header.DeviceID == TDeviceID::AllCall &&
	    Header.SubID1 == TUniversalSubID::GeneralMIDI)
	{
		// GM Mode On/Off
		if (Header.SubID2 == TGMSubID::GeneralMIDIOn || Header.SubID2 == TGMSubID::GeneralMIDIOff)
		{
			ResetMIDIMonitor();
			return true;
		}
	}

	return false;
}

bool CSoundFontSynth::ParseRolandSysEx(const u8* pData, size_t nSize)
{
	// Must be at least size of header plus a data byte, a checksum byte, and Start/End of Exclusive bytes
	if (nSize < sizeof(TRolandSysExHeader) + 4)
		return false;

	const auto& Header = reinterpret_cast<const TRolandSysExHeader&>(pData[1]);
	const u32 nAddressHiMed = Header.Address[0] << 16 | Header.Address[1] << 8;
	const u8 nAddressLo = Header.Address[2];
	const u8* pRolandData = pData + sizeof(TRolandSysExHeader) + 1;
	const size_t nRolandDataSize = nSize - sizeof(TRolandSysExHeader) - 3;
	const u8 nChecksum = pData[nSize - 2];

	if (Header.ManufacturerID != TManufacturerID::Roland)
		return false;

	if (Utility::RolandChecksum(Header.Address, sizeof(Header.Address) + nRolandDataSize) != nChecksum)
		return false;

	// Single byte GS messages
	if (Header.ModelID == TRolandModelID::GS && nRolandDataSize == 1)
	{
		if ((nAddressHiMed == TRolandAddress::GSReset || nAddressHiMed == TRolandAddress::SystemModeSet) && *pRolandData == 0)
		{
			// Reset MIDI monitor on GS reset
			ResetMIDIMonitor();

			// Don't consume; forward to FluidSynth
			return false;
		}
		else if ((nAddressHiMed & TRolandAddressMask::PatchPart) == TRolandAddress::UseForRhythmPart)
		{
			// TODO: If FluidSynth had an API to query the channel mode we wouldn't need to keep track of it
			const u8 nChannel = Header.Address[1] & 0x0F;
			const u8 nMode    = *pRolandData ? 1 : 0;
			m_nPercussionMask ^= (-nMode ^ m_nPercussionMask) & (1 << nChannel);

			// Don't consume; forward to FluidSynth
			return false;
		}
	}
	else if (Header.ModelID == TRolandModelID::SC55)
	{
		if (nAddressHiMed == TRolandAddress::SC55DisplayText)
		{
			if (m_pUI)
				m_pUI->ShowSysExText(CUserInterface::TSysExDisplayMessage::Roland, pRolandData, nRolandDataSize, nAddressLo);

			// Consume
			return true;
		}
		else if (nAddressHiMed == TRolandAddress::SC55DisplayDots)
		{
			if (m_pUI)
				m_pUI->ShowSysExBitmap(CUserInterface::TSysExDisplayMessage::Roland, pRolandData, nRolandDataSize);

			// Consume
			return true;
		}
	}

	return false;
}

bool CSoundFontSynth::ParseYamahaSysEx(const u8* pData, size_t nSize)
{
	// Must be at least size of header plus a data byte and Start/End of Exclusive bytes
	if (nSize < sizeof(TYamahaSysExHeader) + 3)
		return false;

	const auto& Header = reinterpret_cast<const TYamahaSysExHeader&>(pData[1]);
	const u32 nAddressHiMed = Header.Address[0] << 16 | Header.Address[1] << 8;
	const u8 nAddressLo = Header.Address[2];
	const u8* pYamahaData = pData + sizeof(TYamahaSysExHeader) + 1;
	const size_t nYamahaDataSize = nSize - sizeof(TYamahaSysExHeader) - 2;

	if (Header.ManufacturerID != TManufacturerID::Yamaha)
		return false;

	if (Header.ModelID == TYamahaModelID::XG)
	{
		if (nAddressHiMed == TYamahaAddress::XGSystemOn && *pYamahaData == 0)
		{
			// Reset MIDI monitor on XG reset
			ResetMIDIMonitor();

			// Don't consume; forward to FluidSynth
			return false;
		}
		else if (nAddressHiMed == TYamahaAddress::DisplayLetter)
		{
			if (m_pUI)
				m_pUI->ShowSysExText(CUserInterface::TSysExDisplayMessage::Yamaha, pYamahaData, nYamahaDataSize, nAddressLo);

			// Consume
			return true;
		}
		else if (nAddressHiMed == TYamahaAddress::DisplayBitmap)
		{
			if (m_pUI)
				m_pUI->ShowSysExBitmap(CUserInterface::TSysExDisplayMessage::Yamaha, pYamahaData, nYamahaDataSize);

			// Consume
			return true;
		}
	}

	return false;
}
