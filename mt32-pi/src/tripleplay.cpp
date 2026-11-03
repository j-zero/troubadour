//
// tripleplay.cpp
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
#include <circle/timer.h>

#include <cstdio>

#include "tripleplay.h"
#include "utility.h"

LOGMODULE("tripleplay");

namespace
{
	constexpr u8 CommandStatus = 0xB7;	// Control change on MIDI channel 8
	constexpr u8 ParameterIDCC = 31;
	constexpr u8 ParameterValueCC = 63;

	// Parameter IDs
	constexpr u8 InitMode = 0x04;		// 0 simple, 1 hardware, 2 software
	constexpr u8 GuitarState = 0x05;	// The utility sends 0 when entering software mode; the controller answers 3
	constexpr u8 GuitarNavigation = 0x06;	// Button events in software mode
	constexpr u8 BatteryVoltage = 0x07;	// Voltage = (N + 128) / 54.42
	constexpr u8 ProgramChange = 0x28;
	constexpr u8 PitchBendMode = 0x2B;	// 0 Auto, 1 Trigger, 2 Step, 3 Smooth
	constexpr u8 Transpose = 0x2C;		// Semitones + 36
	constexpr u8 StringSensitivity = 0x3C;	// (string index << 4) + level - 1
	constexpr u8 ChannelMode = 0x3F;	// Mono (one channel per string)/poly
	constexpr u8 BendRange = 0x40;
	constexpr u8 PeekGlobal = 0x42;		// Request global n; the reply arrives with the same ID
	constexpr u8 StoreGlobals = 0x43;
	constexpr u8 DelayedPatchSound = 0x52;	// The official utility sends 1 before each button action
	constexpr u8 HandPosition = 0x6E;	// Monitor hand position on/off
	constexpr u8 OpenStringTuning = 0x6F;	// (string index << 4) + semitones from standard tuning + 8
	constexpr u8 EnablePatchBendRange = 0x71;
	constexpr u8 ButtonSinglePress = 0x72;
	constexpr u8 ButtonBothPress = 0x75;
	constexpr u8 ExitSoftwareMode = 0x76;	// The utility sends 0 when it closes

	// Globals
	constexpr u8 GlobalSensitivityFirst = 0;	// 0-5: string sensitivity 0-15, high E first
	constexpr u8 GlobalHandPosition = 9;
	constexpr u8 GlobalOpenStringFirst = 10;	// 10-15: MIDI note of the open string, high E first
	constexpr u8 GlobalButtonSinglePress = 17;
	constexpr u8 GlobalButtonBothPress = 20;

	// Standard tuning, high E first
	const u8 StandardTuning[] = { 64, 59, 55, 50, 45, 40 };
	constexpr int MinOpenStringOffset = -8;
	constexpr int MaxOpenStringOffset = 7;
	const char* const NoteNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

	// GUITAR_NAVIGATION values
	constexpr u8 NavigationUpPressed = 0x12;
	constexpr u8 NavigationDownPressed = 0x13;

	constexpr int TransposeZero = 36;	// The controller's transpose parameter for none
	constexpr u8 BendModeAuto = 0;
	constexpr u8 BendModeTrigger = 1;
	constexpr u8 BendModeStep = 2;
	constexpr u8 BendModeSmooth = 3;

	enum TButton { ButtonUp, ButtonDown, ButtonBoth };

	// The controller echoes a command within a few milliseconds
	constexpr unsigned CommandGapMicros = 2000;
	constexpr unsigned AckTimeoutMicros = 100000;
	constexpr unsigned MaxRetries = 3;
	constexpr unsigned CombineWindowMicros = 60000;
	constexpr unsigned StoreDelayMicros = 2000000;

	const char* const BendModeNames[] = { "Auto", "Trigger", "Step", "Smooth" };
	const char* const SinglePressNames[] = { "Program", "Transpose semi", "Transpose oct", "Bend range/mode", "Channels/bend mode" };
	const char* const BothPressNames[] = { "Channel mode", "Bend Auto/Trigger", "Bend mode cycle", "Bend range", "Hand position",
					       "Bend Auto", "Bend Trigger", "Bend Smooth", "Bend Step" };
}

