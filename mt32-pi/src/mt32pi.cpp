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

#define MT32_PI_NAME "mt32-pi"
LOGMODULE(MT32_PI_NAME);
const char MT32PiFullName[] = MT32_PI_NAME " " MT32_PI_VERSION;

const char WLANFirmwarePath[] = "SD:firmware/";
const char WLANConfigFile[]   = "SD:wpa_supplicant.conf";
const char LastStateFile[]    = "SD:last_state.cfg";

constexpr u32 LCDUpdatePeriodMillis                = 16;
constexpr u32 MisterUpdatePeriodMillis             = 50;
constexpr u32 LEDTimeoutMillis                     = 50;
constexpr u32 ActiveSenseTimeoutMillis             = 330;

constexpr float Sample24BitMax = (1 << 24 - 1) - 1;

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

	  m_bDeferredProgramChangeFlag(false),
	  m_nDeferredProgramChangeIndex(0),
	  m_nDeferredProgramChangeTime(0),

	  m_Menu(TMenu::None),
	  m_nParameterPage(ParameterPageVolume),
	  m_nMenuIndex(0),
	  m_nMenuCurrentIndex(0),
	  m_nMenuTime(0),
	  m_nLastParameterTurnTime(0),

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
			m_UserInterface.ShowStartup(*m_pDisplay, MT32PiFullName, nullptr);
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

	if (m_LastState.Load(LastStateFile))
		LOGNOTE("Restoring settings from '%s'", LastStateFile);

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

	if (m_LastState.nProgram >= 0)
	{
		LCDLog(TLCDLogType::Startup, "Loading instrument");
		ApplyLastInstrument();
	}
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

		// Check for deferred instrument change
		if (m_bDeferredProgramChangeFlag && (nTicks - m_nDeferredProgramChangeTime) >= MSEC2HZ(ProgramChangeDelayMillis))
			ApplyProgramChange(m_nDeferredProgramChangeIndex);

		// Close menus that aren't used anymore
		if (m_Menu != TMenu::None && (nTicks - m_nMenuTime) >= MSEC2HZ(MenuTimeoutMillis))
			CloseMenu();

		// Save settings once they have settled
		if (m_bLastStateChanged && (nTicks - m_nLastStateChangeTime) >= MSEC2HZ(LastStateSaveDelayMillis))
		{
			m_bLastStateChanged = false;
			m_LastState.Save(LastStateFile);
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

	// Flash LED for channel messages
	if ((nMessage & 0xFF) < 0xF0)
		LEDOn();

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
		Awaken();

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
	if (!Event.bPressed)
		return;

	switch (Event.Button)
	{
		// Previous/next instrument for the first MIDI channels
		case TButton::Button1:
			DeferProgramChange(-1);
			break;

		case TButton::Button2:
			DeferProgramChange(1);
			break;

		case TButton::Button3:
		case TButton::Button4:
			SetMasterVolume(m_nMasterVolume + (Event.Button == TButton::Button3 ? -1 : 1));
			LCDLog(TLCDLogType::Notice, "Volume: %d", m_nMasterVolume);
			break;

		// Select the highlighted instrument, or open the list
		case TButton::EncoderButton:
			if (Event.bRepeat)
				break;

			if (m_Menu == TMenu::Instruments)
			{
				CloseMenu();
				m_bDeferredProgramChangeFlag = false;
				ApplyProgramChange(m_nMenuIndex);
			}
			else
				OpenMenu(TMenu::Instruments);
			break;

		// Next parameter; in the SoundFont list, load the highlighted SoundFont if it isn't the current one
		case TButton::Encoder2Button:
			if (Event.bRepeat)
				break;

			if (m_Menu == TMenu::SoundFonts && m_nMenuIndex != m_pSoundFontSynth->GetSoundFontIndex())
			{
				CloseMenu();
				m_bDeferredSoundFontSwitchFlag = false;
				SwitchSoundFont(m_nMenuIndex);
			}
			else if (m_Menu == TMenu::SoundFonts)
			{
				m_nParameterPage = ParameterPageVolume;
				OpenMenu(TMenu::Parameter);
			}
			else if (m_Menu == TMenu::Parameter)
			{
				if (++m_nParameterPage < ParameterPageCount)
					OpenMenu(TMenu::Parameter);
				else
					OpenMenu(TMenu::SoundFonts);
			}
			else
			{
				m_nParameterPage = ParameterPageVolume;
				OpenMenu(TMenu::Parameter);
			}
			break;

		default:
			break;
	}
}

