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
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/storage/lmgr/condition_variable.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "storage/condition_variable.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/proclist.h"
#include "storage/spin.h"
#include "utils/memutils.h"

/* Initially, we are not prepared to sleep on any condition variable. */
static ConditionVariable *cv_sleep_target = NULL;

/* Reusable WaitEventSet. */
static WaitEventSet *cv_wait_event_set = NULL;

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
 * Prepare to wait on a given condition variable.  This can optionally be
 * called before entering a test/sleep loop.  Alternatively, the call to
 * ConditionVariablePrepareToSleep can be omitted.  The only advantage of
 * calling ConditionVariablePrepareToSleep is that it avoids an initial
 * double-test of the user's predicate in the case that we need to wait.
 */
void
ConditionVariablePrepareToSleep(ConditionVariable *cv)
{
	int			pgprocno = MyProc->pgprocno;

	/*
	 * It's not legal to prepare a sleep until the previous sleep has been
	 * completed or canceled.
	 */
	Assert(cv_sleep_target == NULL);

	/* Record the condition variable on which we will sleep. */
	cv_sleep_target = cv;

	/* Create a reusable WaitEventSet. */
	if (cv_wait_event_set == NULL)
	{
		cv_wait_event_set = CreateWaitEventSet(TopMemoryContext, 1);
		AddWaitEventToSet(cv_wait_event_set, WL_LATCH_SET, PGINVALID_SOCKET,
						  MyLatch, NULL);
	}

	/*
	 * Reset my latch before adding myself to the queue and before entering
	 * the caller's predicate loop.
	 */
	ResetLatch(MyLatch);

	/* Add myself to the wait queue. */
	SpinLockAcquire(&cv->mutex);
	if (!proclist_contains(&cv->wakeup, pgprocno, cvWaitLink))
		proclist_push_tail(&cv->wakeup, pgprocno, cvWaitLink);
	SpinLockRelease(&cv->mutex);
}

/*--------------------------------------------------------------------------
 * Wait for the given condition variable to be signaled.  This should be
 * called in a predicate loop that tests for a specific exit condition and
 * otherwise sleeps, like so:
 *
 *	 ConditionVariablePrepareToSleep(cv); [optional]
 *	 while (condition for which we are waiting is not true)
 *		 ConditionVariableSleep(cv, wait_event_info);
 *	 ConditionVariableCancelSleep();
 *
 * Supply a value from one of the WaitEventXXX enums defined in pgstat.h to
 * control the contents of pg_stat_activity's wait_event_type and wait_event
 * columns while waiting.
 *-------------------------------------------------------------------------*/
void
ConditionVariableSleep(ConditionVariable *cv, uint32 wait_event_info)
{
	WaitEvent	event;
	bool		done = false;

	/*
	 * If the caller didn't prepare to sleep explicitly, then do so now and
	 * return immediately.  The caller's predicate loop should immediately
	 * call again if its exit condition is not yet met.  This initial spurious
	 * return can be avoided by calling ConditionVariablePrepareToSleep(cv)
	 * first.  Whether it's worth doing that depends on whether you expect the
	 * condition to be met initially, in which case skipping the prepare
	 * allows you to skip manipulation of the wait list, or not met initially,
	 * in which case preparing first allows you to skip a spurious test of the
	 * caller's exit condition.
	 */
	if (cv_sleep_target == NULL)
	{
		ConditionVariablePrepareToSleep(cv);
		return;
	}

	/* Any earlier condition variable sleep must have been canceled. */
	Assert(cv_sleep_target == cv);

	while (!done)
	{
		CHECK_FOR_INTERRUPTS();

		/*
		 * Wait for latch to be set.  We don't care about the result because
		 * our contract permits spurious returns.
		 */
		WaitEventSetWait(cv_wait_event_set, -1, &event, 1, wait_event_info);

		/* Reset latch before testing whether we can return. */
		ResetLatch(MyLatch);

		/*
		 * If this process has been taken out of the wait list, then we know
		 * that is has been signaled by ConditionVariableSignal.  We put it
		 * back into the wait list, so we don't miss any further signals while
		 * the caller's loop checks its condition.  If it hasn't been taken
		 * out of the wait list, then the latch must have been set by
		 * something other than ConditionVariableSignal; though we don't
		 * guarantee not to return spuriously, we'll avoid these obvious
		 * cases.
		 */
		SpinLockAcquire(&cv->mutex);
		if (!proclist_contains(&cv->wakeup, MyProc->pgprocno, cvWaitLink))
		{
			done = true;
			proclist_push_tail(&cv->wakeup, MyProc->pgprocno, cvWaitLink);
		}
		SpinLockRelease(&cv->mutex);
	}
}

/*
 * Cancel any pending sleep operation.  We just need to remove ourselves
 * from the wait queue of any condition variable for which we have previously
 * prepared a sleep.
 */
void
ConditionVariableCancelSleep(void)
{
	ConditionVariable *cv = cv_sleep_target;

	if (cv == NULL)
		return;

	SpinLockAcquire(&cv->mutex);
	if (proclist_contains(&cv->wakeup, MyProc->pgprocno, cvWaitLink))
		proclist_delete(&cv->wakeup, MyProc->pgprocno, cvWaitLink);
	SpinLockRelease(&cv->mutex);

	cv_sleep_target = NULL;
}

/*
 * Wake up one sleeping process, assuming there is at least one.
 *
 * The return value indicates whether or not we woke somebody up.
 */
bool
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
	{
		SetLatch(&proc->procLatch);
		return true;
	}

	/* No sleeping processes. */
	return false;
}

/*
 * Wake up all sleeping processes.
 *
 * The return value indicates the number of processes we woke.
 */
int
ConditionVariableBroadcast(ConditionVariable *cv)
{
	int			nwoken = 0;

	/*
	 * Let's just do this the dumbest way possible.  We could try to dequeue
	 * all the sleepers at once to save spinlock cycles, but it's a bit hard
	 * to get that right in the face of possible sleep cancelations, and we
	 * don't want to loop holding the mutex.
	 */
	while (ConditionVariableSignal(cv))
		++nwoken;

	return nwoken;
}