CTriplePlay::CTriplePlay()
	: m_RxBuffer{},
	  m_nRxIn(0),
	  m_nRxOut(0),

	  m_TxQueue{},
	  m_nTxIn(0),
	  m_nTxOut(0),
	  m_nLastSendTime(0),
	  m_bAwaitingAck(false),
	  m_nAckTime(0),
	  m_nAckRetries(0),
	  m_bShortFormat(true),

	  m_pDevice(nullptr),
	  m_bConnected(false),
	  m_bNoReply(false),
	  m_bSoftwareMode(false),
	  m_bChanged(false),
	  m_bProgramsToHost(false),
	  m_nProgramStep(0),

	  m_nLastID(-1),

	  m_bPeekActive(false),
	  m_nPeekIndex(0),

	  m_nPendingButton(-1),
	  m_nPendingTime(0),

	  m_bStorePending(false),
	  m_nStoreTime(0),

	  m_nOctaves(0),
	  m_nSemitones(0),

	  m_PendingProfile{},
	  m_bProfilePending(false)
{
	for (auto& Channel : m_NoteTranspose)
		for (s8& nTranspose : Channel)
			nTranspose = 0;

	Reset();
}

bool CTriplePlay::TransposeMessage(u8* pMessage, unsigned nLength)
{
	if (nLength != 3)
		return true;

	const u8 nType = pMessage[0] & 0xF0;
	const u8 nChannel = pMessage[0] & 0x0F;
	const u8 nNote = pMessage[1] & 0x7F;
	const bool bNoteOn = nType == 0x90 && pMessage[2];

	// Not the drums on channel 10, and only the messages of a note
	if (nChannel == 9 || (nType != 0x80 && nType != 0x90 && nType != 0xA0))
		return true;

	// A note is released (and its pressure changed) with the transpose it was played with
	s8& nNoteTranspose = m_NoteTranspose[nChannel][nNote];
	if (bNoteOn)
	{
		const int nTranspose = m_nOctaves * 12 + m_nSemitones;
		nNoteTranspose = nNote + nTranspose >= 0 && nNote + nTranspose <= 127 ? nTranspose : NoteDropped;
	}

	if (nNoteTranspose == NoteDropped)
		return false;

	pMessage[1] = nNote + nNoteTranspose;
	return true;
}

int CTriplePlay::GetTranspose() const
{
	// What the controller transposes itself, if anything
	const int nOwn = m_Values[Transpose] >= 0 ? m_Values[Transpose] - TransposeZero : 0;
	return m_nOctaves * 12 + m_nSemitones + nOwn;
}

void CTriplePlay::SetTranspose(int nSemitones)
{
	// In octaves and the semitones beyond them (e.g. -14 is -1 octave, -2 semitones)
	const int nOctaves = Utility::Clamp(nSemitones / 12, -MaxOctaves, MaxOctaves);
	m_nOctaves = nOctaves;
	m_nSemitones = Utility::Clamp(nSemitones - nOctaves * 12, -MaxSemitones, MaxSemitones);
	m_bChanged = true;

	ResetControllerTranspose();
}

void CTriplePlay::ChangeTranspose(int nOctaves, int nSemitones)
{
	m_nOctaves = Utility::Clamp(m_nOctaves + nOctaves, -MaxOctaves, MaxOctaves);
	m_nSemitones = Utility::Clamp(m_nSemitones + nSemitones, -MaxSemitones, MaxSemitones);
	m_bChanged = true;

	ResetControllerTranspose();
}

void CTriplePlay::ResetControllerTranspose()
{
	// So that the notes aren't transposed twice; e.g. after the controller's buttons have transposed it
	if (m_pDevice && m_Values[Transpose] >= 0 && m_Values[Transpose] != TransposeZero)
		Action(Transpose, TransposeZero);
}

void CTriplePlay::ReceiveMessage(const u8* pMessage, unsigned nLength)
{
	// Only parameter messages are of interest
	if (nLength != 3 || pMessage[0] != CommandStatus)
		return;

	const unsigned nIn = m_nRxIn;
	if (nIn - m_nRxOut >= RxBufferSize)
		return;

	u8* pEntry = m_RxBuffer[nIn % RxBufferSize];
	pEntry[0] = pMessage[0];
	pEntry[1] = pMessage[1];
	pEntry[2] = pMessage[2];
	m_nRxIn = nIn + 1;
}