void CMT32Pi::ProcessEncoderEvent(const TEncoderEvent& Event)
{
	if (Event.nEncoder == 0)
	{
		// Turning the first encoder browses the instruments
		if (m_Menu != TMenu::Instruments)
			OpenMenu(TMenu::Instruments);

		const size_t nPresets = m_pSoundFontSynth->GetPresetCount();
		if (nPresets)
			m_nMenuIndex = (m_nMenuIndex + nPresets + Event.nDelta % static_cast<int>(nPresets)) % nPresets;
	}
	else if (m_Menu == TMenu::SoundFonts)
	{
		const size_t nSoundFonts = m_pSoundFontSynth->GetSoundFontManager().GetSoundFontCount();
		if (nSoundFonts)
			m_nMenuIndex = (m_nMenuIndex + nSoundFonts + Event.nDelta % static_cast<int>(nSoundFonts)) % nSoundFonts;
	}
	else
	{
		// Turning the second encoder changes the volume unless another parameter was selected
		if (m_Menu != TMenu::Parameter)
		{
			m_nParameterPage = ParameterPageVolume;
			OpenMenu(TMenu::Parameter);
		}

		// Values have many steps, so turning quickly makes bigger steps; lists always move by one per detent
		const unsigned int nTicks = CTimer::GetClockTicks();
		const unsigned int nInterval = nTicks - m_nLastParameterTurnTime;
		const int nFactor = nInterval < ParameterFastTurnMicros ? 4 : nInterval < ParameterMediumTurnMicros ? 2 : 1;
		m_nLastParameterTurnTime = nTicks;

		ChangeParameter(Event.nDelta * nFactor);
	}

	m_nMenuTime = m_pTimer->GetTicks();
	UpdateMenu();
}

void CMT32Pi::OpenMenu(TMenu Menu)
{
	m_Menu = Menu;
	m_nMenuTime = m_pTimer->GetTicks();

	if (Menu == TMenu::Instruments)
	{
		m_nMenuCurrentIndex = m_pSoundFontSynth->GetChannelPresetIndex(0);
		m_nMenuIndex = m_bDeferredProgramChangeFlag ? m_nDeferredProgramChangeIndex : m_nMenuCurrentIndex;
	}
	else if (Menu == TMenu::SoundFonts)
		m_nMenuIndex = m_nMenuCurrentIndex = m_pSoundFontSynth->GetSoundFontIndex();

	UpdateMenu();
}

