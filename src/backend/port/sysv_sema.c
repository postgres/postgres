/*-------------------------------------------------------------------------
 *
 * sysv_sema.c
 *	  Implement PGSemaphores using SysV semaphore facilities
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/port/sysv_sema.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/stat.h>

#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/pg_sema.h"
#include "storage/shmem.h"


typedef struct PGSemaphoreData
{
	int			semId;			/* semaphore set identifier */
	int			semNum;			/* semaphore number within set */
} PGSemaphoreData;

#ifndef HAVE_UNION_SEMUN
union semun
{
	int			val;
	struct semid_ds *buf;
	unsigned short *array;
};
#endif

typedef key_t IpcSemaphoreKey;	/* semaphore key passed to semget(2) */
typedef int IpcSemaphoreId;		/* semaphore ID returned by semget(2) */

/*
 * SEMAS_PER_SET is the number of useful semaphores in each semaphore set
 * we allocate.  It must be *less than* your kernel's SEMMSL (max semaphores
 * per set) parameter, which is often around 25.  (Less than, because we
 * allocate one extra sema in each set for identification purposes.)
 */
#define SEMAS_PER_SET	16

#define IPCProtection	(0600)	/* access/modify by user only */

#define PGSemaMagic		537		/* must be less than SEMVMX */


static PGSemaphore sharedSemas; /* array of PGSemaphoreData in shared memory */
static int	numSharedSemas;		/* number of PGSemaphoreDatas used so far */
static int	maxSharedSemas;		/* allocated size of PGSemaphoreData array */
static IpcSemaphoreId *mySemaSets;	/* IDs of sema sets acquired so far */
static int	numSemaSets;		/* number of sema sets acquired so far */
static int	maxSemaSets;		/* allocated size of mySemaSets array */
static IpcSemaphoreKey nextSemaKey; /* next key to try using */
static int	nextSemaNumber;		/* next free sem num in last sema set */


static IpcSemaphoreId InternalIpcSemaphoreCreate(IpcSemaphoreKey semKey,
												 int numSems);
static void IpcSemaphoreInitialize(IpcSemaphoreId semId, int semNum,
								   int value);
static void IpcSemaphoreKill(IpcSemaphoreId semId);
static int	IpcSemaphoreGetValue(IpcSemaphoreId semId, int semNum);
static pid_t IpcSemaphoreGetLastPID(IpcSemaphoreId semId, int semNum);
static IpcSemaphoreId IpcSemaphoreCreate(int numSems);
static void ReleaseSemaphores(int status, Datum arg);


/*
 * InternalIpcSemaphoreCreate
 *
 * Attempt to create a new semaphore set with the specified key.
 * Will fail (return -1) if such a set already exists.
 *
 * If we fail with a failure code other than collision-with-existing-set,
 * print out an error and abort.  Other types of errors suggest nonrecoverable
 * problems.
 */
static IpcSemaphoreId
InternalIpcSemaphoreCreate(IpcSemaphoreKey semKey, int numSems)
{
	int			semId;

	semId = semget(semKey, numSems, IPC_CREAT | IPC_EXCL | IPCProtection);

	if (semId < 0)
	{
		int			saved_errno = errno;

		/*
		 * Fail quietly if error indicates a collision with existing set. One
		 * would expect EEXIST, given that we said IPC_EXCL, but perhaps we
		 * could get a permission violation instead?  Also, EIDRM might occur
		 * if an old set is slated for destruction but not gone yet.
		 */
		if (saved_errno == EEXIST || saved_errno == EACCES
#ifdef EIDRM
			|| saved_errno == EIDRM
#endif
			)
			return -1;

		/*
		 * Else complain and abort
		 */
		ereport(FATAL,
				(errmsg("could not create semaphores: %m"),
				 errdetail("Failed system call was semget(%lu, %d, 0%o).",
						   (unsigned long) semKey, numSems,
						   IPC_CREAT | IPC_EXCL | IPCProtection),
				 (saved_errno == ENOSPC) ?
				 errhint("This error does *not* mean that you have run out of disk space.  "
						 "It occurs when either the system limit for the maximum number of "
						 "semaphore sets (SEMMNI), or the system wide maximum number of "
						 "semaphores (SEMMNS), would be exceeded.  You need to raise the "
						 "respective kernel parameter.  Alternatively, reduce PostgreSQL's "
						 "consumption of semaphores by reducing its \"max_connections\" parameter.\n"
						 "The PostgreSQL documentation contains more information about "
						 "configuring your system for PostgreSQL.") : 0));
	}

	return semId;
}

/*
 * Initialize a semaphore to the specified value.
 */
static void
IpcSemaphoreInitialize(IpcSemaphoreId semId, int semNum, int value)
{
	union semun semun;

	semun.val = value;
	if (semctl(semId, semNum, SETVAL, semun) < 0)
	{
		int			saved_errno = errno;

		ereport(FATAL,
				(errmsg_internal("semctl(%d, %d, SETVAL, %d) failed: %m",
								 semId, semNum, value),
				 (saved_errno == ERANGE) ?
				 errhint("You possibly need to raise your kernel's SEMVMX value to be at least "
						 "%d.  Look into the PostgreSQL documentation for details.",
						 value) : 0));
	}
}

