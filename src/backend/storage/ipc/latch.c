/*-------------------------------------------------------------------------
 *
 * latch.c
 *	  Routines for inter-process latches
 *
 * The latch interface is a reliable replacement for the common pattern of
 * using pg_usleep() or select() to wait until a signal arrives, where the
 * signal handler sets a flag variable.  See latch.h for more information
 * on how to use them.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/latch.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/latch.h"
#include "storage/waiteventset.h"
#include "utils/resowner.h"

/* A common WaitEventSet used to implement WaitLatch() */
static WaitEventSet *LatchWaitSet;

/* The positions of the latch and PM death events in LatchWaitSet */
#define LatchWaitSetLatchPos 0
#define LatchWaitSetPostmasterDeathPos 1

void
InitializeLatchWaitSet(void)
{
	int			latch_pos PG_USED_FOR_ASSERTS_ONLY;

	Assert(LatchWaitSet == NULL);

	/* Set up the WaitEventSet used by WaitLatch(). */
	LatchWaitSet = CreateWaitEventSet(NULL, 2);
	latch_pos = AddWaitEventToSet(LatchWaitSet, WL_LATCH_SET, PGINVALID_SOCKET,
								  MyLatch, NULL);
	Assert(latch_pos == LatchWaitSetLatchPos);

	/*
	 * WaitLatch will modify this to WL_EXIT_ON_PM_DEATH or
	 * WL_POSTMASTER_DEATH on each call.
	 */
	if (IsUnderPostmaster)
	{
		latch_pos = AddWaitEventToSet(LatchWaitSet, WL_EXIT_ON_PM_DEATH,
									  PGINVALID_SOCKET, NULL, NULL);
		Assert(latch_pos == LatchWaitSetPostmasterDeathPos);
	}
}

/*
 * Initialize a process-local latch.
 */
void
InitLatch(Latch *latch)
{
	latch->is_set = false;
	latch->maybe_sleeping = false;
	latch->owner_pid = MyProcPid;
	latch->is_shared = false;

#ifdef WIN32
	latch->event = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (latch->event == NULL)
		elog(ERROR, "CreateEvent failed: error code %lu", GetLastError());
#endif							/* WIN32 */
}

/*
 * Initialize a shared latch that can be set from other processes. The latch
 * is initially owned by no-one; use OwnLatch to associate it with the
 * current process.
 *
 * InitSharedLatch needs to be called in postmaster before forking child
 * processes, usually right after allocating the shared memory block
 * containing the latch with ShmemInitStruct. (The Unix implementation
 * doesn't actually require that, but the Windows one does.) Because of
 * this restriction, we have no concurrency issues to worry about here.
 *
 * Note that other handles created in this module are never marked as
 * inheritable.  Thus we do not need to worry about cleaning up child
 * process references to postmaster-private latches or WaitEventSets.
 */
void
InitSharedLatch(Latch *latch)
{
#ifdef WIN32
	SECURITY_ATTRIBUTES sa;

	/*
	 * Set up security attributes to specify that the events are inherited.
	 */
	ZeroMemory(&sa, sizeof(sa));
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;

	latch->event = CreateEvent(&sa, TRUE, FALSE, NULL);
	if (latch->event == NULL)
		elog(ERROR, "CreateEvent failed: error code %lu", GetLastError());
#endif

	latch->is_set = false;
	latch->maybe_sleeping = false;
	latch->owner_pid = 0;
	latch->is_shared = true;
}

/*
 * Associate a shared latch with the current process, allowing it to
 * wait on the latch.
 *
 * Although there is a sanity check for latch-already-owned, we don't do
 * any sort of locking here, meaning that we could fail to detect the error
 * if two processes try to own the same latch at about the same time.  If
 * there is any risk of that, caller must provide an interlock to prevent it.
 */
void
OwnLatch(Latch *latch)
{
	int			owner_pid;

	/* Sanity checks */
	Assert(latch->is_shared);

	owner_pid = latch->owner_pid;
	if (owner_pid != 0)
		elog(PANIC, "latch already owned by PID %d", owner_pid);

	latch->owner_pid = MyProcPid;
}

/*
 * Disown a shared latch currently owned by the current process.
 */
