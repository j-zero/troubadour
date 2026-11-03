//
// mt32pi.h
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

#ifndef _mt32pi_h
#define _mt32pi_h

#include <circle/actled.h>
#include <circle/bcm54213.h>
#include <circle/bcmrandom.h>
#include <circle/cputhrottle.h>
#include <circle/devicenameservice.h>
#include <circle/gpiomanager.h>
#include <circle/i2cmaster.h>
#include <circle/interrupt.h>
#include <circle/logger.h>
#include <circle/multicore.h>
#include <circle/net/netsubsystem.h>
#include <circle/sched/scheduler.h>
#include <circle/sound/soundbasedevice.h>
#include <circle/spimaster.h>
#include <circle/timer.h>
#include <circle/types.h>
#include <circle/usb/usbhcidevice.h>
#include <circle/usb/usbmassdevice.h>
#include <circle/usb/usbmidi.h>
#include <circle/usb/usbserial.h>
#include <fatfs/ff.h>
#include <wlan/bcm4343.h>
#include <wlan/hostap/wpa_supplicant/wpasupplicant.h>

#include "config.h"
#include "control/control.h"
#include "control/mister.h"
#include "event.h"
#include "laststate.h"
#include "lcd/drivers/gc9a01.h"
#include "lcd/ui.h"
#include "midiparser.h"
#include "net/applemidi.h"
#include "net/ftpdaemon.h"
#include "net/udpmidi.h"
#include "pisound.h"
#include "power.h"
#include "ringbuffer.h"
#include "synth/soundfontsynth.h"
#include "tripleplay.h"
#include "presets.h"

//#define MONITOR_TEMPERATURE

class CMT32Pi : CMultiCoreSupport, CPower, CMIDIParser, CAppleMIDIHandler, CUDPMIDIHandler
{
public:
	CMT32Pi(CI2CMaster* pI2CMaster, CSPIMaster* pSPIMaster, CInterruptSystem* pInterrupt, CGPIOManager* pGPIOManager, CSerialDevice* pSerialDevice, CUSBHCIDevice* pUSBHCI);
	virtual ~CMT32Pi() override;

	bool Initialize(bool bSerialMIDIAvailable = true);

	virtual void Run(unsigned nCore) override;

private:
	enum class TLCDLogType
	{
		Startup,
		Error,
		Warning,
		Notice,
		Spinner,
	};

	static constexpr size_t MIDIRxBufferSize = 2048;

	// The instrument list sets the instrument for these channels
	// (one channel per string for MIDI guitars such as the Fishman TriplePlay)
	static constexpr u8 ButtonProgramChannels = 6;

	// Settings are saved once they haven't changed for this long, so that turning the encoder doesn't write each step
	static constexpr unsigned LastStateSaveDelayMillis = 2000;

	// Menus of the simple_encoder scheme: the first encoder selects the instrument from a list. Pressing the second
	// one opens the settings, with submenus; either encoder selects an item and pressing the second one opens it. The
	// second encoder then changes its value (or selects the drum kit/SoundFont from a list), and pressing it returns
	// to the list.
	enum class TMenu
	{
		None,
		Instruments,
		Settings,
		Parameter,
		DrumKits,
		SoundFonts,
		Presets,
		PresetInstruments,	// The instrument of the current preset, chosen in the settings
	};

	static constexpr unsigned MenuTimeoutMillis = 8000;

	// Detents of the second encoder that follow each other this quickly change values in bigger steps
	static constexpr unsigned ParameterFastTurnMicros = 25000;
	static constexpr unsigned ParameterMediumTurnMicros = 60000;

