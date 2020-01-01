/*-------------------------------------------------------------------------
 *
 * win32_sema.c
 *	  Microsoft Windows Win32 Semaphores Emulation
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/port/win32_sema.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/pg_sema.h"

static HANDLE *mySemSet;		/* IDs of sema sets acquired so far */
static int	numSems;			/* number of sema sets acquired so far */
static int	maxSems;			/* allocated size of mySemaSet array */

static void ReleaseSemaphores(int code, Datum arg);


/*
 * Report amount of shared memory needed for semaphores
 */
Size
PGSemaphoreShmemSize(int maxSemas)
{
	/* No shared memory needed on Windows */
	return 0;
}

/*
 * PGReserveSemaphores --- initialize semaphore support
 *
 * In the Win32 implementation, we acquire semaphores on-demand; the
 * maxSemas parameter is just used to size the array that keeps track of
 * acquired semas for subsequent releasing.  We use anonymous semaphores
 * so the semaphores are automatically freed when the last referencing
 * process exits.
 */
void
PGReserveSemaphores(int maxSemas)
{
	mySemSet = (HANDLE *) malloc(maxSemas * sizeof(HANDLE));
	if (mySemSet == NULL)
		elog(PANIC, "out of memory");
	numSems = 0;
	maxSems = maxSemas;

	on_shmem_exit(ReleaseSemaphores, 0);
}

/*
 * Release semaphores at shutdown or shmem reinitialization
 *
 * (called as an on_shmem_exit callback, hence funny argument list)
 */
static void
ReleaseSemaphores(int code, Datum arg)
{
	int			i;

	for (i = 0; i < numSems; i++)
		CloseHandle(mySemSet[i]);
	free(mySemSet);
}

/*
 * PGSemaphoreCreate
 *
 * Allocate a PGSemaphore structure with initial count 1
 */
PGSemaphore
PGSemaphoreCreate(void)
{
	HANDLE		cur_handle;
	SECURITY_ATTRIBUTES sec_attrs;

	/* Can't do this in a backend, because static state is postmaster's */
	Assert(!IsUnderPostmaster);

	if (numSems >= maxSems)
		elog(PANIC, "too many semaphores created");

	ZeroMemory(&sec_attrs, sizeof(sec_attrs));
	sec_attrs.nLength = sizeof(sec_attrs);
	sec_attrs.lpSecurityDescriptor = NULL;
	sec_attrs.bInheritHandle = TRUE;

	/* We don't need a named semaphore */
	cur_handle = CreateSemaphore(&sec_attrs, 1, 32767, NULL);
	if (cur_handle)
	{
		/* Successfully done */
		mySemSet[numSems++] = cur_handle;
	}
	else
		ereport(PANIC,
				(errmsg("could not create semaphore: error code %lu",
						GetLastError())));

	return (PGSemaphore) cur_handle;
}

/*
 * PGSemaphoreReset
 *
 * Reset a previously-initialized PGSemaphore to have count 0
 */
void
PGSemaphoreReset(PGSemaphore sema)
{
	/*
	 * There's no direct API for this in Win32, so we have to ratchet the
	 * semaphore down to 0 with repeated trylock's.
	 */
	while (PGSemaphoreTryLock(sema))
		 /* loop */ ;
}

/*
 * PGSemaphoreLock
 *
 * Lock a semaphore (decrement count), blocking if count would be < 0.
 */
void
PGSemaphoreLock(PGSemaphore sema)
{
	HANDLE		wh[2];
	bool		done = false;

	/*
	 * Note: pgwin32_signal_event should be first to ensure that it will be
	 * reported when multiple events are set.  We want to guarantee that
	 * pending signals are serviced.
	 */
	wh[0] = pgwin32_signal_event;
	wh[1] = sema;

	/*
	 * As in other implementations of PGSemaphoreLock, we need to check for
	 * cancel/die interrupts each time through the loop.  But here, there is
	 * no hidden magic about whether the syscall will internally service a
	 * signal --- we do that ourselves.
	 */
	while (!done)
	{
		DWORD		rc;

		CHECK_FOR_INTERRUPTS();

		rc = WaitForMultipleObjectsEx(2, wh, FALSE, INFINITE, TRUE);
		switch (rc)
		{
			case WAIT_OBJECT_0:
				/* Signal event is set - we have a signal to deliver */
				pgwin32_dispatch_queued_signals();
				break;
			case WAIT_OBJECT_0 + 1:
				/* We got it! */
				done = true;
				break;
			case WAIT_IO_COMPLETION:

				/*
				 * The system interrupted the wait to execute an I/O
				 * completion routine or asynchronous procedure call in this
				 * thread.  PostgreSQL does not provoke either of these, but
				 * atypical loaded DLLs or even other processes might do so.
				 * Now, resume waiting.
				 */
				break;
			case WAIT_FAILED:
				ereport(FATAL,
						(errmsg("could not lock semaphore: error code %lu",
								GetLastError())));
				break;
			default:
				elog(FATAL, "unexpected return code from WaitForMultipleObjectsEx(): %lu", rc);
				break;
		}
	}
}

/*
 * PGSemaphoreUnlock
 *
 * Unlock a semaphore (increment count)
 */
void
PGSemaphoreUnlock(PGSemaphore sema)
{
	if (!ReleaseSemaphore(sema, 1, NULL))
		ereport(FATAL,
				(errmsg("could not unlock semaphore: error code %lu",
						GetLastError())));
}

/*
 * PGSemaphoreTryLock
 *
 * Lock a semaphore only if able to do so without blocking
 */
bool
PGSemaphoreTryLock(PGSemaphore sema)
{
	DWORD		ret;

	ret = WaitForSingleObject(sema, 0);

	if (ret == WAIT_OBJECT_0)
	{
		/* We got it! */
		return true;
	}
	else if (ret == WAIT_TIMEOUT)
	{
		/* Can't get it */
		errno = EAGAIN;
		return false;
	}

	/* Otherwise we are in trouble */
	ereport(FATAL,
			(errmsg("could not try-lock semaphore: error code %lu",
					GetLastError())));

	/* keep compiler quiet */
	return false;
}
