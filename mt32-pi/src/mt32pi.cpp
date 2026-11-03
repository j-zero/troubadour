//
// mt32pi.cpp
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

#include <circle/memory.h>
#include <circle/serial.h>
#include <circle/sound/hdmisoundbasedevice.h>
#include <circle/sound/i2ssoundbasedevice.h>
#include <circle/sound/pwmsoundbasedevice.h>

#include <cstdarg>
#include <cstring>

#include "lcd/drivers/gc9a01.h"
#include "lcd/ui.h"
#include "mt32pi.h"

#define MT32_PI_NAME "Troubadour"

// Set from the git tag when building
#ifndef MT32_PI_VERSION
#define MT32_PI_VERSION "<unknown>"
#endif
LOGMODULE(MT32_PI_NAME);

const char WLANFirmwarePath[] = "SD:firmware/";
const char WLANConfigFile[]   = "SD:wpa_supplicant.conf";
const char LastStateFile[]    = "SD:last_state.cfg";
const char PresetsDirectory[] = "SD:presets";

constexpr u32 LCDUpdatePeriodMillis                = 16;
constexpr u32 MisterUpdatePeriodMillis             = 50;
constexpr u32 LEDTimeoutMillis                     = 50;
constexpr u32 ActiveSenseTimeoutMillis             = 330;

constexpr float Sample24BitMax = (1 << (24 - 1)) - 1;

enum class TCustomSysExCommand : u8
{
	Reboot                = 0x00,
	SwitchSoundFont       = 0x02,
};

CMT32Pi* CMT32Pi::s_pThis = nullptr;

CMT32Pi::CMT32Pi(CI2CMaster* pI2CMaster, CSPIMaster* pSPIMaster, CInterruptSystem* pInterrupt, CGPIOManager* pGPIOManager, CSerialDevice* pSerialDevice, CUSBHCIDevice* pUSBHCI)
	: CMultiCoreSupport(CMemorySystem::Get()),
	  CMIDIParser(),

	  m_pConfig(CConfig::Get()),

	  m_pTimer(CTimer::Get()),
	  m_pActLED(CActLED::Get()),

	  m_pI2CMaster(pI2CMaster),
	  m_pSPIMaster(pSPIMaster),
	  m_pInterrupt(pInterrupt),
	  m_pGPIOManager(pGPIOManager),
	  m_pSerial(pSerialDevice),
	  m_pUSBHCI(pUSBHCI),
	  m_USBFileSystem{},
	  m_bUSBAvailable(false),

	  m_pNet(nullptr),
	  m_pNetDevice(nullptr),
	  m_WLAN(WLANFirmwarePath),
	  m_WPASupplicant(WLANConfigFile),
	  m_bNetworkReady(false),
	  m_pAppleMIDIParticipant(nullptr),
	  m_pUDPMIDIReceiver(nullptr),
	  m_pFTPDaemon(nullptr),

	  m_pDisplay(nullptr),
	  m_nLCDUpdateTime(0),
#ifdef MONITOR_TEMPERATURE
	  m_nTempUpdateTime(0),
#endif

	  m_pControl(nullptr),
	  m_MisterControl(pI2CMaster, m_EventQueue),
	  m_nMisterUpdateTime(0),

	  m_bDeferredSoundFontSwitchFlag(false),
	  m_nDeferredSoundFontSwitchIndex(0),
	  m_nDeferredSoundFontSwitchTime(0),

	  m_bEncoder2ButtonHeld(false),
	  m_bEncoder2LongPress(false),

	  m_Menu(TMenu::None),
	  m_SettingsPage(TSettingsPage::Root),
	  m_nSetting(SettingPresetMenu),
	  m_nMenuIndex(0),
	  m_nMenuCurrentIndex(0),
	  m_nMenuTime(0),
	  m_nLastParameterTurnTime(0),

	  m_pLastStateSaver(nullptr),
	  m_bLastStateChanged(false),
	  m_nLastStateChangeTime(0),

	  m_bSerialMIDIAvailable(false),
	  m_bSerialMIDIEnabled(false),
	  m_pUSBMIDIDevice(nullptr),
	  m_pUSBSerialDevice(nullptr),
	  m_pUSBMassStorageDevice(nullptr),

	  m_bActiveSenseFlag(false),
	  m_nActiveSenseTime(0),

	  m_bRunning(true),
	  m_bUITaskDone(false),
	  m_bLEDOn(false),
	  m_nLEDOnTime(0),

	  m_pSound(nullptr),
	  m_pPisound(nullptr),
	  m_pTriplePlay(nullptr),
	  m_bTriplePlayEnabled(false),
	  m_bTriplePlaySoftwareMode(false),
	  m_bTriplePlayProgramPresets(false),
	  m_nTriplePlayProgram(-1),

	  m_Presets(PresetsDirectory),
	  m_pPresetSaver(nullptr),
	  m_nCurrentPreset(-1),
	  m_bApplyingPreset(false),
	  m_bPresetChanged(false),
	  m_nPresetChangeTime(0),

	  m_nMasterVolume(100),
	  m_pSoundFontSynth(nullptr),

	  m_bStartupDone(false)
{
	s_pThis = this;
}

CMT32Pi::~CMT32Pi()
{
}

bool CMT32Pi::Initialize(bool bSerialMIDIAvailable)
{
	m_bSerialMIDIAvailable = bSerialMIDIAvailable;
	m_bSerialMIDIEnabled = bSerialMIDIAvailable;

	if (m_pConfig->LCDType == CConfig::TLCDType::GC9A01SPI)
	{
		const CGC9A01::TPins Pins =
		{
			static_cast<u8>(m_pConfig->LCDSPIDCPin & 31),
			static_cast<u8>(m_pConfig->LCDSPIResetPin & 31),
			static_cast<u8>(m_pConfig->LCDSPIBacklightPin & 31),
		};
		m_pDisplay = new CGC9A01(m_pSPIMaster, Pins, m_pConfig->LCDSPIClockSpeed, m_pConfig->LCDRotation, m_pConfig->LCDMirror);

		if (m_pDisplay->Initialize())
		{
			CLogger::Get()->RegisterPanicHandler(PanicHandler);

			// Splash screen
			m_UserInterface.ShowStartup(*m_pDisplay, MT32_PI_VERSION, nullptr);
		}
		else
		{
			LOGWARN("Display init failed");
			delete m_pDisplay;
			m_pDisplay = nullptr;
		}
	}

#if !defined(__aarch64__) || !defined(LEAVE_QEMU_ON_HALT)
	// The USB driver is not supported under 64-bit QEMU, so
	// the initialization must be skipped in this case, or an
	// exit happens here under 64-bit QEMU.
	LCDLog(TLCDLogType::Startup, "Init USB");
	if (m_pConfig->SystemUSB && m_pUSBHCI->Initialize())
	{
		m_bUSBAvailable = true;

		// Perform an initial Plug and Play update to initialize devices early
		UpdateUSB(true);
	}
#endif

	LCDLog(TLCDLogType::Startup, "Init Network");
	InitNetwork();

	// The Pisound check drives some GPIO pins, which the second encoder of the simple_encoder scheme may use
	bool bEncoder2OnPisoundPins = false;
	if (m_pConfig->ControlScheme == CConfig::TControlScheme::SimpleEncoder)
	{
		const int Pins[] = { m_pConfig->ControlEncoder2CLKPin, m_pConfig->ControlEncoder2DATPin, m_pConfig->ControlEncoder2ButtonPin };
		for (int nPin : Pins)
			bEncoder2OnPisoundPins |= nPin == 12 || nPin == 13 || nPin == 16 || nPin == 24 || nPin == 25 || nPin == 26;
	}

	// Check for Blokas Pisound, but only when not using the SPI display or the Pisound's pins otherwise (GPIO pin conflict)
	if (m_pConfig->LCDType != CConfig::TLCDType::GC9A01SPI && !bEncoder2OnPisoundPins)
	{
		m_pPisound = new CPisound(m_pSPIMaster, m_pGPIOManager, m_pConfig->AudioSampleRate);
		if (m_pPisound->Initialize())
		{
			LOGWARN("Blokas Pisound detected");
			m_pPisound->RegisterMIDIReceiveHandler(IRQMIDIReceiveHandler);
			m_bSerialMIDIEnabled = false;
		}
		else
		{
			delete m_pPisound;
			m_pPisound = nullptr;
		}
	}

	// Queue size of just one chunk
	unsigned int nQueueSize = m_pConfig->AudioChunkSize;
	TSoundFormat Format = TSoundFormat::SoundFormatSigned24;

	switch (m_pConfig->AudioOutputDevice)
	{
		case CConfig::TAudioOutputDevice::PWM:
			LCDLog(TLCDLogType::Startup, "Init audio (PWM)");
			m_pSound = new CPWMSoundBaseDevice(m_pInterrupt, m_pConfig->AudioSampleRate, m_pConfig->AudioChunkSize);
			break;

		case CConfig::TAudioOutputDevice::HDMI:
		{
			LCDLog(TLCDLogType::Startup, "Init audio (HDMI)");

			// Chunk size must be a multiple of 384
			const unsigned int nChunkSize = Utility::RoundToNearestMultiple(m_pConfig->AudioChunkSize, IEC958_SUBFRAMES_PER_BLOCK);
			nQueueSize = nChunkSize;

			m_pSound = new CHDMISoundBaseDevice(m_pInterrupt, m_pConfig->AudioSampleRate, nChunkSize);
			break;
		}

		case CConfig::TAudioOutputDevice::I2S:
		{
			LCDLog(TLCDLogType::Startup, "Init audio (I2S)");

			// Pisound provides clock
			const bool bSlave = m_pPisound != nullptr;

			// Don't probe if using Pisound
			CI2CMaster* const pI2CMaster = bSlave ? nullptr : m_pI2CMaster;

			m_pSound = new CI2SSoundBaseDevice(m_pInterrupt, m_pConfig->AudioSampleRate, m_pConfig->AudioChunkSize, bSlave, pI2CMaster);
			Format = TSoundFormat::SoundFormatSigned24_32;

			break;
		}
	}

	m_pSound->SetWriteFormat(Format);
	if (!m_pSound->AllocateQueueFrames(nQueueSize))
		LOGPANIC("Failed to allocate sound queue");

	LCDLog(TLCDLogType::Startup, "Init controls");
	if (m_pConfig->ControlScheme == CConfig::TControlScheme::SimpleButtons)
		m_pControl = new CControlSimpleButtons(m_EventQueue);
	else if (m_pConfig->ControlScheme == CConfig::TControlScheme::SimpleEncoder)
	{
		const CControlSimpleEncoder::TEncoderPins Encoder2Pins =
		{
			static_cast<u8>(m_pConfig->ControlEncoder2CLKPin & 31),
			static_cast<u8>(m_pConfig->ControlEncoder2DATPin & 31),
			static_cast<u8>(m_pConfig->ControlEncoder2ButtonPin & 31),
		};
		m_pControl = new CControlSimpleEncoder(m_EventQueue, m_pConfig->ControlEncoderType, m_pConfig->ControlEncoderReversed, Encoder2Pins, m_pConfig->ControlEncoder2Reversed);
	}

	if (m_pControl && !m_pControl->Initialize())
	{
		LOGWARN("Control init failed");
		delete m_pControl;
		m_pControl = nullptr;
	}

	m_pTriplePlay = new CTriplePlay();
	m_Presets.Load();
	m_pPresetSaver = new CPresetSaver(m_Presets);

	if (m_LastState.Load(LastStateFile))
		LOGNOTE("Restoring settings from '%s'", LastStateFile);
	m_pLastStateSaver = new CLastStateSaver(m_LastState, LastStateFile);

	LCDLog(TLCDLogType::Startup, "Loading SoundFont");
	CSoundFontSynth::SetLoadProgressHandler(LoadProgressHandler, this);
	if (!InitSoundFontSynth())
	{
		LOGPANIC("No SoundFonts found");
		return false;
	}

	RestoreLastState();

	if (m_pPisound)
		LOGNOTE("Using Pisound MIDI interface");
	else if (m_bSerialMIDIEnabled)
		LOGNOTE("Using serial MIDI interface");

	CCPUThrottle::Get()->DumpStatus();
	SetPowerSaveTimeout(m_pConfig->SystemPowerSaveTimeout);

	// Start audio
	m_pSound->Start();

	// Start other cores; the UI task draws the LCD from now on
	m_bStartupDone = true;
	if (!CMultiCoreSupport::Initialize())
		return false;

	return true;
}

