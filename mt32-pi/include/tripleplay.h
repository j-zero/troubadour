//
// tripleplay.h
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

#ifndef _tripleplay_h
#define _tripleplay_h

#include <circle/types.h>
#include <circle/usb/usbmidi.h>

// Fishman TriplePlay Express. Its receiver is a USB MIDI device; the second port (cable 1) takes commands as two
// control changes on MIDI channel 8: CC#31 = parameter ID, then CC#63 = value. The controller reports changed
// parameters the same way, and global settings on request (PEEK). In software mode, the controller only reports its
// Up/Down buttons, and the host performs the actions assigned to them.
//
// The protocol is taken from the TPExpress library, which recovered it from the official TriplePlay utility.
class CTriplePlay
{
public:
	static constexpr u8 CommandCable = 1;
	static constexpr u8 Strings = 6;
	static constexpr int MinSensitivity = 1;
	static constexpr int MaxSensitivity = 16;
	static constexpr int Unknown = -1;
	static constexpr int MaxOctaves = 3;		// Transpose: up to 3 octaves and 11 semitones either way
	static constexpr int MaxSemitones = 11;

	// Settings that are set at once (e.g. for a preset); Unknown for values that are left as they are
	struct TProfile
	{
		int nTranspose;		// Semitones; performed by Troubadour (see TransposeMessage())
		int nBendMode;
		int nBendRange;
		int nChannelMode;
		int Sensitivity[Strings];	// 1-16, high E first
	};

	CTriplePlay();

	// Called from interrupt context with a MIDI message from the command port
	void ReceiveMessage(const u8* pMessage, unsigned nLength);

	// Called regularly by the main task; pDevice is null while no USB MIDI device is connected (or the TriplePlay
	// support is off). Returns true when something that is shown has changed.
	bool Update(CUSBMIDIDevice* pDevice);

	// Switches the controller into or out of software mode (then it performs its buttons itself again)
	void SetSoftwareMode(bool bSoftwareMode);
	// In software mode, the buttons' program up/down action is left to Troubadour (e.g. to switch presets) instead
	// of changing the controller's program; TakeProgramStep() returns the steps since it was called last
	void SetProgramsToHost(bool bToHost) { m_bProgramsToHost = bToHost; }
	int TakeProgramStep() { const int nStep = m_nProgramStep; m_nProgramStep = 0; return nStep; }
	// Commands are still waiting to be sent
	bool IsSending() const { return m_nTxOut != m_nTxIn; }

	// Transposes a note message from the guitar (called from interrupt context, for the first port); returns false
	// if the message is to be dropped (the note would be out of range). The transpose is performed here rather than
	// by the controller, whose transpose parameter is kept at zero: only its octave steps are known to work, and so
	// each note's off message is transposed as its on message was, also when the transpose has changed meanwhile.
	bool TransposeMessage(u8* pMessage, unsigned nLength);
	// Octaves * 12 + semitones, plus what the controller transposes itself (when it performs its buttons)
	int GetTranspose() const;
	void SetTranspose(int nSemitones);

	// A parameter message from the controller (on whichever port it arrives)
	static bool IsCommandMessage(const u8* pMessage, unsigned nLength)
	{
		return nLength == 3 && pMessage[0] == 0xB7 && (pMessage[1] == 31 || pMessage[1] == 63);
	}

	// The controller has answered since the USB MIDI device was connected
	bool IsConnected() const { return m_bConnected; }
	// The USB MIDI device didn't answer when its settings were read
	bool HasNoReply() const { return m_bNoReply; }

	// Settings that can be changed in the menu: the patch's (sent as the official utility does when a button is
	// pressed) and the button assignments (globals, stored in the controller)
	enum class TSetting : u8
	{
		Octave,			// Transpose: octaves and semitones, added up
		Semitone,
		BendMode,		// 0 Auto, 1 Trigger, 2 Step, 3 Smooth
		BendRange,		// Semitones
		ChannelMode,		// 0 multi, 1 single
		ButtonAction,		// What the Up/Down buttons do ("Single Press")
		BothButtonsAction,	// What pressing both does ("Simultaneous Press")
		HandPosition,		// Monitor hand position: 0 off, 1 on
		Count
	};

