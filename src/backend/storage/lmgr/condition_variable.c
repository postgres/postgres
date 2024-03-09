/*-------------------------------------------------------------------------
 *
 * condition_variable.c
 *	  Implementation of condition variables.  Condition variables provide
 *	  a way for one process to wait until a specific condition occurs,
 *	  without needing to know the specific identity of the process for
 *	  which they are waiting.  Waits for condition variables can be
 *	  interrupted, unlike LWLock waits.  Condition variables are safe
 *	  to use within dynamic shared memory segments.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/storage/lmgr/condition_variable.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "portability/instr_time.h"
#include "storage/condition_variable.h"
#include "storage/proc.h"
#include "storage/proclist.h"
#include "storage/spin.h"

/* Initially, we are not prepared to sleep on any condition variable. */
static ConditionVariable *cv_sleep_target = NULL;

/*
 * Initialize a condition variable.
 */
void
ConditionVariableInit(ConditionVariable *cv)
{
	SpinLockInit(&cv->mutex);
	proclist_init(&cv->wakeup);
}

/*
 * Prepare to wait on a given condition variable.
 *
 * This can optionally be called before entering a test/sleep loop.
 * Doing so is more efficient if we'll need to sleep at least once.
 * However, if the first test of the exit condition is likely to succeed,
 * it's more efficient to omit the ConditionVariablePrepareToSleep call.
 * See comments in ConditionVariableSleep for more detail.
 *
 * Caution: "before entering the loop" means you *must* test the exit
 * condition between calling ConditionVariablePrepareToSleep and calling
 * ConditionVariableSleep.  If that is inconvenient, omit calling
 * ConditionVariablePrepareToSleep.
 */
void
ConditionVariablePrepareToSleep(ConditionVariable *cv)
{
	int			pgprocno = MyProcNumber;

	/*
	 * If some other sleep is already prepared, cancel it; this is necessary
	 * because we have just one static variable tracking the prepared sleep,
	 * and also only one cvWaitLink in our PGPROC.  It's okay to do this
	 * because whenever control does return to the other test-and-sleep loop,
	 * its ConditionVariableSleep call will just re-establish that sleep as
	 * the prepared one.
	 */
	if (cv_sleep_target != NULL)
		ConditionVariableCancelSleep();

	/* Record the condition variable on which we will sleep. */
	cv_sleep_target = cv;

	/* Add myself to the wait queue. */
	SpinLockAcquire(&cv->mutex);
	proclist_push_tail(&cv->wakeup, pgprocno, cvWaitLink);
	SpinLockRelease(&cv->mutex);
}

/*
 * Wait for the given condition variable to be signaled.
 *
 * This should be called in a predicate loop that tests for a specific exit
 * condition and otherwise sleeps, like so:
 *
 *	 ConditionVariablePrepareToSleep(cv);  // optional
 *	 while (condition for which we are waiting is not true)
 *		 ConditionVariableSleep(cv, wait_event_info);
 *	 ConditionVariableCancelSleep();
 *
 * wait_event_info should be a value from one of the WaitEventXXX enums
 * defined in pgstat.h.  This controls the contents of pg_stat_activity's
 * wait_event_type and wait_event columns while waiting.
 */
void
ConditionVariableSleep(ConditionVariable *cv, uint32 wait_event_info)
{
	(void) ConditionVariableTimedSleep(cv, -1 /* no timeout */ ,
									   wait_event_info);
}

/*
 * Wait for a condition variable to be signaled or a timeout to be reached.
 *
 * The "timeout" is given in milliseconds.
 *
 * Returns true when timeout expires, otherwise returns false.
 *
 * See ConditionVariableSleep() for general usage.
 */