bool CMT32Pi::InitNetwork()
{
	assert(m_pNet == nullptr);

	TNetDeviceType NetDeviceType = NetDeviceTypeUnknown;

	if (m_pConfig->NetworkMode == CConfig::TNetworkMode::WiFi)
	{
		LOGNOTE("Initializing Wi-Fi");

		if (m_WLAN.Initialize() && m_WPASupplicant.Initialize())
			NetDeviceType = NetDeviceTypeWLAN;
		else
			LOGERR("Failed to initialize Wi-Fi");
	}
	else if (m_pConfig->NetworkMode == CConfig::TNetworkMode::Ethernet)
	{
		LOGNOTE("Initializing Ethernet");
		NetDeviceType = NetDeviceTypeEthernet;
	}

	if (NetDeviceType != NetDeviceTypeUnknown)
	{
		if (m_pConfig->NetworkDHCP)
			m_pNet = new CNetSubSystem(0, 0, 0, 0, m_pConfig->NetworkHostname, NetDeviceType);
		else
			m_pNet = new CNetSubSystem(
				m_pConfig->NetworkIPAddress.Get(),
				m_pConfig->NetworkSubnetMask.Get(),
				m_pConfig->NetworkDefaultGateway.Get(),
				m_pConfig->NetworkDNSServer.Get(),
				m_pConfig->NetworkHostname,
				NetDeviceType
			);

		if (!m_pNet->Initialize(false))
		{
			LOGERR("Failed to initialize network subsystem");
			delete m_pNet;
			m_pNet = nullptr;
		}

		m_pNetDevice = CNetDevice::GetNetDevice(NetDeviceType);
	}

	return m_pNet != nullptr;
}

bool CMT32Pi::InitSoundFontSynth()
{
	assert(m_pSoundFontSynth == nullptr);

	m_pSoundFontSynth = new CSoundFontSynth(m_pConfig->AudioSampleRate);
	m_pSoundFontSynth->SetInitialSoundFont(m_LastState.SoundFontPath);
	m_pSoundFontSynth->SetEffectOverrides(m_LastState.Effects);
	if (!m_pSoundFontSynth->Initialize())
	{
		LOGWARN("FluidSynth init failed; no SoundFonts present?");
		delete m_pSoundFontSynth;
		m_pSoundFontSynth = nullptr;
		return false;
	}

	m_pSoundFontSynth->SetUserInterface(&m_UserInterface);

	return true;
}

void CMT32Pi::RestoreLastState()
{
	if (m_LastState.nVolume >= 0)
	{
		m_nMasterVolume = Utility::Clamp(m_LastState.nVolume, 0, 100);
		m_pSoundFontSynth->SetMasterVolume(m_nMasterVolume);
		m_UserInterface.SetVolume(m_nMasterVolume);
	}

	if (m_LastState.nDisplayChannels > 0)
		m_UserInterface.SetDisplayChannels(m_LastState.nDisplayChannels);

	// Set in the settings, otherwise from the config file
	const int nStandbyTimeout = m_LastState.nDisplayStandby >= 0 ? m_LastState.nDisplayStandby : m_pConfig->LCDStandbyTimeout;
	m_UserInterface.SetStandbyTimeout(Utility::Clamp(nStandbyTimeout, 0, static_cast<int>(CUserInterface::MaxStandbyTimeout)));

	// Switched in the settings, otherwise from the config file
	m_bTriplePlaySoftwareMode = m_LastState.nTriplePlaySoftwareMode >= 0 ? m_LastState.nTriplePlaySoftwareMode : m_pConfig->TriplePlaySoftwareMode;
	m_pTriplePlay->SetSoftwareMode(m_bTriplePlaySoftwareMode);
	m_bTriplePlayEnabled = m_LastState.nTriplePlay >= 0 ? m_LastState.nTriplePlay : m_pConfig->TriplePlayEnabled;
	m_bTriplePlayProgramPresets = m_LastState.nTriplePlayProgramPresets >= 0 ? m_LastState.nTriplePlayProgramPresets : m_pConfig->TriplePlayProgramChangePresets;
	m_pTriplePlay->SetProgramsToHost(m_bTriplePlayProgramPresets);

	// The preset that was selected last, or the first one (also sends its settings to the TriplePlay once it's there)
	const size_t nPresets = m_Presets.GetCount();
	const bool bPreset = nPresets > 0;
	if (bPreset && (m_LastState.nPreset < 0 || static_cast<size_t>(m_LastState.nPreset) >= nPresets))
		m_LastState.nPreset = 0;

	if (m_LastState.nProgram >= 0 || m_LastState.nDrumProgram >= 0 || bPreset)
	{
		LCDLog(TLCDLogType::Startup, "Loading instrument");
		if (bPreset)
			ApplyPreset(m_LastState.nPreset);
		else
			ApplyLastInstrument();
	}

	// There is always a current preset: without any, the first one is made from the instrument that was restored
	if (!bPreset)
		CapturePreset();

	UpdatePresetLabel();

	// The other presets' instruments, so that switching to them is immediate
	if (m_Presets.GetCount() > 1)
		LCDLog(TLCDLogType::Startup, "Loading presets");
	UpdatePinnedInstruments();
}

void CMT32Pi::MainTask()
{
	CScheduler* const pScheduler = CScheduler::Get();

	LOGNOTE("Main task on Core 0 starting up");

	Awaken();

	while (m_bRunning)
	{
		// Process MIDI data
		UpdateMIDI();

		// Process network packets
		UpdateNetwork();

		// Update controls
		if (m_pControl)
			m_pControl->Update();

		// Process events
		ProcessEventQueue();

		const unsigned int nTicks = m_pTimer->GetTicks();

		// Update activity LED
		if (m_bLEDOn && (nTicks - m_nLEDOnTime) >= MSEC2HZ(LEDTimeoutMillis))
		{
			m_pActLED->Off();
			m_bLEDOn = false;
		}

		// Check for active sensing timeout
		if (m_bActiveSenseFlag && (nTicks > m_nActiveSenseTime) && (nTicks - m_nActiveSenseTime) >= MSEC2HZ(ActiveSenseTimeoutMillis))
		{
			m_pSoundFontSynth->AllSoundOff();
			m_bActiveSenseFlag = false;
			LOGNOTE("Active sense timeout - turning notes off");
		}

		// Update power management
		if (m_pSoundFontSynth->IsActive())
			Awaken();

#ifdef MONITOR_TEMPERATURE
		if (nTicks - m_nTempUpdateTime >= MSEC2HZ(5000))
		{
			const unsigned int nTemp = CCPUThrottle::Get()->GetTemperature();
			LOGDBG("Temperature: %dC", nTemp);
			LCDLog(TLCDLogType::Notice, "Temp: %dC", nTemp);
			m_nTempUpdateTime = nTicks;
		}
#endif

		CPower::Update();

		// Check for deferred SoundFont switch
		if (m_bDeferredSoundFontSwitchFlag)
		{
			if ((nTicks - m_nDeferredSoundFontSwitchTime) >= static_cast<unsigned int>(m_pConfig->ControlSwitchTimeout) * HZ)
			{
				SwitchSoundFont(m_nDeferredSoundFontSwitchIndex);
				m_bDeferredSoundFontSwitchFlag = false;

				// Trigger an awaken so we don't immediately go to sleep
				Awaken();
			}
		}

		// Close menus that aren't used anymore
		if (m_Menu != TMenu::None && (nTicks - m_nMenuTime) >= MSEC2HZ(MenuTimeoutMillis))
			CloseMenu();

		// TriplePlay: commands, button actions and the settings shown. When it has been switched off, the commands
		// that take it out of software mode are still sent.
		CUSBMIDIDevice* const pTriplePlayDevice = m_bTriplePlayEnabled || m_pTriplePlay->IsSending() ? m_pUSBMIDIDevice : nullptr;
		if (m_pTriplePlay->Update(pTriplePlayDevice))
		{
			m_UserInterface.NotifyActivity();
			UpdateTriplePlayStatus();
			if (m_Menu == TMenu::Settings || m_Menu == TMenu::Parameter)
				UpdateMenu();

			// Its bend mode and transpose belong to the current preset
			CapturePreset();
		}

		// Its program changes, which switch presets
		if (m_bTriplePlayEnabled && m_bTriplePlayProgramPresets)
			UpdateTriplePlayPrograms();

		// Save settings and the current preset once they have settled
		if (m_bLastStateChanged && (nTicks - m_nLastStateChangeTime) >= MSEC2HZ(LastStateSaveDelayMillis))
		{
			m_bLastStateChanged = false;
			m_pLastStateSaver->Trigger();
		}

		if (m_bPresetChanged && (nTicks - m_nPresetChangeTime) >= MSEC2HZ(LastStateSaveDelayMillis))
		{
			m_bPresetChanged = false;
			m_pPresetSaver->Trigger();
		}

		// Check for USB PnP events
		UpdateUSB();

		// Allow other tasks to run
		pScheduler->Yield();
	}

	// Don't lose a pending change on reboot
	if (m_bLastStateChanged)
		m_LastState.Save(LastStateFile);


	// Stop audio
	m_pSound->Cancel();

	// Wait for UI task to finish
	while (!m_bUITaskDone)
		;
}

void CMT32Pi::UITask()
{
	LOGNOTE("UI task on Core 1 starting up");

	const bool bMisterEnabled = m_pConfig->ControlMister;

	// Nothing for this core to do; bail out
	if (!(m_pDisplay || bMisterEnabled))
	{
		m_bUITaskDone = true;
		return;
	}

	// Display current SoundFont
	m_pSoundFontSynth->ReportStatus();

	while (m_bRunning)
	{
		const unsigned int nTicks = CTimer::GetClockTicks();

		// Update display
		if (m_pDisplay && (nTicks - m_nLCDUpdateTime) >= Utility::MillisToTicks(LCDUpdatePeriodMillis))
		{
			m_UserInterface.Update(*m_pDisplay, *m_pSoundFontSynth, nTicks);
			m_nLCDUpdateTime = nTicks;
		}

		// Poll MiSTer interface
		if (bMisterEnabled && (nTicks - m_nMisterUpdateTime) >= Utility::MillisToTicks(MisterUpdatePeriodMillis))
		{
			TMisterStatus Status{TMisterSynth::SoundFont, 0xFF, static_cast<u8>(m_pSoundFontSynth->GetSoundFontIndex())};

			m_MisterControl.Update(Status);
			m_nMisterUpdateTime = nTicks;
		}
	}

	// Turn the display off
	if (m_pDisplay)
		m_pDisplay->SetBacklightState(false);

	m_bUITaskDone = true;
}

