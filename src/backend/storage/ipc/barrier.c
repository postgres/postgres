/*-------------------------------------------------------------------------
 *
 * barrier.c
 *	  Barriers for synchronizing cooperating processes.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * From Wikipedia[1]: "In parallel computing, a barrier is a type of
 * synchronization method.  A barrier for a group of threads or processes in
 * the source code means any thread/process must stop at this point and cannot
 * proceed until all other threads/processes reach this barrier."
 *
 * This implementation of barriers allows for static sets of participants
 * known up front, or dynamic sets of participants which processes can join or
 * leave at any time.  In the dynamic case, a phase number can be used to
 * track progress through a parallel algorithm, and may be necessary to
 * synchronize with the current phase of a multi-phase algorithm when a new
 * participant joins.  In the static case, the phase number is used
 * internally, but it isn't strictly necessary for client code to access it
 * because the phase can only advance when the declared number of participants
 * reaches the barrier, so client code should be in no doubt about the current
 * phase of computation at all times.
 *
 * Consider a parallel algorithm that involves separate phases of computation
 * A, B and C where the output of each phase is needed before the next phase
 * can begin.
 *
 * In the case of a static barrier initialized with 4 participants, each
 * participant works on phase A, then calls BarrierArriveAndWait to wait until
 * all 4 participants have reached that point.  When BarrierArriveAndWait
 * returns control, each participant can work on B, and so on.  Because the
 * barrier knows how many participants to expect, the phases of computation
 * don't need labels or numbers, since each process's program counter implies
 * the current phase.  Even if some of the processes are slow to start up and
 * begin running phase A, the other participants are expecting them and will
 * patiently wait at the barrier.  The code could be written as follows:
 *
 *     perform_a();
 *     BarrierArriveAndWait(&barrier, ...);
 *     perform_b();
 *     BarrierArriveAndWait(&barrier, ...);
 *     perform_c();
 *     BarrierArriveAndWait(&barrier, ...);
 *
 * If the number of participants is not known up front, then a dynamic barrier
 * is needed and the number should be set to zero at initialization.  New
 * complications arise because the number necessarily changes over time as
 * participants attach and detach, and therefore phases B, C or even the end
 * of processing may be reached before any given participant has started
 * running and attached.  Therefore the client code must perform an initial
 * test of the phase number after attaching, because it needs to find out
 * which phase of the algorithm has been reached by any participants that are
 * already attached in order to synchronize with that work.  Once the program
 * counter or some other representation of current progress is synchronized
 * with the barrier's phase, normal control flow can be used just as in the
 * static case.  Our example could be written using a switch statement with
 * cases that fall-through, as follows:
 *
 *     phase = BarrierAttach(&barrier);
 *     switch (phase)
 *     {
 *     case PHASE_A:
 *         perform_a();
 *         BarrierArriveAndWait(&barrier, ...);
 *     case PHASE_B:
 *         perform_b();
 *         BarrierArriveAndWait(&barrier, ...);
 *     case PHASE_C:
 *         perform_c();
 *         BarrierArriveAndWait(&barrier, ...);
 *     }
 *     BarrierDetach(&barrier);
 *
 * Static barriers behave similarly to POSIX's pthread_barrier_t.  Dynamic
 * barriers behave similarly to Java's java.util.concurrent.Phaser.
 *
 * [1] https://en.wikipedia.org/wiki/Barrier_(computer_science)
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/barrier.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "storage/barrier.h"

static inline bool BarrierDetachImpl(Barrier *barrier, bool arrive);

/*
 * Initialize this barrier.  To use a static party size, provide the number of
 * participants to wait for at each phase indicating that that number of
 * backends is implicitly attached.  To use a dynamic party size, specify zero
 * here and then use BarrierAttach() and
 * BarrierDetach()/BarrierArriveAndDetach() to register and deregister
 * participants explicitly.
 */
void
BarrierInit(Barrier *barrier, int participants)
{
	SpinLockInit(&barrier->mutex);
	barrier->participants = participants;
	barrier->arrived = 0;
	barrier->phase = 0;
	barrier->elected = 0;
	barrier->static_party = participants > 0;
	ConditionVariableInit(&barrier->condition_variable);
}

/*
 * Arrive at this barrier, wait for all other attached participants to arrive
 * too and then return.  Increments the current phase.  The caller must be
 * attached.
 *
 * While waiting, pg_stat_activity shows a wait_event_type and wait_event
 * controlled by the wait_event_info passed in, which should be a value from
 * one of the WaitEventXXX enums defined in pgstat.h.
 *
 * Return true in one arbitrarily chosen participant.  Return false in all
 * others.  The return code can be used to elect one participant to execute a
 * phase of work that must be done serially while other participants wait.
 */