void
DisownLatch(Latch *latch)
{
	Assert(latch->is_shared);
	Assert(latch->owner_pid == MyProcPid);

	latch->owner_pid = 0;
}

/*
 * Wait for a given latch to be set, or for postmaster death, or until timeout
 * is exceeded. 'wakeEvents' is a bitmask that specifies which of those events
 * to wait for. If the latch is already set (and WL_LATCH_SET is given), the
 * function returns immediately.
 *
 * The "timeout" is given in milliseconds. It must be >= 0 if WL_TIMEOUT flag
 * is given.  Although it is declared as "long", we don't actually support
 * timeouts longer than INT_MAX milliseconds.  Note that some extra overhead
 * is incurred when WL_TIMEOUT is given, so avoid using a timeout if possible.
 *
 * The latch must be owned by the current process, ie. it must be a
 * process-local latch initialized with InitLatch, or a shared latch
 * associated with the current process by calling OwnLatch.
 *
 * Returns bit mask indicating which condition(s) caused the wake-up. Note
 * that if multiple wake-up conditions are true, there is no guarantee that
 * we return all of them in one call, but we will return at least one.
 */
int
WaitLatch(Latch *latch, int wakeEvents, long timeout,
		  uint32 wait_event_info)
{
	WaitEvent	event;

	/* Postmaster-managed callers must handle postmaster death somehow. */
	Assert(!IsUnderPostmaster ||
		   (wakeEvents & WL_EXIT_ON_PM_DEATH) ||
		   (wakeEvents & WL_POSTMASTER_DEATH));

	/*
	 * Some callers may have a latch other than MyLatch, or no latch at all,
	 * or want to handle postmaster death differently.  It's cheap to assign
	 * those, so just do it every time.
	 */
	if (!(wakeEvents & WL_LATCH_SET))
		latch = NULL;
	ModifyWaitEvent(LatchWaitSet, LatchWaitSetLatchPos, WL_LATCH_SET, latch);
	ModifyWaitEvent(LatchWaitSet, LatchWaitSetPostmasterDeathPos,
					(wakeEvents & (WL_EXIT_ON_PM_DEATH | WL_POSTMASTER_DEATH)),
					NULL);

	if (WaitEventSetWait(LatchWaitSet,
						 (wakeEvents & WL_TIMEOUT) ? timeout : -1,
						 &event, 1,
						 wait_event_info) == 0)
		return WL_TIMEOUT;
	else
		return event.events;
}

/*
 * Like WaitLatch, but with an extra socket argument for WL_SOCKET_*
 * conditions.
 *
 * When waiting on a socket, EOF and error conditions always cause the socket
 * to be reported as readable/writable/connected, so that the caller can deal
 * with the condition.
 *
 * wakeEvents must include either WL_EXIT_ON_PM_DEATH for automatic exit
 * if the postmaster dies or WL_POSTMASTER_DEATH for a flag set in the
 * return value if the postmaster dies.  The latter is useful for rare cases
 * where some behavior other than immediate exit is needed.
 *
 * NB: These days this is just a wrapper around the WaitEventSet API. When
 * using a latch very frequently, consider creating a longer living
 * WaitEventSet instead; that's more efficient.
 */
int
WaitLatchOrSocket(Latch *latch, int wakeEvents, pgsocket sock,
				  long timeout, uint32 wait_event_info)
{
	int			ret = 0;
	int			rc;
	WaitEvent	event;
	WaitEventSet *set = CreateWaitEventSet(CurrentResourceOwner, 3);

	if (wakeEvents & WL_TIMEOUT)
		Assert(timeout >= 0);
	else
		timeout = -1;

	if (wakeEvents & WL_LATCH_SET)
		AddWaitEventToSet(set, WL_LATCH_SET, PGINVALID_SOCKET,
						  latch, NULL);

	/* Postmaster-managed callers must handle postmaster death somehow. */
	Assert(!IsUnderPostmaster ||
		   (wakeEvents & WL_EXIT_ON_PM_DEATH) ||
		   (wakeEvents & WL_POSTMASTER_DEATH));

	if ((wakeEvents & WL_POSTMASTER_DEATH) && IsUnderPostmaster)
		AddWaitEventToSet(set, WL_POSTMASTER_DEATH, PGINVALID_SOCKET,
						  NULL, NULL);

	if ((wakeEvents & WL_EXIT_ON_PM_DEATH) && IsUnderPostmaster)
		AddWaitEventToSet(set, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET,
						  NULL, NULL);

	if (wakeEvents & WL_SOCKET_MASK)
	{
		int			ev;

		ev = wakeEvents & WL_SOCKET_MASK;
		AddWaitEventToSet(set, ev, sock, NULL, NULL);
	}

	rc = WaitEventSetWait(set, timeout, &event, 1, wait_event_info);

	if (rc == 0)
		ret |= WL_TIMEOUT;
	else
	{
		ret |= event.events & (WL_LATCH_SET |
							   WL_POSTMASTER_DEATH |
							   WL_SOCKET_MASK);
	}

	FreeWaitEventSet(set);

	return ret;
}