/*
 * IpcSemaphoreKill(semId)	- removes a semaphore set
 */
static void
IpcSemaphoreKill(IpcSemaphoreId semId)
{
	union semun semun;

	semun.val = 0;				/* unused, but keep compiler quiet */

	if (semctl(semId, 0, IPC_RMID, semun) < 0)
		elog(LOG, "semctl(%d, 0, IPC_RMID, ...) failed: %m", semId);
}

/* Get the current value (semval) of the semaphore */
static int
IpcSemaphoreGetValue(IpcSemaphoreId semId, int semNum)
{
	union semun dummy;			/* for Solaris */

	dummy.val = 0;				/* unused */

	return semctl(semId, semNum, GETVAL, dummy);
}

/* Get the PID of the last process to do semop() on the semaphore */
static pid_t
IpcSemaphoreGetLastPID(IpcSemaphoreId semId, int semNum)
{
	union semun dummy;			/* for Solaris */

	dummy.val = 0;				/* unused */

	return semctl(semId, semNum, GETPID, dummy);
}


/*
 * Create a semaphore set with the given number of useful semaphores
 * (an additional sema is actually allocated to serve as identifier).
 * Dead Postgres sema sets are recycled if found, but we do not fail
 * upon collision with non-Postgres sema sets.
 *
 * The idea here is to detect and re-use keys that may have been assigned
 * by a crashed postmaster or backend.
 */
static IpcSemaphoreId
IpcSemaphoreCreate(int numSems)
{
	IpcSemaphoreId semId;
	union semun semun;
	PGSemaphoreData mysema;

	/* Loop till we find a free IPC key */
	for (nextSemaKey++;; nextSemaKey++)
	{
		pid_t		creatorPID;

		/* Try to create new semaphore set */
		semId = InternalIpcSemaphoreCreate(nextSemaKey, numSems + 1);
		if (semId >= 0)
			break;				/* successful create */

		/* See if it looks to be leftover from a dead Postgres process */
		semId = semget(nextSemaKey, numSems + 1, 0);
		if (semId < 0)
			continue;			/* failed: must be some other app's */
		if (IpcSemaphoreGetValue(semId, numSems) != PGSemaMagic)
			continue;			/* sema belongs to a non-Postgres app */

		/*
		 * If the creator PID is my own PID or does not belong to any extant
		 * process, it's safe to zap it.
		 */
		creatorPID = IpcSemaphoreGetLastPID(semId, numSems);
		if (creatorPID <= 0)
			continue;			/* oops, GETPID failed */
		if (creatorPID != getpid())
		{
			if (kill(creatorPID, 0) == 0 || errno != ESRCH)
				continue;		/* sema belongs to a live process */
		}

		/*
		 * The sema set appears to be from a dead Postgres process, or from a
		 * previous cycle of life in this same process.  Zap it, if possible.
		 * This probably shouldn't fail, but if it does, assume the sema set
		 * belongs to someone else after all, and continue quietly.
		 */
		semun.val = 0;			/* unused, but keep compiler quiet */
		if (semctl(semId, 0, IPC_RMID, semun) < 0)
			continue;

		/*
		 * Now try again to create the sema set.
		 */
		semId = InternalIpcSemaphoreCreate(nextSemaKey, numSems + 1);
		if (semId >= 0)
			break;				/* successful create */

		/*
		 * Can only get here if some other process managed to create the same
		 * sema key before we did.  Let him have that one, loop around to try
		 * next key.
		 */
	}

	/*
	 * OK, we created a new sema set.  Mark it as created by this process. We
	 * do this by setting the spare semaphore to PGSemaMagic-1 and then
	 * incrementing it with semop().  That leaves it with value PGSemaMagic
	 * and sempid referencing this process.
	 */
	IpcSemaphoreInitialize(semId, numSems, PGSemaMagic - 1);
	mysema.semId = semId;
	mysema.semNum = numSems;
	PGSemaphoreUnlock(&mysema);

	return semId;
}


/*
 * Report amount of shared memory needed for semaphores
 */
Size
PGSemaphoreShmemSize(int maxSemas)
{
	return mul_size(maxSemas, sizeof(PGSemaphoreData));
}

/*
 * PGReserveSemaphores --- initialize semaphore support
 *
 * This is called during postmaster start or shared memory reinitialization.
 * It should do whatever is needed to be able to support up to maxSemas
 * subsequent PGSemaphoreCreate calls.  Also, if any system resources
 * are acquired here or in PGSemaphoreCreate, register an on_shmem_exit
 * callback to release them.
 *
 * In the SysV implementation, we acquire semaphore sets on-demand; the
 * maxSemas parameter is just used to size the arrays.  There is an array
 * of PGSemaphoreData structs in shared memory, and a postmaster-local array
 * with one entry per SysV semaphore set, which we use for releasing the
 * semaphore sets when done.  (This design ensures that postmaster shutdown
 * doesn't rely on the contents of shared memory, which a failed backend might
 * have clobbered.)
 */