bool CTriplePlay::Update(CUSBMIDIDevice* pDevice)
{
	const unsigned nTicks = CTimer::GetClockTicks();

	// Set up the controller whenever the receiver is plugged in
	if (pDevice != m_pDevice)
	{
		m_pDevice = pDevice;
		Reset();
		m_bChanged = true;

		if (pDevice)
		{
			if (m_bSoftwareMode)
				SendSoftwareMode(true);

			// The transpose is performed here
			if (Send(Transpose, TransposeZero))
				m_Values[Transpose] = TransposeZero;

			// Before reading the globals, so that the sensitivities read back are the ones sent
			if (m_bProfilePending)
			{
				m_bProfilePending = false;
				ApplyProfile(m_PendingProfile);
			}

			ReadGlobals();
		}
	}

	if (!m_pDevice)
	{
		const bool bChanged = m_bChanged;
		m_bChanged = false;
		return bChanged;
	}

	while (m_nRxOut != m_nRxIn)
	{
		const u8* pMessage = m_RxBuffer[m_nRxOut % RxBufferSize];

		// The controller only repeats the ID when it changes, so a value without one belongs to the last ID
		if (pMessage[1] == ParameterIDCC)
			m_nLastID = pMessage[2];
		else if (pMessage[1] == ParameterValueCC && m_nLastID >= 0)
			HandleParameter(m_nLastID, pMessage[2]);

		m_nRxOut = m_nRxOut + 1;
	}

	if (m_nPendingButton >= 0 && nTicks - m_nPendingTime >= CombineWindowMicros)
	{
		const int nButton = m_nPendingButton;
		m_nPendingButton = -1;
		PerformButton(nButton);
	}

	if (m_bStorePending && nTicks - m_nStoreTime >= StoreDelayMicros)
	{
		m_bStorePending = false;
		Send(StoreGlobals, 0);
	}

	// Like the official utility and the FC-1, each command waits for the controller to echo it; one that isn't
	// echoed is sent again (in the long format, if the short one doesn't seem to work), and in the end given up
	if (m_bAwaitingAck && nTicks - m_nAckTime >= AckTimeoutMicros)
	{
		if (m_bShortFormat)
		{
			LOGNOTE("No answer to the short command format, using the long one");
			m_bShortFormat = false;
			m_nAckRetries = 0;
		}
		else if (++m_nAckRetries >= MaxRetries)
			GiveUpCommand();

		if (m_bAwaitingAck)
		{
			SendCommand(m_TxQueue[m_nTxOut % TxQueueSize]);
			m_nAckTime = nTicks;
		}
	}

	if (!m_bAwaitingAck && m_nTxOut != m_nTxIn && nTicks - m_nLastSendTime >= CommandGapMicros)
	{
		SendCommand(m_TxQueue[m_nTxOut % TxQueueSize]);
		m_bAwaitingAck = true;
		m_nAckTime = m_nLastSendTime = nTicks;
		m_nAckRetries = 0;
	}

	const bool bChanged = m_bChanged;
	m_bChanged = false;
	return bChanged;
}

void CTriplePlay::SetSoftwareMode(bool bSoftwareMode)
{
	if (bSoftwareMode == m_bSoftwareMode)
		return;

	m_bSoftwareMode = bSoftwareMode;
	m_nPendingButton = -1;
	m_bChanged = true;

	// Otherwise sent once the receiver is connected
	if (m_pDevice)
		SendSoftwareMode(bSoftwareMode);
}

void CTriplePlay::SendCommand(const TCommand& Command)
{
	LOGDBG("Sending 0x%02X = %d", Command.nID, Command.nValue);

	// Short format, as the FC-1 sends some commands: the parameter ID as the controller number, one message on the
	// first port. Not for the IDs of the long format's controllers.
	if (m_bShortFormat && Command.nID != ParameterIDCC && Command.nID != ParameterValueCC)
	{
		const u8 Message[] = { CommandStatus, Command.nID, Command.nValue };
		m_pDevice->SendPlainMIDI(0, Message, sizeof(Message));
		return;
	}

	const u8 ID[] = { CommandStatus, ParameterIDCC, Command.nID };
	const u8 Value[] = { CommandStatus, ParameterValueCC, Command.nValue };
	m_pDevice->SendPlainMIDI(CommandCable, ID, sizeof(ID));
	m_pDevice->SendPlainMIDI(CommandCable, Value, sizeof(Value));
}

