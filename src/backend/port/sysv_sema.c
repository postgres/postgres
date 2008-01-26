/*-------------------------------------------------------------------------
 *
 * sysv_sema.c
 *	  Implement PGSemaphores using SysV semaphore facilities
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/port/sysv_sema.c,v 1.23 2008/01/26 19:55:08 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>
#include <sys/file.h>
#ifdef HAVE_SYS_IPC_H
#include <sys/ipc.h>
#endif
#ifdef HAVE_SYS_SEM_H
#include <sys/sem.h>
#endif
#ifdef HAVE_KERNEL_OS_H
#include <kernel/OS.h>
#endif

#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/pg_sema.h"


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


static IpcSemaphoreId *mySemaSets;		/* IDs of sema sets acquired so far */
static int	numSemaSets;		/* number of sema sets acquired so far */
static int	maxSemaSets;		/* allocated size of mySemaSets array */
static IpcSemaphoreKey nextSemaKey;		/* next key to try using */
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
		/*
		 * Fail quietly if error indicates a collision with existing set. One
		 * would expect EEXIST, given that we said IPC_EXCL, but perhaps we
		 * could get a permission violation instead?  Also, EIDRM might occur
		 * if an old set is slated for destruction but not gone yet.
		 */
		if (errno == EEXIST || errno == EACCES
#ifdef EIDRM
			|| errno == EIDRM
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
				 (errno == ENOSPC) ?
				 errhint("This error does *not* mean that you have run out of disk space.\n"
		  "It occurs when either the system limit for the maximum number of "
			 "semaphore sets (SEMMNI), or the system wide maximum number of "
			"semaphores (SEMMNS), would be exceeded.  You need to raise the "
		  "respective kernel parameter.  Alternatively, reduce PostgreSQL's "
		"consumption of semaphores by reducing its max_connections parameter "
						 "(currently %d).\n"
			  "The PostgreSQL documentation contains more information about "
						 "configuring your system for PostgreSQL.",
						 MaxBackends) : 0));
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
		ereport(FATAL,
				(errmsg_internal("semctl(%d, %d, SETVAL, %d) failed: %m",
								 semId, semNum, value),
				 (errno == ERANGE) ?
				 errhint("You possibly need to raise your kernel's SEMVMX value to be at least "
				  "%d.  Look into the PostgreSQL documentation for details.",
						 value) : 0));
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
		 * sema key before we did.	Let him have that one, loop around to try
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
 * PGReserveSemaphores --- initialize semaphore support
 *
 * This is called during postmaster start or shared memory reinitialization.
 * It should do whatever is needed to be able to support up to maxSemas
 * subsequent PGSemaphoreCreate calls.	Also, if any system resources
 * are acquired here or in PGSemaphoreCreate, register an on_shmem_exit
 * callback to release them.
 *
 * The port number is passed for possible use as a key (for SysV, we use
 * it to generate the starting semaphore key).	In a standalone backend,
 * zero will be passed.
 *
 * In the SysV implementation, we acquire semaphore sets on-demand; the
 * maxSemas parameter is just used to size the array that keeps track of
 * acquired sets for subsequent releasing.
 */
void
PGReserveSemaphores(int maxSemas, int port)
{
	maxSemaSets = (maxSemas + SEMAS_PER_SET - 1) / SEMAS_PER_SET;
	mySemaSets = (IpcSemaphoreId *)
		malloc(maxSemaSets * sizeof(IpcSemaphoreId));
	if (mySemaSets == NULL)
		elog(PANIC, "out of memory");
	numSemaSets = 0;
	nextSemaKey = port * 1000;
	nextSemaNumber = SEMAS_PER_SET;		/* force sema set alloc on 1st call */

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
 * Initialize a PGSemaphore structure to represent a sema with count 1
 */
void
PGSemaphoreCreate(PGSemaphore sema)
{
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
	/* Assign the next free semaphore in the current set */
	sema->semId = mySemaSets[numSemaSets - 1];
	sema->semNum = nextSemaNumber++;
	/* Initialize it to count 1 */
	IpcSemaphoreInitialize(sema->semId, sema->semNum, 1);
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
PGSemaphoreLock(PGSemaphore sema, bool interruptOK)
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
	 * Each time around the loop, we check for a cancel/die interrupt.  On
	 * some platforms, if such an interrupt comes in while we are waiting,
	 * it will cause the semop() call to exit with errno == EINTR, allowing
	 * us to service the interrupt (if not in a critical section already)
	 * during the next loop iteration.
	 *
	 * Once we acquire the lock, we do NOT check for an interrupt before
	 * returning.  The caller needs to be able to record ownership of the lock
	 * before any interrupt can be accepted.
	 *
	 * There is a window of a few instructions between CHECK_FOR_INTERRUPTS
	 * and entering the semop() call.  If a cancel/die interrupt occurs in
	 * that window, we would fail to notice it until after we acquire the lock
	 * (or get another interrupt to escape the semop()).  We can avoid this
	 * problem by temporarily setting ImmediateInterruptOK to true before we
	 * do CHECK_FOR_INTERRUPTS; then, a die() interrupt in this interval will
	 * execute directly.  However, there is a huge pitfall: there is another
	 * window of a few instructions after the semop() before we are able to
	 * reset ImmediateInterruptOK.	If an interrupt occurs then, we'll lose
	 * control, which means that the lock has been acquired but our caller did
	 * not get a chance to record the fact. Therefore, we only set
	 * ImmediateInterruptOK if the caller tells us it's OK to do so, ie, the
	 * caller does not need to record acquiring the lock.  (This is currently
	 * true for lockmanager locks, since the process that granted us the lock
	 * did all the necessary state updates. It's not true for SysV semaphores
	 * used to implement LW locks or emulate spinlocks --- but the wait time
	 * for such locks should not be very long, anyway.)
	 *
	 * On some platforms, signals marked SA_RESTART (which is most, for us)
	 * will not interrupt the semop(); it will just keep waiting.  Therefore
	 * it's necessary for cancel/die interrupts to be serviced directly by
	 * the signal handler.  On these platforms the behavior is really the same
	 * whether the signal arrives just before the semop() begins, or while it
	 * is waiting.  The loop on EINTR is thus important only for other types
	 * of interrupts.
	 */
	do
	{
		ImmediateInterruptOK = interruptOK;
		CHECK_FOR_INTERRUPTS();
		errStatus = semop(sema->semId, &sops, 1);
		ImmediateInterruptOK = false;
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