void CMT32Pi::AudioTask()
{
	LOGNOTE("Audio task on Core 2 starting up");

	constexpr u8 nChannels = 2;

	// Circle's "fast path" for I2S 24-bit really expects 32-bit samples
	const bool bI2S = m_pConfig->AudioOutputDevice == CConfig::TAudioOutputDevice::I2S;
	const bool bReversedStereo = m_pConfig->AudioReversedStereo;
	const u8 nBytesPerSample = bI2S ? sizeof(s32) : (sizeof(s8) * 3);
	const u8 nBytesPerFrame = 2 * nBytesPerSample;

	const size_t nQueueSizeFrames = m_pSound->GetQueueSizeFrames();

	// Extra byte so that we can write to the 24-bit buffer with overlapping 32-bit writes (efficiency)
	float FloatBuffer[nQueueSizeFrames * nChannels];
	s8 IntBuffer[nQueueSizeFrames * nBytesPerFrame + bI2S ? 0 : 1];

	while (m_bRunning)
	{
		const size_t nFrames = nQueueSizeFrames - m_pSound->GetQueueFramesAvail();
		const size_t nWriteBytes = nFrames * nBytesPerFrame;

		m_pSoundFontSynth->Render(FloatBuffer, nFrames);

		if (bReversedStereo)
		{
			// Convert to signed 24-bit integers with channel swap
			for (size_t i = 0; i < nFrames * nChannels; i += nChannels)
			{
				s32* const pLeftSample = reinterpret_cast<s32*>(IntBuffer + i * nBytesPerSample);
				s32* const pRightSample = reinterpret_cast<s32*>(IntBuffer + (i + 1) * nBytesPerSample);
				*pLeftSample = FloatBuffer[i + 1] * Sample24BitMax;
				*pRightSample = FloatBuffer[i] * Sample24BitMax;
			}
		}
		else
		{
			// Convert to signed 24-bit integers
			for (size_t i = 0; i < nFrames * nChannels; ++i)
			{
				s32* const pSample = reinterpret_cast<s32*>(IntBuffer + i * nBytesPerSample);
				*pSample = FloatBuffer[i] * Sample24BitMax;
			}
		}

		const int nResult = m_pSound->Write(IntBuffer, nWriteBytes);
		if (nResult != static_cast<int>(nWriteBytes))
			LOGERR("Sound data dropped");
	}
}

void CMT32Pi::Run(unsigned nCore)
{
	// Assign tasks to different CPU cores
	switch (nCore)
	{
		case 0:
			return MainTask();

		case 1:
			return UITask();

		case 2:
			return AudioTask();

		default:
			break;
	}
}

void CMT32Pi::OnEnterPowerSavingMode()
{
	CPower::OnEnterPowerSavingMode();
	m_pSound->Cancel();
	m_UserInterface.EnterPowerSavingMode();
}

void CMT32Pi::OnExitPowerSavingMode()
{
	CPower::OnExitPowerSavingMode();
	m_pSound->Start();
	m_UserInterface.ExitPowerSavingMode();
}

void CMT32Pi::OnThrottleDetected()
{
	CPower::OnThrottleDetected();
	LCDLog(TLCDLogType::Warning, "CPU throttl! Chk PSU");
}

void CMT32Pi::OnUnderVoltageDetected()
{
	CPower::OnUnderVoltageDetected();
	LCDLog(TLCDLogType::Warning, "Low voltage! Chk PSU");
}

void CMT32Pi::OnShortMessage(u32 nMessage)
{
	// Active sensing
	if (nMessage == 0xFE)
	{
		m_bActiveSenseFlag = true;
		return;
	}

	// Flash LED and keep the display on for channel messages (not for clock and the like)
	if ((nMessage & 0xFF) < 0xF0)
	{
		LEDOn();
		m_UserInterface.NotifyActivity();
	}

	m_pSoundFontSynth->HandleMIDIShortMessage(nMessage);

	// Wake from power saving mode if necessary
	Awaken();
}

void CMT32Pi::OnSysExMessage(const u8* pData, size_t nSize)
{
	// Flash LED
	LEDOn();

	// If we don't consume the SysEx message, forward it to the synthesizer
	if (!ParseCustomSysEx(pData, nSize))
		m_pSoundFontSynth->HandleMIDISysExMessage(pData, nSize);

	// Wake from power saving mode if necessary
	Awaken();
}

void CMT32Pi::OnUnexpectedStatus()
{
	CMIDIParser::OnUnexpectedStatus();
	if (m_pConfig->SystemVerbose)
		LCDLog(TLCDLogType::Warning, "Unexp. MIDI status!");
}

void CMT32Pi::OnSysExOverflow()
{
	CMIDIParser::OnSysExOverflow();
	LCDLog(TLCDLogType::Error, "SysEx overflow!");
}

void CMT32Pi::OnAppleMIDIConnect(const CIPAddress* pIPAddress, const char* pName)
{
	LCDLog(TLCDLogType::Notice, "%s connected!", pName);
}

void CMT32Pi::OnAppleMIDIDisconnect(const CIPAddress* pIPAddress, const char* pName)
{
	LCDLog(TLCDLogType::Notice, "%s disconnected!", pName);
}

bool CMT32Pi::ParseCustomSysEx(const u8* pData, size_t nSize)
{
	if (nSize < 4)
		return false;

	// 'Educational' manufacturer
	if (pData[1] != 0x7D)
		return false;

	const auto Command = static_cast<TCustomSysExCommand>(pData[2]);

	// Reboot (F0 7D 00 F7)
	if (nSize == 4 && Command == TCustomSysExCommand::Reboot)
	{
		LOGNOTE("Reboot command received");
		m_bRunning = false;
		return true;
	}

	if (nSize != 5)
		return false;

	// Switch SoundFont (F0 7D 02 xx F7)
	if (Command == TCustomSysExCommand::SwitchSoundFont)
	{
		SwitchSoundFont(pData[3]);
		return true;
	}

	return false;
}

void CMT32Pi::UpdateUSB(bool bStartup)
{
	if (!m_bUSBAvailable || !m_pUSBHCI->UpdatePlugAndPlay())
		return;

	Awaken();

	CUSBBulkOnlyMassStorageDevice* pUSBMassStorageDevice = static_cast<CUSBBulkOnlyMassStorageDevice*>(CDeviceNameService::Get()->GetDevice("umsd1", TRUE));

	if (!m_pUSBMassStorageDevice && pUSBMassStorageDevice)
	{
		// USB disk was attached
		LOGNOTE("USB mass storage device attached");

		if (f_mount(&m_USBFileSystem, "USB:", 1) != FR_OK)
			LOGERR("Failed to mount USB mass storage device");
		else
		{
			if (!bStartup)
			{
				LCDLog(TLCDLogType::Spinner, "SoundFont rescan");
				m_pSoundFontSynth->GetSoundFontManager().ScanSoundFonts();
				LCDLog(TLCDLogType::Notice, "%d SoundFonts avail", m_pSoundFontSynth->GetSoundFontManager().GetSoundFontCount());
			}
		}
	}
	else if (m_pUSBMassStorageDevice && !pUSBMassStorageDevice)
	{
		// USB disk was removed
		LOGNOTE("USB mass storage device removed");

		f_unmount("USB:");

		LCDLog(TLCDLogType::Spinner, "SoundFont rescan");
		m_pSoundFontSynth->GetSoundFontManager().ScanSoundFonts();
		LCDLog(TLCDLogType::Notice, "%d SoundFonts avail", m_pSoundFontSynth->GetSoundFontManager().GetSoundFontCount());
	}
	m_pUSBMassStorageDevice = pUSBMassStorageDevice;

	if (!m_pUSBMIDIDevice && (m_pUSBMIDIDevice = static_cast<CUSBMIDIDevice*>(CDeviceNameService::Get()->GetDevice("umidi1", FALSE))))
	{
		m_pUSBMIDIDevice->RegisterRemovedHandler(USBMIDIDeviceRemovedHandler, &m_pUSBMIDIDevice);
		m_pUSBMIDIDevice->RegisterPacketHandler(USBMIDIPacketHandler);
		LOGNOTE("Using USB MIDI interface");
		m_bSerialMIDIEnabled = false;
	}

	if (!m_pUSBSerialDevice && (m_pUSBSerialDevice = static_cast<CUSBSerialDevice*>(CDeviceNameService::Get()->GetDevice("utty1", FALSE))))
	{
		m_pUSBSerialDevice->SetBaudRate(m_pConfig->MIDIUSBSerialBaudRate);
		m_pUSBSerialDevice->RegisterRemovedHandler(USBMIDIDeviceRemovedHandler, &m_pUSBSerialDevice);
		LOGNOTE("Using USB serial interface");
		m_bSerialMIDIEnabled = false;
	}
}

void CMT32Pi::UpdateNetwork()
{
	if (!m_pNet)
		return;

	bool bNetIsRunning = m_pNet->IsRunning();
	if (m_pConfig->NetworkMode == CConfig::TNetworkMode::Ethernet)
		bNetIsRunning &= m_pNetDevice->IsLinkUp();
	else if (m_pConfig->NetworkMode == CConfig::TNetworkMode::WiFi)
		bNetIsRunning &= m_WPASupplicant.IsConnected();

	if (!m_bNetworkReady && bNetIsRunning)
	{
		m_bNetworkReady = true;

		CString IPString;
		m_pNet->GetConfig()->GetIPAddress()->Format(&IPString);

		LOGNOTE("Network up and running at: %s", static_cast<const char *>(IPString));
		LCDLog(TLCDLogType::Notice, "%s: %s", GetNetworkDeviceShortName(), static_cast<const char*>(IPString));

		if (m_pConfig->NetworkRTPMIDI && !m_pAppleMIDIParticipant)
		{
			m_pAppleMIDIParticipant = new CAppleMIDIParticipant(&m_Random, this);
			if (!m_pAppleMIDIParticipant->Initialize())
			{
				LOGERR("Failed to init AppleMIDI receiver");
				delete m_pAppleMIDIParticipant;
				m_pAppleMIDIParticipant = nullptr;
			}
			else
				LOGNOTE("AppleMIDI receiver initialized");
		}

		if (m_pConfig->NetworkUDPMIDI && !m_pUDPMIDIReceiver)
		{
			m_pUDPMIDIReceiver = new CUDPMIDIReceiver(this);
			if (!m_pUDPMIDIReceiver->Initialize())
			{
				LOGERR("Failed to init UDP MIDI receiver");
				delete m_pUDPMIDIReceiver;
				m_pUDPMIDIReceiver = nullptr;
			}
			else
				LOGNOTE("UDP MIDI receiver initialized");
		}

		if (m_pConfig->NetworkFTPServer && !m_pFTPDaemon)
		{
			m_pFTPDaemon = new CFTPDaemon(m_pConfig->NetworkFTPUsername, m_pConfig->NetworkFTPPassword);
			if (!m_pFTPDaemon->Initialize())
			{
				LOGERR("Failed to init FTP daemon");
				delete m_pFTPDaemon;
				m_pFTPDaemon = nullptr;
			}
			else
				LOGNOTE("FTP daemon initialized");
		}
	}
	else if (m_bNetworkReady && !bNetIsRunning)
	{
		m_bNetworkReady = false;
		LOGNOTE("Network disconnected.");
		LCDLog(TLCDLogType::Notice, "%s disconnected!", GetNetworkDeviceShortName());

	}
}

void CMT32Pi::UpdateMIDI()
{
	size_t nBytes;
	u8 Buffer[MIDIRxBufferSize];

	// Read MIDI messages from serial device or ring buffer
	if (m_bSerialMIDIEnabled)
		nBytes = ReceiveSerialMIDI(Buffer, sizeof(Buffer));
	else if (m_pUSBSerialDevice)
	{
		const int nResult = m_pUSBSerialDevice->Read(Buffer, sizeof(Buffer));
		nBytes = nResult > 0 ? static_cast<size_t>(nResult) : 0;
	}
	else
		nBytes = m_MIDIRxBuffer.Dequeue(Buffer, sizeof(Buffer));

	if (nBytes == 0)
		return;

	// Process MIDI messages
	ParseMIDIBytes(Buffer, nBytes);

	// Reset the Active Sense timer
	s_pThis->m_nActiveSenseTime = s_pThis->m_pTimer->GetTicks();
}

void CMT32Pi::PurgeMIDIBuffers()
{
	size_t nBytes;
	u8 Buffer[MIDIRxBufferSize];

	// Process MIDI messages from all devices/ring buffers, but ignore note-ons
	while (m_bSerialMIDIEnabled && (nBytes = ReceiveSerialMIDI(Buffer, sizeof(Buffer))) > 0)
		ParseMIDIBytes(Buffer, nBytes, true);

	while (m_pUSBSerialDevice && (nBytes = m_pUSBSerialDevice->Read(Buffer, sizeof(Buffer))) > 0)
		ParseMIDIBytes(Buffer, nBytes, true);

	while ((nBytes = m_MIDIRxBuffer.Dequeue(Buffer, sizeof(Buffer))) > 0)
		ParseMIDIBytes(Buffer, nBytes, true);
}