bool
ConditionVariableTimedSleep(ConditionVariable *cv, long timeout,
							uint32 wait_event_info)
{
	long		cur_timeout = -1;
	instr_time	start_time;
	instr_time	cur_time;
	int			wait_events;

	/*
	 * If the caller didn't prepare to sleep explicitly, then do so now and
	 * return immediately.  The caller's predicate loop should immediately
	 * call again if its exit condition is not yet met.  This will result in
	 * the exit condition being tested twice before we first sleep.  The extra
	 * test can be prevented by calling ConditionVariablePrepareToSleep(cv)
	 * first.  Whether it's worth doing that depends on whether you expect the
	 * exit condition to be met initially, in which case skipping the prepare
	 * is recommended because it avoids manipulations of the wait list, or not
	 * met initially, in which case preparing first is better because it
	 * avoids one extra test of the exit condition.
	 *
	 * If we are currently prepared to sleep on some other CV, we just cancel
	 * that and prepare this one; see ConditionVariablePrepareToSleep.
	 */
	if (cv_sleep_target != cv)
	{
		ConditionVariablePrepareToSleep(cv);
		return false;
	}

	/*
	 * Record the current time so that we can calculate the remaining timeout
	 * if we are woken up spuriously.
	 */
	if (timeout >= 0)
	{
		INSTR_TIME_SET_CURRENT(start_time);
		Assert(timeout >= 0 && timeout <= INT_MAX);
		cur_timeout = timeout;
		wait_events = WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH;
	}
	else
		wait_events = WL_LATCH_SET | WL_EXIT_ON_PM_DEATH;

	while (true)
	{
		bool		done = false;

		/*
		 * Wait for latch to be set.  (If we're awakened for some other
		 * reason, the code below will cope anyway.)
		 */
		(void) WaitLatch(MyLatch, wait_events, cur_timeout, wait_event_info);

		/* Reset latch before examining the state of the wait list. */
		ResetLatch(MyLatch);

		/*
		 * If this process has been taken out of the wait list, then we know
		 * that it has been signaled by ConditionVariableSignal (or
		 * ConditionVariableBroadcast), so we should return to the caller. But
		 * that doesn't guarantee that the exit condition is met, only that we
		 * ought to check it.  So we must put the process back into the wait
		 * list, to ensure we don't miss any additional wakeup occurring while
		 * the caller checks its exit condition.  We can take ourselves out of
		 * the wait list only when the caller calls
		 * ConditionVariableCancelSleep.
		 *
		 * If we're still in the wait list, then the latch must have been set
		 * by something other than ConditionVariableSignal; though we don't
		 * guarantee not to return spuriously, we'll avoid this obvious case.
		 */
		SpinLockAcquire(&cv->mutex);
		if (!proclist_contains(&cv->wakeup, MyProcNumber, cvWaitLink))
		{
			done = true;
			proclist_push_tail(&cv->wakeup, MyProcNumber, cvWaitLink);
		}
		SpinLockRelease(&cv->mutex);

		/*
		 * Check for interrupts, and return spuriously if that caused the
		 * current sleep target to change (meaning that interrupt handler code
		 * waited for a different condition variable).
		 */
		CHECK_FOR_INTERRUPTS();
		if (cv != cv_sleep_target)
			done = true;

		/* We were signaled, so return */
		if (done)
			return false;

		/* If we're not done, update cur_timeout for next iteration */
		if (timeout >= 0)
		{
			INSTR_TIME_SET_CURRENT(cur_time);
			INSTR_TIME_SUBTRACT(cur_time, start_time);
			cur_timeout = timeout - (long) INSTR_TIME_GET_MILLISEC(cur_time);

			/* Have we crossed the timeout threshold? */
			if (cur_timeout <= 0)
				return true;
		}
	}
}

/*
 * Cancel any pending sleep operation.
 *
 * We just need to remove ourselves from the wait queue of any condition
 * variable for which we have previously prepared a sleep.
 *
 * Do nothing if nothing is pending; this allows this function to be called
 * during transaction abort to clean up any unfinished CV sleep.
 *
 * Return true if we've been signaled.
 */
bool
ConditionVariableCancelSleep(void)
{
	ConditionVariable *cv = cv_sleep_target;
	bool		signaled = false;

	if (cv == NULL)
		return false;

	SpinLockAcquire(&cv->mutex);
	if (proclist_contains(&cv->wakeup, MyProcNumber, cvWaitLink))
		proclist_delete(&cv->wakeup, MyProcNumber, cvWaitLink);
	else
		signaled = true;
	SpinLockRelease(&cv->mutex);

	cv_sleep_target = NULL;

	return signaled;
}

/*
 * Wake up the oldest process sleeping on the CV, if there is any.
 *
 * Note: it's difficult to tell whether this has any real effect: we know
 * whether we took an entry off the list, but the entry might only be a
 * sentinel.  Hence, think twice before proposing that this should return
 * a flag telling whether it woke somebody.
 */