void CTriplePlay::GiveUpCommand()
{
	const TCommand& Command = m_TxQueue[m_nTxOut % TxQueueSize];
	LOGDBG("No answer to 0x%02X", Command.nID);
	m_bAwaitingAck = false;
	++m_nTxOut;

	if (!m_bConnected)
	{
		// Nothing has come back: give the buttons back to the controller rather than leaving them dead
		if (!m_bNoReply)
		{
			LOGWARN("TriplePlay doesn't answer");
			m_bNoReply = true;
			m_bPeekActive = false;
			m_bChanged = true;
			if (m_bSoftwareMode)
				SendSoftwareMode(false);
		}
	}
	else if (Command.nID == PeekGlobal && m_bPeekActive)
	{
		// A global that wasn't answered stays unknown
		++m_nPeekIndex;
		SendNextPeek();
	}
}

// As the official utility does (from a USB capture)
void CTriplePlay::SendSoftwareMode(bool bOn)
{
	if (bOn)
	{
		Send(InitMode, 2);
		Send(GuitarState, 0);
		Send(EnablePatchBendRange, 1);
	}
	else
		Send(ExitSoftwareMode, 0);
}

int CTriplePlay::GetSetting(TSetting Setting) const
{
	switch (Setting)
	{
		case TSetting::Octave:			return m_nOctaves;
		case TSetting::Semitone:		return m_nSemitones;

		case TSetting::BendMode:		return m_Values[PitchBendMode];
		case TSetting::BendRange:		return m_Values[BendRange];
		case TSetting::ChannelMode:		return m_Values[ChannelMode];
		case TSetting::ButtonAction:		return m_Globals[GlobalButtonSinglePress];
		case TSetting::BothButtonsAction:	return m_Globals[GlobalButtonBothPress];
		case TSetting::HandPosition:		return m_Globals[GlobalHandPosition];
		default:				return Unknown;
	}
}

void CTriplePlay::ChangeSetting(TSetting Setting, int nDelta)
{
	const int nValue = GetSetting(Setting);

	switch (Setting)
	{
		case TSetting::Octave:
			ChangeTranspose(nDelta, 0);
			break;

		case TSetting::Semitone:
			ChangeTranspose(0, nDelta);
			break;

		case TSetting::BendMode:
			Action(PitchBendMode, Utility::Clamp((nValue >= 0 ? nValue : BendModeAuto) + nDelta, 0, static_cast<int>(BendModeSmooth)));
			break;

		case TSetting::BendRange:
			Action(BendRange, Utility::Clamp((nValue >= 0 ? nValue : 2) + nDelta, 1, 24));
			break;

		case TSetting::ChannelMode:
			Action(ChannelMode, nDelta > 0 ? 1 : 0);
			break;

		// Globals: stored in the controller, so that they also apply without Troubadour
		case TSetting::ButtonAction:
		case TSetting::BothButtonsAction:
		{
			const bool bSingle = Setting == TSetting::ButtonAction;
			const int nMax = bSingle ? Utility::ArraySize(SinglePressNames) - 1 : Utility::ArraySize(BothPressNames) - 1;
			const int nNewValue = Utility::Clamp((nValue >= 0 ? nValue : 2) + nDelta, 0, nMax);
			if (Send(bSingle ? ButtonSinglePress : ButtonBothPress, nNewValue))
			{
				m_Globals[bSingle ? GlobalButtonSinglePress : GlobalButtonBothPress] = nNewValue;
				StoreGlobalsLater();
				m_bChanged = true;
			}
			break;
		}

		case TSetting::HandPosition:
			if (Send(HandPosition, nDelta > 0 ? 1 : 0))
			{
				m_Globals[GlobalHandPosition] = nDelta > 0 ? 1 : 0;
				StoreGlobalsLater();
				m_bChanged = true;
			}
			break;

		default:
			break;
	}
}

