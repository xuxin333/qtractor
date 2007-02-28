// qtractorMidiEngine.cpp
//
/****************************************************************************
   Copyright (C) 2005-2007, rncbc aka Rui Nuno Capela. All rights reserved.

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

#include "qtractorAbout.h"
#include "qtractorMidiEngine.h"
#include "qtractorMidiMonitor.h"
#include "qtractorMidiEvent.h"

#include "qtractorSessionCursor.h"
#include "qtractorSessionDocument.h"
#include "qtractorAudioEngine.h"

#include "qtractorMidiSequence.h"
#include "qtractorMidiClip.h"

#include <QApplication>

#include <QThread>
#include <QMutex>
#include <QWaitCondition>

#include <QSocketNotifier>


// Specific controller definitions
#define BANK_SELECT_MSB		0x00
#define BANK_SELECT_LSB		0x20

#define ALL_SOUND_OFF		0x78
#define ALL_CONTROLLERS_OFF	0x79
#define ALL_NOTES_OFF		0x7b

#define CHANNEL_VOLUME		0x07
#define CHANNEL_PANNING		0x0a


//----------------------------------------------------------------------
// class qtractorMidiInputThread -- MIDI input thread (singleton).
//

class qtractorMidiInputThread : public QThread
{
public:

	// Constructor.
	qtractorMidiInputThread(qtractorSession *pSession);

	// Destructor.
	~qtractorMidiInputThread();

	// Thread run state accessors.
	void setRunState(bool bRunState);
	bool runState() const;

protected:

	// The main thread executive.
	void run();

private:

	// The thread launcher engine.
	qtractorSession *m_pSession;

	// Whether the thread is logically running.
	bool m_bRunState;
};


//----------------------------------------------------------------------
// class qtractorMidiOutputThread -- MIDI output thread (singleton).
//

class qtractorMidiOutputThread : public QThread
{
public:

	// Constructor.
	qtractorMidiOutputThread(qtractorSession *pSession,
		unsigned int iReadAhead = 0);

	// Destructor.
	~qtractorMidiOutputThread();

	// Thread run state accessors.
	void setRunState(bool bRunState);
	bool runState() const;

	// Read ahead frames configuration.
	void setReadAhead(unsigned int iReadAhead);
	unsigned int readAhead() const;

	// MIDI/Audio sync-check predicate.
	qtractorSessionCursor *midiCursorSync(bool bStart = false);

	// MIDI track output process resync.
	void trackSync(qtractorTrack *pTrack, unsigned long iFrameStart);

	// MIDI output process cycle iteration (locked).
	void processSync();

	// Wake from executive wait condition.
	void sync();

protected:

	// The main thread executive.
	void run();

	// MIDI output process cycle iteration.
	void process();

private:

	// The thread launcher engine.
	qtractorSession *m_pSession;

	// The number of frames to read-ahead.
	unsigned int m_iReadAhead;

	// Whether the thread is logically running.
	bool m_bRunState;

	// Thread synchronization objects.
	QMutex m_mutex;
	QWaitCondition m_cond;
};


//----------------------------------------------------------------------
// class qtractorMidiInputThread -- MIDI input thread (singleton).
//

// Constructor.
qtractorMidiInputThread::qtractorMidiInputThread (
	qtractorSession *pSession ) : QThread()
{
	m_pSession  = pSession;
	m_bRunState = false;
}


// Destructor.
qtractorMidiInputThread::~qtractorMidiInputThread (void)
{
	// Try to wake and terminate executive thread.
	if (runState())
		setRunState(false);

	// Give it a bit of time to cleanup...
	if (isRunning())
		QThread::msleep(100);
}


// Thread run state accessors.
void qtractorMidiInputThread::setRunState ( bool bRunState )
{
	m_bRunState = bRunState;
}

bool qtractorMidiInputThread::runState (void) const
{
	return m_bRunState;
}


// The main thread executive.
void qtractorMidiInputThread::run (void)
{
	snd_seq_t *pAlsaSeq = m_pSession->midiEngine()->alsaSeq();
	if (pAlsaSeq == NULL)
		return;

#ifdef CONFIG_DEBUG_0
	fprintf(stderr, "qtractorMidiInputThread::run(%p): started.\n", this);
#endif

	int nfds;
	struct pollfd *pfds;

	nfds = snd_seq_poll_descriptors_count(pAlsaSeq, POLLIN);
	pfds = (struct pollfd *) alloca(nfds * sizeof(struct pollfd));
	snd_seq_poll_descriptors(pAlsaSeq, pfds, nfds, POLLIN);

	m_bRunState = true;

	int iPoll = 0;
	while (m_bRunState && iPoll >= 0) {
		// Wait for events...
		iPoll = poll(pfds, nfds, 200);
		while (iPoll > 0) {
			snd_seq_event_t *pEv = NULL;
			snd_seq_event_input(pAlsaSeq, &pEv);
			// Process input event - ...
			// - enqueue to input track mapping;
			m_pSession->midiEngine()->capture(pEv);
			// - direct route to output;
			//	 ...
		//	snd_seq_free_event(pEv);
			iPoll = snd_seq_event_input_pending(pAlsaSeq, 0);
		}
	}

#ifdef CONFIG_DEBUG_0
	fprintf(stderr, "qtractorMidiInputThread::run(%p): stopped.\n", this);
#endif
}


//----------------------------------------------------------------------
// class qtractorMidiOutputThread -- MIDI output thread (singleton).
//

// Constructor.
qtractorMidiOutputThread::qtractorMidiOutputThread (
	qtractorSession *pSession, unsigned int iReadAhead ) : QThread()
{
	if (iReadAhead < 1)
		iReadAhead = pSession->sampleRate();

	m_pSession   = pSession;
	m_bRunState  = false;
	m_iReadAhead = iReadAhead;
}


// Destructor.
qtractorMidiOutputThread::~qtractorMidiOutputThread (void)
{
	// Try to wake and terminate executive thread.
	if (runState())
		setRunState(false);

	// Give it a bit of time to cleanup...
	if (isRunning()) {
		sync();
		QThread::msleep(100);
	}
}


// Thread run state accessors.
void qtractorMidiOutputThread::setRunState ( bool bRunState )
{
	m_bRunState = bRunState;
}

bool qtractorMidiOutputThread::runState (void) const
{
	return m_bRunState;
}


// Read ahead frames configuration.
void qtractorMidiOutputThread::setReadAhead ( unsigned int iReadAhead )
{
	m_iReadAhead = iReadAhead;
}

unsigned int qtractorMidiOutputThread::readAhead (void) const
{
	return m_iReadAhead;
}


// Audio/MIDI sync-check and cursor predicate.
qtractorSessionCursor *qtractorMidiOutputThread::midiCursorSync ( bool bStart )
{
	// We'll need access to master audio engine...
	qtractorSessionCursor *pAudioCursor
		= m_pSession->audioEngine()->sessionCursor();
	if (pAudioCursor == NULL)
		return NULL;

	// And to our slave MIDI engine too...
	qtractorSessionCursor *pMidiCursor
		= m_pSession->midiEngine()->sessionCursor();
	if (pMidiCursor == NULL)
		return NULL;

	// Can MIDI be ever behind audio?
	if (bStart) {
		pMidiCursor->seek(pAudioCursor->frame());
	//	pMidiCursor->setFrameTime(pAudioCursor->frameTime());
	}
	else // No, it cannot be behind more than the read-ahead period...
	if (pMidiCursor->frameTime() > pAudioCursor->frameTime() + m_iReadAhead)
		return NULL;

	// Nope. OK.
	return pMidiCursor;
}


// The main thread executive.
void qtractorMidiOutputThread::run (void)
{
#ifdef CONFIG_DEBUG_0
	fprintf(stderr, "qtractorMidiOutputThread::run(%p): started.\n", this);
#endif

	m_bRunState = true;

	m_mutex.lock();
	while (m_bRunState) {
		// Wait for sync...
		m_cond.wait(&m_mutex);
#ifdef CONFIG_DEBUG_0
		fprintf(stderr, "qtractorMidiOutputThread::run(%p): waked.\n", this);
#endif
		// Only if playing, the output process cycle.
		if (m_pSession->isPlaying())
			process();
	}
	m_mutex.unlock();

#ifdef CONFIG_DEBUG_0
	fprintf(stderr, "qtractorMidiOutputThread::run(%p): stopped.\n", this);
#endif
}


// MIDI output process cycle iteration.
void qtractorMidiOutputThread::process (void)
{
	// Get a handle on our slave MIDI engine...
	qtractorSessionCursor *pMidiCursor = midiCursorSync();
	// Isn't MIDI slightly behind audio?
	if (pMidiCursor == NULL)
		return;

	// Now for the next readahead bunch...
	unsigned long iFrameStart = pMidiCursor->frame();
	unsigned long iFrameEnd   = iFrameStart + m_iReadAhead;

#ifdef CONFIG_DEBUG_0
	fprintf(stderr, "qtractorMidiOutputThread::process(%p, %lu, %lu)\n",
		this, iFrameStart, iFrameEnd);
#endif

	// Split processing, in case we're looping...
	if (m_pSession->isLooping() && iFrameStart < m_pSession->loopEnd()) {
		// Loop-length might be shorter than the read-ahead...
		while (iFrameEnd >= m_pSession->loopEnd()) {
			// Process the remaining until end-of-loop...
			m_pSession->process(pMidiCursor, iFrameStart, m_pSession->loopEnd());
			// Reset to start-of-loop...
			iFrameStart = m_pSession->loopStart();
			iFrameEnd   = iFrameStart + (iFrameEnd - m_pSession->loopEnd());
			pMidiCursor->seek(iFrameStart);
			// This is really a must...
			m_pSession->midiEngine()->restartLoop();
		}
	}

	// Regular range...
	m_pSession->process(pMidiCursor, iFrameStart, iFrameEnd);

	// Sync with loop boundaries (unlikely?)...
	if (m_pSession->isLooping() && iFrameStart < m_pSession->loopEnd()
		&& iFrameEnd >= m_pSession->loopEnd()) {
		iFrameEnd = m_pSession->loopStart()
			+ (iFrameEnd - m_pSession->loopEnd());
	}

	// Sync to the next bunch, also critical for Audio-MIDI sync...
	pMidiCursor->seek(iFrameEnd);
	pMidiCursor->process(m_iReadAhead);

	// Flush the MIDI engine output queue...
	m_pSession->midiEngine()->flush();
}


// MIDI output process cycle iteration (locked).
void qtractorMidiOutputThread::processSync (void)
{
	QMutexLocker locker(&m_mutex);
#ifdef CONFIG_DEBUG_0
	fprintf(stderr, "qtractorMidiOutputThread::processSync(%p)\n", this);
#endif
	process();
}


// MIDI track output process resync.
void qtractorMidiOutputThread::trackSync ( qtractorTrack *pTrack,
	unsigned long iFrameStart )
{
	QMutexLocker locker(&m_mutex);

	// Pick our actual MIDI sequencer cursor...
	qtractorSessionCursor *pMidiCursor
		= m_pSession->midiEngine()->sessionCursor();
	if (pMidiCursor == NULL)
		return;

	// This is the last framestamp to be trown out...
	unsigned long iFrameEnd = pMidiCursor->frame();

#ifdef CONFIG_DEBUG_0
	fprintf(stderr, "qtractorMidiOutputThread::trackSync(%p, %lu, %lu)\n",
		this, iFrameStart, iFrameEnd);
#endif

	// Locate the immediate nearest clip in track
	// and render them all thereafter, immediately...
	qtractorClip *pClip = pTrack->clips().first();
	while (pClip && pClip->clipStart() < iFrameEnd) {
		if (iFrameStart < pClip->clipStart() + pClip->clipLength())
			pClip->process(iFrameStart, iFrameEnd);
		pClip = pClip->next();
	}

	// Surely must realize the output queue...
	m_pSession->midiEngine()->flush();
}


// Wake from executive wait condition.
void qtractorMidiOutputThread::sync (void)
{
	if (m_mutex.tryLock()) {
		m_cond.wakeAll();
		m_mutex.unlock();
	}
#ifdef CONFIG_DEBUG_0
	else fprintf(stderr, "qtractorMidiOutputThread::sync(%p): tryLock() failed.\n", this);
#endif
}


//----------------------------------------------------------------------
// class qtractorMidiEngine -- ALSA sequencer client instance (singleton).
//

// Constructor.
qtractorMidiEngine::qtractorMidiEngine ( qtractorSession *pSession )
	: qtractorEngine(pSession, qtractorTrack::Midi)
{
	m_pAlsaSeq       = NULL;
	m_iAlsaClient    = -1;
	m_iAlsaQueue     = -1;

	m_pAlsaSubsSeq   = NULL;
	m_iAlsaSubsPort  = -1;
	m_pAlsaNotifier  = NULL;

	m_pInputThread   = NULL;
	m_pOutputThread  = NULL;

	m_iTimeStart     = 0;
	m_iTimeDelta     = 0;

	m_pNotifyWidget  = NULL;
	m_eNotifyMmcType = QEvent::None;

	m_pIControlBus   = NULL;
	m_pOControlBus   = NULL;
}


// ALSA sequencer client descriptor accessor.
snd_seq_t *qtractorMidiEngine::alsaSeq (void) const
{
	return m_pAlsaSeq;
}

int qtractorMidiEngine::alsaClient (void) const
{
	return m_iAlsaClient;
}

int qtractorMidiEngine::alsaQueue (void) const
{
	return m_iAlsaQueue;
}


// ALSA subscription port notifier.
QSocketNotifier *qtractorMidiEngine::alsaNotifier (void) const
{
	return m_pAlsaNotifier;
}


// ALSA subscription notifier acknowledgment.
void qtractorMidiEngine::alsaNotifyAck (void)
{
	if (m_pAlsaSubsSeq == NULL)
		return;

	do {
		snd_seq_event_t *pAlsaEvent;
		snd_seq_event_input(m_pAlsaSubsSeq, &pAlsaEvent);
		snd_seq_free_event(pAlsaEvent);
	}
	while (snd_seq_event_input_pending(m_pAlsaSubsSeq, 0) > 0);
}


// Special slave sync method.
void qtractorMidiEngine::sync (void)
{
	// Pure conditional thread slave syncronization...
	if (m_pOutputThread && m_pOutputThread->midiCursorSync())
		m_pOutputThread->sync();
}


// Read ahead frames configuration.
void qtractorMidiEngine::setReadAhead ( unsigned int iReadAhead )
{
	if (m_pOutputThread)
		m_pOutputThread->setReadAhead(iReadAhead);
}

unsigned int qtractorMidiEngine::readAhead (void) const
{
	return (m_pOutputThread ? m_pOutputThread->readAhead() : 0);
}


// Reset queue tempo.
void qtractorMidiEngine::resetTempo (void)
{
	// It must be surely activated...
	if (!isActivated())
		return;

	// There must a session reference...
	qtractorSession *pSession = session();
	if (pSession == NULL)
		return;

	// Set queue tempo...
	snd_seq_queue_tempo_t *tempo;
	snd_seq_queue_tempo_alloca(&tempo);
	// Fill tempo struct with current tempo info.
	snd_seq_get_queue_tempo(m_pAlsaSeq, m_iAlsaQueue, tempo);
	// Set the new intended ones...
	snd_seq_queue_tempo_set_ppq(tempo, (int) pSession->ticksPerBeat());
	snd_seq_queue_tempo_set_tempo(tempo,
		(unsigned int) (60000000.0f / pSession->tempo()));
	// Give tempo struct to the queue.
	snd_seq_set_queue_tempo(m_pAlsaSeq, m_iAlsaQueue, tempo);
}


// Reset all MIDI monitoring...
void qtractorMidiEngine::resetAllMonitors (void)
{
	// There must a session reference...
	qtractorSession *pSession = session();
	if (pSession == NULL)
		return;

	// Reset all MIDI buses monitors...
	for (qtractorBus *pBus = buses().first();
			pBus; pBus = pBus->next()) {
		qtractorMidiBus *pMidiBus
			= static_cast<qtractorMidiBus *> (pBus);
		if (pMidiBus) {
			if (pMidiBus->midiMonitor_in()) {
				pMidiBus->midiMonitor_in()->reset();
				if (pMidiBus->midiMonitor_out() == NULL)
					pMidiBus->setMasterVolume(pMidiBus->midiMonitor_in()->gain());
			}
			if (pMidiBus->midiMonitor_out()) {
				pMidiBus->midiMonitor_out()->reset();
				pMidiBus->setMasterVolume(pMidiBus->midiMonitor_out()->gain());
			}
		}
	}

	// Reset all MIDI monitors...
	for (qtractorTrack *pTrack = pSession->tracks().first();
			pTrack; pTrack = pTrack->next()) {
		if (pTrack->trackType() == qtractorTrack::Midi) {
			qtractorMidiBus *pMidiBus
				= static_cast<qtractorMidiBus *> (pTrack->outputBus());
			qtractorMidiMonitor *pMidiMonitor
				= static_cast<qtractorMidiMonitor *> (pTrack->monitor());
			if (pMidiBus && pMidiMonitor) {
				pMidiMonitor->reset();
				pMidiBus->setVolume(pTrack->midiChannel(), pMidiMonitor->gain());
				pMidiBus->setPanning(pTrack->midiChannel(), pMidiMonitor->panning());
			}
		}
	}
}


// Control bus mode selector.
void qtractorMidiEngine::resetControlBus ( qtractorBus::BusMode busMode )
{
	// Reset both control buses...
	m_pIControlBus = NULL;
	m_pOControlBus = NULL;

	// Find available control buses...
	if (busMode & qtractorBus::Duplex) {
		for (qtractorBus *pBus = qtractorEngine::buses().first();
				pBus; pBus = pBus->next()) {
			if (m_pIControlBus == NULL
				&& (pBus->busMode() & (busMode & qtractorBus::Input)))
				m_pIControlBus = static_cast<qtractorMidiBus *> (pBus);
			if (m_pOControlBus == NULL
				&& (pBus->busMode() & (busMode & qtractorBus::Output)))
				m_pOControlBus = static_cast<qtractorMidiBus *> (pBus);
		}
	}
}


// MIDI event capture method.
void qtractorMidiEngine::capture ( snd_seq_event_t *pEv )
{
	qtractorMidiEvent::EventType type;

	unsigned short iChannel = 0;
	unsigned char  data1    = 0;
	unsigned char  data2    = 0;
	unsigned long  duration = 0;

	unsigned char *pSysex   = NULL;
	unsigned short iSysex   = 0;

#ifdef CONFIG_DEBUG
	// - show event for debug purposes...
	fprintf(stderr, "MIDI In  %05d 0x%02x", pEv->time.tick, pEv->type);
	if (pEv->type == SND_SEQ_EVENT_SYSEX) {
		fprintf(stderr, " sysex {");
		unsigned char *data = (unsigned char *) pEv->data.ext.ptr;
		for (unsigned int i = 0; i < pEv->data.ext.len; i++)
			fprintf(stderr, " %02x", data[i]);
		fprintf(stderr, " }\n");
	} else {
		for (unsigned int i = 0; i < sizeof(pEv->data.raw8.d); i++)
			fprintf(stderr, " %3d", pEv->data.raw8.d[i]);
		fprintf(stderr, "\n");
	}
#endif

	switch (pEv->type) {
	case SND_SEQ_EVENT_NOTE:
	case SND_SEQ_EVENT_NOTEON:
		type     = qtractorMidiEvent::NOTEON;
		iChannel = pEv->data.note.channel;
		data1    = pEv->data.note.note;
		data2    = pEv->data.note.velocity;
		duration = pEv->data.note.duration;
		if (data2 == 0)
			type = qtractorMidiEvent::NOTEOFF;
		break;
	case SND_SEQ_EVENT_NOTEOFF:
		type     = qtractorMidiEvent::NOTEOFF;
		iChannel = pEv->data.note.channel;
		data1    = pEv->data.note.note;
		data2    = pEv->data.note.velocity;
		duration = pEv->data.note.duration;
		break;
	case SND_SEQ_EVENT_KEYPRESS:
		type     = qtractorMidiEvent::KEYPRESS;
		iChannel = pEv->data.control.channel;
		data1    = pEv->data.control.param;
		data2    = pEv->data.control.value;
		break;
	case SND_SEQ_EVENT_CONTROLLER:
		type     = qtractorMidiEvent::CONTROLLER;
		iChannel = pEv->data.control.channel;
		data1    = pEv->data.control.param;
		data2    = pEv->data.control.value;
		break;
	case SND_SEQ_EVENT_PGMCHANGE:
		type     = qtractorMidiEvent::PGMCHANGE;
		iChannel = pEv->data.control.channel;
		data1    = 0;
		data2    = pEv->data.control.value;
		break;
	case SND_SEQ_EVENT_CHANPRESS:
		type     = qtractorMidiEvent::CHANPRESS;
		iChannel = pEv->data.control.channel;
		data1    = 0;
		data2    = pEv->data.control.value;
		break;
	case SND_SEQ_EVENT_PITCHBEND:
		type     = qtractorMidiEvent::PITCHBEND;
		iChannel = pEv->data.control.channel;
		data1    = 0;
		data2    = pEv->data.control.value;
		break;
	case SND_SEQ_EVENT_SYSEX:
		type     = qtractorMidiEvent::SYSEX;
		pSysex   = (unsigned char *) pEv->data.ext.ptr;
		iSysex   = (unsigned short)  pEv->data.ext.len;
		// Trap MMC commands...
		if (pSysex[1] == 0x7f && pSysex[3] == 0x06
			// check if it was intended to our input control bus!
			&& m_pIControlBus
			&& m_pIControlBus->alsaPort() == pEv->dest.port) {
			// Post the stuffed event...
			if (m_pNotifyWidget) {
				QApplication::postEvent(m_pNotifyWidget,
					new qtractorMmcEvent(m_eNotifyMmcType, pSysex));
			}
			// Bail out, right now!
			return;
		}
		break;
	default:
		// Not handled here...
		return;
	}

	// Now check which bus and track we're into...
	for (qtractorTrack *pTrack = session()->tracks().first();
			pTrack; pTrack = pTrack->next()) {
		// Must be a MIDI track, in capture mode
		// and for the intended channel...
		if (pTrack->trackType() == qtractorTrack::Midi
			&& pTrack->isRecord() && pTrack->midiChannel() == iChannel) {
			qtractorMidiBus *pMidiBus
				= static_cast<qtractorMidiBus *> (pTrack->inputBus());
			if (pMidiBus && pMidiBus->alsaPort() == pEv->dest.port) {
				// Is it actually recording?...
				qtractorMidiClip *pMidiClip
					= static_cast<qtractorMidiClip *> (pTrack->clipRecord());
				if (pMidiClip) {
					// Yep, we got a new MIDI event...
					qtractorMidiEvent *pEvent = new qtractorMidiEvent(
						pEv->time.tick, type, data1, data2, duration);
					if (pSysex)
						pEvent->setSysex(pSysex, iSysex);
					pMidiClip->sequence()->addEvent(pEvent);
				}
				// Track input monitoring...
				qtractorMidiMonitor *pMidiMonitor
					= static_cast<qtractorMidiMonitor *> (pTrack->monitor());
				if (pMidiMonitor)
					pMidiMonitor->enqueue(type, data2);
			}
		}
	}

	// Bus monitoring...
	for (qtractorBus *pBus = buses().first(); pBus; pBus = pBus->next()) {
		qtractorMidiBus *pMidiBus
			= static_cast<qtractorMidiBus *> (pBus);
		if (pMidiBus && pMidiBus->alsaPort() == pEv->dest.port
			&& pMidiBus->midiMonitor_in()) {
			pMidiBus->midiMonitor_in()->enqueue(type, data2);
		}
	}
}


// MIDI event enqueue method.
void qtractorMidiEngine::enqueue ( qtractorTrack *pTrack,
	qtractorMidiEvent *pEvent, unsigned long iTime, float fGain )
{
	// Target MIDI bus...
	qtractorMidiBus *pMidiBus
		= static_cast<qtractorMidiBus *> (pTrack->outputBus());
	if (pMidiBus == NULL)
		return;

	// HACK: Ignore our own mixer-monitor supplied controllers...
	if (pEvent->type() == qtractorMidiEvent::CONTROLLER) {
		if (pEvent->controller() == CHANNEL_VOLUME ||
			pEvent->controller() == CHANNEL_PANNING)
			return;
	}

	// Scheduled delivery: take into account
	// the time playback/queue started...
	unsigned long tick = iTime - m_iTimeStart;

#ifdef CONFIG_DEBUG_0
	// - show event for debug purposes...
	fprintf(stderr, "MIDI Out %05lu 0x%02x", tick,
		(int) pEvent->type() | pTrack->midiChannel());
	if (pEvent->type() == qtractorMidiEvent::SYSEX) {
		fprintf(stderr, " sysex {");
		unsigned char *data = (unsigned char *) pEvent->sysex();
		for (unsigned int i = 0; i < pEvent->sysex_len(); i++)
			fprintf(stderr, " %02x", data[i]);
		fprintf(stderr, " }\n");
	} else {
		fprintf(stderr, " %3d %3d (duration=%lu)\n",
			pEvent->note(), pEvent->velocity(),
			pEvent->duration());
	}
#endif

	// Intialize outbound event...
	snd_seq_event_t ev;
	snd_seq_ev_clear(&ev);

	// Set Event tag...
	ev.tag = (unsigned char) (pTrack->midiTag() & 0xff);

	// Addressing...
	snd_seq_ev_set_source(&ev, pMidiBus->alsaPort());
	snd_seq_ev_set_subs(&ev);

	// Scheduled delivery...
	snd_seq_ev_schedule_tick(&ev, m_iAlsaQueue, 0, tick);

	// Set proper event data...
	unsigned char value = pEvent->value();
	switch (pEvent->type()) {
		case qtractorMidiEvent::NOTEON:
			value = (unsigned char) (fGain * float(value)) & 0x7f;
			ev.type = SND_SEQ_EVENT_NOTE;
			ev.data.note.channel    = pTrack->midiChannel();
			ev.data.note.note       = pEvent->note();
			ev.data.note.velocity   = value;
			ev.data.note.duration   = pEvent->duration();
			break;
		case qtractorMidiEvent::KEYPRESS:
			ev.type = SND_SEQ_EVENT_KEYPRESS;
			ev.data.control.channel = pTrack->midiChannel();
			ev.data.control.param   = pEvent->note();
			ev.data.control.value   = value;
			break;
		case qtractorMidiEvent::CONTROLLER:
			ev.type = SND_SEQ_EVENT_CONTROLLER;
			ev.data.control.channel = pTrack->midiChannel();
			ev.data.control.param   = pEvent->controller();
			ev.data.control.value   = value;
			break;
		case qtractorMidiEvent::PGMCHANGE:
			ev.type = SND_SEQ_EVENT_PGMCHANGE;
			ev.data.control.channel = pTrack->midiChannel();
			ev.data.control.value   = value;
			break;
		case qtractorMidiEvent::CHANPRESS:
			ev.type = SND_SEQ_EVENT_CHANPRESS;
			ev.data.control.channel = pTrack->midiChannel();
			ev.data.control.value   = value;
			break;
		case qtractorMidiEvent::PITCHBEND:
			ev.type = SND_SEQ_EVENT_PITCHBEND;
			ev.data.control.channel = pTrack->midiChannel();
			ev.data.control.value   = value;
			break;
		case qtractorMidiEvent::SYSEX: {
			ev.type = SND_SEQ_EVENT_SYSEX;
			snd_seq_ev_set_sysex(&ev, pEvent->sysex_len(), pEvent->sysex());
			break;
		}
		default:
			break;
	}

	// Pump it into the queue.
	snd_seq_event_output(m_pAlsaSeq, &ev);

	// MIDI track monitoring...
	qtractorMidiMonitor *pMidiMonitor
		= static_cast<qtractorMidiMonitor *> (pTrack->monitor());
	if (pMidiMonitor)
		pMidiMonitor->enqueue(pEvent->type(), value, tick);
	// MIDI bus monitoring...
	if (pMidiBus->midiMonitor_out()) {
		pMidiBus->midiMonitor_out()->enqueue(pEvent->type(), value, tick);
	}
}


// Flush ouput queue (if necessary)...
void qtractorMidiEngine::flush (void)
{
	// Really flush MIDI output...
	snd_seq_drain_output(m_pAlsaSeq);

	// Time to have some corrective approach...?
	snd_seq_queue_status_t *pQueueStatus;
	snd_seq_queue_status_alloca(&pQueueStatus);
	if (snd_seq_get_queue_status(
			m_pAlsaSeq, m_iAlsaQueue, pQueueStatus) >= 0) {
		unsigned long iMidiTime
			= snd_seq_queue_status_get_tick_time(pQueueStatus);
		unsigned long iAudioTime = session()->tickFromFrame(
			session()->audioEngine()->sessionCursor()->frameTime());
		long iTimeDelta = (iAudioTime - iMidiTime) - m_iTimeDelta;
		if (iTimeDelta && iAudioTime > 0 && iMidiTime > 0) {
			m_iTimeStart += iTimeDelta;
			m_iTimeDelta += iTimeDelta;
#ifdef CONFIG_DEBUG
			fprintf(stderr, "qtractorMidiEngine::flush(): "
				"iAudioTime=%lu iMidiTime=%lu iTimeDelta=%ld (%ld)\n",
				iAudioTime, iMidiTime, iTimeDelta, m_iTimeDelta);
#endif
		}
	}
}


// Device engine initialization method.
bool qtractorMidiEngine::init ( const QString& sClientName )
{
	// Try open a new client...
	if (snd_seq_open(&m_pAlsaSeq, "hw", SND_SEQ_OPEN_DUPLEX, 0) < 0)
		return false;
	if (m_pAlsaSeq == NULL)
		return false;

	// Fix client name.
	snd_seq_set_client_name(m_pAlsaSeq, sClientName.toUtf8().constData());

	m_iAlsaClient = snd_seq_client_id(m_pAlsaSeq);
	m_iAlsaQueue  = snd_seq_alloc_queue(m_pAlsaSeq);

	// Setup subscriptions stuff...
	if (snd_seq_open(&m_pAlsaSubsSeq, "hw", SND_SEQ_OPEN_DUPLEX, 0) >= 0) {
		m_iAlsaSubsPort = snd_seq_create_simple_port(
			m_pAlsaSubsSeq, clientName().toUtf8().constData(),
			SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE |
			SND_SEQ_PORT_CAP_NO_EXPORT, SND_SEQ_PORT_TYPE_APPLICATION);		
		if (m_iAlsaSubsPort >= 0) {
			struct pollfd pfd[1];
			snd_seq_addr_t seq_addr;
			snd_seq_port_subscribe_t *pAlsaSubs;
			snd_seq_port_subscribe_alloca(&pAlsaSubs);
			seq_addr.client = SND_SEQ_CLIENT_SYSTEM;
			seq_addr.port   = SND_SEQ_PORT_SYSTEM_ANNOUNCE;
			snd_seq_port_subscribe_set_sender(pAlsaSubs, &seq_addr);
			seq_addr.client = snd_seq_client_id(m_pAlsaSubsSeq);
			seq_addr.port   = m_iAlsaSubsPort;
			snd_seq_port_subscribe_set_dest(pAlsaSubs, &seq_addr);
			snd_seq_subscribe_port(m_pAlsaSubsSeq, pAlsaSubs);
			snd_seq_poll_descriptors(m_pAlsaSubsSeq, pfd, 1, POLLIN);
			m_pAlsaNotifier = new QSocketNotifier(
				pfd[0].fd, QSocketNotifier::Read);
		}
	}

	return true;
}


// Device engine activation method.
bool qtractorMidiEngine::activate (void)
{
	// There must a session reference...
	qtractorSession *pSession = session();
	if (pSession == NULL)
		return false;

	// Create and start our own MIDI input queue thread...
	m_pInputThread = new qtractorMidiInputThread(pSession);
	m_pInputThread->start(QThread::TimeCriticalPriority);

	// Create and start our own MIDI output queue thread...
	m_pOutputThread = new qtractorMidiOutputThread(pSession);
	m_pOutputThread->start(QThread::HighPriority);

	m_iTimeStart = 0;
	m_iTimeDelta = 0;

	// Reset control buses...
	resetControlBus(qtractorBus::Duplex);
	// Reset all dependable monitoring...
	resetAllMonitors();

	return true;
}


// Device engine start method.
bool qtractorMidiEngine::start (void)
{
	// It must be surely activated...
	if (!isActivated())
		return false;

	// There must a session reference...
	qtractorSession *pSession = session();
	if (pSession == NULL)
		return false;

	// Output thread must be around too...
	if (m_pOutputThread == NULL)
		return false;

	// Initial output thread bumping...
	qtractorSessionCursor *pMidiCursor
		= m_pOutputThread->midiCursorSync(true);
	if (pMidiCursor == NULL)
		return false;

	// Reset all dependables...
	resetTempo();
	resetAllMonitors();

	// Start queue timer...
	m_iTimeStart = (long) pSession->tickFromFrame(pMidiCursor->frame());
	m_iTimeDelta = 0;

	// Effectively start sequencer queue timer...
	snd_seq_start_queue(m_pAlsaSeq, m_iAlsaQueue, NULL);

	// Carry on...
	m_pOutputThread->processSync();

	return true;
}


// Device engine stop method.
void qtractorMidiEngine::stop (void)
{
	if (!isActivated())
		return;

	// Cleanup queues...
	snd_seq_drop_input(m_pAlsaSeq);
	snd_seq_drop_output(m_pAlsaSeq);

	// Stop queue timer...
	snd_seq_stop_queue(m_pAlsaSeq, m_iAlsaQueue, NULL);

	// Shut-off all MIDI buses...
	for (qtractorBus *pBus = qtractorEngine::buses().first();
			pBus; pBus = pBus->next()) {
		qtractorMidiBus *pMidiBus
			= static_cast<qtractorMidiBus *> (pBus);
		if (pMidiBus)
			pMidiBus->shutOff();
	}
}


// Device engine deactivation method.
void qtractorMidiEngine::deactivate (void)
{
	// We're stopping now...
	setPlaying(false);

	// Stop our queue threads...
	m_pInputThread->setRunState(false);
	m_pOutputThread->setRunState(false);
	m_pOutputThread->sync();

	// Reset existing control buses...
	resetControlBus(qtractorBus::None);
}


// Device engine cleanup method.
void qtractorMidiEngine::clean (void)
{
	// Delete output thread...
	if (m_pOutputThread) {
		// Make it nicely...
		if (m_pOutputThread->isRunning()) {
		//	m_pOutputThread->terminate();
			m_pOutputThread->wait();
		}
		delete m_pOutputThread;
		m_pOutputThread = NULL;
		m_iTimeStart = 0;
		m_iTimeDelta = 0;
	}

	// Last but not least, delete input thread...
	if (m_pInputThread) {
		// Make it nicely...
		if (m_pInputThread->isRunning()) {
		//	m_pInputThread->terminate();
			m_pInputThread->wait();
		}
		delete m_pInputThread;
		m_pInputThread = NULL;
	}

	
	// Drop subscription stuff.
	if (m_pAlsaSubsSeq) {
		if (m_pAlsaNotifier) {
			delete m_pAlsaNotifier;
			m_pAlsaNotifier = NULL;
		}
		if (m_iAlsaSubsPort >= 0) {
			snd_seq_delete_simple_port(m_pAlsaSubsSeq, m_iAlsaSubsPort);
			m_iAlsaSubsPort = -1;
		}
		snd_seq_close(m_pAlsaSubsSeq);
		m_pAlsaSubsSeq = NULL;
	}

	// Drop everything else, finally.
	if (m_pAlsaSeq) {
		// And now, the sequencer queue and handle...
		snd_seq_free_queue(m_pAlsaSeq, m_iAlsaQueue);
		snd_seq_close(m_pAlsaSeq);
		m_iAlsaQueue  = -1;
		m_iAlsaClient = -1;
		m_pAlsaSeq    = NULL;
	}
}


// Special rewind method, for queue loop.
void qtractorMidiEngine::restartLoop (void)
{
	qtractorSession *pSession = session();
	if (pSession && pSession->isLooping()) {
		m_iTimeStart -= (long) pSession->tickFromFrame(
			pSession->loopEnd() - pSession->loopStart());
	}
}


// Immediate track mute.
void qtractorMidiEngine::trackMute ( qtractorTrack *pTrack, bool bMute )
{
#ifdef CONFIG_DEBUG
	fprintf(stderr, "qtractorMidiEngine::trackMute(%p, %d)\n", pTrack, bMute);
#endif

	unsigned long iFrame = session()->playHead();

	if (bMute) {
		// Remove all already enqueued events
		// for the given track and channel...
		snd_seq_remove_events_t *pre;
		snd_seq_remove_events_alloca(&pre);
		snd_seq_timestamp_t ts;
		long iTime = (long) session()->tickFromFrame(iFrame);
		ts.tick = (iTime > m_iTimeStart ? iTime - m_iTimeStart : 0);
		snd_seq_remove_events_set_time(pre, &ts);
		snd_seq_remove_events_set_tag(pre, pTrack->midiTag());
		snd_seq_remove_events_set_channel(pre, pTrack->midiChannel());
		snd_seq_remove_events_set_queue(pre, m_iAlsaQueue);
		snd_seq_remove_events_set_condition(pre, SND_SEQ_REMOVE_OUTPUT
			| SND_SEQ_REMOVE_TIME_AFTER | SND_SEQ_REMOVE_TIME_TICK
			| SND_SEQ_REMOVE_DEST_CHANNEL | SND_SEQ_REMOVE_IGNORE_OFF
			| SND_SEQ_REMOVE_TAG_MATCH);
		snd_seq_remove_events(m_pAlsaSeq, pre);
		// Immediate all current notes off.
		qtractorMidiBus *pMidiBus
			= static_cast<qtractorMidiBus *> (pTrack->outputBus());
		if (pMidiBus)
			pMidiBus->setController(pTrack->midiChannel(), ALL_NOTES_OFF);
		// Reset track monitor...
		qtractorMidiMonitor *pMidiMonitor
			= static_cast<qtractorMidiMonitor *> (pTrack->monitor());
		if (pMidiMonitor)
			pMidiMonitor->reset();
		// Done mute.
	} else {
		// Must redirect to MIDI ouput thread:
		// the immediate re-enqueueing of MIDI events.
		m_pOutputThread->trackSync(pTrack, iFrame);
		// Done unmute.
	}
}


// Event notifier widget settings.
void qtractorMidiEngine::setNotifyWidget ( QWidget *pNotifyWidget )
{
	m_pNotifyWidget = pNotifyWidget;
}

void qtractorMidiEngine::setNotifyMmcType ( QEvent::Type eNotifyMmcType )
{
	m_eNotifyMmcType = eNotifyMmcType;
}


QWidget *qtractorMidiEngine::notifyWidget (void) const
{
	return m_pNotifyWidget;
}

QEvent::Type qtractorMidiEngine::notifyMmcType (void) const
{
	return m_eNotifyMmcType;
}


// Control buses accessors.
qtractorMidiBus *qtractorMidiEngine::controlBus_in() const
{
	return m_pIControlBus;
}

qtractorMidiBus *qtractorMidiEngine::controlBus_out() const
{
	return m_pOControlBus;
}


// MMC dispatch special commands.
void qtractorMidiEngine::sendMmcLocate ( unsigned long iLocate ) const
{
	unsigned char data[6];

	data[0] = 0x01;
	data[1] = iLocate / (3600 * 30); iLocate -= (3600 * 30) * (int) data[1];
	data[2] = iLocate / (  60 * 30); iLocate -= (  60 * 30) * (int) data[2];
	data[3] = iLocate / (       30); iLocate -= (       30) * (int) data[3];
	data[4] = iLocate;
	data[5] = 0;

	sendMmcCommand(qtractorMmcEvent::LOCATE, data, sizeof(data));
}

void qtractorMidiEngine::sendMmcMaskedWrite ( qtractorMmcEvent::SubCommand scmd,
	int iTrack,	bool bOn ) const
{
	unsigned char data[4];
	int iMask = (1 << (iTrack < 2 ? iTrack + 5 : (iTrack - 2) % 7));

	data[0] = scmd;
	data[1] = (unsigned char) (iTrack < 2 ? 0 : 1 + (iTrack - 2) / 7);
	data[2] = (unsigned char) iMask;
	data[3] = (unsigned char) (bOn ? iMask : 0);

	sendMmcCommand(qtractorMmcEvent::MASKED_WRITE, data, sizeof(data));
}

void qtractorMidiEngine::sendMmcCommand ( qtractorMmcEvent::Command cmd,
	unsigned char *pMmcData, unsigned short iMmcData ) const
{
	// We surely need a output control bus...
	if (m_pOControlBus == NULL)
		return;

	// Build up the MMC sysex message...
	unsigned char *pSysex;
	unsigned short iSysex;

	iSysex = 6;
	if (pMmcData && iMmcData > 0)
		iSysex += 1 + iMmcData;
	pSysex = new unsigned char [iSysex];
	iSysex = 0;

	pSysex[iSysex++] = 0xf0;				// Sysex header.
	pSysex[iSysex++] = 0x7f;				// Realtime sysex.
	pSysex[iSysex++] = 0x7f;				// All-caller-id.
	pSysex[iSysex++] = 0x06;				// MMC command mode.
	pSysex[iSysex++] = (unsigned char) cmd;	// MMC command code.
	if (pMmcData && iMmcData > 0) {
		pSysex[iSysex++] = iMmcData;
		::memcpy(&pSysex[iSysex], pMmcData, iMmcData);
		iSysex += iMmcData;
	}
	pSysex[iSysex++] = 0xf7;				// Sysex trailer.

	// Send it out, now.
	m_pOControlBus->sendSysex(pSysex, iSysex);

	// Done.
	delete pSysex;
}


// Document element methods.
bool qtractorMidiEngine::loadElement ( qtractorSessionDocument *pDocument,
	QDomElement *pElement )
{
	qtractorEngine::clear();

	// Load session children...
	for (QDomNode nChild = pElement->firstChild();
			!nChild.isNull();
				nChild = nChild.nextSibling()) {

		// Convert node to element...
		QDomElement eChild = nChild.toElement();
		if (eChild.isNull())
			continue;

		if (eChild.tagName() == "midi-bus") {
			QString sBusName = eChild.attribute("name");
			qtractorMidiBus::BusMode busMode
				= pDocument->loadBusMode(eChild.attribute("mode"));
			qtractorMidiBus *pMidiBus
				= new qtractorMidiBus(this, sBusName, busMode);
			// Load bus properties...
			for (QDomNode nProp = eChild.firstChild();
					!nProp.isNull();
						nProp = nProp.nextSibling()) {
				// Convert node to element...
				QDomElement eProp = nProp.toElement();
				if (eProp.isNull())
					continue;
				// Load map elements (non-critical)...
				if (eProp.tagName() == "midi-map") {
					pMidiBus->loadMidiMap(pDocument, &eProp);
				} else if (eProp.tagName() == "input-gain") {
					if (pMidiBus->monitor_in())
						pMidiBus->monitor_in()->setGain(
							eProp.text().toFloat());
				} else if (eProp.tagName() == "input-panning") {
					if (pMidiBus->monitor_in())
						pMidiBus->monitor_in()->setPanning(
							eProp.text().toFloat());
				} else if (eProp.tagName() == "input-connects") {
					pMidiBus->loadConnects(
						pMidiBus->inputs(), pDocument, &eProp);
				} else if (eProp.tagName() == "output-gain") {
					if (pMidiBus->monitor_out())
						pMidiBus->monitor_out()->setGain(
							eProp.text().toFloat());
				} else if (eProp.tagName() == "output-panning") {
					if (pMidiBus->monitor_out())
						pMidiBus->monitor_out()->setPanning(
							eProp.text().toFloat());
				} else if (eProp.tagName() == "output-connects") {
					pMidiBus->loadConnects(
						pMidiBus->outputs(), pDocument, &eProp);
				}
			}
			qtractorMidiEngine::addBus(pMidiBus);
		}
	}

	return true;
}


bool qtractorMidiEngine::saveElement ( qtractorSessionDocument *pDocument,
	QDomElement *pElement )
{
	// Save MIDI buses...
	for (qtractorBus *pBus = qtractorEngine::buses().first();
			pBus; pBus = pBus->next()) {
		qtractorMidiBus *pMidiBus
			= static_cast<qtractorMidiBus *> (pBus);
		if (pMidiBus) {
			// Create the new MIDI bus element...
			QDomElement eMidiBus
				= pDocument->document()->createElement("midi-bus");
			eMidiBus.setAttribute("name",
				pMidiBus->busName());
			eMidiBus.setAttribute("mode",
				pDocument->saveBusMode(pMidiBus->busMode()));
			if (pMidiBus->busMode() & qtractorBus::Input) {
				pDocument->saveTextElement("input-gain",
					QString::number(pMidiBus->monitor_in()->gain()),
						&eMidiBus);
				pDocument->saveTextElement("input-panning",
					QString::number(pMidiBus->monitor_in()->panning()),
						&eMidiBus);
				QDomElement eMidiInputs
					= pDocument->document()->createElement("input-connects");
				qtractorBus::ConnectList inputs;
				pMidiBus->updateConnects(qtractorBus::Input, inputs);
				pMidiBus->saveConnects(inputs, pDocument, &eMidiInputs);
				eMidiBus.appendChild(eMidiInputs);
			}
			if (pMidiBus->busMode() & qtractorBus::Output) {
				pDocument->saveTextElement("output-gain",
					QString::number(pMidiBus->monitor_out()->gain()),
						&eMidiBus);
				pDocument->saveTextElement("output-panning",
					QString::number(pMidiBus->monitor_out()->panning()),
						&eMidiBus);
				QDomElement eMidiOutputs
					= pDocument->document()->createElement("output-connects");
				qtractorBus::ConnectList outputs;
				pMidiBus->updateConnects(qtractorBus::Output, outputs);
				pMidiBus->saveConnects(outputs, pDocument, &eMidiOutputs);
				eMidiBus.appendChild(eMidiOutputs);
			}
			// Create the map element...
			QDomElement eMidiMap
				= pDocument->document()->createElement("midi-map");
			pMidiBus->saveMidiMap(pDocument, &eMidiMap);
			// Add this clip...
			eMidiBus.appendChild(eMidiMap);
			pElement->appendChild(eMidiBus);
		}
	}

	return true;
}


//----------------------------------------------------------------------
// class qtractorMidiBus -- Managed ALSA sequencer port set
//

// Constructor.
qtractorMidiBus::qtractorMidiBus ( qtractorMidiEngine *pMidiEngine,
	const QString& sBusName, BusMode busMode )
	: qtractorBus(pMidiEngine, sBusName, busMode)
{
	m_iAlsaPort = -1;

	if (busMode & qtractorBus::Input)
		m_pIMidiMonitor = new qtractorMidiMonitor(pMidiEngine->session());
	else
		m_pIMidiMonitor = NULL;

	if (busMode & qtractorBus::Output)
		m_pOMidiMonitor = new qtractorMidiMonitor(pMidiEngine->session());
	else
		m_pOMidiMonitor = NULL;
}

// Destructor.
qtractorMidiBus::~qtractorMidiBus (void)
{
	close();

	if (m_pIMidiMonitor)
		delete m_pIMidiMonitor;
	if (m_pOMidiMonitor)
		delete m_pOMidiMonitor;
}


// ALSA sequencer port accessor.
int qtractorMidiBus::alsaPort (void) const
{
	return m_iAlsaPort;
}


// Register and pre-allocate bus port buffers.
bool qtractorMidiBus::open (void)
{
//	close();

	qtractorMidiEngine *pMidiEngine
		= static_cast<qtractorMidiEngine *> (engine());
	if (pMidiEngine == NULL)
		return false;
	if (pMidiEngine->alsaSeq() == NULL)
		return false;

	// The verry same port might be used for input and output...
	unsigned int flags = 0;

	if (busMode() & qtractorBus::Input)
		flags |= SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;
	if (busMode() & qtractorBus::Output)
		flags |= SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;

	m_iAlsaPort = snd_seq_create_simple_port(
		pMidiEngine->alsaSeq(), busName().toUtf8().constData(), flags,
		SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);

	if (m_iAlsaPort < 0)
		return false;

	// We want to know when the events get delivered to us...
	snd_seq_port_info_t *pinfo;
	snd_seq_port_info_alloca(&pinfo);

	if (snd_seq_get_port_info(pMidiEngine->alsaSeq(), m_iAlsaPort, pinfo) < 0)
		return false;

	snd_seq_port_info_set_timestamping(pinfo, 1);
	snd_seq_port_info_set_timestamp_queue(pinfo, pMidiEngine->alsaQueue());
	snd_seq_port_info_set_timestamp_real(pinfo, 0);	// MIDI ticks.

	if (snd_seq_set_port_info(pMidiEngine->alsaSeq(), m_iAlsaPort, pinfo) < 0)
		return false;

	// Done.
	return true;
}


// Unregister and post-free bus port buffers.
void qtractorMidiBus::close (void)
{
	qtractorMidiEngine *pMidiEngine
		= static_cast<qtractorMidiEngine *> (engine());
	if (pMidiEngine == NULL)
		return;
	if (pMidiEngine->alsaSeq() == NULL)
		return;

	shutOff(true);

	snd_seq_delete_simple_port(pMidiEngine->alsaSeq(), m_iAlsaPort);

	m_iAlsaPort = -1;
}


// Bus mode change event.
void qtractorMidiBus::updateBusMode (void)
{
	// Have a new/old input monitor?
	if (busMode() & qtractorBus::Input) {
		if (m_pIMidiMonitor == NULL)
			m_pIMidiMonitor = new qtractorMidiMonitor(engine()->session());
	} else if (m_pIMidiMonitor) {
		delete m_pIMidiMonitor;
		m_pIMidiMonitor = NULL;
	}

	// Have a new/old output monitor?
	if (busMode() & qtractorBus::Output) {
		if (m_pOMidiMonitor == NULL)
			m_pOMidiMonitor = new qtractorMidiMonitor(engine()->session());
	} else if (m_pOMidiMonitor) {
		delete m_pOMidiMonitor;
		m_pOMidiMonitor = NULL;
	}
}


// Shut-off everything out there.
void qtractorMidiBus::shutOff ( bool bClose ) const
{
	qtractorMidiEngine *pMidiEngine
		= static_cast<qtractorMidiEngine *> (engine());
	if (pMidiEngine == NULL)
		return;
	if (pMidiEngine->alsaSeq() == NULL)
		return;

#ifdef CONFIG_DEBUG
	fprintf(stderr, "qtractorMidiBus::shutOff(bClose=%d)\n", (int) bClose);
#endif

	QMap<unsigned short, Patch>::ConstIterator iter;
	for (iter = m_patches.begin(); iter != m_patches.end(); ++iter) {
		unsigned short iChannel = iter.key();
		setController(iChannel, ALL_SOUND_OFF);
		setController(iChannel, ALL_NOTES_OFF);
		if (bClose)
			setController(iChannel, ALL_CONTROLLERS_OFF);
	}
}


// Direct MIDI bank/program selection helper.
void qtractorMidiBus::setPatch ( unsigned short iChannel,
	const QString& sInstrumentName, int iBankSelMethod, int iBank, int iProg )
{
	// We always need our MIDI engine reference...
	qtractorMidiEngine *pMidiEngine
		= static_cast<qtractorMidiEngine *> (engine());
	if (pMidiEngine == NULL)
		return;

	// Don't do anything else if engine
	// has not been activated...
	if (pMidiEngine->alsaSeq() == NULL)
		return;

#ifdef CONFIG_DEBUG
	fprintf(stderr, "qtractorMidiBus::setPatch(%d, \"%s\", %d, %d, %d)\n",
		iChannel, sInstrumentName.toUtf8().constData(), iBankSelMethod, iBank, iProg);
#endif

	// Update patch mapping...
	if (!sInstrumentName.isEmpty()) {
		Patch& patch = m_patches[iChannel & 0x0f];
		patch.instrumentName = sInstrumentName;
		patch.bankSelMethod  = iBankSelMethod;
		patch.bank = iBank;
		patch.prog = iProg;
	}

	// Initialize sequencer event...
	snd_seq_event_t ev;
	snd_seq_ev_clear(&ev);

	// Addressing...
	snd_seq_ev_set_source(&ev, m_iAlsaPort);
	snd_seq_ev_set_subs(&ev);

	// The event will be direct...
	snd_seq_ev_set_direct(&ev);

	// Select Bank MSB.
	if (iBank >= 0 && (iBankSelMethod == 0 || iBankSelMethod == 1)) {
		ev.type = SND_SEQ_EVENT_CONTROLLER;
		ev.data.control.channel = iChannel;
		ev.data.control.param   = BANK_SELECT_MSB;
		ev.data.control.value   = (iBank & 0x3f80) >> 7;
		snd_seq_event_output(pMidiEngine->alsaSeq(), &ev);
	}

	// Select Bank LSB.
	if (iBank >= 0 && (iBankSelMethod == 0 || iBankSelMethod == 2)) {
		ev.type = SND_SEQ_EVENT_CONTROLLER;
		ev.data.control.channel = iChannel;
		ev.data.control.param   = BANK_SELECT_LSB;
		ev.data.control.value   = (iBank & 0x007f);
		snd_seq_event_output(pMidiEngine->alsaSeq(), &ev);
	}

	// Program change...
	ev.type = SND_SEQ_EVENT_PGMCHANGE;
	ev.data.control.channel = iChannel;
	ev.data.control.value   = iProg;
	snd_seq_event_output(pMidiEngine->alsaSeq(), &ev);

	pMidiEngine->flush();
}


// Direct MIDI controller helper.
void qtractorMidiBus::setController ( unsigned short iChannel,
	int iController, int iValue ) const
{
	// We always need our MIDI engine reference...
	qtractorMidiEngine *pMidiEngine
		= static_cast<qtractorMidiEngine *> (engine());
	if (pMidiEngine == NULL)
		return;

	// Don't do anything else if engine
	// has not been activated...
	if (pMidiEngine->alsaSeq() == NULL)
		return;

#ifdef CONFIG_DEBUG
	fprintf(stderr, "qtractorMidiBus::setController(%d, %d, %d)\n",
		iChannel, iController, iValue);
#endif

	// Initialize sequencer event...
	snd_seq_event_t ev;
	snd_seq_ev_clear(&ev);

	// Addressing...
	snd_seq_ev_set_source(&ev, m_iAlsaPort);
	snd_seq_ev_set_subs(&ev);

	// The event will be direct...
	snd_seq_ev_set_direct(&ev);

	// Set controller parameters...
	ev.type = SND_SEQ_EVENT_CONTROLLER;
	ev.data.control.channel = iChannel;
	ev.data.control.param   = iController;
	ev.data.control.value   = iValue;
	snd_seq_event_output(pMidiEngine->alsaSeq(), &ev);

	pMidiEngine->flush();
}


// Direct SysEx helper.
void qtractorMidiBus::sendSysex ( unsigned char *pSysex, unsigned int iSysex )
{
	// Yet again, we need our MIDI engine reference...
	qtractorMidiEngine *pMidiEngine
		= static_cast<qtractorMidiEngine *> (engine());
	if (pMidiEngine == NULL)
		return;

	// Don't do anything else if engine
	// has not been activated...
	if (pMidiEngine->alsaSeq() == NULL)
		return;

#ifdef CONFIG_DEBUG
	fprintf(stderr, "qtractorMidiBus::sendSysEx(%p, %u)", pSysex, iSysex);
	fprintf(stderr, " sysex {");
	for (unsigned int i = 0; i < iSysex; i++)
		fprintf(stderr, " %02x", pSysex[i]);
	fprintf(stderr, " }\n");
#endif

	// Initialize sequencer event...
	snd_seq_event_t ev;
	snd_seq_ev_clear(&ev);

	// Addressing...
	snd_seq_ev_set_source(&ev, m_iAlsaPort);
	snd_seq_ev_set_subs(&ev);

	// The event will be direct...
	snd_seq_ev_set_direct(&ev);

	// Just set SYSEX stuff and send it out..
	ev.type = SND_SEQ_EVENT_SYSEX;
	snd_seq_ev_set_sysex(&ev, iSysex, pSysex);
	snd_seq_event_output(pMidiEngine->alsaSeq(), &ev);

	pMidiEngine->flush();
}


// Virtual I/O bus-monitor accessors.
qtractorMonitor *qtractorMidiBus::monitor_in (void) const
{
	return midiMonitor_in();
}

qtractorMonitor *qtractorMidiBus::monitor_out (void) const
{
	return midiMonitor_out();
}


// MIDI I/O bus-monitor accessors.
qtractorMidiMonitor *qtractorMidiBus::midiMonitor_in (void) const
{
	return m_pIMidiMonitor;
}

qtractorMidiMonitor *qtractorMidiBus::midiMonitor_out (void) const
{
	return m_pOMidiMonitor;
}


// Retrive all current ALSA connections for a given bus mode interface;
// return the effective number of connection attempts...
int qtractorMidiBus::updateConnects ( qtractorBus::BusMode busMode,
	ConnectList& connects, bool bConnect )
{
	qtractorMidiEngine *pMidiEngine
		= static_cast<qtractorMidiEngine *> (engine());
	if (pMidiEngine == NULL)
		return 0;
	if (pMidiEngine->alsaSeq() == NULL)
		return 0;

	// Modes must match, at least...
	if ((busMode & qtractorMidiBus::busMode()) == 0)
		return 0;
	if (bConnect && connects.isEmpty())
		return 0;

	// Which kind of subscription?
	snd_seq_query_subs_type_t subs_type
		= (busMode == qtractorBus::Input ?
			SND_SEQ_QUERY_SUBS_WRITE : SND_SEQ_QUERY_SUBS_READ);

	snd_seq_query_subscribe_t *pAlsaSubs;
	snd_seq_addr_t seq_addr;
	
	snd_seq_query_subscribe_alloca(&pAlsaSubs);

	snd_seq_client_info_t *pClientInfo;
	snd_seq_port_info_t   *pPortInfo;

	snd_seq_client_info_alloca(&pClientInfo);
	snd_seq_port_info_alloca(&pPortInfo);

	ConnectItem item;
	item.index = 0;

	// Get port connections...
	snd_seq_query_subscribe_set_type(pAlsaSubs, subs_type);
	snd_seq_query_subscribe_set_index(pAlsaSubs, 0);
	seq_addr.client = pMidiEngine->alsaClient();
	seq_addr.port   = m_iAlsaPort;
	snd_seq_query_subscribe_set_root(pAlsaSubs, &seq_addr);
	while (snd_seq_query_port_subscribers(
			pMidiEngine->alsaSeq(), pAlsaSubs) >= 0) {
		seq_addr = *snd_seq_query_subscribe_get_addr(pAlsaSubs);
		snd_seq_get_any_client_info(
			pMidiEngine->alsaSeq(), seq_addr.client, pClientInfo);
		item.clientName  = QString::number(seq_addr.client);
		item.clientName += ':';
		item.clientName += snd_seq_client_info_get_name(pClientInfo);
		snd_seq_get_any_port_info(
			pMidiEngine->alsaSeq(), seq_addr.client, seq_addr.port, pPortInfo);
		item.portName  = QString::number(seq_addr.port);
		item.portName += ':';
		item.portName += snd_seq_port_info_get_name(pPortInfo);
		// Check if already in list/connected...
		ConnectItem *pItem = connects.findItem(item);
		if (pItem && bConnect) {
			int iItem = connects.indexOf(pItem);
			if (iItem >= 0) {
				connects.removeAt(iItem);
				delete pItem;
			}
		} else if (!bConnect)
			connects.append(new ConnectItem(item));
		// Fetch next connection...
		snd_seq_query_subscribe_set_index(pAlsaSubs,
			snd_seq_query_subscribe_get_index(pAlsaSubs) + 1);
	}

	// Shall we proceed for actual connections?
	if (!bConnect)
		return 0;

	snd_seq_port_subscribe_t *pPortSubs;
	snd_seq_port_subscribe_alloca(&pPortSubs);

	// For each (remaining) connection, try...
	int iUpdate = 0;
	QListIterator<ConnectItem *> iter(connects);
	while (iter.hasNext()) {
		ConnectItem *pItem = iter.next();
		int iAlsaClient = pItem->clientName.section(':', 0, 0).toInt();
		int iAlsaPort   = pItem->portName.section(':', 0, 0).toInt();
		// Mangle which is output and input...
		if (busMode == qtractorBus::Input) {
			seq_addr.client = iAlsaClient;
			seq_addr.port   = iAlsaPort;
			snd_seq_port_subscribe_set_sender(pPortSubs, &seq_addr);
			seq_addr.client = pMidiEngine->alsaClient();
			seq_addr.port   = m_iAlsaPort;
			snd_seq_port_subscribe_set_dest(pPortSubs, &seq_addr);
		} else {
			seq_addr.client = pMidiEngine->alsaClient();
			seq_addr.port   = m_iAlsaPort;
			snd_seq_port_subscribe_set_sender(pPortSubs, &seq_addr);
			seq_addr.client = iAlsaClient;
			seq_addr.port   = iAlsaPort;
			snd_seq_port_subscribe_set_dest(pPortSubs, &seq_addr);
		}
#ifdef CONFIG_DEBUG
		const QString sPortName	= QString::number(m_iAlsaPort) + ':' + busName();
		fprintf(stderr, "qtractorMidiBus::updateConnects(%p, %d): "
			"snd_seq_subscribe_port: [%d:%s] => [%d:%s]\n", this, (int) busMode,
				pMidiEngine->alsaClient(), sPortName.toUtf8().constData(),
				iAlsaClient, pItem->portName.toUtf8().constData());
#endif
		if (snd_seq_subscribe_port(pMidiEngine->alsaSeq(), pPortSubs) == 0) {
			int iItem = connects.indexOf(pItem);
			if (iItem >= 0) {
				connects.removeAt(iItem);
				delete pItem;
				iUpdate++;
			}
		}
	}

	// Done.
	return iUpdate;
}


// MIDI master volume.
void qtractorMidiBus::setMasterVolume ( float fVolume )
{
	unsigned char vol = (unsigned char) (int(127.0f * fVolume) & 0x7f);
	// Build Universal SysEx and let it go...
	static unsigned char aMasterVolSysex[]
		= { 0xf0, 0x7f, 0x7f, 0x04, 0x01, 0x00, 0x00, 0xf7 };
	// Set the course value right...
	aMasterVolSysex[6] = vol;
	sendSysex(aMasterVolSysex, sizeof(aMasterVolSysex));
}


// MIDI channel volume.
void qtractorMidiBus::setVolume ( unsigned short iChannel, float fVolume )
{
	unsigned char vol = (unsigned char) (int(127.0f * fVolume) & 0x7f);
	setController((unsigned short) iChannel, CHANNEL_VOLUME, vol);
}


// MIDI channel stereo panning.
void qtractorMidiBus::setPanning ( unsigned short iChannel, float fPanning )
{
	unsigned char pan = (int(63.0f * (1.0f + fPanning)) + 1) & 0x7f;
	setController(iChannel, CHANNEL_PANNING, pan);
}


// Document element methods.
bool qtractorMidiBus::loadMidiMap ( qtractorSessionDocument * /* pDocument */,
	QDomElement *pElement )
{
	m_patches.clear();

	// Load map items...
	for (QDomNode nChild = pElement->firstChild();
			!nChild.isNull();
				nChild = nChild.nextSibling()) {

		// Convert node to element...
		QDomElement eChild = nChild.toElement();
		if (eChild.isNull())
			continue;

		// Load map item...
		if (eChild.tagName() == "midi-patch") {
			unsigned short iChannel = eChild.attribute("channel").toUShort();
			Patch& patch = m_patches[iChannel & 0x0f];
			for (QDomNode nPatch = eChild.firstChild();
					!nPatch.isNull();
						nPatch = nPatch.nextSibling()) {
				// Convert patch node to element...
				QDomElement ePatch = nPatch.toElement();
				if (ePatch.isNull())
					continue;
				// Add this one to map...
				if (ePatch.tagName() == "midi-instrument")
					patch.instrumentName = ePatch.text();
				else
				if (ePatch.tagName() == "midi-bank-sel-method")
					patch.bankSelMethod = ePatch.text().toInt();
				else
				if (ePatch.tagName() == "midi-bank")
					patch.bank = ePatch.text().toInt();
				else
				if (ePatch.tagName() == "midi-program")
					patch.prog = ePatch.text().toInt();
			}
			// Rollback if instrument-patch is invalid...
			if (patch.instrumentName.isEmpty())
				m_patches.remove(iChannel & 0x0f);
		}
	}

	return true;
}


bool qtractorMidiBus::saveMidiMap ( qtractorSessionDocument *pDocument,
	QDomElement *pElement )
{
	// Save map items...
	QMap<unsigned short, Patch>::Iterator iter;
	for (iter = m_patches.begin(); iter != m_patches.end(); ++iter) {
		const Patch& patch = iter.value();
		QDomElement ePatch = pDocument->document()->createElement("midi-patch");
		ePatch.setAttribute("channel", QString::number(iter.key()));
		if (!patch.instrumentName.isEmpty()) {
			pDocument->saveTextElement("midi-instrument",
				patch.instrumentName, &ePatch);
		}
		if (patch.bankSelMethod >= 0) {
			pDocument->saveTextElement("midi-bank-sel-method",
				QString::number(patch.bankSelMethod), &ePatch);
		}
		if (patch.bank >= 0) {
			pDocument->saveTextElement("midi-bank",
				QString::number(patch.bank), &ePatch);
		}
		if (patch.prog >= 0) {
			pDocument->saveTextElement("midi-prog",
				QString::number(patch.prog), &ePatch);
		}
		pElement->appendChild(ePatch);
	}

	return true;
}


// end of qtractorMidiEngine.cpp
