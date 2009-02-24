// qtractorMidiMonitor.cpp
//
/****************************************************************************
   Copyright (C) 2005-2009, rncbc aka Rui Nuno Capela. All rights reserved.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

*****************************************************************************/

#include "qtractorMidiMonitor.h"

#include "qtractorSession.h"
#include "qtractorSessionCursor.h"
#include "qtractorAudioEngine.h"
#include "qtractorMidiEngine.h"


// Module constants.
const unsigned int c_iQueueSize = 16; // Must be power of 2;
const unsigned int c_iQueueMask = c_iQueueSize - 1;

// Singleton variables.
unsigned long qtractorMidiMonitor::s_iFrameSlot = 0;
unsigned long qtractorMidiMonitor::s_iTimeSlot  = 0;


//----------------------------------------------------------------------------
// qtractorMidiMonitor -- MIDI monitor bridge value processor.

// Constructor.
qtractorMidiMonitor::qtractorMidiMonitor ( float fGain, float fPanning )
	: qtractorMonitor(fGain, fPanning)
{
	// Allocate actual buffer stuff...
	m_pQueue = new QueueItem [c_iQueueSize];

	// May reset now...
	reset();
}

// Destructor.
qtractorMidiMonitor::~qtractorMidiMonitor (void)
{
	delete [] m_pQueue;
}


// Monitor enqueue method.
void qtractorMidiMonitor::enqueue ( qtractorMidiEvent::EventType type,
	unsigned char val, unsigned long tick )
{
	// Check whether this is a scheduled value...
	if (m_iTimeStart < tick && s_iTimeSlot > 0) {
		// Find queue offset index...
		unsigned int iOffset = (tick - m_iTimeStart) / s_iTimeSlot;
		// FIXME: Ignore outsiders (which would manifest as
		// out-of-time phantom monitor peak values...)
		if (iOffset > c_iQueueMask)
			iOffset = c_iQueueMask;
		unsigned int iIndex = (m_iQueueIndex + iOffset) & c_iQueueMask;
		// Set the value in buffer...
		QueueItem& item = m_pQueue[iIndex];
		if (item.value < val && type == qtractorMidiEvent::NOTEON)
			item.value = val;
		// Increment enqueued count.
		item.count++;
		// Done with enqueueing.
	} else {
		// Alternative is sending it directly
		// as a non-enqueued direct value...
		if (m_item.value < val && type == qtractorMidiEvent::NOTEON)
			m_item.value = val;
		// Increment direct count.
		m_item.count++;
		// Done direct.
	}
}


// Monitor value dequeue method.
float qtractorMidiMonitor::value (void)
{
	// Grab-and-reset current direct value...
	unsigned char val = m_item.value;
	m_item.value = 0;

	qtractorSession *pSession = qtractorSession::getInstance();
	if (pSession && s_iFrameSlot > 0) {
		// Sweep the queue until current time...
		unsigned long iFrameEnd
			= pSession->audioEngine()->sessionCursor()->frameTime();
		while (m_iFrameStart < iFrameEnd) {
			QueueItem& item = m_pQueue[m_iQueueIndex];
			if (val < item.value)
				val = item.value;
			m_item.count += item.count;
			item.value = 0;
			item.count = 0;
			++m_iQueueIndex &= c_iQueueMask;
			m_iFrameStart += s_iFrameSlot;
			m_iTimeStart += s_iTimeSlot;
		}
	}

	// Dequeue done.
	return (gain() * val) / 127.0f;
}


// Monitor count dequeue method.
int qtractorMidiMonitor::count (void)
{
	// Grab latest direct/dequeued count...
	int iCount = int(m_item.count);
	m_item.count = 0;
	return iCount;
}


// Reset monitor.
void qtractorMidiMonitor::reset (void)
{
	// (Re)initialize all...
	m_item.value  = 0;
	m_item.count  = 0;
	m_iQueueIndex = 0;

	qtractorSession *pSession = qtractorSession::getInstance();
	if (pSession) {
		// Reset time references...
		unsigned long iFrame = pSession->playHead();
		qtractorTimeScale::Cursor cursor(pSession->timeScale());
		qtractorTimeScale::Node *pNode = cursor.seekFrame(iFrame);
		unsigned long t0 = pNode->tickFromFrame(iFrame);
		// Time (re)start: the time (in ticks) of the
		// current queue head slot; usually zero ;)
		m_iFrameStart = pSession->audioEngine()->sessionCursor()->frameTime();
		m_iTimeStart  = pNode->tickFromFrame(iFrame + m_iFrameStart) - t0;
	} else {
		m_iFrameStart = 0;
		m_iTimeStart  = 0;
	}

	// Time to reset buffer...
	for (unsigned int i = 0; i < c_iQueueSize; ++i) {
		m_pQueue[i].value = 0;
		m_pQueue[i].count = 0;
	}
}


// Update monitor (nothing really done here).
void qtractorMidiMonitor::update (void)
{
	// Do nothing yet...
}


// Singleton sync reset.
void qtractorMidiMonitor::syncReset (void)
{
	qtractorSession *pSession = qtractorSession::getInstance();
	if (pSession == NULL)
		return;

	// Reset time references...
	unsigned long iFrame = pSession->playHead();
	qtractorTimeScale::Cursor cursor(pSession->timeScale());
	qtractorTimeScale::Node *pNode = cursor.seekFrame(iFrame);
	unsigned long t0 = pNode->tickFromFrame(iFrame);

	// Time slot: the amount of time (in ticks)
	// each queue slot will hold scheduled events;
	s_iFrameSlot = (pSession->midiEngine()->readAhead() << 1) / c_iQueueSize;
	s_iTimeSlot  = (pNode->tickFromFrame(iFrame + s_iFrameSlot) - t0);
}


// end of qtractorMidiMonitor.cpp