	// Items of the settings menus
	static constexpr u8 SettingVolume    = 0;
	static constexpr u8 SettingEffects   = 1;
	static constexpr u8 SettingChannels  = SettingEffects + static_cast<u8>(CSoundFontSynth::TEffect::Count);
	static constexpr u8 SettingDrumKit   = SettingChannels + 1;
	static constexpr u8 SettingSoundFont = SettingDrumKit + 1;
	// TriplePlay: switches for the support and the software mode, its patch settings, the button assignments, the
	// battery and the string sensitivities
	static constexpr u8 SettingTPEnabled      = SettingSoundFont + 1;
	static constexpr u8 SettingTPSoftwareMode = SettingTPEnabled + 1;
	static constexpr u8 SettingTPFirst        = SettingTPSoftwareMode + 1;	// CTriplePlay::TSetting, in its order
	static constexpr u8 SettingTPBattery      = SettingTPFirst + static_cast<u8>(CTriplePlay::TSetting::Count);
	static constexpr u8 SettingTPSensitivity  = SettingTPBattery + 1;
	static constexpr u8 SettingTPOpenString   = SettingTPSensitivity + CTriplePlay::Strings;
	// Submenus, and the items that leave them
	static constexpr u8 SettingPresetMenu       = SettingTPOpenString + CTriplePlay::Strings;
	static constexpr u8 SettingPresetTPMenu     = SettingPresetMenu + 1;
	static constexpr u8 SettingGlobalMenu       = SettingPresetTPMenu + 1;
	static constexpr u8 SettingGlobalTPMenu     = SettingGlobalMenu + 1;
	static constexpr u8 SettingEffectsMenu      = SettingGlobalTPMenu + 1;
	static constexpr u8 SettingBack             = SettingEffectsMenu + 1;
	static constexpr u8 SettingExit             = SettingBack + 1;
	// Presets: the list, and adding and deleting
	static constexpr u8 SettingPresetSelect     = SettingExit + 1;
	static constexpr u8 SettingPresetNew        = SettingPresetSelect + 1;
	static constexpr u8 SettingPresetDelete     = SettingPresetNew + 1;
	static constexpr u8 SettingDisplayStandby   = SettingPresetDelete + 1;
	static constexpr u8 SettingPresetInstrument = SettingDisplayStandby + 1;
	static constexpr u8 SettingTPProgramPresets = SettingPresetInstrument + 1;

	// Settings > Preset holds what is stored in the current preset, Settings > Global everything else
	enum class TSettingsPage
	{
		Root,
		Preset,
		PresetTP,
		Effects,
		Global,
		GlobalTP,
	};

	static constexpr size_t MaxSettingsPageItems = 24;
	static constexpr u8 MaxDisplayChannels = 16;

	// Display standby times that can be chosen in the settings (seconds; 0 for never)
	static constexpr unsigned StandbySteps[] = { 0, 15, 30, 60, 120, 300, 600, 900, 1800, 3600 };
	// The last of them that isn't longer than nSeconds (which may be any value from the config file)
	static size_t GetStandbyStep(unsigned nSeconds)
	{
		size_t nStep = 0;
		while (nStep + 1 < sizeof(StandbySteps) / sizeof(*StandbySteps) && StandbySteps[nStep + 1] <= nSeconds)
			++nStep;
		return nStep;
	}

	// CPower
	virtual void OnEnterPowerSavingMode() override;
	virtual void OnExitPowerSavingMode() override;
	virtual void OnThrottleDetected() override;
	virtual void OnUnderVoltageDetected() override;

	// CMIDIParser
	virtual void OnShortMessage(u32 nMessage) override;
	virtual void OnSysExMessage(const u8* pData, size_t nSize) override;
	virtual void OnUnexpectedStatus() override;
	virtual void OnSysExOverflow() override;

	// CAppleMIDIHandler
	virtual void OnAppleMIDIDataReceived(const u8* pData, size_t nSize) override { ParseMIDIBytes(pData, nSize); };
	virtual void OnAppleMIDIConnect(const CIPAddress* pIPAddress, const char* pName) override;
	virtual void OnAppleMIDIDisconnect(const CIPAddress* pIPAddress, const char* pName) override;

	// CUDPMIDIHandler
	virtual void OnUDPMIDIDataReceived(const u8* pData, size_t nSize) override { ParseMIDIBytes(pData, nSize); };

	// Initialization
	bool InitNetwork();
	bool InitSoundFontSynth();
	void RestoreLastState();

