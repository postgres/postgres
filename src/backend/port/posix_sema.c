/*-------------------------------------------------------------------------
 *
 * posix_sema.c
 *	  Implement PGSemaphores using POSIX semaphore facilities
 *
 * We use the unnamed style of POSIX semaphore (the kind made with
 * sem_init).  We used to support the kind made with sem_open, but
 * that has several disadvantages, and there are no longer any
 * platforms where it is a preferable choice.
 *
 * In this implementation, typedef PGSemaphore is equivalent to "sem_t *".
 * The sem_t structs live in an array in shared memory.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/port/posix_sema.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <semaphore.h>

#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/pg_sema.h"
#include "storage/shmem.h"


typedef union SemTPadded
{
	sem_t		pgsem;
	char		pad[PG_CACHE_LINE_SIZE];
} SemTPadded;

/* typedef PGSemaphore is equivalent to pointer to sem_t */
typedef struct PGSemaphoreData
{
	SemTPadded	sem_padded;
} PGSemaphoreData;

#define PG_SEM_REF(x)	(&(x)->sem_padded.pgsem)


static PGSemaphore sharedSemas; /* array of PGSemaphoreData in shared memory */
static int	numSems;			/* number of semas acquired so far */
static int	maxSems;			/* allocated size of above array */


static void ReleaseSemaphores(int status, Datum arg);


/*
 * PosixSemaphoreCreate
 *
 * Attempt to create a new unnamed semaphore.
 */
static void
PosixSemaphoreCreate(sem_t *sem)
{
	if (sem_init(sem, 1, 1) < 0)
		elog(FATAL, "sem_init failed: %m");
}


/*
 * PosixSemaphoreKill	- removes a semaphore
 */
static void
PosixSemaphoreKill(sem_t *sem)
{
	if (sem_destroy(sem) < 0)
		elog(LOG, "sem_destroy failed: %m");
}


/*
 * Request shared memory needed for semaphores
 */
void
PGSemaphoreShmemRequest(int maxSemas)
{
	/* Need a PGSemaphoreData per semaphore */
	ShmemRequestStruct(.name = "Semaphores",
					   .size = mul_size(maxSemas, sizeof(PGSemaphoreData)),
					   .ptr = (void **) &sharedSemas,
		);
}

/*
 * PGSemaphoreInit --- initialize semaphore support
 *
 * This is called during postmaster start or shared memory reinitialization.
 * It should do whatever is needed to be able to support up to maxSemas
 * subsequent PGSemaphoreCreate calls.  Also, if any system resources
 * are acquired here or in PGSemaphoreCreate, register an on_shmem_exit
 * callback to release them.
 *
 * In the Posix implementation, we acquire semaphores on-demand; the
 * maxSemas parameter is just used to size the arrays.  We keep track
 * of them using an array of PGSemaphoreData structs in shared memory.
 * (In the event that a failing backend scribbles on that array, the
 * postmaster's subsequent sem_destroy() calls might fail, but with
 * luck the consequences are no worse than some leaked resources.)
 */
void
PGSemaphoreInit(int maxSemas)
{
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
ReleaseSemaphores(int status, Datum arg)
{
	for (int i = 0; i < numSems; i++)
		PosixSemaphoreKill(PG_SEM_REF(sharedSemas + i));
}

/*
 * PGSemaphoreCreate
 *
 * Allocate a PGSemaphore structure with initial count 1
 */
PGSemaphore
PGSemaphoreCreate(void)
{
	PGSemaphore sema;
	sem_t	   *newsem;

	/* Can't do this in a backend, because static state is postmaster's */
	Assert(!IsUnderPostmaster);

	if (numSems >= maxSems)
		elog(PANIC, "too many semaphores created");

	sema = &sharedSemas[numSems];
	newsem = PG_SEM_REF(sema);
	PosixSemaphoreCreate(newsem);

	numSems++;

	return sema;
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
	 * There's no direct API for this in POSIX, so we have to ratchet the
	 * semaphore down to 0 with repeated trywait's.
	 */
	for (;;)
	{
		if (sem_trywait(PG_SEM_REF(sema)) < 0)
		{
			if (errno == EAGAIN || errno == EDEADLK)
				break;			/* got it down to 0 */
			if (errno == EINTR)
				continue;		/* can this happen? */
			elog(FATAL, "sem_trywait failed: %m");
		}
	}
}

/*
 * PGSemaphoreLock
 *
 * Lock a semaphore (decrement count), blocking if count would be < 0
 */
void
PGSemaphoreLock(PGSemaphore sema)
{
	int			errStatus;

	/* See notes in sysv_sema.c's implementation of PGSemaphoreLock. */
	do
	{
		errStatus = sem_wait(PG_SEM_REF(sema));
	} while (errStatus < 0 && errno == EINTR);

	if (errStatus < 0)
		elog(FATAL, "sem_wait failed: %m");
}

/*
 * PGSemaphoreUnlock
 *
 * Unlock a semaphore (increment count)
 */
void
PGSemaphoreUnlock(PGSemaphore sema)
{
	int			errStatus;

	/*
	 * Note: if errStatus is -1 and errno == EINTR then it means we returned
	 * from the operation prematurely because we were sent a signal.  So we
	 * try and unlock the semaphore again. Not clear this can really happen,
	 * but might as well cope.
	 */
	do
	{
		errStatus = sem_post(PG_SEM_REF(sema));
	} while (errStatus < 0 && errno == EINTR);

	if (errStatus < 0)
		elog(FATAL, "sem_post failed: %m");
}

/*
 * PGSemaphoreTryLock
 *
 * Lock a semaphore only if able to do so without blocking
 */
bool
PGSemaphoreTryLock(PGSemaphore sema)
{
	int			errStatus;

	/*
	 * Note: if errStatus is -1 and errno == EINTR then it means we returned
	 * from the operation prematurely because we were sent a signal.  So we
	 * try and lock the semaphore again.
	 */
	do
	{
		errStatus = sem_trywait(PG_SEM_REF(sema));
	} while (errStatus < 0 && errno == EINTR);

	if (errStatus < 0)
	{
		if (errno == EAGAIN || errno == EDEADLK)
			return false;		/* failed to lock it */
		/* Otherwise we got trouble */
		elog(FATAL, "sem_trywait failed: %m");
	}

	return true;
}