size_t CMT32Pi::ReceiveSerialMIDI(u8* pOutData, size_t nSize)
{
	// Read serial MIDI data
	int nResult = m_pSerial->Read(pOutData, nSize);

	// No data
	if (nResult == 0)
		return 0;

	// Error
	if (nResult < 0)
	{
		if (m_pConfig->SystemVerbose)
		{
			const char* pErrorString;
			switch (nResult)
			{
				case -SERIAL_ERROR_BREAK:
					pErrorString = "UART break error!";
					break;

				case -SERIAL_ERROR_OVERRUN:
					pErrorString = "UART overrun error!";
					break;

				case -SERIAL_ERROR_FRAMING:
					pErrorString = "UART framing error!";
					break;

				default:
					pErrorString = "Unknown UART error!";
					break;
			}

			LOGWARN(pErrorString);
			LCDLog(TLCDLogType::Warning, pErrorString);
		}

		return 0;
	}

	// Replay received MIDI data out via the serial port ('software thru')
	if (m_pConfig->MIDIGPIOThru)
	{
		int nSendResult = m_pSerial->Write(pOutData, nResult);
		if (nSendResult != nResult)
		{
			LOGERR("received %d bytes, but only sent %d bytes", nResult, nSendResult);
			LCDLog(TLCDLogType::Error, "UART TX error!");
		}
	}

	return static_cast<size_t>(nResult);
}

void CMT32Pi::ProcessEventQueue()
{
	TEvent Buffer[EventQueueSize];
	const size_t nEvents = m_EventQueue.Dequeue(Buffer, sizeof(Buffer));

	// We got some events, wake up
	if (nEvents > 0)
	{
		Awaken();
		m_UserInterface.NotifyActivity();
	}

	for (size_t i = 0; i < nEvents; ++i)
	{
		const TEvent& Event = Buffer[i];

		switch (Event.Type)
		{
			case TEventType::Button:
				ProcessButtonEvent(Event.Button);
				break;

			case TEventType::SwitchSoundFont:
				DeferSwitchSoundFont(Event.SwitchSoundFont.Index);
				break;

			case TEventType::AllSoundOff:
				m_pSoundFontSynth->AllSoundOff();
				break;

			case TEventType::DisplayImage:
				m_UserInterface.DisplayImage(Event.DisplayImage.Image);
				break;

			case TEventType::Encoder:
				ProcessEncoderEvent(Event.Encoder);
				break;
		}
	}
}

void CMT32Pi::ProcessButtonEvent(const TButtonEvent& Event)
{
	if (Event.Button == TButton::Button1 || Event.Button == TButton::Button2)
	{
		ProcessFootswitchEvent(Event);
		return;
	}

	if (Event.Button == TButton::Encoder2Button)
	{
		ProcessEncoder2ButtonEvent(Event);
		return;
	}

	if (!Event.bPressed)
		return;

	switch (Event.Button)
	{
		case TButton::Button3:
		case TButton::Button4:
			SetMasterVolume(m_nMasterVolume + (Event.Button == TButton::Button3 ? -1 : 1));
			LCDLog(TLCDLogType::Notice, "Volume: %d", m_nMasterVolume);
			break;

		// Instrument list: select the highlighted instrument; settings: leave them
		case TButton::EncoderButton:
			if (Event.bRepeat)
				break;

			if (m_Menu == TMenu::Instruments)
				SelectMenuInstrument();
			else if (IsSettingsMenu())
				CloseMenu();
			else
				OpenMenu(TMenu::Instruments);
			break;

		default:
			break;
	}
}

void CMT32Pi::ProcessEncoder2ButtonEvent(const TButtonEvent& Event)
{
	// A long press leaves the settings; a short one acts when released
	if (Event.bPressed)
	{
		if (!Event.bRepeat)
		{
			m_bEncoder2ButtonHeld = true;
			m_bEncoder2LongPress = false;
		}
		else if (m_bEncoder2ButtonHeld && !m_bEncoder2LongPress)
		{
			m_bEncoder2LongPress = true;
			if (IsSettingsMenu())
			{
				// From a list, remember its highlighted item
				if (m_Menu == TMenu::Settings)
				{
					u8 Items[MaxSettingsPageItems];
					if (m_nMenuIndex < GetSettingsItems(Items))
						m_nSetting = Items[m_nMenuIndex];
				}
				CloseMenu(true);
			}
		}
		return;
	}

	const bool bShortPress = m_bEncoder2ButtonHeld && !m_bEncoder2LongPress;
	m_bEncoder2ButtonHeld = false;
	if (!bShortPress)
		return;

	// Settings list: open the highlighted item; a setting: back to the list, loading the highlighted drum
	// kit/SoundFont if it isn't the current one
	if (m_Menu == TMenu::Settings)
	{
		u8 Items[MaxSettingsPageItems];
		if (m_nMenuIndex < GetSettingsItems(Items))
			OpenSetting(Items[m_nMenuIndex]);
	}
	else if (m_Menu == TMenu::SoundFonts && m_nMenuIndex != m_nMenuCurrentIndex)
	{
		// The SoundFont's name is shown once it has loaded
		CloseMenu();
		m_bDeferredSoundFontSwitchFlag = false;
		SwitchSoundFont(m_nMenuIndex);
	}
	else if (m_Menu == TMenu::DrumKits && m_nMenuIndex != m_nMenuCurrentIndex)
	{
		// Leave an error message visible
		if (ApplyDrumKit(m_nMenuIndex))
			OpenMenu(TMenu::Settings);
		else
			CloseMenu();
	}
	else if (m_Menu == TMenu::PresetInstruments && m_nMenuIndex != m_nMenuCurrentIndex)
	{
		// Saved to the current preset with the other changes; leave an error message visible
		if (ApplyProgramChange(m_nMenuIndex))
			OpenMenu(TMenu::Settings);
		else
			CloseMenu();
	}
	else if (m_Menu == TMenu::Presets)
	{
		// Loading a SoundFont shows its name, so the menu is closed then
		const bool bOtherSoundFont = m_nMenuIndex < m_Presets.GetCount()
					     && strcmp(m_Presets.Get(m_nMenuIndex).SoundFontPath,
						       m_pSoundFontSynth->GetSoundFontManager().GetSoundFontPath(m_pSoundFontSynth->GetSoundFontIndex()));
		if (bOtherSoundFont)
			CloseMenu();
		ApplyPreset(m_nMenuIndex);
		if (!bOtherSoundFont)
			OpenMenu(TMenu::Settings);
	}
	else
		OpenMenu(TMenu::Settings);
}

void CMT32Pi::ProcessFootswitchEvent(const TButtonEvent& Event)
{
	const int nDelta = Event.Button == TButton::Button1 ? 1 : -1;

	// Each footswitch switches to the next/previous preset when released, round the list
	const size_t nCount = m_Presets.GetCount();
	if (Event.bPressed || !nCount)
		return;

	const int nCurrent = m_nCurrentPreset >= 0 ? m_nCurrentPreset : (nDelta > 0 ? -1 : 0);
	ApplyPreset((nCurrent + nDelta + static_cast<int>(nCount)) % static_cast<int>(nCount));
}

void CMT32Pi::ProcessEncoderEvent(const TEncoderEvent& Event)
{
	const bool bInSetting = m_Menu == TMenu::Parameter || m_Menu == TMenu::DrumKits || m_Menu == TMenu::SoundFonts
				|| m_Menu == TMenu::Presets || m_Menu == TMenu::PresetInstruments;

	if (Event.nEncoder == 0)
	{
		// The first encoder moves through the settings list (going back to it from a setting), otherwise
		// through the instruments
		if (m_Menu == TMenu::Settings)
			MoveListSelection(Event.nDelta);
		else if (bInSetting)
			OpenMenu(TMenu::Settings);
		else
			MoveInstrumentSelection(Event.nDelta);
		return;
	}

	// The second encoder also moves through the settings list; in a setting, it changes the value, and outside of
	// the menus the volume
	if (m_Menu == TMenu::Settings)
	{
		MoveListSelection(Event.nDelta);
		return;
	}

	if (!bInSetting)
	{
		m_SettingsPage = TSettingsPage::Global;
		OpenSetting(SettingVolume);
	}

	if (m_Menu == TMenu::SoundFonts || m_Menu == TMenu::DrumKits || m_Menu == TMenu::Presets || m_Menu == TMenu::PresetInstruments)
	{
		MoveListSelection(Event.nDelta);
		return;
	}

	if (m_Menu != TMenu::Parameter)
		return;

	// Values have many steps, so turning quickly makes bigger steps; lists always move by one per detent
	const unsigned int nTicks = CTimer::GetClockTicks();
	const unsigned int nInterval = nTicks - m_nLastParameterTurnTime;
	const int nFactor = nInterval < ParameterFastTurnMicros ? 4 : nInterval < ParameterMediumTurnMicros ? 2 : 1;
	m_nLastParameterTurnTime = nTicks;

	// The number of channels and the TriplePlay's settings have few steps
	const bool bFewSteps = m_nSetting == SettingChannels || m_nSetting >= SettingTPEnabled;	// Also the standby time
	ChangeParameter(bFewSteps ? Event.nDelta : Event.nDelta * nFactor);

	m_nMenuTime = m_pTimer->GetTicks();
	UpdateMenu();
}

size_t CMT32Pi::GetSettingsItems(u8* pItems) const
{
	size_t nCount = 0;
	auto Add = [&](u8 nSetting) { pItems[nCount++] = nSetting; };
	const bool bTP = m_bTriplePlayEnabled;

	switch (m_SettingsPage)
	{
		case TSettingsPage::Root:
			Add(SettingPresetMenu);
			Add(SettingGlobalMenu);
			Add(SettingExit);
			break;

		// What the current preset holds, the list, and adding and deleting (presets are saved automatically)
		case TSettingsPage::Preset:
			if (m_Presets.GetCount())
				Add(SettingPresetSelect);
			if (m_nCurrentPreset >= 0)
			{
				Add(SettingSoundFont);
				Add(SettingPresetInstrument);
				Add(SettingEffectsMenu);
				if (bTP)
					Add(SettingPresetTPMenu);
			}
			Add(SettingPresetNew);
			if (m_Presets.GetCount() > 1)
				Add(SettingPresetDelete);
			Add(SettingBack);
			break;

		// The TriplePlay's settings that belong to the preset
		case TSettingsPage::PresetTP:
		{
			static const CTriplePlay::TSetting PatchSettings[] =
			{
				CTriplePlay::TSetting::Octave, CTriplePlay::TSetting::Semitone, CTriplePlay::TSetting::BendMode,
				CTriplePlay::TSetting::BendRange, CTriplePlay::TSetting::ChannelMode,
			};
			for (CTriplePlay::TSetting Setting : PatchSettings)
				Add(SettingTPFirst + static_cast<u8>(Setting));
			for (u8 nString = 0; nString < CTriplePlay::Strings; ++nString)
				Add(SettingTPSensitivity + nString);
			Add(SettingBack);
			break;
		}

		case TSettingsPage::Global:
			Add(SettingVolume);
			Add(SettingDrumKit);
			Add(SettingChannels);
			Add(SettingDisplayStandby);
			Add(SettingGlobalTPMenu);
			Add(SettingBack);
			break;

		// Reverb and chorus
		case TSettingsPage::Effects:
			for (u8 nEffect = 0; nEffect < static_cast<u8>(CSoundFontSynth::TEffect::Count); ++nEffect)
				Add(SettingEffects + nEffect);
			Add(SettingBack);
			break;

		// The TriplePlay's own settings
		case TSettingsPage::GlobalTP:
			Add(SettingTPEnabled);
			if (bTP)
			{
				Add(SettingTPSoftwareMode);
				Add(SettingTPProgramPresets);
				Add(SettingTPFirst + static_cast<u8>(CTriplePlay::TSetting::ButtonAction));
				Add(SettingTPFirst + static_cast<u8>(CTriplePlay::TSetting::BothButtonsAction));
				Add(SettingTPFirst + static_cast<u8>(CTriplePlay::TSetting::HandPosition));
				for (u8 nString = 0; nString < CTriplePlay::Strings; ++nString)
					Add(SettingTPOpenString + nString);
				if (m_pTriplePlay->HasBattery())
					Add(SettingTPBattery);
			}
			Add(SettingBack);
			break;
	}

	return nCount;
}