void CTriplePlay::FormatSetting(TSetting Setting, char* pBuffer, size_t nSize) const
{
	const int nValue = GetSetting(Setting);
	const char* pText = nullptr;

	// The transpose is always known, and may be -1
	const bool bTranspose = Setting == TSetting::Octave || Setting == TSetting::Semitone;

	if (nValue == Unknown && !bTranspose)
		pText = "-";
	else if (Setting == TSetting::BendMode)
		pText = nValue <= BendModeSmooth ? BendModeNames[nValue] : "?";
	else if (Setting == TSetting::ChannelMode)
		pText = nValue == 0 ? "Multi" : "Single";	// Unverified: 0 = mono, one channel per string
	else if (Setting == TSetting::HandPosition)
		pText = nValue ? "On" : "Off";
	else if (Setting == TSetting::ButtonAction)
		pText = nValue < static_cast<int>(Utility::ArraySize(SinglePressNames)) ? SinglePressNames[nValue] : "?";
	else if (Setting == TSetting::BothButtonsAction)
		pText = nValue < static_cast<int>(Utility::ArraySize(BothPressNames)) ? BothPressNames[nValue] : "?";

	if (pText)
		snprintf(pBuffer, nSize, "%s", pText);
	else if (Setting == TSetting::BendRange)
		snprintf(pBuffer, nSize, "+/-%d", nValue);
	else if (nValue)
		snprintf(pBuffer, nSize, "%+d", nValue);
	else
		snprintf(pBuffer, nSize, "0");
}

int CTriplePlay::GetStringSensitivity(u8 nString) const
{
	if (nString < 1 || nString > Strings)
		return 0;

	const int nValue = m_Globals[GlobalSensitivityFirst + nString - 1];
	return nValue >= 0 ? nValue + 1 : 0;
}

void CTriplePlay::SetStringSensitivity(u8 nString, int nLevel)
{
	if (nString < 1 || nString > Strings)
		return;

	nLevel = Utility::Clamp(nLevel, MinSensitivity, MaxSensitivity);
	if (!Send(StringSensitivity, ((nString - 1) << 4) + nLevel - 1))
		return;

	m_Globals[GlobalSensitivityFirst + nString - 1] = nLevel - 1;
	StoreGlobalsLater();
}

void CTriplePlay::FormatOpenString(u8 nString, char* pBuffer, size_t nSize) const
{
	const int nNote = nString >= 1 && nString <= Strings ? m_Globals[GlobalOpenStringFirst + nString - 1] : Unknown;
	if (nNote < 0)
	{
		snprintf(pBuffer, nSize, "-");
		return;
	}

	// With the difference from standard tuning, e.g. "D (-2)"
	const int nOffset = nNote - StandardTuning[nString - 1];
	if (nOffset)
		snprintf(pBuffer, nSize, "%s (%+d)", NoteNames[nNote % 12], nOffset);
	else
		snprintf(pBuffer, nSize, "%s", NoteNames[nNote % 12]);
}

void CTriplePlay::ChangeOpenString(u8 nString, int nDelta)
{
	if (nString < 1 || nString > Strings)
		return;

	// Not before the current tuning has been read, so that it can't be changed blindly
	const int nNote = m_Globals[GlobalOpenStringFirst + nString - 1];
	if (nNote < 0)
		return;

	const int nOffset = Utility::Clamp(nNote - StandardTuning[nString - 1] + nDelta, MinOpenStringOffset, MaxOpenStringOffset);
	if (Send(OpenStringTuning, ((nString - 1) << 4) + nOffset - MinOpenStringOffset))
	{
		m_Globals[GlobalOpenStringFirst + nString - 1] = StandardTuning[nString - 1] + nOffset;
		StoreGlobalsLater();
		m_bChanged = true;
	}
}

void CTriplePlay::StoreGlobalsLater()
{
	// Changes are lost when the controller is switched off unless they are stored; once they have settled, so that
	// turning the encoder doesn't store each step
	m_bStorePending = true;
	m_nStoreTime = CTimer::GetClockTicks();
}

void CTriplePlay::GetProfile(TProfile& Profile) const
{
	Profile.nTranspose = m_nOctaves * 12 + m_nSemitones;
	Profile.nBendMode = m_Values[PitchBendMode];
	Profile.nBendRange = m_Values[BendRange];
	Profile.nChannelMode = m_Values[ChannelMode];

	for (u8 nString = 1; nString <= Strings; ++nString)
	{
		const int nLevel = GetStringSensitivity(nString);
		Profile.Sensitivity[nString - 1] = nLevel ? nLevel : Unknown;
	}
}

void CTriplePlay::SetProfile(const TProfile& Profile)
{
	// Performed here, so it applies at once
	if (Profile.nTranspose != Unknown)
		SetTranspose(Profile.nTranspose);

	if (m_pDevice)
		ApplyProfile(Profile);
	else
	{
		m_PendingProfile = Profile;
		m_bProfilePending = true;
	}
}