bool
BarrierArriveAndWait(Barrier *barrier, uint32 wait_event_info)
{
	bool		release = false;
	bool		elected;
	int			start_phase;
	int			next_phase;

	SpinLockAcquire(&barrier->mutex);
	start_phase = barrier->phase;
	next_phase = start_phase + 1;
	++barrier->arrived;
	if (barrier->arrived == barrier->participants)
	{
		release = true;
		barrier->arrived = 0;
		barrier->phase = next_phase;
		barrier->elected = next_phase;
	}
	SpinLockRelease(&barrier->mutex);

	/*
	 * If we were the last expected participant to arrive, we can release our
	 * peers and return true to indicate that this backend has been elected to
	 * perform any serial work.
	 */
	if (release)
	{
		ConditionVariableBroadcast(&barrier->condition_variable);

		return true;
	}

	/*
	 * Otherwise we have to wait for the last participant to arrive and
	 * advance the phase.
	 */
	elected = false;
	ConditionVariablePrepareToSleep(&barrier->condition_variable);
	for (;;)
	{
		/*
		 * We know that phase must either be start_phase, indicating that we
		 * need to keep waiting, or next_phase, indicating that the last
		 * participant that we were waiting for has either arrived or detached
		 * so that the next phase has begun.  The phase cannot advance any
		 * further than that without this backend's participation, because
		 * this backend is attached.
		 */
		SpinLockAcquire(&barrier->mutex);
		Assert(barrier->phase == start_phase || barrier->phase == next_phase);
		release = barrier->phase == next_phase;
		if (release && barrier->elected != next_phase)
		{
			/*
			 * Usually the backend that arrives last and releases the other
			 * backends is elected to return true (see above), so that it can
			 * begin processing serial work while it has a CPU timeslice.
			 * However, if the barrier advanced because someone detached, then
			 * one of the backends that is awoken will need to be elected.
			 */
			barrier->elected = barrier->phase;
			elected = true;
		}
		SpinLockRelease(&barrier->mutex);
		if (release)
			break;
		ConditionVariableSleep(&barrier->condition_variable, wait_event_info);
	}
	ConditionVariableCancelSleep();

	return elected;
}

/*
 * Arrive at this barrier, but detach rather than waiting.  Returns true if
 * the caller was the last to detach.
 */
bool
BarrierArriveAndDetach(Barrier *barrier)
{
	return BarrierDetachImpl(barrier, true);
}

/*
 * Arrive at a barrier, and detach all but the last to arrive.  Returns true if
 * the caller was the last to arrive, and is therefore still attached.
 */
bool
BarrierArriveAndDetachExceptLast(Barrier *barrier)
{
	SpinLockAcquire(&barrier->mutex);
	if (barrier->participants > 1)
	{
		--barrier->participants;
		SpinLockRelease(&barrier->mutex);

		return false;
	}
	Assert(barrier->participants == 1);
	++barrier->phase;
	SpinLockRelease(&barrier->mutex);

	return true;
}

/*
 * Attach to a barrier.  All waiting participants will now wait for this
 * participant to call BarrierArriveAndWait(), BarrierDetach() or
 * BarrierArriveAndDetach().  Return the current phase.
 */
int
BarrierAttach(Barrier *barrier)
{
	int			phase;

	Assert(!barrier->static_party);

	SpinLockAcquire(&barrier->mutex);
	++barrier->participants;
	phase = barrier->phase;
	SpinLockRelease(&barrier->mutex);

	return phase;
}

/*
 * Detach from a barrier.  This may release other waiters from
 * BarrierArriveAndWait() and advance the phase if they were only waiting for
 * this backend.  Return true if this participant was the last to detach.
 */
bool
BarrierDetach(Barrier *barrier)
{
	return BarrierDetachImpl(barrier, false);
}

/*
 * Return the current phase of a barrier.  The caller must be attached.
 */
int
BarrierPhase(Barrier *barrier)
{
	/*
	 * It is OK to read barrier->phase without locking, because it can't
	 * change without us (we are attached to it), and we executed a memory
	 * barrier when we either attached or participated in changing it last
	 * time.
	 */
	return barrier->phase;
}

/*
 * Return an instantaneous snapshot of the number of participants currently
 * attached to this barrier.  For debugging purposes only.
 */
int
BarrierParticipants(Barrier *barrier)
{
	int			participants;

	SpinLockAcquire(&barrier->mutex);
	participants = barrier->participants;
	SpinLockRelease(&barrier->mutex);

	return participants;
}

/*
 * Detach from a barrier.  If 'arrive' is true then also increment the phase
 * if there are no other participants.  If there are other participants
 * waiting, then the phase will be advanced and they'll be released if they
 * were only waiting for the caller.  Return true if this participant was the
 * last to detach.
 */
static inline bool
BarrierDetachImpl(Barrier *barrier, bool arrive)
{
	bool		release;
	bool		last;

	Assert(!barrier->static_party);

	SpinLockAcquire(&barrier->mutex);
	Assert(barrier->participants > 0);
	--barrier->participants;

	/*
	 * If any other participants are waiting and we were the last participant
	 * waited for, release them.  If no other participants are waiting, but
	 * this is a BarrierArriveAndDetach() call, then advance the phase too.
	 */
	if ((arrive || barrier->participants > 0) &&
		barrier->arrived == barrier->participants)
	{
		release = true;
		barrier->arrived = 0;
		++barrier->phase;
	}
	else
		release = false;

	last = barrier->participants == 0;
	SpinLockRelease(&barrier->mutex);

	if (release)
		ConditionVariableBroadcast(&barrier->condition_variable);

	return last;
}