int CMT32Pi::FindSettingsItem(u8 nSetting) const
{
	u8 Items[MaxSettingsPageItems];
	const size_t nCount = GetSettingsItems(Items);
	for (size_t i = 0; i < nCount; ++i)
		if (Items[i] == nSetting)
			return i;

	return -1;
}

void CMT32Pi::OpenSettingsPage(TSettingsPage Page, u8 nSelectedSetting)
{
	m_SettingsPage = Page;
	m_nSetting = nSelectedSetting;
	OpenMenu(TMenu::Settings);
}

void CMT32Pi::OpenSetting(u8 nSetting)
{
	switch (nSetting)
	{
		case SettingExit:
			CloseMenu();
			return;

		// Submenus, and back to the menu that has them
		case SettingPresetMenu:
			OpenSettingsPage(TSettingsPage::Preset, m_Presets.GetCount() ? SettingPresetSelect : SettingPresetNew);
			return;
		case SettingPresetTPMenu:	OpenSettingsPage(TSettingsPage::PresetTP, SettingTPFirst); return;
		case SettingEffectsMenu:	OpenSettingsPage(TSettingsPage::Effects, SettingEffects); return;
		case SettingGlobalMenu:		OpenSettingsPage(TSettingsPage::Global, SettingVolume); return;
		case SettingGlobalTPMenu:	OpenSettingsPage(TSettingsPage::GlobalTP, SettingTPEnabled); return;

		case SettingBack:
			switch (m_SettingsPage)
			{
				case TSettingsPage::Preset:	OpenSettingsPage(TSettingsPage::Root, SettingPresetMenu); break;
				case TSettingsPage::PresetTP:	OpenSettingsPage(TSettingsPage::Preset, SettingPresetTPMenu); break;
				case TSettingsPage::Effects:	OpenSettingsPage(TSettingsPage::Preset, SettingEffectsMenu); break;
				case TSettingsPage::GlobalTP:	OpenSettingsPage(TSettingsPage::Global, SettingGlobalTPMenu); break;
				default:			OpenSettingsPage(TSettingsPage::Root, SettingGlobalMenu); break;
			}
			return;

		// The preset list, and the instruments for the current one
		case SettingPresetSelect:
			m_nSetting = nSetting;
			OpenMenu(TMenu::Presets);
			return;

		case SettingPresetInstrument:
			m_nSetting = nSetting;
			if (m_pSoundFontSynth->GetPresetCount())
				OpenMenu(TMenu::PresetInstruments);
			else
			{
				CloseMenu();
				LCDLog(TLCDLogType::Error, "No instruments!");
			}
			return;

		// Actions rather than values; the list shows the result
		case SettingPresetNew:
			NewPreset();
			OpenSettingsPage(TSettingsPage::Preset, SettingPresetSelect);
			return;

		case SettingPresetDelete:
			DeletePreset();
			OpenSettingsPage(TSettingsPage::Preset, SettingPresetSelect);
			return;

		// Only shown
		case SettingTPBattery:
			return;

		default:
			break;
	}

	m_nSetting = nSetting;

	if (nSetting == SettingSoundFont)
		OpenMenu(TMenu::SoundFonts);
	else if (nSetting != SettingDrumKit)
		OpenMenu(TMenu::Parameter);
	else if (m_pSoundFontSynth->GetPresetCount(CSoundFontSynth::TPresetList::DrumKits))
		OpenMenu(TMenu::DrumKits);
	else
	{
		CloseMenu();
		LCDLog(TLCDLogType::Warning, "No drum kits!");
	}
}

void CMT32Pi::UpdateTriplePlayStatus()
{
	// The transpose (in octaves) and bend mode of the guitar's patch, which its buttons usually change
	char Main[CUserInterface::StatusTextLength] = "";
	char Side[CUserInterface::StatusTextLength] = "";
	if (m_pTriplePlay->IsConnected())
	{
		// In octaves and the semitones beyond them: "Oct. +1", "Oct. +1 +3 semi", "-2 semi"
		const int nTranspose = m_pTriplePlay->GetTranspose();
		const int nOctaves = nTranspose / 12;
		const int nSemitones = nTranspose - nOctaves * 12;

		if (!nSemitones && !nOctaves)
			snprintf(Main, sizeof(Main), "Oct. 0");
		else if (!nSemitones)
			snprintf(Main, sizeof(Main), "Oct. %+d", nOctaves);
		else if (!nOctaves)
			snprintf(Main, sizeof(Main), "%+d semi", nSemitones);
		else
			snprintf(Main, sizeof(Main), "Oct. %+d %+d semi", nOctaves, nSemitones);

		m_pTriplePlay->FormatSetting(CTriplePlay::TSetting::BendMode, Side, sizeof(Side));
	}
	else if (m_bTriplePlayEnabled && m_pTriplePlay->HasNoReply())
		snprintf(Main, sizeof(Main), "TP: no reply");

	m_UserInterface.SetStatusText(Main, Side);
}

void CMT32Pi::SetTriplePlayEnabled(bool bEnabled)
{
	if (bEnabled == m_bTriplePlayEnabled)
		return;

	// Without the support, the TriplePlay has to perform its buttons itself again
	m_pTriplePlay->SetSoftwareMode(bEnabled && m_bTriplePlaySoftwareMode);
	m_bTriplePlayEnabled = bEnabled;

	// The current preset's settings
	if (bEnabled && m_nCurrentPreset >= 0)
		ApplyPreset(m_nCurrentPreset);

	m_LastState.nTriplePlay = bEnabled;
	DeferSaveLastState();
}

bool CMT32Pi::GetCurrentInstrument(const char** ppSoundFontPath, int* pBank, int* pProgram)
{
	// The instrument of the first channels
	CSoundFontSynth::TDisplayInfo Info;
	m_pSoundFontSynth->GetDisplayInfo(Info);
	*ppSoundFontPath = m_pSoundFontSynth->GetSoundFontManager().GetSoundFontPath(m_pSoundFontSynth->GetSoundFontIndex());
	*pBank = Info.nBank;
	*pProgram = Info.nProgram;

	return Info.ProgramName[0] && *ppSoundFontPath;
}

void CMT32Pi::ApplyPreset(size_t nIndex)
{
	if (nIndex >= m_Presets.GetCount())
		return;

	// A copy, as applying it changes the current state, which isn't saved back into it meanwhile
	const CPresets::TPreset Preset = m_Presets.Get(nIndex);
	LOGNOTE("Preset %u: %s", static_cast<unsigned>(nIndex + 1), Preset.Name);

	m_bApplyingPreset = true;
	m_nCurrentPreset = nIndex;
	m_LastState.nPreset = nIndex;
	DeferSaveLastState();
	UpdatePresetLabel();

	// SoundFont, then the instrument from it (both only loaded if they aren't already)
	size_t nSoundFont;
	CSoundFontManager& SoundFonts = m_pSoundFontSynth->GetSoundFontManager();
	if (SoundFonts.FindSoundFont(Preset.SoundFontPath, &nSoundFont) && nSoundFont != m_pSoundFontSynth->GetSoundFontIndex())
		SwitchSoundFont(nSoundFont);

	size_t nInstrument;
	if (m_pSoundFontSynth->FindPreset(Preset.nBank, Preset.nProgram, &nInstrument))
		ApplyProgramChange(nInstrument);
	else
		LCDLog(TLCDLogType::Warning, "Instrument not found!");

	if (Preset.bHasEffects)
	{
		// Only those that change: setting the chorus restarts its modulation
		for (u8 nEffect = 0; nEffect < CPresets::EffectCount; ++nEffect)
		{
			const auto Effect = static_cast<CSoundFontSynth::TEffect>(nEffect);
			const float nDifference = m_pSoundFontSynth->GetEffect(Effect) - Preset.Effects[nEffect];
			if (nDifference > 0.001f || nDifference < -0.001f)
				m_pSoundFontSynth->SetEffect(Effect, Preset.Effects[nEffect]);
		}
		m_LastState.Effects = m_pSoundFontSynth->GetEffectOverrides();
	}

	// The TriplePlay's settings (sent once it's connected, if it isn't yet; only those that differ)
	if (m_bTriplePlayEnabled)
	{
		auto Value = [](int nValue) { return nValue != CPresets::Unknown ? nValue : CTriplePlay::Unknown; };

		// The transpose is performed here, and set directly: in a profile, -1 semitone would read as unknown
		if (Preset.nTPTranspose != CPresets::Unknown)
			m_pTriplePlay->SetTranspose(Preset.nTPTranspose);

		CTriplePlay::TProfile Profile;
		Profile.nTranspose = CTriplePlay::Unknown;
		Profile.nBendMode = Value(Preset.nTPBendMode);
		Profile.nBendRange = Value(Preset.nTPBendRange);
		Profile.nChannelMode = Value(Preset.nTPChannelMode);
		for (u8 nString = 0; nString < CTriplePlay::Strings; ++nString)
			Profile.Sensitivity[nString] = Value(Preset.TPSensitivity[nString]);
		m_pTriplePlay->SetProfile(Profile);
	}

	m_bApplyingPreset = false;
}

bool CMT32Pi::GetCurrentPresetState(CPresets::TPreset& Preset)
{
	const char* pSoundFontPath;
	int nBank, nProgram;
	if (!GetCurrentInstrument(&pSoundFontPath, &nBank, &nProgram))
		return false;

	CSoundFontSynth::TDisplayInfo Info;
	m_pSoundFontSynth->GetDisplayInfo(Info);
	snprintf(Preset.Name, sizeof(Preset.Name), "%s", Info.ProgramName);
	snprintf(Preset.SoundFontPath, sizeof(Preset.SoundFontPath), "%s", pSoundFontPath);
	Preset.nBank = nBank;
	Preset.nProgram = nProgram;

	Preset.bHasEffects = true;
	for (u8 nEffect = 0; nEffect < CPresets::EffectCount; ++nEffect)
		Preset.Effects[nEffect] = m_pSoundFontSynth->GetEffect(static_cast<CSoundFontSynth::TEffect>(nEffect));

	// The transpose is performed here; what the TriplePlay hasn't reported (e.g. before it is connected) is left as it is
	if (m_bTriplePlayEnabled)
	{
		auto Take = [](int nValue, int& nStored) { if (nValue != CTriplePlay::Unknown) nStored = nValue; };

		CTriplePlay::TProfile Profile;
		m_pTriplePlay->GetProfile(Profile);
		Preset.nTPTranspose = m_pTriplePlay->GetTranspose();
		Take(Profile.nBendMode, Preset.nTPBendMode);
		Take(Profile.nBendRange, Preset.nTPBendRange);
		Take(Profile.nChannelMode, Preset.nTPChannelMode);
		for (u8 nString = 0; nString < CTriplePlay::Strings; ++nString)
			Take(Profile.Sensitivity[nString], Preset.TPSensitivity[nString]);
	}

	return true;
}