void CTriplePlay::ApplyProfile(const TProfile& Profile)
{
	// Patch settings, as the official utility changes them
	const struct
	{
		u8 nID;
		int nValue;
	} PatchValues[] =
	{
		{ PitchBendMode, Profile.nBendMode },
		{ BendRange, Profile.nBendRange },
		{ ChannelMode, Profile.nChannelMode },
	};

	// Only what changes: each change of the patch (with DEL_PTCSND before it) may interrupt the guitar's notes
	bool bPatchSoundDelayed = false;
	for (const auto& Value : PatchValues)
	{
		if (Value.nValue < 0 || Value.nValue > 0x7F || m_Values[Value.nID] == Value.nValue)
			continue;

		if (!bPatchSoundDelayed)
			bPatchSoundDelayed = Send(DelayedPatchSound, 1);
		if (Send(Value.nID, Value.nValue))
			m_Values[Value.nID] = Value.nValue;
	}

	// The sensitivities are globals; they aren't stored permanently here, as they are sent again with the instrument
	for (u8 nString = 1; nString <= Strings; ++nString)
	{
		const int nLevel = Profile.Sensitivity[nString - 1];
		if (nLevel >= MinSensitivity && nLevel <= MaxSensitivity && m_Globals[GlobalSensitivityFirst + nString - 1] != nLevel - 1
		    && Send(StringSensitivity, ((nString - 1) << 4) + nLevel - 1))
			m_Globals[GlobalSensitivityFirst + nString - 1] = nLevel - 1;
	}

	m_bChanged = true;
}

bool CTriplePlay::HasBattery() const
{
	return m_Values[BatteryVoltage] >= 0;
}

void CTriplePlay::FormatBattery(char* pBuffer, size_t nSize) const
{
	const int nValue = m_Values[BatteryVoltage];
	if (nValue >= 0)
		snprintf(pBuffer, nSize, "%.2f V", (nValue + 128) / 54.42f);
	else
		snprintf(pBuffer, nSize, "-");
}

void CTriplePlay::Reset()
{
	for (s16& nValue : m_Values)
		nValue = -1;
	for (s16& nValue : m_Globals)
		nValue = -1;

	m_nRxOut = m_nRxIn;
	m_nTxIn = m_nTxOut = 0;
	m_bAwaitingAck = false;
	m_bShortFormat = true;
	m_bConnected = false;
	m_bNoReply = false;
	m_nLastID = -1;
	m_bPeekActive = false;
	m_nPendingButton = -1;
	m_bStorePending = false;
}

void CTriplePlay::HandleParameter(u8 nID, u8 nValue)
{
	if (!m_bConnected)
		LOGNOTE("TriplePlay is answering");

	if (nID == GuitarNavigation)
		LOGNOTE("Button event 0x%02X", nValue);
	else
		LOGDBG("Parameter 0x%02X = %d", nID, nValue);

	// Answering after all: back into software mode
	if (m_bNoReply)
	{
		m_bNoReply = false;
		if (m_bSoftwareMode)
			SendSoftwareMode(true);
	}

	m_bConnected = true;
	m_bChanged = true;

	// The echo of the command that was sent last (peeks are answered with their ID, too)
	if (m_bAwaitingAck && nID == m_TxQueue[m_nTxOut % TxQueueSize].nID)
	{
		m_bAwaitingAck = false;
		++m_nTxOut;
	}

	if (nID < Utility::ArraySize(m_Values))
		m_Values[nID] = nValue;

	if (nID == ButtonSinglePress)
		m_Globals[GlobalButtonSinglePress] = nValue;
	else if (nID == ButtonBothPress)
		m_Globals[GlobalButtonBothPress] = nValue;
	else if (nID == PeekGlobal && m_bPeekActive)
	{
		SetGlobal(m_nPeekIndex++, nValue);
		SendNextPeek();
	}
	else if (nID == GuitarNavigation && m_bSoftwareMode)
		HandleNavigation(nValue);
}