	// Tasks for specific CPU cores
	void MainTask();
	void UITask();
	void AudioTask();

	void UpdateUSB(bool bStartup = false);
	void UpdateNetwork();
	void UpdateMIDI();
	void PurgeMIDIBuffers();
	size_t ReceiveSerialMIDI(u8* pOutData, size_t nSize);
	bool ParseCustomSysEx(const u8* pData, size_t nSize);

	void ProcessEventQueue();
	void ProcessButtonEvent(const TButtonEvent& Event);
	void ProcessFootswitchEvent(const TButtonEvent& Event);
	void ProcessEncoder2ButtonEvent(const TButtonEvent& Event);
	void ProcessEncoderEvent(const TEncoderEvent& Event);

	// Menus
	void OpenMenu(TMenu Menu);
	// Keeping the position, the settings open at the same submenu and item the next time
	void CloseMenu(bool bKeepSettingsPosition = false);
	void UpdateMenu();
	// The items of the current settings page; returns their number
	size_t GetSettingsItems(u8* pItems) const;
	int FindSettingsItem(u8 nSetting) const;
	void OpenSettingsPage(TSettingsPage Page, u8 nSelectedSetting);
	bool IsSettingsMenu() const
	{
		return m_Menu == TMenu::Settings || m_Menu == TMenu::Parameter || m_Menu == TMenu::DrumKits || m_Menu == TMenu::SoundFonts
		    || m_Menu == TMenu::Presets || m_Menu == TMenu::PresetInstruments;
	}
	void OpenSetting(u8 nSetting);
	void UpdateTriplePlayStatus();
	void SetTriplePlayEnabled(bool bEnabled);
	void SetTriplePlaySoftwareMode(bool bSoftwareMode);
	void SetTriplePlayProgramPresets(bool bOn);
	void UpdateTriplePlayPrograms();
	void UpdatePinnedInstruments();
	bool GetCurrentInstrument(const char** ppSoundFontPath, int* pBank, int* pProgram);
	// Presets
	void ApplyPreset(size_t nIndex);
	bool GetCurrentPresetState(CPresets::TPreset& Preset);
	void CapturePreset();
	void NewPreset();
	void DeletePreset();
	void UpdatePresetLabel();
	const char* GetSettingName(u8 nSetting) const;
	const char* GetSettingsPageTitle() const;
	// Returns false for items without a value (submenus, "Back", "Exit")
	bool FormatSettingValue(u8 nSetting, char* pBuffer, size_t nSize, float* pFraction = nullptr);
	size_t GetMenuListCount() const;
	void MoveListSelection(int nDelta);
	void MoveInstrumentSelection(int nDelta);
	void SelectMenuInstrument();
	void ChangeParameter(int nDelta);
	// Actions that can be triggered via events
	void SwitchSoundFont(size_t nIndex);
	void DeferSwitchSoundFont(size_t nIndex);
	bool ApplyProgramChange(size_t nIndex);
	bool ApplyDrumKit(size_t nIndex);
	void ApplyLastInstrument();
	void SetMasterVolume(s32 nVolume);
	void DeferSaveLastState();

	const char* GetNetworkDeviceShortName() const;
	void LEDOn();
	void LCDLog(TLCDLogType Type, const char* pFormat...);

	CConfig* volatile m_pConfig;

	CTimer* m_pTimer;
	CActLED* m_pActLED;

	CI2CMaster* m_pI2CMaster;
	CSPIMaster* m_pSPIMaster;
	CInterruptSystem* m_pInterrupt;
	CGPIOManager* m_pGPIOManager;
	CSerialDevice* m_pSerial;
	CUSBHCIDevice* m_pUSBHCI;
	FATFS m_USBFileSystem;
	bool m_bUSBAvailable;

	// Networking
	CNetSubSystem* m_pNet;
	CNetDevice* m_pNetDevice;
	CBcm4343Device m_WLAN;
	CWPASupplicant m_WPASupplicant;
	bool m_bNetworkReady;
	CAppleMIDIParticipant* m_pAppleMIDIParticipant;
	CUDPMIDIReceiver* m_pUDPMIDIReceiver;
	CFTPDaemon* m_pFTPDaemon;