void CMT32Pi::CapturePreset()
{
	// The current preset follows every change; it is saved once the changes have settled
	if (m_bApplyingPreset)
		return;

	// There is always one once there is an instrument
	if (!m_Presets.GetCount())
	{
		CPresets::TPreset Preset;
		CPresets::Clear(Preset);
		if (GetCurrentPresetState(Preset))
			NewPreset();
		return;
	}

	if (m_nCurrentPreset < 0 || static_cast<size_t>(m_nCurrentPreset) >= m_Presets.GetCount())
		return;

	const CPresets::TPreset& Stored = m_Presets.Get(m_nCurrentPreset);
	CPresets::TPreset Preset = Stored;
	if (!GetCurrentPresetState(Preset))
		return;

	const bool bSame = !strcmp(Preset.Name, Stored.Name) && !strcmp(Preset.SoundFontPath, Stored.SoundFontPath)
			   && Preset.nBank == Stored.nBank && Preset.nProgram == Stored.nProgram
			   && Stored.bHasEffects && !memcmp(Preset.Effects, Stored.Effects, sizeof(Preset.Effects))
			   && Preset.nTPTranspose == Stored.nTPTranspose && Preset.nTPBendMode == Stored.nTPBendMode
			   && Preset.nTPBendRange == Stored.nTPBendRange && Preset.nTPChannelMode == Stored.nTPChannelMode
			   && !memcmp(Preset.TPSensitivity, Stored.TPSensitivity, sizeof(Preset.TPSensitivity));
	if (bSame)
		return;

	const bool bOtherInstrument = strcmp(Preset.SoundFontPath, Stored.SoundFontPath) || Preset.nBank != Stored.nBank
				      || Preset.nProgram != Stored.nProgram;

	m_Presets.Replace(m_nCurrentPreset, Preset);
	m_bPresetChanged = true;
	m_nPresetChangeTime = m_pTimer->GetTicks();

	// Its new instrument is the one kept in memory now (Stored has been replaced)
	if (bOtherInstrument)
		UpdatePinnedInstruments();
}

void CMT32Pi::NewPreset()
{
	// From the current state
	CPresets::TPreset Preset;
	CPresets::Clear(Preset);
	if (!GetCurrentPresetState(Preset))
	{
		LCDLog(TLCDLogType::Warning, "No instrument!");
		return;
	}

	if (!m_Presets.Add(Preset))
	{
		LCDLog(TLCDLogType::Warning, "No more presets!");
		return;
	}

	m_nCurrentPreset = m_Presets.GetCount() - 1;
	m_LastState.nPreset = m_nCurrentPreset;
	DeferSaveLastState();
	UpdatePresetLabel();
	UpdatePinnedInstruments();

	m_bPresetChanged = true;
	m_nPresetChangeTime = m_pTimer->GetTicks();
}

void CMT32Pi::DeletePreset()
{
	// There is always a current preset
	if (m_nCurrentPreset < 0 || m_Presets.GetCount() < 2)
		return;

	m_Presets.Delete(m_nCurrentPreset);

	// The preset that moved into its place is current now, without changing the sound
	if (static_cast<size_t>(m_nCurrentPreset) >= m_Presets.GetCount())
		m_nCurrentPreset = static_cast<int>(m_Presets.GetCount()) - 1;
	m_LastState.nPreset = m_nCurrentPreset;
	DeferSaveLastState();
	UpdatePresetLabel();
	UpdatePinnedInstruments();
}

void CMT32Pi::UpdatePresetLabel()
{
	// Shown before the instrument's number on the home screen
	char Label[16] = "";
	if (m_nCurrentPreset >= 0)
		snprintf(Label, sizeof(Label), "P%d", m_nCurrentPreset + 1);
	m_UserInterface.SetPresetLabel(Label);
}

void CMT32Pi::SetTriplePlaySoftwareMode(bool bSoftwareMode)
{
	if (bSoftwareMode == m_bTriplePlaySoftwareMode)
		return;

	m_bTriplePlaySoftwareMode = bSoftwareMode;
	if (m_bTriplePlayEnabled)
		m_pTriplePlay->SetSoftwareMode(bSoftwareMode);

	m_LastState.nTriplePlaySoftwareMode = bSoftwareMode;
	DeferSaveLastState();
}

void CMT32Pi::SetTriplePlayProgramPresets(bool bOn)
{
	if (bOn == m_bTriplePlayProgramPresets)
		return;

	m_bTriplePlayProgramPresets = bOn;
	m_pTriplePlay->SetProgramsToHost(bOn);
	m_nTriplePlayProgram = -1;

	m_LastState.nTriplePlayProgramPresets = bOn;
	DeferSaveLastState();
}

void CMT32Pi::UpdateTriplePlayPrograms()
{
	// The program change it sent last (one per channel it plays on, so the same one several times)
	const int nProgram = m_nTriplePlayProgram;
	m_nTriplePlayProgram = -1;
	const int nCount = static_cast<int>(m_Presets.GetCount());

	if (nProgram >= 0 && nCount)
	{
		if (nProgram >= nCount)
			LCDLog(TLCDLogType::Warning, "No preset %d!", nProgram + 1);
		else if (nProgram != m_nCurrentPreset)
			ApplyPreset(nProgram);
	}

	// Its buttons' program up/down in software mode, round the list as the footswitches
	const int nStep = m_pTriplePlay->TakeProgramStep();
	if (nStep && nCount)
	{
		const int nCurrent = m_nCurrentPreset >= 0 ? m_nCurrentPreset : (nStep > 0 ? -1 : 0);
		ApplyPreset(((nCurrent + nStep) % nCount + nCount) % nCount);
	}
}

void CMT32Pi::UpdatePinnedInstruments()
{
	// The instruments of the presets from the current SoundFont stay in memory, so that switching
	// between them doesn't load anything (loading interrupts the sound)
	int Banks[CSoundFontSynth::MaxPinnedInstruments];
	int Programs[CSoundFontSynth::MaxPinnedInstruments];
	size_t nCount = 0;

	const char* pSoundFontPath = m_pSoundFontSynth->GetSoundFontManager().GetSoundFontPath(m_pSoundFontSynth->GetSoundFontIndex());
	if (pSoundFontPath)
	{
		// The current one first, then those that follow it, as they are the likeliest to come next
		const size_t nPresets = m_Presets.GetCount();
		const size_t nFirst = m_nCurrentPreset >= 0 ? m_nCurrentPreset : 0;
		for (size_t i = 0; i < nPresets && nCount < CSoundFontSynth::MaxPinnedInstruments; ++i)
		{
			const CPresets::TPreset& Preset = m_Presets.Get((nFirst + i) % nPresets);
			if (strcmp(Preset.SoundFontPath, pSoundFontPath))
				continue;
			Banks[nCount] = Preset.nBank;
			Programs[nCount] = Preset.nProgram;
			++nCount;
		}
	}

	m_pSoundFontSynth->SetPinnedInstruments(Banks, Programs, nCount);
}

const char* CMT32Pi::GetSettingName(u8 nSetting) const
{
	// The TriplePlay's settings are in its own submenus, so they don't need its name
	static const char* const TPNames[] =
	{
		"Octave", "Semitones", "Bend mode", "Bend range", "Channels", "Up/Down buttons", "Both buttons", "Hand position",
	};
	static_assert(Utility::ArraySize(TPNames) == static_cast<size_t>(CTriplePlay::TSetting::Count), "TriplePlay setting names");

	// 1 = high E
	static const char* const SensitivityNames[CTriplePlay::Strings] =
	{
		"Sens. string 1 (e)", "Sens. string 2 (B)", "Sens. string 3 (G)",
		"Sens. string 4 (D)", "Sens. string 5 (A)", "Sens. string 6 (E)",
	};

	switch (nSetting)
	{
		case SettingVolume:		return "Volume";
		case SettingChannels:		return "Channels shown";
		case SettingDisplayStandby:	return "Display standby";
		case SettingDrumKit:		return "Drum kit";
		case SettingSoundFont:		return "SoundFont";
		case SettingTPEnabled:		return "TriplePlay";
		case SettingTPSoftwareMode:	return "Software mode";
		case SettingTPProgramPresets:	return "Program change";
		case SettingPresetMenu:		return "Preset";
		case SettingPresetSelect:	return "Preset";
		case SettingPresetInstrument:	return "Instrument";
		case SettingPresetNew:		return "+ New preset";
		case SettingPresetDelete:	return "Delete preset";
		case SettingTPBattery:		return "Battery";
		case SettingPresetTPMenu:	return "TriplePlay";
		case SettingGlobalMenu:		return "Global";
		case SettingGlobalTPMenu:	return "TriplePlay";
		case SettingEffectsMenu:	return "Effects";
		case SettingBack:		return "Back";
		case SettingExit:		return "Exit";
		default:			break;
	}

	static const char* const OpenStringNames[CTriplePlay::Strings] =
	{
		"Open string 1", "Open string 2", "Open string 3", "Open string 4", "Open string 5", "Open string 6",
	};

	if (nSetting >= SettingTPOpenString)
		return OpenStringNames[nSetting - SettingTPOpenString];
	if (nSetting >= SettingTPSensitivity)
		return SensitivityNames[nSetting - SettingTPSensitivity];
	if (nSetting >= SettingTPFirst)
		return TPNames[nSetting - SettingTPFirst];

	return CSoundFontSynth::GetEffectInfo(static_cast<CSoundFontSynth::TEffect>(nSetting - SettingEffects)).pName;
}

const char* CMT32Pi::GetSettingsPageTitle() const
{
	switch (m_SettingsPage)
	{
		case TSettingsPage::Preset:		return "Preset";
		case TSettingsPage::PresetTP:		return "TriplePlay: preset";
		case TSettingsPage::Global:		return "Global";
		case TSettingsPage::Effects:		return "Effects";
		case TSettingsPage::GlobalTP:		return "TriplePlay";
		default:				return "Settings";
	}
}

bool CMT32Pi::FormatSettingValue(u8 nSetting, char* pBuffer, size_t nSize, float* pFraction)
{
	using TPresetList = CSoundFontSynth::TPresetList;
	float nFraction = 0.0f;

	if (nSetting == SettingVolume)
	{
		snprintf(pBuffer, nSize, "%d", m_nMasterVolume);
		nFraction = m_nMasterVolume / 100.0f;
	}
	else if (nSetting == SettingChannels)
	{
		const u8 nChannels = m_UserInterface.GetDisplayChannels();
		snprintf(pBuffer, nSize, "%d", nChannels);
		nFraction = static_cast<float>(nChannels) / MaxDisplayChannels;
	}
	else if (nSetting == SettingDisplayStandby)
	{
		const unsigned nSeconds = m_UserInterface.GetStandbyTimeout();
		if (!nSeconds)
			snprintf(pBuffer, nSize, "Off");
		else if (nSeconds % 60)
			snprintf(pBuffer, nSize, "%u s", nSeconds);
		else
			snprintf(pBuffer, nSize, "%u min", nSeconds / 60);
		nFraction = static_cast<float>(GetStandbyStep(nSeconds)) / (Utility::ArraySize(StandbySteps) - 1);
	}
	else if (nSetting == SettingDrumKit)
	{
		const size_t nIndex = m_pSoundFontSynth->GetChannelPresetIndex(CSoundFontSynth::DrumChannel, TPresetList::DrumKits);
		m_pSoundFontSynth->FormatPresetName(nIndex, pBuffer, nSize, TPresetList::DrumKits);
	}
	else if (nSetting == SettingPresetInstrument)
		m_pSoundFontSynth->FormatPresetName(m_pSoundFontSynth->GetChannelPresetIndex(0), pBuffer, nSize);
	else if (nSetting == SettingSoundFont)
	{
		const char* pName = m_pSoundFontSynth->GetSoundFontManager().GetSoundFontName(m_pSoundFontSynth->GetSoundFontIndex());
		snprintf(pBuffer, nSize, "%s", pName ? pName : "-");
	}
	else if (nSetting == SettingTPEnabled || nSetting == SettingTPSoftwareMode)
	{
		const bool bOn = nSetting == SettingTPEnabled ? m_bTriplePlayEnabled : m_bTriplePlaySoftwareMode;
		snprintf(pBuffer, nSize, "%s", bOn ? "On" : "Off");
		nFraction = bOn ? 1.0f : 0.0f;
	}
	else if (nSetting == SettingTPProgramPresets)
	{
		// What the TriplePlay's program changes select
		snprintf(pBuffer, nSize, "%s", m_bTriplePlayProgramPresets ? "Preset" : "Instrument");
		nFraction = m_bTriplePlayProgramPresets ? 1.0f : 0.0f;
	}
	else if (nSetting == SettingPresetSelect || nSetting == SettingPresetDelete)
	{
		// The current preset
		if (m_nCurrentPreset >= 0 && static_cast<size_t>(m_nCurrentPreset) < m_Presets.GetCount())
			snprintf(pBuffer, nSize, "P%d %s", m_nCurrentPreset + 1, m_Presets.Get(m_nCurrentPreset).Name);
		else
			snprintf(pBuffer, nSize, "-");
	}
	else if (nSetting == SettingPresetNew)
		snprintf(pBuffer, nSize, "P%u", static_cast<unsigned>(m_Presets.GetCount() + 1));
	else if (nSetting == SettingTPBattery)
		m_pTriplePlay->FormatBattery(pBuffer, nSize);
	else if (nSetting >= SettingTPSensitivity && nSetting < SettingTPSensitivity + CTriplePlay::Strings)
	{
		const int nLevel = m_pTriplePlay->GetStringSensitivity(nSetting - SettingTPSensitivity + 1);
		if (nLevel)
			snprintf(pBuffer, nSize, "%d", nLevel);
		else
			snprintf(pBuffer, nSize, "-");
		nFraction = static_cast<float>(nLevel) / CTriplePlay::MaxSensitivity;
	}
	else if (nSetting >= SettingTPOpenString && nSetting < SettingTPOpenString + CTriplePlay::Strings)
		m_pTriplePlay->FormatOpenString(nSetting - SettingTPOpenString + 1, pBuffer, nSize);
	else if (nSetting >= SettingTPFirst && nSetting < SettingTPBattery)
		m_pTriplePlay->FormatSetting(static_cast<CTriplePlay::TSetting>(nSetting - SettingTPFirst), pBuffer, nSize);
	else if (nSetting >= SettingEffects && nSetting < SettingChannels)
	{
		const auto Effect = static_cast<CSoundFontSynth::TEffect>(nSetting - SettingEffects);
		const CSoundFontSynth::TEffectInfo& Info = CSoundFontSynth::GetEffectInfo(Effect);
		const float nValue = m_pSoundFontSynth->GetEffect(Effect);

		snprintf(pBuffer, nSize, "%.*f", Info.nDecimals, nValue);
		nFraction = (nValue - Info.nMin) / (Info.nMax - Info.nMin);
	}
	else
	{
		// Submenus and the items that leave them have no value
		*pBuffer = '\0';
		return false;
	}

	if (pFraction)
		*pFraction = nFraction;
	return true;
}