void
ConditionVariableSignal(ConditionVariable *cv)
{
	PGPROC	   *proc = NULL;

	/* Remove the first process from the wakeup queue (if any). */
	SpinLockAcquire(&cv->mutex);
	if (!proclist_is_empty(&cv->wakeup))
		proc = proclist_pop_head_node(&cv->wakeup, cvWaitLink);
	SpinLockRelease(&cv->mutex);

	/* If we found someone sleeping, set their latch to wake them up. */
	if (proc != NULL)
		SetLatch(&proc->procLatch);
}

/*
 * Wake up all processes sleeping on the given CV.
 *
 * This guarantees to wake all processes that were sleeping on the CV
 * at time of call, but processes that add themselves to the list mid-call
 * will typically not get awakened.
 */
void
ConditionVariableBroadcast(ConditionVariable *cv)
{
	int			pgprocno = MyProcNumber;
	PGPROC	   *proc = NULL;
	bool		have_sentinel = false;

	/*
	 * In some use-cases, it is common for awakened processes to immediately
	 * re-queue themselves.  If we just naively try to reduce the wakeup list
	 * to empty, we'll get into a potentially-indefinite loop against such a
	 * process.  The semantics we really want are just to be sure that we have
	 * wakened all processes that were in the list at entry.  We can use our
	 * own cvWaitLink as a sentinel to detect when we've finished.
	 *
	 * A seeming flaw in this approach is that someone else might signal the
	 * CV and in doing so remove our sentinel entry.  But that's fine: since
	 * CV waiters are always added and removed in order, that must mean that
	 * every previous waiter has been wakened, so we're done.  We'll get an
	 * extra "set" on our latch from the someone else's signal, which is
	 * slightly inefficient but harmless.
	 *
	 * We can't insert our cvWaitLink as a sentinel if it's already in use in
	 * some other proclist.  While that's not expected to be true for typical
	 * uses of this function, we can deal with it by simply canceling any
	 * prepared CV sleep.  The next call to ConditionVariableSleep will take
	 * care of re-establishing the lost state.
	 */
	if (cv_sleep_target != NULL)
		ConditionVariableCancelSleep();

	/*
	 * Inspect the state of the queue.  If it's empty, we have nothing to do.
	 * If there's exactly one entry, we need only remove and signal that
	 * entry.  Otherwise, remove the first entry and insert our sentinel.
	 */
	SpinLockAcquire(&cv->mutex);
	/* While we're here, let's assert we're not in the list. */
	Assert(!proclist_contains(&cv->wakeup, pgprocno, cvWaitLink));

	if (!proclist_is_empty(&cv->wakeup))
	{
		proc = proclist_pop_head_node(&cv->wakeup, cvWaitLink);
		if (!proclist_is_empty(&cv->wakeup))
		{
			proclist_push_tail(&cv->wakeup, pgprocno, cvWaitLink);
			have_sentinel = true;
		}
	}
	SpinLockRelease(&cv->mutex);

	/* Awaken first waiter, if there was one. */
	if (proc != NULL)
		SetLatch(&proc->procLatch);

	while (have_sentinel)
	{
		/*
		 * Each time through the loop, remove the first wakeup list entry, and
		 * signal it unless it's our sentinel.  Repeat as long as the sentinel
		 * remains in the list.
		 *
		 * Notice that if someone else removes our sentinel, we will waken one
		 * additional process before exiting.  That's intentional, because if
		 * someone else signals the CV, they may be intending to waken some
		 * third process that added itself to the list after we added the
		 * sentinel.  Better to give a spurious wakeup (which should be
		 * harmless beyond wasting some cycles) than to lose a wakeup.
		 */
		proc = NULL;
		SpinLockAcquire(&cv->mutex);
		if (!proclist_is_empty(&cv->wakeup))
			proc = proclist_pop_head_node(&cv->wakeup, cvWaitLink);
		have_sentinel = proclist_contains(&cv->wakeup, pgprocno, cvWaitLink);
		SpinLockRelease(&cv->mutex);

		if (proc != NULL && proc != MyProc)
			SetLatch(&proc->procLatch);
	}
}