	// Unknown if the controller hasn't reported it yet
	int GetSetting(TSetting Setting) const;
	// Changing a value that is unknown starts from the controller's default
	void ChangeSetting(TSetting Setting, int nDelta);
	// The value as shown, "-" if unknown
	void FormatSetting(TSetting Setting, char* pBuffer, size_t nSize) const;

	// 1 (high E) to 6 (low E); returns 1-16, or 0 if unknown
	int GetStringSensitivity(u8 nString) const;
	void SetStringSensitivity(u8 nString, int nLevel);

	// Tuning of the open strings, 1 (high E) to 6 (low E): the note's name (e.g. "D"), or "-" if unknown; changing
	// it moves the note by semitones, at most 8 down and 7 up from standard tuning
	void FormatOpenString(u8 nString, char* pBuffer, size_t nSize) const;
	void ChangeOpenString(u8 nString, int nDelta);

	// The current settings, and setting them all at once (when connected, or else once it is)
	void GetProfile(TProfile& Profile) const;
	void SetProfile(const TProfile& Profile);

	// The controller has reported its battery voltage
	bool HasBattery() const;
	void FormatBattery(char* pBuffer, size_t nSize) const;

private:
	struct TCommand
	{
		u8 nID;
		u8 nValue;
	};

	static constexpr unsigned RxBufferSize = 64;
	static constexpr unsigned TxQueueSize = 32;
	static constexpr unsigned GlobalCount = 21;

	void Reset();
	void SendSoftwareMode(bool bOn);
	void ApplyProfile(const TProfile& Profile);
	void HandleParameter(u8 nID, u8 nValue);
	void HandleNavigation(u8 nValue);
	void PerformButton(int nButton);
	void PerformSingle(bool bUp);
	void PerformBoth();
	void Action(u8 nID, int nValue);
	int GetValue(u8 nID, int nDefault) const;
	bool Send(u8 nID, u8 nValue);
	void SendCommand(const TCommand& Command);
	void GiveUpCommand();
	void ChangeTranspose(int nOctaves, int nSemitones);
	void ResetControllerTranspose();
	void ReadGlobals();
	void SendNextPeek();
	void SetGlobal(u8 nIndex, int nValue);
	void StoreGlobalsLater();

	// Messages from the command port, filled in interrupt context
	u8 m_RxBuffer[RxBufferSize][3];
	volatile unsigned m_nRxIn;
	volatile unsigned m_nRxOut;

	// Commands are sent one at a time, each once the controller has echoed the one before
	TCommand m_TxQueue[TxQueueSize];
	unsigned m_nTxIn;
	unsigned m_nTxOut;
	unsigned m_nLastSendTime;
	bool m_bAwaitingAck;
	unsigned m_nAckTime;
	unsigned m_nAckRetries;
	bool m_bShortFormat;	// Until the controller doesn't answer to it

	CUSBMIDIDevice* m_pDevice;
	bool m_bConnected;
	bool m_bNoReply;
	bool m_bSoftwareMode;
	bool m_bChanged;
	bool m_bProgramsToHost;
	int m_nProgramStep;

	// Last value reported or sent for each parameter, -1 if unknown
	s16 m_Values[128];
	s16 m_Globals[GlobalCount];
	int m_nLastID;

	// Reading the globals one at a time
	bool m_bPeekActive;
	u8 m_nPeekIndex;

	// A press of both buttons arrives as both press codes within a short time
	int m_nPendingButton;
	unsigned m_nPendingTime;

	// Globals are stored in the controller once the sensitivity hasn't changed for a while
	bool m_bStorePending;
	unsigned m_nStoreTime;

	// Transpose performed by Troubadour; read in interrupt context
	volatile int m_nOctaves;
	volatile int m_nSemitones;
	// Per MIDI channel and note played: the transpose of its on message, or NoteDropped
	static constexpr s8 NoteDropped = -128;
	s8 m_NoteTranspose[16][128];

	// Settings to send once the receiver is connected
	TProfile m_PendingProfile;
	bool m_bProfilePending;
};

#endif