void CMT32Pi::OpenMenu(TMenu Menu)
{
	m_Menu = Menu;
	m_nMenuTime = m_pTimer->GetTicks();

	if (Menu == TMenu::Instruments || Menu == TMenu::PresetInstruments)
		m_nMenuIndex = m_nMenuCurrentIndex = m_pSoundFontSynth->GetChannelPresetIndex(0);
	else if (Menu == TMenu::Settings)
	{
		// The item that was open last is highlighted; no item is "in use"
		const int nIndex = FindSettingsItem(m_nSetting);
		m_nMenuIndex = nIndex >= 0 ? nIndex : 0;
		m_nMenuCurrentIndex = MaxSettingsPageItems;
	}
	else if (Menu == TMenu::DrumKits)
		m_nMenuIndex = m_nMenuCurrentIndex = m_pSoundFontSynth->GetChannelPresetIndex(CSoundFontSynth::DrumChannel, CSoundFontSynth::TPresetList::DrumKits);
	else if (Menu == TMenu::SoundFonts)
		m_nMenuIndex = m_nMenuCurrentIndex = m_pSoundFontSynth->GetSoundFontIndex();
	else if (Menu == TMenu::Presets)
		m_nMenuIndex = m_nMenuCurrentIndex = m_nCurrentPreset >= 0 ? m_nCurrentPreset : 0;

	UpdateMenu();
}

void CMT32Pi::CloseMenu(bool bKeepSettingsPosition)
{
	// The settings start from the top again, unless they were left only for the moment
	if (IsSettingsMenu() && !bKeepSettingsPosition)
	{
		m_SettingsPage = TSettingsPage::Root;
		m_nSetting = SettingPresetMenu;
	}

	m_Menu = TMenu::None;
	m_UserInterface.HideMenu();
}

void CMT32Pi::UpdateMenu()
{
	if (!m_pDisplay || m_Menu == TMenu::None)
		return;

	CUserInterface::TMenu Menu = {};
	Menu.nCurrentRow = -1;

	if (m_Menu == TMenu::Parameter)
	{
		// Name and value with a gauge; the position in the list, without "Back"/"Exit"
		u8 Items[MaxSettingsPageItems];
		Menu.Type = CUserInterface::TMenu::TType::Parameter;
		Menu.nPage = FindSettingsItem(m_nSetting) + 1;
		Menu.nPages = GetSettingsItems(Items) - 1;
		snprintf(Menu.Title, sizeof(Menu.Title), "%s", GetSettingName(m_nSetting));
		FormatSettingValue(m_nSetting, Menu.Value, sizeof(Menu.Value), &Menu.nFraction);

		m_UserInterface.ShowMenu(Menu);
		return;
	}

	// List with the selection in the middle row
	using TPresetList = CSoundFontSynth::TPresetList;
	const size_t nCount = GetMenuListCount();
	constexpr int nSelectedRow = CUserInterface::TMenu::SelectedRow;

	Menu.Type = CUserInterface::TMenu::TType::List;
	if (m_Menu == TMenu::Instruments)
	{
		// The SoundFont the instruments are from
		const char* pName = m_pSoundFontSynth->GetSoundFontManager().GetSoundFontName(m_pSoundFontSynth->GetSoundFontIndex());
		snprintf(Menu.Title, sizeof(Menu.Title), "%s", pName ? pName : "Instrument");
	}
	else if (m_Menu == TMenu::Settings)
		snprintf(Menu.Title, sizeof(Menu.Title), "%s", GetSettingsPageTitle());
	else
	{
		const char* const pTitle = m_Menu == TMenu::DrumKits ? "Drum kit" : m_Menu == TMenu::Presets ? "Preset"
					 : m_Menu == TMenu::PresetInstruments ? "Instrument" : "SoundFont";
		snprintf(Menu.Title, sizeof(Menu.Title), "%s %d/%d", pTitle, static_cast<int>(m_nMenuIndex + 1), static_cast<int>(nCount));
	}

	u8 SettingsItems[MaxSettingsPageItems];
	if (m_Menu == TMenu::Settings)
		GetSettingsItems(SettingsItems);

	for (int nRow = 0; nRow < static_cast<int>(CUserInterface::TMenu::ListRows); ++nRow)
	{
		const int nIndex = static_cast<int>(m_nMenuIndex) + nRow - nSelectedRow;
		if (nIndex < 0 || nIndex >= static_cast<int>(nCount))
			continue;

		char* pRow = Menu.Rows[nRow];
		if (m_Menu == TMenu::Instruments || m_Menu == TMenu::PresetInstruments)
			m_pSoundFontSynth->FormatPresetName(nIndex, pRow, sizeof(Menu.Rows[nRow]));
		else if (m_Menu == TMenu::DrumKits)
			m_pSoundFontSynth->FormatPresetName(nIndex, pRow, sizeof(Menu.Rows[nRow]), TPresetList::DrumKits);
		else if (m_Menu == TMenu::Settings)
		{
			// Name and current value, which is shortened if needed; submenus are marked
			const u8 nSetting = SettingsItems[nIndex];
			snprintf(pRow, sizeof(Menu.Rows[nRow]), "%s: ", GetSettingName(nSetting));
			const size_t nLength = strlen(pRow);
			if (!FormatSettingValue(nSetting, pRow + nLength, sizeof(Menu.Rows[nRow]) - nLength))
			{
				const char* pName = GetSettingName(nSetting);
				if (nSetting == SettingBack)
					snprintf(pRow, sizeof(Menu.Rows[nRow]), "< %s", pName);
				else if (nSetting == SettingExit)
					snprintf(pRow, sizeof(Menu.Rows[nRow]), "%s", pName);
				else
					snprintf(pRow, sizeof(Menu.Rows[nRow]), "%s >", pName);
			}
		}
		else if (m_Menu == TMenu::Presets)
		{
			// Number and name, which is shortened if needed
			snprintf(pRow, sizeof(Menu.Rows[nRow]), "P%d ", nIndex + 1);
			const size_t nLength = strlen(pRow);
			snprintf(pRow + nLength, sizeof(Menu.Rows[nRow]) - nLength, "%s", m_Presets.Get(nIndex).Name);
		}
		else
		{
			const char* pName = m_pSoundFontSynth->GetSoundFontManager().GetSoundFontName(nIndex);
			snprintf(pRow, sizeof(Menu.Rows[nRow]), "%s", pName ? pName : "?");
		}

		if (static_cast<size_t>(nIndex) == m_nMenuCurrentIndex)
			Menu.nCurrentRow = nRow;
	}

	m_UserInterface.ShowMenu(Menu);
}

size_t CMT32Pi::GetMenuListCount() const
{
	switch (m_Menu)
	{
		case TMenu::Instruments:
		case TMenu::PresetInstruments:
			return m_pSoundFontSynth->GetPresetCount();

		case TMenu::Settings:
		{
			u8 Items[MaxSettingsPageItems];
			return GetSettingsItems(Items);
		}

		case TMenu::DrumKits:
			return m_pSoundFontSynth->GetPresetCount(CSoundFontSynth::TPresetList::DrumKits);

		case TMenu::SoundFonts:
			return m_pSoundFontSynth->GetSoundFontManager().GetSoundFontCount();

		case TMenu::Presets:
			return m_Presets.GetCount();

		default:
			return 0;
	}
}

void CMT32Pi::MoveListSelection(int nDelta)
{
	const size_t nCount = GetMenuListCount();
	if (!nCount)
	{
		CloseMenu();
		LCDLog(TLCDLogType::Error, "Nothing to select!");
		return;
	}

	m_nMenuIndex = (m_nMenuIndex + nCount + nDelta % static_cast<int>(nCount)) % nCount;
	m_nMenuTime = m_pTimer->GetTicks();
	UpdateMenu();
}

void CMT32Pi::MoveInstrumentSelection(int nDelta)
{
	if (m_Menu != TMenu::Instruments)
		OpenMenu(TMenu::Instruments);

	if (!m_pSoundFontSynth->GetPresetCount())
	{
		CloseMenu();
		LCDLog(TLCDLogType::Error, "No instruments!");
		return;
	}

	MoveListSelection(nDelta);
}

void CMT32Pi::SelectMenuInstrument()
{
	CloseMenu();
	ApplyProgramChange(m_nMenuIndex);
}

void CMT32Pi::ChangeParameter(int nDelta)
{
	if (m_nSetting == SettingVolume)
	{
		SetMasterVolume(m_nMasterVolume + nDelta);
		return;
	}

	if (m_nSetting == SettingChannels)
	{
		const int nChannels = Utility::Clamp(m_UserInterface.GetDisplayChannels() + nDelta, 1, static_cast<int>(MaxDisplayChannels));
		m_UserInterface.SetDisplayChannels(nChannels);
		m_LastState.nDisplayChannels = nChannels;
		DeferSaveLastState();
		return;
	}

	if (m_nSetting == SettingDisplayStandby)
	{
		const int nStep = Utility::Clamp(static_cast<int>(GetStandbyStep(m_UserInterface.GetStandbyTimeout())) + nDelta, 0, static_cast<int>(Utility::ArraySize(StandbySteps)) - 1);
		m_UserInterface.SetStandbyTimeout(StandbySteps[nStep]);
		m_LastState.nDisplayStandby = StandbySteps[nStep];
		DeferSaveLastState();
		return;
	}

	// Switches: turning right switches on, left off
	if (m_nSetting == SettingTPEnabled)
	{
		SetTriplePlayEnabled(nDelta > 0);
		return;
	}

	if (m_nSetting == SettingTPSoftwareMode)
	{
		SetTriplePlaySoftwareMode(nDelta > 0);
		return;
	}

	if (m_nSetting == SettingTPProgramPresets)
	{
		SetTriplePlayProgramPresets(nDelta > 0);
		return;
	}

	// Stored in the TriplePlay itself
	if (m_nSetting >= SettingTPSensitivity && m_nSetting < SettingTPSensitivity + CTriplePlay::Strings)
	{
		// Not before the current value has been read, so that it can't be changed blindly
		const u8 nString = m_nSetting - SettingTPSensitivity + 1;
		const int nLevel = m_pTriplePlay->GetStringSensitivity(nString);
		if (nLevel)
			m_pTriplePlay->SetStringSensitivity(nString, nLevel + nDelta);
		return;
	}

	if (m_nSetting >= SettingTPOpenString && m_nSetting < SettingTPOpenString + CTriplePlay::Strings)
	{
		m_pTriplePlay->ChangeOpenString(m_nSetting - SettingTPOpenString + 1, nDelta);
		return;
	}

	// The TriplePlay's patch settings, button assignments and hand position monitor
	if (m_nSetting >= SettingTPFirst && m_nSetting < SettingTPBattery)
	{
		m_pTriplePlay->ChangeSetting(static_cast<CTriplePlay::TSetting>(m_nSetting - SettingTPFirst), nDelta);
		return;
	}

	if (m_nSetting < SettingEffects || m_nSetting >= SettingChannels)
		return;

	const auto Effect = static_cast<CSoundFontSynth::TEffect>(m_nSetting - SettingEffects);
	const CSoundFontSynth::TEffectInfo& Info = CSoundFontSynth::GetEffectInfo(Effect);

	// Move in whole steps from the current value, so that values stay on the step grid
	const float nValue = m_pSoundFontSynth->GetEffect(Effect);
	const int nStep = static_cast<int>((nValue - Info.nMin) / Info.nStep + 0.5f) + nDelta;
	m_pSoundFontSynth->SetEffect(Effect, Info.nMin + nStep * Info.nStep);

	m_LastState.Effects = m_pSoundFontSynth->GetEffectOverrides();
	DeferSaveLastState();
}