void CTriplePlay::HandleNavigation(u8 nValue)
{
	int nButton;
	if (nValue == NavigationUpPressed)
		nButton = ButtonUp;
	else if (nValue == NavigationDownPressed)
		nButton = ButtonDown;
	else
		return;

	// Wait briefly for the other button of a press of both
	if (m_nPendingButton < 0)
	{
		m_nPendingButton = nButton;
		m_nPendingTime = CTimer::GetClockTicks();
	}
	else if (m_nPendingButton != nButton)
	{
		m_nPendingButton = -1;
		PerformButton(ButtonBoth);
	}
}

void CTriplePlay::PerformButton(int nButton)
{
	if (nButton == ButtonBoth)
		PerformBoth();
	else
		PerformSingle(nButton == ButtonUp);
}

// The actions assigned in the official utility ("Single Press")
void CTriplePlay::PerformSingle(bool bUp)
{
	switch (m_Globals[GlobalButtonSinglePress] >= 0 ? m_Globals[GlobalButtonSinglePress] : 2)
	{
		case 0:
			if (m_bProgramsToHost)
			{
				m_nProgramStep += bUp ? 1 : -1;
				m_bChanged = true;
			}
			else
				Action(ProgramChange, Utility::Clamp(GetValue(ProgramChange, 0) + (bUp ? 1 : -1), 0, 127));
			break;

		case 1:
			ChangeTranspose(0, bUp ? 1 : -1);
			break;

		case 2:
			ChangeTranspose(bUp ? 1 : -1, 0);
			break;

		case 3:
			if (bUp)
				Action(BendRange, GetValue(BendRange, 2) == 2 ? 12 : 2);
			else
				Action(PitchBendMode, GetValue(PitchBendMode, BendModeAuto) == BendModeAuto ? BendModeTrigger : BendModeAuto);
			break;

		case 4:
			if (bUp)
				Action(ChannelMode, GetValue(ChannelMode, 0) ? 0 : 1);
			else
				Action(PitchBendMode, GetValue(PitchBendMode, BendModeAuto) == BendModeAuto ? BendModeTrigger : BendModeAuto);
			break;

		default:
			break;
	}
}

// The actions assigned in the official utility ("Simultaneous Press")
void CTriplePlay::PerformBoth()
{
	const int nBendMode = GetValue(PitchBendMode, BendModeAuto);

	switch (m_Globals[GlobalButtonBothPress] >= 0 ? m_Globals[GlobalButtonBothPress] : 2)
	{
		case 0: Action(ChannelMode, GetValue(ChannelMode, 0) ? 0 : 1); break;
		case 1: Action(PitchBendMode, nBendMode == BendModeAuto ? BendModeTrigger : BendModeAuto); break;
		case 2: Action(PitchBendMode, (nBendMode + 1) % 4); break;
		case 3: Action(BendRange, GetValue(BendRange, 2) == 2 ? 12 : 2); break;
		case 4: Action(HandPosition, GetValue(HandPosition, 0) ? 0 : 1); break;
		case 5: Action(PitchBendMode, BendModeAuto); break;
		case 6: Action(PitchBendMode, BendModeTrigger); break;
		case 7: Action(PitchBendMode, BendModeSmooth); break;
		case 8: Action(PitchBendMode, BendModeStep); break;
		default: break;
	}
}

void CTriplePlay::Action(u8 nID, int nValue)
{
	Send(DelayedPatchSound, 1);
	if (Send(nID, nValue))
	{
		// The controller echoes the value, but the next press may come first
		m_Values[nID] = nValue;
		m_bChanged = true;
	}
}

int CTriplePlay::GetValue(u8 nID, int nDefault) const
{
	return m_Values[nID] >= 0 ? m_Values[nID] : nDefault;
}

bool CTriplePlay::Send(u8 nID, u8 nValue)
{
	if (!m_pDevice || nID > 0x7F || nValue > 0x7F || m_nTxIn - m_nTxOut >= TxQueueSize)
		return false;

	m_TxQueue[m_nTxIn % TxQueueSize] = { nID, nValue };
	++m_nTxIn;
	return true;
}

void CTriplePlay::ReadGlobals()
{
	m_nPeekIndex = 0;
	m_bPeekActive = true;
	SendNextPeek();
}

void CTriplePlay::SendNextPeek()
{
	if (m_nPeekIndex >= GlobalCount || !Send(PeekGlobal, m_nPeekIndex))
		m_bPeekActive = false;
}

void CTriplePlay::SetGlobal(u8 nIndex, int nValue)
{
	if (nIndex < GlobalCount)
		m_Globals[nIndex] = nValue;
}