void
PGReserveSemaphores(int maxSemas)
{
	struct stat statbuf;

	/*
	 * We use the data directory's inode number to seed the search for free
	 * semaphore keys.  This minimizes the odds of collision with other
	 * postmasters, while maximizing the odds that we will detect and clean up
	 * semaphores left over from a crashed postmaster in our own directory.
	 */
	if (stat(DataDir, &statbuf) < 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not stat data directory \"%s\": %m",
						DataDir)));

	/*
	 * We must use ShmemAllocUnlocked(), since the spinlock protecting
	 * ShmemAlloc() won't be ready yet.
	 */
	sharedSemas = (PGSemaphore)
		ShmemAllocUnlocked(PGSemaphoreShmemSize(maxSemas));
	numSharedSemas = 0;
	maxSharedSemas = maxSemas;

	maxSemaSets = (maxSemas + SEMAS_PER_SET - 1) / SEMAS_PER_SET;
	mySemaSets = (IpcSemaphoreId *)
		malloc(maxSemaSets * sizeof(IpcSemaphoreId));
	if (mySemaSets == NULL)
		elog(PANIC, "out of memory");
	numSemaSets = 0;
	nextSemaKey = statbuf.st_ino;
	nextSemaNumber = SEMAS_PER_SET; /* force sema set alloc on 1st call */

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

	for (i = 0; i < numSemaSets; i++)
		IpcSemaphoreKill(mySemaSets[i]);
	free(mySemaSets);
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

	/* Can't do this in a backend, because static state is postmaster's */
	Assert(!IsUnderPostmaster);

	if (nextSemaNumber >= SEMAS_PER_SET)
	{
		/* Time to allocate another semaphore set */
		if (numSemaSets >= maxSemaSets)
			elog(PANIC, "too many semaphores created");
		mySemaSets[numSemaSets] = IpcSemaphoreCreate(SEMAS_PER_SET);
		numSemaSets++;
		nextSemaNumber = 0;
	}
	/* Use the next shared PGSemaphoreData */
	if (numSharedSemas >= maxSharedSemas)
		elog(PANIC, "too many semaphores created");
	sema = &sharedSemas[numSharedSemas++];
	/* Assign the next free semaphore in the current set */
	sema->semId = mySemaSets[numSemaSets - 1];
	sema->semNum = nextSemaNumber++;
	/* Initialize it to count 1 */
	IpcSemaphoreInitialize(sema->semId, sema->semNum, 1);

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
	IpcSemaphoreInitialize(sema->semId, sema->semNum, 0);
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
	struct sembuf sops;

	sops.sem_op = -1;			/* decrement */
	sops.sem_flg = 0;
	sops.sem_num = sema->semNum;

	/*
	 * Note: if errStatus is -1 and errno == EINTR then it means we returned
	 * from the operation prematurely because we were sent a signal.  So we
	 * try and lock the semaphore again.
	 *
	 * We used to check interrupts here, but that required servicing
	 * interrupts directly from signal handlers. Which is hard to do safely
	 * and portably.
	 */
	do
	{
		errStatus = semop(sema->semId, &sops, 1);
	} while (errStatus < 0 && errno == EINTR);

	if (errStatus < 0)
		elog(FATAL, "semop(id=%d) failed: %m", sema->semId);
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
	struct sembuf sops;

	sops.sem_op = 1;			/* increment */
	sops.sem_flg = 0;
	sops.sem_num = sema->semNum;

	/*
	 * Note: if errStatus is -1 and errno == EINTR then it means we returned
	 * from the operation prematurely because we were sent a signal.  So we
	 * try and unlock the semaphore again. Not clear this can really happen,
	 * but might as well cope.
	 */
	do
	{
		errStatus = semop(sema->semId, &sops, 1);
	} while (errStatus < 0 && errno == EINTR);

	if (errStatus < 0)
		elog(FATAL, "semop(id=%d) failed: %m", sema->semId);
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
	struct sembuf sops;

	sops.sem_op = -1;			/* decrement */
	sops.sem_flg = IPC_NOWAIT;	/* but don't block */
	sops.sem_num = sema->semNum;

	/*
	 * Note: if errStatus is -1 and errno == EINTR then it means we returned
	 * from the operation prematurely because we were sent a signal.  So we
	 * try and lock the semaphore again.
	 */
	do
	{
		errStatus = semop(sema->semId, &sops, 1);
	} while (errStatus < 0 && errno == EINTR);

	if (errStatus < 0)
	{
		/* Expect EAGAIN or EWOULDBLOCK (platform-dependent) */
#ifdef EAGAIN
		if (errno == EAGAIN)
			return false;		/* failed to lock it */
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || (EWOULDBLOCK != EAGAIN))
		if (errno == EWOULDBLOCK)
			return false;		/* failed to lock it */
#endif
		/* Otherwise we got trouble */
		elog(FATAL, "semop(id=%d) failed: %m", sema->semId);
	}

	return true;
}
