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
	 * If first time through in this process, create a WaitEventSet, which
	 * we'll reuse for all condition variable sleeps.
	 */
	if (cv_wait_event_set == NULL)
	{
		WaitEventSet *new_event_set;

		new_event_set = CreateWaitEventSet(TopMemoryContext, 1);
		AddWaitEventToSet(new_event_set, WL_LATCH_SET, PGINVALID_SOCKET,
						  MyLatch, NULL);
		/* Don't set cv_wait_event_set until we have a correct WES. */
		cv_wait_event_set = new_event_set;
	}

	/*
	 * It's not legal to prepare a sleep until the previous sleep has been
	 * completed or canceled.
	 */
	Assert(cv_sleep_target == NULL);

	/* Record the condition variable on which we will sleep. */
	cv_sleep_target = cv;

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
	int			pgprocno = MyProc->pgprocno;
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
	{
		SetLatch(&proc->procLatch);
		++nwoken;
	}

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
		{
			SetLatch(&proc->procLatch);
			++nwoken;
		}
	}

	return nwoken;
}
