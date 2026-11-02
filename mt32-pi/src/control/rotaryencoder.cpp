//
// rotaryencoder.cpp
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

#include "control/rotaryencoder.h"
#include "utility.h"

// Based on the rotary encoder reading routines used in FlashFloppy by Kier Fraser
// https://github.com/keirf/flashfloppy/blob/master/src/main.c

// Returns the previous valid transition in same direction.
// eg. PreviousTransition(0b0111) == 0b0001
static constexpr u8 PreviousTransition(u8 nTransition)
{
	return (nTransition >> 2) | (((nTransition ^ 3) & 3) << 2);
}

// Rotary encoder outputs a Gray code, counting clockwise: 00-01-11-10
static constexpr u32 nTransitions = 0b00100100010000101000000100011000;

// Number of back-to-back transitions we see per detent on various types of rotary encoder
static constexpr u8 TransitionsPerDetent[] = { 4, 2, 1 };

CRotaryEncoder::CRotaryEncoder(TEncoderType Type, bool bReversed, unsigned GPIOPinCLK, unsigned GPIOPinDAT)
	: m_CLKPin(GPIOPinCLK, GPIOModeInputPullUp),
	  m_DATPin(GPIOPinDAT, GPIOModeInputPullUp),

	  m_Type(Type),
	  m_bReversed(bReversed),

	  m_nDelta(0),
	  m_nState(0),
	  nPreviousTransitions{ 0 }
{
}

s8 CRotaryEncoder::Read()
{
	// The delta is updated from interrupt context; take it atomically so that no step is lost
	const s8 nResult = __atomic_exchange_n(&m_nDelta, 0, __ATOMIC_ACQ_REL);

	return m_bReversed ? -nResult : nResult;
}

void CRotaryEncoder::ReadGPIOPins()
{
	ReadGPIOPins(m_CLKPin.Read(), m_DATPin.Read());
}

void CRotaryEncoder::ReadGPIOPins(bool bCLKValue, bool bDATValue)
{
	m_nState = ((m_nState << 2) | (bDATValue << 1) | bCLKValue) & 0x0F;

	// Check if we have seen a valid CW or CCW state transition
	u8 nRotaryBits = (nTransitions >> (m_nState << 1)) & 3;
	if (likely(!nRotaryBits))
		return;

	// Have we seen the /previous/ transition in this direction?
	// If not, any previously-recorded transitions are not in a contiguous step-wise
	// sequence, and should be discarded as switch bounce.
	u16 nTransition = nPreviousTransitions[nRotaryBits-1];

	// Clear any existing bounce transitions
	if (!(nTransition & (1 << PreviousTransition(m_nState))))
		nTransition = 0;

	// Record this transition and check if we have seen enough to get us from one detent to another
	nTransition |= (1 << m_nState);

	// Full-step encoders rest with both contacts open (11) or, on some models, both closed (00); after all four
	// transitions of a detent, the encoder is back at its resting state
	const u8 nPins = m_nState & 3;
	if ((__builtin_popcount(nTransition) < TransitionsPerDetent[static_cast<u8>(m_Type)])
	   || ((m_Type == TEncoderType::Full) && nPins != 3 && nPins != 0))
	{
		// Not enough transitions yet; remember where we are for next time
		nPreviousTransitions[nRotaryBits-1] = nTransition;
		return;
	}

	// This is a valid movement between detents; clear transition state and accumulate movement
	nPreviousTransitions[0] = nPreviousTransitions[1] = 0;
	m_nDelta += (nRotaryBits & 0x01) ? 1 : -1;
}
