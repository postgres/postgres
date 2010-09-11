/*-------------------------------------------------------------------------
 *
 * win32_latch.c
 *	  Windows implementation of latches.
 *
 * The Windows implementation uses Windows events. See unix_latch.c for
 * information on usage.
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/port/win32_latch.c,v 1.1 2010/09/11 15:48:04 heikki Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include "miscadmin.h"
#include "replication/walsender.h"
#include "storage/latch.h"
#include "storage/shmem.h"
#include "storage/spin.h"

/*
 * Shared latches are implemented with Windows events that are shared by
 * all postmaster child processes. At postmaster startup we create enough
 * Event objects, and mark them as inheritable so that they are accessible
 * in child processes. The handles are stored in sharedHandles.
 */
typedef struct
{
	slock_t		mutex;			/* protects all the other fields */

	int			maxhandles;		/* number of shared handles created */
	int			nfreehandles;	/* number of free handles in array */
	HANDLE		handles[1];		/* free handles, variable length */
} SharedEventHandles;

static SharedEventHandles *sharedHandles;

void
InitLatch(volatile Latch *latch)
{
	latch->event = CreateEvent(NULL, TRUE, FALSE, NULL);
	latch->is_shared = false;
	latch->is_set = false;
}

void
InitSharedLatch(volatile Latch *latch)
{
	latch->is_shared = true;
	latch->is_set = false;
	latch->event = NULL;
}

void
OwnLatch(volatile Latch *latch)
{
	HANDLE event;

	/* Sanity checks */
	Assert(latch->is_shared);
	if (latch->event != 0)
		elog(ERROR, "latch already owned");

	/* Reserve an event handle from the shared handles array */
	SpinLockAcquire(&sharedHandles->mutex);
	if (sharedHandles->nfreehandles <= 0)
	{
		SpinLockRelease(&sharedHandles->mutex);
		elog(ERROR, "out of shared event objects");
	}
	sharedHandles->nfreehandles--;
	event = sharedHandles->handles[sharedHandles->nfreehandles];
	SpinLockRelease(&sharedHandles->mutex);

	latch->event = event;
}

void
DisownLatch(volatile Latch *latch)
{
	Assert(latch->is_shared);
	Assert(latch->event != NULL);

	/* Put the event handle back to the pool */
	SpinLockAcquire(&sharedHandles->mutex);
	if (sharedHandles->nfreehandles >= sharedHandles->maxhandles)
	{
		SpinLockRelease(&sharedHandles->mutex);
		elog(PANIC, "too many free event handles");
	}
	sharedHandles->handles[sharedHandles->nfreehandles] = latch->event;
	sharedHandles->nfreehandles++;
	SpinLockRelease(&sharedHandles->mutex);

	latch->event = NULL;
}

bool
WaitLatch(volatile Latch *latch, long timeout)
{
	return WaitLatchOrSocket(latch, PGINVALID_SOCKET, timeout) > 0;
}

int
WaitLatchOrSocket(volatile Latch *latch, SOCKET sock, long timeout)
{
	DWORD		rc;
	HANDLE		events[3];
	HANDLE		latchevent;
	HANDLE		sockevent;
	int			numevents;
	int			result = 0;

	latchevent = latch->event;

	events[0] = latchevent;
	events[1] = pgwin32_signal_event;
	numevents = 2;
	if (sock != PGINVALID_SOCKET)
	{
		sockevent = WSACreateEvent();
		WSAEventSelect(sock, sockevent, FD_READ);
		events[numevents++] = sockevent;
	}

	for (;;)
	{
		/*
		 * Reset the event, and check if the latch is set already. If someone
		 * sets the latch between this and the WaitForMultipleObjects() call
		 * below, the setter will set the event and WaitForMultipleObjects()
		 * will return immediately.
		 */
		if (!ResetEvent(latchevent))
			elog(ERROR, "ResetEvent failed: error code %d", (int) GetLastError());
		if (latch->is_set)
		{
			result = 1;
			break;
		}

		rc = WaitForMultipleObjects(numevents, events, FALSE,
								(timeout >= 0) ? (timeout / 1000) : INFINITE);
		if (rc == WAIT_FAILED)
			elog(ERROR, "WaitForMultipleObjects() failed: error code %d", (int) GetLastError());
		else if (rc == WAIT_TIMEOUT)
		{
			result = 0;
			break;
		}
		else if (rc == WAIT_OBJECT_0 + 1)
			pgwin32_dispatch_queued_signals();
		else if (rc == WAIT_OBJECT_0 + 2)
		{
			Assert(sock != PGINVALID_SOCKET);
			result = 2;
			break;
		}
		else if (rc != WAIT_OBJECT_0)
			elog(ERROR, "unexpected return code from WaitForMultipleObjects(): %d", rc);
	}

	/* Clean up the handle we created for the socket */
		if (sock != PGINVALID_SOCKET)
	{
		WSAEventSelect(sock, sockevent, 0);
		WSACloseEvent(sockevent);
	}

	return result;
}

void
SetLatch(volatile Latch *latch)
{
	HANDLE handle;

	/* Quick exit if already set */
	if (latch->is_set)
		return;

	latch->is_set = true;

	/*
	 * See if anyone's waiting for the latch. It can be the current process
	 * if we're in a signal handler. Use a local variable here in case the
	 * latch is just disowned between the test and the SetEvent call, and
	 * event field set to NULL.
	 *
	 * Fetch handle field only once, in case the owner simultaneously
	 * disowns the latch and clears handle. This assumes that HANDLE is
	 * atomic, which isn't guaranteed to be true! In practice, it should be,
	 * and in the worst case we end up calling SetEvent with a bogus handle,
	 * and SetEvent will return an error with no harm done.
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
}

void
ResetLatch(volatile Latch *latch)
{
	latch->is_set = false;
}

/*
 * Number of shared latches, used to allocate the right number of shared
 * Event handles at postmaster startup. You must update this if you
 * introduce a new shared latch!
 */
static int
NumSharedLatches(void)
{
	int numLatches = 0;

	/* Each walsender needs one latch */
	numLatches += max_wal_senders;

	return numLatches;
}

/*
 * LatchShmemSize
 *		Compute space needed for latch's shared memory
 */
Size
LatchShmemSize(void)
{
	return offsetof(SharedEventHandles, handles) +
		NumSharedLatches() * sizeof(HANDLE);
}

/*
 * LatchShmemInit
 *		Allocate and initialize shared memory needed for latches
 */
void
LatchShmemInit(void)
{
	Size		size = LatchShmemSize();
	bool		found;

	sharedHandles = ShmemInitStruct("SharedEventHandles", size, &found);

	/* If we're first, initialize the struct and allocate handles */
	if (!found)
	{
		int i;
		SECURITY_ATTRIBUTES sa;

		/*
		 * Set up security attributes to specify that the events are
		 * inherited.
		 */
		ZeroMemory(&sa, sizeof(sa));
		sa.nLength = sizeof(sa);
		sa.bInheritHandle = TRUE;

		SpinLockInit(&sharedHandles->mutex);
		sharedHandles->maxhandles = NumSharedLatches();
		sharedHandles->nfreehandles = sharedHandles->maxhandles;
		for (i = 0; i < sharedHandles->maxhandles; i++)
		{
			sharedHandles->handles[i] = CreateEvent(&sa, TRUE, FALSE, NULL);
			if (sharedHandles->handles[i] == NULL)
				elog(ERROR, "CreateEvent failed: error code %d", (int) GetLastError());
		}
	}
}