	CBcmRandomNumberGenerator m_Random;

	CGC9A01* m_pDisplay;
	unsigned m_nLCDUpdateTime;
	CUserInterface m_UserInterface;
#ifdef MONITOR_TEMPERATURE
	unsigned m_nTempUpdateTime;
#endif

	CControl* m_pControl;

	// MiSTer control interface
	CMisterControl m_MisterControl;
	unsigned m_nMisterUpdateTime;

	// Deferred SoundFont switch
	bool m_bDeferredSoundFontSwitchFlag;
	size_t m_nDeferredSoundFontSwitchIndex;
	unsigned m_nDeferredSoundFontSwitchTime;

	// The second encoder's button acts when released; held, it leaves the settings
	bool m_bEncoder2ButtonHeld;
	bool m_bEncoder2LongPress;

	// Current menu and its selection
	TMenu m_Menu;
	TSettingsPage m_SettingsPage;
	u8 m_nSetting;		// The settings item that is open (or highlighted when going back to the list)
	size_t m_nMenuIndex;
	size_t m_nMenuCurrentIndex;
	unsigned m_nMenuTime;
	unsigned m_nLastParameterTurnTime;

	// Settings that are restored on startup
	CLastState m_LastState;
	CLastStateSaver* m_pLastStateSaver;
	bool m_bLastStateChanged;
	unsigned m_nLastStateChangeTime;

	// Serial GPIO MIDI
	bool m_bSerialMIDIAvailable;
	bool m_bSerialMIDIEnabled;

	// USB devices
	CUSBMIDIDevice* m_pUSBMIDIDevice;
	CUSBSerialDevice* m_pUSBSerialDevice;
	CUSBBulkOnlyMassStorageDevice* volatile m_pUSBMassStorageDevice;

	bool m_bActiveSenseFlag;
	unsigned m_nActiveSenseTime;

	volatile bool m_bRunning;
	volatile bool m_bUITaskDone;
	bool m_bLEDOn;
	unsigned m_nLEDOnTime;

	// Audio output
	CSoundBaseDevice* m_pSound;

	// Extra devices
	CPisound* m_pPisound;

	// Fishman TriplePlay on the USB MIDI device; switched on and off in the config file or the settings
	CTriplePlay* m_pTriplePlay;
	volatile bool m_bTriplePlayEnabled;
	bool m_bTriplePlaySoftwareMode;
	// Its program changes switch presets rather than the synth's instrument; the program received last (set in
	// interrupt context), -1 for none
	volatile bool m_bTriplePlayProgramPresets;
	volatile int m_nTriplePlayProgram;

	// Presets that the footswitches switch between; all changes to the sound go into the current one
	CPresets m_Presets;
	CPresetSaver* m_pPresetSaver;
	int m_nCurrentPreset;	// -1 for none
	bool m_bApplyingPreset;	// Its own changes aren't saved back into it
	bool m_bPresetChanged;
	unsigned m_nPresetChangeTime;

	// Synthesizer
	u8 m_nMasterVolume;
	CSoundFontSynth* m_pSoundFontSynth;

	// MIDI receive buffer
	CRingBuffer<u8, MIDIRxBufferSize> m_MIDIRxBuffer;

	// Event handling
	TEventQueue m_EventQueue;

	static void EventHandler(const TEvent& Event);
	static void USBMIDIDeviceRemovedHandler(CDevice* pDevice, void* pContext);
	static void USBMIDIPacketHandler(unsigned nCable, u8* pPacket, unsigned nLength);
	static void IRQMIDIReceiveHandler(const u8* pData, size_t nSize);

	static void PanicHandler();
	static void LoadProgressHandler(u64 nLoadedBytes, u64 nTotalBytes, void* pContext);

	// Set once the UI task owns the LCD; startup messages are printed directly before
	volatile bool m_bStartupDone;

	static CMT32Pi* s_pThis;
};

#endif
