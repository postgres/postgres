/*-------------------------------------------------------------------------
 *
 * posix_sema.c
 *	  Implement PGSemaphores using POSIX semaphore facilities
 *
 * We prefer the unnamed style of POSIX semaphore (the kind made with
 * sem_init).  We can cope with the kind made with sem_open, however.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/port/posix_sema.c,v 1.20 2008/01/26 19:55:08 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/pg_sema.h"


#ifdef USE_NAMED_POSIX_SEMAPHORES
/* PGSemaphore is pointer to pointer to sem_t */
#define PG_SEM_REF(x)	(*(x))
#else
/* PGSemaphore is pointer to sem_t */
#define PG_SEM_REF(x)	(x)
#endif


#define IPCProtection	(0600)	/* access/modify by user only */

static sem_t **mySemPointers;	/* keep track of created semaphores */
static int	numSems;			/* number of semas acquired so far */
static int	maxSems;			/* allocated size of mySemaPointers array */
static int	nextSemKey;			/* next name to try */


static void ReleaseSemaphores(int status, Datum arg);


#ifdef USE_NAMED_POSIX_SEMAPHORES

/*
 * PosixSemaphoreCreate
 *
 * Attempt to create a new named semaphore.
 *
 * If we fail with a failure code other than collision-with-existing-sema,
 * print out an error and abort.  Other types of errors suggest nonrecoverable
 * problems.
 */
static sem_t *
PosixSemaphoreCreate(void)
{
	int			semKey;
	char		semname[64];
	sem_t	   *mySem;

	for (;;)
	{
		semKey = nextSemKey++;

		snprintf(semname, sizeof(semname), "/pgsql-%d", semKey);

		mySem = sem_open(semname, O_CREAT | O_EXCL,
						 (mode_t) IPCProtection, (unsigned) 1);

#ifdef SEM_FAILED
		if (mySem != (sem_t *) SEM_FAILED)
			break;
#else
		if (mySem != (sem_t *) (-1))
			break;
#endif

		/* Loop if error indicates a collision */
		if (errno == EEXIST || errno == EACCES || errno == EINTR)
			continue;

		/*
		 * Else complain and abort
		 */
		elog(FATAL, "sem_open(\"%s\") failed: %m", semname);
	}

	/*
	 * Unlink the semaphore immediately, so it can't be accessed externally.
	 * This also ensures that it will go away if we crash.
	 */
	sem_unlink(semname);

	return mySem;
}
#else							/* !USE_NAMED_POSIX_SEMAPHORES */

/*
 * PosixSemaphoreCreate
 *
 * Attempt to create a new unnamed semaphore.
 */
static void
PosixSemaphoreCreate(sem_t * sem)
{
	if (sem_init(sem, 1, 1) < 0)
		elog(FATAL, "sem_init failed: %m");
}
#endif   /* USE_NAMED_POSIX_SEMAPHORES */


/*
 * PosixSemaphoreKill	- removes a semaphore
 */
static void
PosixSemaphoreKill(sem_t * sem)
{
#ifdef USE_NAMED_POSIX_SEMAPHORES
	/* Got to use sem_close for named semaphores */
	if (sem_close(sem) < 0)
		elog(LOG, "sem_close failed: %m");
#else
	/* Got to use sem_destroy for unnamed semaphores */
	if (sem_destroy(sem) < 0)
		elog(LOG, "sem_destroy failed: %m");
#endif
}


/*
 * PGReserveSemaphores --- initialize semaphore support
 *
 * This is called during postmaster start or shared memory reinitialization.
 * It should do whatever is needed to be able to support up to maxSemas
 * subsequent PGSemaphoreCreate calls.	Also, if any system resources
 * are acquired here or in PGSemaphoreCreate, register an on_shmem_exit
 * callback to release them.
 *
 * The port number is passed for possible use as a key (for Posix, we use
 * it to generate the starting semaphore name).  In a standalone backend,
 * zero will be passed.
 *
 * In the Posix implementation, we acquire semaphores on-demand; the
 * maxSemas parameter is just used to size the array that keeps track of
 * acquired semas for subsequent releasing.
 */
void
PGReserveSemaphores(int maxSemas, int port)
{
	mySemPointers = (sem_t **) malloc(maxSemas * sizeof(sem_t *));
	if (mySemPointers == NULL)
		elog(PANIC, "out of memory");
	numSems = 0;
	maxSems = maxSemas;
	nextSemKey = port * 1000;

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
	int			i;

	for (i = 0; i < numSems; i++)
		PosixSemaphoreKill(mySemPointers[i]);
	free(mySemPointers);
}

/*
 * PGSemaphoreCreate
 *
 * Initialize a PGSemaphore structure to represent a sema with count 1
 */
void
PGSemaphoreCreate(PGSemaphore sema)
{
	sem_t	   *newsem;

	/* Can't do this in a backend, because static state is postmaster's */
	Assert(!IsUnderPostmaster);

	if (numSems >= maxSems)
		elog(PANIC, "too many semaphores created");

#ifdef USE_NAMED_POSIX_SEMAPHORES
	*sema = newsem = PosixSemaphoreCreate();
#else
	PosixSemaphoreCreate(sema);
	newsem = sema;
#endif

	/* Remember new sema for ReleaseSemaphores */
	mySemPointers[numSems++] = newsem;
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
PGSemaphoreLock(PGSemaphore sema, bool interruptOK)
{
	int			errStatus;

	/*
	 * See notes in sysv_sema.c's implementation of PGSemaphoreLock.
	 * Just as that code does for semop(), we handle both the case where
	 * sem_wait() returns errno == EINTR after a signal, and the case
	 * where it just keeps waiting.
	 */
	do
	{
		ImmediateInterruptOK = interruptOK;
		CHECK_FOR_INTERRUPTS();
		errStatus = sem_wait(PG_SEM_REF(sema));
		ImmediateInterruptOK = false;
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