void CMT32Pi::SwitchSoundFont(size_t nIndex)
{
	LOGNOTE("Switching to SoundFont %d", nIndex);

	if (m_pSoundFontSynth->SwitchSoundFont(nIndex))
	{
		m_LastState.SoundFontPath = m_pSoundFontSynth->GetSoundFontManager().GetSoundFontPath(nIndex);
		DeferSaveLastState();

		// Keep the instrument if the new SoundFont has it
		ApplyLastInstrument();

		// The new synth keeps nothing in memory yet
		UpdatePinnedInstruments();

		// Handle any MIDI data that has been queued up while busy
		PurgeMIDIBuffers();

		m_pSoundFontSynth->ReportStatus();
	}
}

void CMT32Pi::ApplyLastInstrument()
{
	using TPresetList = CSoundFontSynth::TPresetList;
	size_t nIndex;

	// Only if the SoundFont has them
	if (m_LastState.nProgram >= 0 && m_pSoundFontSynth->FindPreset(m_LastState.nBank, m_LastState.nProgram, &nIndex))
	{
		LOGNOTE("Selecting instrument %d:%d", m_LastState.nBank, m_LastState.nProgram);
		m_pSoundFontSynth->SetChannelsPreset(nIndex, 0, ButtonProgramChannels);
	}

	if (m_LastState.nDrumProgram >= 0 && m_pSoundFontSynth->FindPreset(m_LastState.nDrumBank, m_LastState.nDrumProgram, &nIndex, TPresetList::DrumKits))
	{
		LOGNOTE("Selecting drum kit %d:%d", m_LastState.nDrumBank, m_LastState.nDrumProgram);
		m_pSoundFontSynth->SetChannelsPreset(nIndex, CSoundFontSynth::DrumChannel, 1, nullptr, TPresetList::DrumKits);
	}
}

bool CMT32Pi::ApplyProgramChange(size_t nIndex)
{
	bool bSamplesLoaded;
	const bool bResult = m_pSoundFontSynth->SetChannelsPreset(nIndex, 0, ButtonProgramChannels, &bSamplesLoaded);

	// Drop notes played while loading rather than playing them late
	if (bSamplesLoaded)
		PurgeMIDIBuffers();

	if (!bResult)
		LCDLog(TLCDLogType::Error, "Out of memory!");
	else if (m_pSoundFontSynth->GetPreset(nIndex, &m_LastState.nBank, &m_LastState.nProgram))
		DeferSaveLastState();

	Awaken();
	return bResult;
}

bool CMT32Pi::ApplyDrumKit(size_t nIndex)
{
	using TPresetList = CSoundFontSynth::TPresetList;

	bool bSamplesLoaded;
	const bool bResult = m_pSoundFontSynth->SetChannelsPreset(nIndex, CSoundFontSynth::DrumChannel, 1, &bSamplesLoaded, TPresetList::DrumKits);

	if (bSamplesLoaded)
		PurgeMIDIBuffers();

	if (!bResult)
		LCDLog(TLCDLogType::Error, "Out of memory!");
	else if (m_pSoundFontSynth->GetPreset(nIndex, &m_LastState.nDrumBank, &m_LastState.nDrumProgram, TPresetList::DrumKits))
		DeferSaveLastState();

	Awaken();
	return bResult;
}

void CMT32Pi::DeferSwitchSoundFont(size_t nIndex)
{
	const char* pName = m_pSoundFontSynth->GetSoundFontManager().GetSoundFontName(nIndex);
	LCDLog(TLCDLogType::Notice, "SF %ld: %s", nIndex, pName ? pName : "- N/A -");
	m_nDeferredSoundFontSwitchIndex = nIndex;
	m_nDeferredSoundFontSwitchTime  = CTimer::Get()->GetTicks();
	m_bDeferredSoundFontSwitchFlag  = true;
}

void CMT32Pi::SetMasterVolume(s32 nVolume)
{
	m_nMasterVolume = Utility::Clamp(nVolume, 0, 100);
	m_pSoundFontSynth->SetMasterVolume(m_nMasterVolume);
	m_UserInterface.SetVolume(m_nMasterVolume);

	m_LastState.nVolume = m_nMasterVolume;
	DeferSaveLastState();
}

void CMT32Pi::DeferSaveLastState()
{
	m_nLastStateChangeTime = CTimer::Get()->GetTicks();
	m_bLastStateChanged    = true;

	// The SoundFont, instrument and effects also belong to the current preset
	CapturePreset();
}

void CMT32Pi::LEDOn()
{
	m_pActLED->On();
	m_nLEDOnTime = m_pTimer->GetTicks();
	m_bLEDOn = true;
}

void CMT32Pi::LCDLog(TLCDLogType Type, const char* pFormat...)
{
	if (!m_pDisplay)
		return;

	char Buffer[256];

	va_list Args;
	va_start(Args, pFormat);
	vsnprintf(Buffer, sizeof(Buffer), pFormat, Args);
	va_end(Args);

	// UI task hasn't started yet; draw directly
	if (Type == TLCDLogType::Startup)
	{
		if (!m_bStartupDone)
			m_UserInterface.ShowStartup(*m_pDisplay, MT32_PI_VERSION, Buffer);
		return;
	}

	// Let UI task pick up the message in its next update
	CUserInterface::TMessageType MessageType = CUserInterface::TMessageType::Notice;
	if (Type == TLCDLogType::Error)
		MessageType = CUserInterface::TMessageType::Error;
	else if (Type == TLCDLogType::Warning)
		MessageType = CUserInterface::TMessageType::Warning;

	m_UserInterface.ShowSystemMessage(Buffer, Type == TLCDLogType::Spinner, MessageType);
}

const char* CMT32Pi::GetNetworkDeviceShortName() const
{
	switch (m_pConfig->NetworkMode)
	{
		case CConfig::TNetworkMode::Ethernet:	return "Ether";
		case CConfig::TNetworkMode::WiFi:	return "WiFi";
		default:				return "None";
	}
}

void CMT32Pi::EventHandler(const TEvent& Event)
{
	assert(s_pThis != nullptr);

	// Enqueue event
	s_pThis->m_EventQueue.Enqueue(Event);
}

void CMT32Pi::USBMIDIDeviceRemovedHandler(CDevice* pDevice, void* pContext)
{
	assert(s_pThis != nullptr);

	void** pDevicePointer = reinterpret_cast<void**>(pContext);
	*pDevicePointer = nullptr;

	// Re-enable serial MIDI if not in-use by logger and no other MIDI devices available
	if (s_pThis->m_bSerialMIDIAvailable && !(s_pThis->m_pUSBMIDIDevice || s_pThis->m_pUSBSerialDevice || s_pThis->m_pPisound))
	{
		LOGNOTE("Using serial MIDI interface");
		s_pThis->m_bSerialMIDIEnabled = true;
	}
}

// The following handlers are called from interrupt context, enqueue into ring buffer for main thread
void CMT32Pi::USBMIDIPacketHandler(unsigned nCable, u8* pPacket, unsigned nLength)
{
	// The TriplePlay's second port carries its settings (and copies of the guitar's pitch bends), not notes to play.
	// Its settings are taken from any port, as it isn't certain which one the controller answers on.
	if (s_pThis->m_bTriplePlayEnabled && (nCable == CTriplePlay::CommandCable || CTriplePlay::IsCommandMessage(pPacket, nLength)))
	{
		s_pThis->m_pTriplePlay->ReceiveMessage(pPacket, nLength);
		return;
	}

	if (s_pThis->m_bTriplePlayEnabled)
	{
		// Its program changes select presets (if there are any) instead of the synth's instrument
		if (s_pThis->m_bTriplePlayProgramPresets && nLength == 2 && (pPacket[0] & 0xF0) == 0xC0 && s_pThis->m_Presets.GetCount())
		{
			s_pThis->m_nTriplePlayProgram = pPacket[1] & 0x7F;
			return;
		}

		// The guitar's notes, with the transpose (of the current preset)
		if (!s_pThis->m_pTriplePlay->TransposeMessage(pPacket, nLength))
			return;
	}

	IRQMIDIReceiveHandler(pPacket, nLength);
}

void CMT32Pi::IRQMIDIReceiveHandler(const u8* pData, size_t nSize)
{
	assert(s_pThis != nullptr);

	// Enqueue data into ring buffer
	if (s_pThis->m_MIDIRxBuffer.Enqueue(pData, nSize) != nSize)
	{
		static const char* pErrorString = "MIDI overrun error!";
		LOGWARN(pErrorString);
		s_pThis->LCDLog(TLCDLogType::Error, pErrorString);
	}
}

void CMT32Pi::LoadProgressHandler(u64 nLoadedBytes, u64 nTotalBytes, void* pContext)
{
	CMT32Pi* const pThis = static_cast<CMT32Pi*>(pContext);
	if (!pThis->m_pDisplay)
		return;

	// The UI task hasn't started yet during startup; draw directly
	if (pThis->m_bStartupDone)
		pThis->m_UserInterface.ShowLoadProgress(nLoadedBytes, nTotalBytes);
	else
		pThis->m_UserInterface.ShowStartupProgress(*pThis->m_pDisplay, MT32_PI_VERSION, nLoadedBytes, nTotalBytes);
}

void CMT32Pi::PanicHandler()
{
	if (!s_pThis || !s_pThis->m_pDisplay)
		return;

	// Kill UI task, if it is running already
	s_pThis->m_bRunning = false;
	while (s_pThis->m_bStartupDone && !s_pThis->m_bUITaskDone)
		;

	char Buffer[LOGGER_BUFSIZE];
	CLogger::Get()->Read(Buffer, sizeof(Buffer), false);

	// Find the last message: after the second-last newline, past the timestamp and log source, without colour codes
	const char* pMessage = "Unknown error";
	char* pMessageStart = strrchr(Buffer, '\n');
	if (pMessageStart)
	{
		*pMessageStart = '\0';
		pMessageStart = strrchr(Buffer, '\n');
		pMessageStart = pMessageStart ? strstr(pMessageStart, ": ") : nullptr;
		if (pMessageStart)
		{
			pMessageStart += 2;
			char* pMessageEnd = strstr(pMessageStart, "\x1B[0m");
			if (pMessageEnd)
				*pMessageEnd = '\0';
			pMessage = pMessageStart;
		}
	}

	s_pThis->m_UserInterface.ShowPanic(*s_pThis->m_pDisplay, pMessage);
}