void CMT32Pi::CloseMenu()
{
	m_Menu = TMenu::None;
	m_nParameterPage = ParameterPageVolume;
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
		// Name and value with a gauge
		Menu.Type = CUserInterface::TMenu::TType::Parameter;
		Menu.nPage = m_nParameterPage + 1;
		Menu.nPages = ParameterPageCount + 1;

		if (m_nParameterPage == ParameterPageVolume)
		{
			snprintf(Menu.Title, sizeof(Menu.Title), "Volume");
			snprintf(Menu.Value, sizeof(Menu.Value), "%d", m_nMasterVolume);
			Menu.nFraction = m_nMasterVolume / 100.0f;
		}
		else
		{
			const auto Effect = static_cast<CSoundFontSynth::TEffect>(m_nParameterPage - 1);
			const CSoundFontSynth::TEffectInfo& Info = CSoundFontSynth::GetEffectInfo(Effect);
			const float nValue = m_pSoundFontSynth->GetEffect(Effect);

			snprintf(Menu.Title, sizeof(Menu.Title), "%s", Info.pName);
			snprintf(Menu.Value, sizeof(Menu.Value), "%.*f", Info.nDecimals, nValue);
			Menu.nFraction = (nValue - Info.nMin) / (Info.nMax - Info.nMin);
		}

		m_UserInterface.ShowMenu(Menu);
		return;
	}

	// List with the selection in the middle row
	const bool bInstruments = m_Menu == TMenu::Instruments;
	const size_t nCount = bInstruments ? m_pSoundFontSynth->GetPresetCount() : m_pSoundFontSynth->GetSoundFontManager().GetSoundFontCount();
	constexpr int nSelectedRow = CUserInterface::TMenu::SelectedRow;

	Menu.Type = CUserInterface::TMenu::TType::List;
	snprintf(Menu.Title, sizeof(Menu.Title), "%s %d/%d", bInstruments ? "Instrument" : "SoundFont", static_cast<int>(m_nMenuIndex + 1), static_cast<int>(nCount));

	for (int nRow = 0; nRow < static_cast<int>(CUserInterface::TMenu::ListRows); ++nRow)
	{
		const int nIndex = static_cast<int>(m_nMenuIndex) + nRow - nSelectedRow;
		if (nIndex < 0 || nIndex >= static_cast<int>(nCount))
			continue;

		char* pRow = Menu.Rows[nRow];
		if (bInstruments)
			m_pSoundFontSynth->FormatPresetName(nIndex, pRow, sizeof(Menu.Rows[nRow]));
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

void CMT32Pi::ChangeParameter(int nDelta)
{
	if (m_nParameterPage == ParameterPageVolume)
	{
		SetMasterVolume(m_nMasterVolume + nDelta);
		return;
	}

	const auto Effect = static_cast<CSoundFontSynth::TEffect>(m_nParameterPage - 1);
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

	// The pending instrument refers to the old SoundFont
	m_bDeferredProgramChangeFlag = false;
	if (m_pSoundFontSynth->SwitchSoundFont(nIndex))
	{
		m_LastState.SoundFontPath = m_pSoundFontSynth->GetSoundFontManager().GetSoundFontPath(nIndex);
		DeferSaveLastState();

		// Keep the instrument if the new SoundFont has it
		ApplyLastInstrument();

		// Handle any MIDI data that has been queued up while busy
		PurgeMIDIBuffers();

		m_pSoundFontSynth->ReportStatus();
	}
}

bool CMT32Pi::ApplyLastInstrument()
{
	size_t nIndex;
	if (m_LastState.nProgram < 0 || !m_pSoundFontSynth->FindPreset(m_LastState.nBank, m_LastState.nProgram, &nIndex))
		return false;

	LOGNOTE("Selecting instrument %d:%d", m_LastState.nBank, m_LastState.nProgram);
	return m_pSoundFontSynth->SetChannelsPreset(nIndex, 0, ButtonProgramChannels);
}

void CMT32Pi::DeferProgramChange(int nDelta)
{
	const size_t nPresets = m_pSoundFontSynth->GetPresetCount();
	if (!nPresets)
	{
		LCDLog(TLCDLogType::Error, "No instruments!");
		return;
	}

	CloseMenu();

	size_t nIndex = m_bDeferredProgramChangeFlag ? m_nDeferredProgramChangeIndex : m_pSoundFontSynth->GetChannelPresetIndex(0);
	nIndex = (nIndex + nPresets + nDelta) % nPresets;

	char Name[32];
	m_pSoundFontSynth->FormatPresetName(nIndex, Name, sizeof(Name));
	LCDLog(TLCDLogType::Notice, "%s", Name);

	m_nDeferredProgramChangeIndex = nIndex;
	m_nDeferredProgramChangeTime  = CTimer::Get()->GetTicks();
	m_bDeferredProgramChangeFlag  = true;
}

void CMT32Pi::ApplyProgramChange(size_t nIndex)
{
	m_bDeferredProgramChangeFlag = false;

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
			m_UserInterface.ShowStartup(*m_pDisplay, MT32PiFullName, Buffer);
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

void CMT32Pi::LoadProgressHandler(unsigned nMegabytes, void* pContext)
{
	CMT32Pi* const pThis = static_cast<CMT32Pi*>(pContext);

	if (pThis->m_bStartupDone)
		pThis->LCDLog(TLCDLogType::Spinner, "Loading %u MB", nMegabytes);
	else
		pThis->LCDLog(TLCDLogType::Startup, "Loading %u MB", nMegabytes);
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