/*
 * Sets a latch and wakes up anyone waiting on it.
 *
 * This is cheap if the latch is already set, otherwise not so much.
 *
 * NB: when calling this in a signal handler, be sure to save and restore
 * errno around it.  (That's standard practice in most signal handlers, of
 * course, but we used to omit it in handlers that only set a flag.)
 *
 * NB: this function is called from critical sections and signal handlers so
 * throwing an error is not a good idea.
 */
void
SetLatch(Latch *latch)
{
#ifndef WIN32
	pid_t		owner_pid;
#else
	HANDLE		handle;
#endif

	/*
	 * The memory barrier has to be placed here to ensure that any flag
	 * variables possibly changed by this process have been flushed to main
	 * memory, before we check/set is_set.
	 */
	pg_memory_barrier();

	/* Quick exit if already set */
	if (latch->is_set)
		return;

	latch->is_set = true;

	pg_memory_barrier();
	if (!latch->maybe_sleeping)
		return;

#ifndef WIN32

	/*
	 * See if anyone's waiting for the latch. It can be the current process if
	 * we're in a signal handler. We use the self-pipe or SIGURG to ourselves
	 * to wake up WaitEventSetWaitBlock() without races in that case. If it's
	 * another process, send a signal.
	 *
	 * Fetch owner_pid only once, in case the latch is concurrently getting
	 * owned or disowned. XXX: This assumes that pid_t is atomic, which isn't
	 * guaranteed to be true! In practice, the effective range of pid_t fits
	 * in a 32 bit integer, and so should be atomic. In the worst case, we
	 * might end up signaling the wrong process. Even then, you're very
	 * unlucky if a process with that bogus pid exists and belongs to
	 * Postgres; and PG database processes should handle excess SIGUSR1
	 * interrupts without a problem anyhow.
	 *
	 * Another sort of race condition that's possible here is for a new
	 * process to own the latch immediately after we look, so we don't signal
	 * it. This is okay so long as all callers of ResetLatch/WaitLatch follow
	 * the standard coding convention of waiting at the bottom of their loops,
	 * not the top, so that they'll correctly process latch-setting events
	 * that happen before they enter the loop.
	 */
	owner_pid = latch->owner_pid;
	if (owner_pid == 0)
		return;
	else if (owner_pid == MyProcPid)
		WakeupMyProc();
	else
		WakeupOtherProc(owner_pid);

#else

	/*
	 * See if anyone's waiting for the latch. It can be the current process if
	 * we're in a signal handler.
	 *
	 * Use a local variable here just in case somebody changes the event field
	 * concurrently (which really should not happen).
	 */
	handle = latch->event;
	if (handle)
	{
		SetEvent(handle);

		/*
		 * Note that we silently ignore any errors. We might be in a signal
		 * handler or other critical path where it's not safe to call elog().
		 */
	}
#endif
}

/*
 * Clear the latch. Calling WaitLatch after this will sleep, unless
 * the latch is set again before the WaitLatch call.
 */
void
ResetLatch(Latch *latch)
{
	/* Only the owner should reset the latch */
	Assert(latch->owner_pid == MyProcPid);
	Assert(latch->maybe_sleeping == false);

	latch->is_set = false;

	/*
	 * Ensure that the write to is_set gets flushed to main memory before we
	 * examine any flag variables.  Otherwise a concurrent SetLatch might
	 * falsely conclude that it needn't signal us, even though we have missed
	 * seeing some flag updates that SetLatch was supposed to inform us of.
	 */
	pg_memory_barrier();
}
