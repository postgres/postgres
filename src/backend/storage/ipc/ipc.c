/*-------------------------------------------------------------------------
 *
 * ipc.c
 *	  POSTGRES inter-process communication definitions.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/ipc/ipc.c,v 1.62 2001/01/24 19:43:07 momjian Exp $
 *
 * NOTES
 *
 *	  Currently, semaphores are used (my understanding anyway) in two
 *	  different ways:
 *		1. as mutexes on machines that don't have test-and-set (eg.
 *		   mips R3000).
 *		2. for putting processes to sleep when waiting on a lock
 *		   and waking them up when the lock is free.
 *	  The number of semaphores in (1) is fixed and those are shared
 *	  among all backends. In (2), there is 1 semaphore per process and those
 *	  are not shared with anyone else.
 *														  -ay 4/95
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/types.h>
#include <sys/file.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>

#include "storage/ipc.h"
#include "storage/s_lock.h"
/* In Ultrix, sem.h and shm.h must be included AFTER ipc.h */
#ifdef HAVE_SYS_SEM_H
#include <sys/sem.h>
#endif
#ifdef HAVE_SYS_SHM_H
#include <sys/shm.h>
#endif
#ifdef HAVE_KERNEL_OS_H
#include <kernel/OS.h>
#endif

#if defined(solaris_sparc)
#include <sys/ipc.h>
#endif

#if defined(__darwin__)
#include "port/darwin/sem.h"
#endif

#include "miscadmin.h"
#include "utils/memutils.h"
#include "libpq/libpq.h"


/*
 * This flag is set during proc_exit() to change elog()'s behavior,
 * so that an elog() from an on_proc_exit routine cannot get us out
 * of the exit procedure.  We do NOT want to go back to the idle loop...
 */
bool		proc_exit_inprogress = false;

static IpcSemaphoreId InternalIpcSemaphoreCreate(IpcSemaphoreKey semKey,
						   int numSems, int permission,
						   int semStartValue, bool removeOnExit);
static void CallbackSemaphoreKill(int status, Datum semId);
static void *InternalIpcMemoryCreate(IpcMemoryKey memKey, uint32 size,
									 int permission);
static void IpcMemoryDetach(int status, Datum shmaddr);
static void IpcMemoryDelete(int status, Datum shmId);
static void *PrivateMemoryCreate(uint32 size);
static void PrivateMemoryDelete(int status, Datum memaddr);


/* ----------------------------------------------------------------
 *						exit() handling stuff
 *
 * These functions are in generally the same spirit as atexit(2),
 * but provide some additional features we need --- in particular,
 * we want to register callbacks to invoke when we are disconnecting
 * from a broken shared-memory context but not exiting the postmaster.
 *
 * Callback functions can take zero, one, or two args: the first passed
 * arg is the integer exitcode, the second is the Datum supplied when
 * the callback was registered.
 *
 * XXX these functions probably ought to live in some other module.
 * ----------------------------------------------------------------
 */

#define MAX_ON_EXITS 20

static struct ONEXIT
{
	void		(*function) ();
	Datum		arg;
}			on_proc_exit_list[MAX_ON_EXITS],
			on_shmem_exit_list[MAX_ON_EXITS];

static int	on_proc_exit_index,
			on_shmem_exit_index;


/* ----------------------------------------------------------------
 *		proc_exit
 *
 *		this function calls all the callbacks registered
 *		for it (to free resources) and then calls exit.
 *		This should be the only function to call exit().
 *		-cim 2/6/90
 * ----------------------------------------------------------------
 */
void
proc_exit(int code)
{

	/*
	 * Once we set this flag, we are committed to exit.  Any elog() will
	 * NOT send control back to the main loop, but right back here.
	 */
	proc_exit_inprogress = true;

	/*
	 * Forget any pending cancel or die requests; we're doing our best
	 * to close up shop already.  Note that the signal handlers will not
	 * set these flags again, now that proc_exit_inprogress is set.
	 */
	InterruptPending = false;
	ProcDiePending = false;
	QueryCancelPending = false;
	/* And let's just make *sure* we're not interrupted ... */
	ImmediateInterruptOK = false;
	InterruptHoldoffCount = 1;
	CritSectionCount = 0;

	if (DebugLvl > 1)
		elog(DEBUG, "proc_exit(%d)", code);

	/* do our shared memory exits first */
	shmem_exit(code);

	/* ----------------
	 *	call all the callbacks registered before calling exit().
	 *
	 *	Note that since we decrement on_proc_exit_index each time,
	 *	if a callback calls elog(ERROR) or elog(FATAL) then it won't
	 *	be invoked again when control comes back here (nor will the
	 *	previously-completed callbacks).  So, an infinite loop
	 *	should not be possible.
	 * ----------------
	 */
	while (--on_proc_exit_index >= 0)
		(*on_proc_exit_list[on_proc_exit_index].function) (code,
							  on_proc_exit_list[on_proc_exit_index].arg);

	if (DebugLvl > 1)
		elog(DEBUG, "exit(%d)", code);
	exit(code);
}

/* ------------------
 * Run all of the on_shmem_exit routines --- but don't actually exit.
 * This is used by the postmaster to re-initialize shared memory and
 * semaphores after a backend dies horribly.
 * ------------------
 */
void
shmem_exit(int code)
{
	if (DebugLvl > 1)
		elog(DEBUG, "shmem_exit(%d)", code);

	/* ----------------
	 *	call all the registered callbacks.
	 *
	 *	As with proc_exit(), we remove each callback from the list
	 *	before calling it, to avoid infinite loop in case of error.
	 * ----------------
	 */
	while (--on_shmem_exit_index >= 0)
		(*on_shmem_exit_list[on_shmem_exit_index].function) (code,
							on_shmem_exit_list[on_shmem_exit_index].arg);

	on_shmem_exit_index = 0;
}

/* ----------------------------------------------------------------
 *		on_proc_exit
 *
 *		this function adds a callback function to the list of
 *		functions invoked by proc_exit().	-cim 2/6/90
 * ----------------------------------------------------------------
 */
void
on_proc_exit(void (*function) (), Datum arg)
{
	if (on_proc_exit_index >= MAX_ON_EXITS)
		elog(FATAL, "Out of on_proc_exit slots");

	on_proc_exit_list[on_proc_exit_index].function = function;
	on_proc_exit_list[on_proc_exit_index].arg = arg;

	++on_proc_exit_index;
}

/* ----------------------------------------------------------------
 *		on_shmem_exit
 *
 *		this function adds a callback function to the list of
 *		functions invoked by shmem_exit().	-cim 2/6/90
 * ----------------------------------------------------------------
 */
void
on_shmem_exit(void (*function) (), Datum arg)
{
	if (on_shmem_exit_index >= MAX_ON_EXITS)
		elog(FATAL, "Out of on_shmem_exit slots");

	on_shmem_exit_list[on_shmem_exit_index].function = function;
	on_shmem_exit_list[on_shmem_exit_index].arg = arg;

	++on_shmem_exit_index;
}

/* ----------------------------------------------------------------
 *		on_exit_reset
 *
 *		this function clears all on_proc_exit() and on_shmem_exit()
 *		registered functions.  This is used just after forking a backend,
 *		so that the backend doesn't believe it should call the postmaster's
 *		on-exit routines when it exits...
 * ----------------------------------------------------------------
 */
void
on_exit_reset(void)
{
	on_shmem_exit_index = 0;
	on_proc_exit_index = 0;
}


/* ----------------------------------------------------------------
 *						Semaphore support
 *
 * These routines represent a fairly thin layer on top of SysV semaphore
 * functionality.
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *	InternalIpcSemaphoreCreate(semKey, numSems, permission,
 *							   semStartValue, removeOnExit)
 *
 * Attempt to create a new semaphore set with the specified key.
 * Will fail (return -1) if such a set already exists.
 * On success, a callback is optionally registered with on_shmem_exit
 * to delete the semaphore set when on_shmem_exit is called.
 *
 * If we fail with a failure code other than collision-with-existing-set,
 * print out an error and abort.  Other types of errors are not recoverable.
 * ----------------------------------------------------------------
 */
static IpcSemaphoreId
InternalIpcSemaphoreCreate(IpcSemaphoreKey semKey,
						   int numSems, int permission,
						   int semStartValue, bool removeOnExit)
{
	int			semId;
	int			i;
	u_short		array[IPC_NMAXSEM];
	union semun semun;

	Assert(numSems > 0 && numSems <= IPC_NMAXSEM);

	semId = semget(semKey, numSems, IPC_CREAT | IPC_EXCL | permission);

	if (semId < 0)
	{
		/*
		 * Fail quietly if error indicates a collision with existing set.
		 * One would expect EEXIST, given that we said IPC_EXCL, but perhaps
		 * we could get a permission violation instead?  Also, EIDRM might
		 * occur if an old set is slated for destruction but not gone yet.
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
		fprintf(stderr, "IpcSemaphoreCreate: semget(key=%d, num=%d, 0%o) failed: %s\n",
				(int) semKey, numSems, (IPC_CREAT|IPC_EXCL|permission),
				strerror(errno));

		if (errno == ENOSPC)
			fprintf(stderr,
					"\nThis error does *not* mean that you have run out of disk space.\n\n"
					"It occurs either because system limit for the maximum number of\n"
					"semaphore sets (SEMMNI), or the system wide maximum number of\n"
					"semaphores (SEMMNS), would be exceeded.  You need to raise the\n"
					"respective kernel parameter.  Look into the PostgreSQL documentation\n"
					"for details.\n\n");

		proc_exit(1);
	}

	/* Initialize new semas to specified start value */
	for (i = 0; i < numSems; i++)
		array[i] = semStartValue;
	semun.array = array;
	if (semctl(semId, 0, SETALL, semun) < 0)
	{
		fprintf(stderr, "IpcSemaphoreCreate: semctl(id=%d, 0, SETALL, ...) failed: %s\n",
				semId, strerror(errno));

		if (errno == ERANGE)
			fprintf(stderr,
					"You possibly need to raise your kernel's SEMVMX value to be at least\n"
					"%d.  Look into the PostgreSQL documentation for details.\n",
					semStartValue);

		IpcSemaphoreKill(semId);
		proc_exit(1);
	}

	/* Register on-exit routine to delete the new set */
	if (removeOnExit)
		on_shmem_exit(CallbackSemaphoreKill, Int32GetDatum(semId));

	return semId;
}

/****************************************************************************/
/*	 IpcSemaphoreKill(semId)	- removes a semaphore set					*/
/*																			*/
/****************************************************************************/
void
IpcSemaphoreKill(IpcSemaphoreId semId)
{
	union semun semun;

	semun.val = 0;		/* unused, but keep compiler quiet */

	if (semctl(semId, 0, IPC_RMID, semun) < 0)
		fprintf(stderr, "IpcSemaphoreKill: semctl(%d, 0, IPC_RMID, ...) failed: %s\n",
				semId, strerror(errno));
	/* We used to report a failure via elog(NOTICE), but that's pretty
	 * pointless considering any client has long since disconnected ...
	 */
}

/****************************************************************************/
/*	 CallbackSemaphoreKill(status, semId)									*/
/*	(called as an on_shmem_exit callback, hence funny argument list)		*/
/****************************************************************************/
static void
CallbackSemaphoreKill(int status, Datum semId)
{
	IpcSemaphoreKill(DatumGetInt32(semId));
}

/****************************************************************************/
/*	 IpcSemaphoreLock(semId, sem) - locks a semaphore						*/
/****************************************************************************/
void
IpcSemaphoreLock(IpcSemaphoreId semId, int sem, bool interruptOK)
{
	int			errStatus;
	struct sembuf sops;

	sops.sem_op = -1;			/* decrement */
	sops.sem_flg = 0;
	sops.sem_num = sem;

	/* ----------------
	 *	Note: if errStatus is -1 and errno == EINTR then it means we
	 *		  returned from the operation prematurely because we were
	 *		  sent a signal.  So we try and lock the semaphore again.
	 *
	 *	Each time around the loop, we check for a cancel/die interrupt.
	 *	We assume that if such an interrupt comes in while we are waiting,
	 *	it will cause the semop() call to exit with errno == EINTR, so that
	 *	we will be able to service the interrupt (if not in a critical
	 *	section already).
	 *
	 *	Once we acquire the lock, we do NOT check for an interrupt before
	 *	returning.  The caller needs to be able to record ownership of
	 *	the lock before any interrupt can be accepted.
	 *
	 *	There is a window of a few instructions between CHECK_FOR_INTERRUPTS
	 *	and entering the semop() call.  If a cancel/die interrupt occurs in
	 *	that window, we would fail to notice it until after we acquire the
	 *	lock (or get another interrupt to escape the semop()).  We can avoid
	 *	this problem by temporarily setting ImmediateInterruptOK = true
	 *	before we do CHECK_FOR_INTERRUPTS; then, a die() interrupt in this
	 *	interval will execute directly.  However, there is a huge pitfall:
	 *	there is another window of a few instructions after the semop()
	 *	before we are able to reset ImmediateInterruptOK.  If an interrupt
	 *	occurs then, we'll lose control, which means that the lock has been
	 *	acquired but our caller did not get a chance to record the fact.
	 *	Therefore, we only set ImmediateInterruptOK if the caller tells us
	 *	it's OK to do so, ie, the caller does not need to record acquiring
	 *	the lock.  (This is currently true for lockmanager locks, since the
	 *	process that granted us the lock did all the necessary state updates.
	 *	It's not true for SysV semaphores used to emulate spinlocks --- but
	 *	our performance on such platforms is so horrible anyway that I'm
	 *	not going to worry too much about it.)
	 *	----------------
	 */
	do
	{
		ImmediateInterruptOK = interruptOK;
		CHECK_FOR_INTERRUPTS();
		errStatus = semop(semId, &sops, 1);
		ImmediateInterruptOK = false;
	} while (errStatus == -1 && errno == EINTR);

	if (errStatus == -1)
	{
        fprintf(stderr, "IpcSemaphoreLock: semop(id=%d) failed: %s\n",
				semId, strerror(errno));
		proc_exit(255);
	}
}

/****************************************************************************/
/*	 IpcSemaphoreUnlock(semId, sem)		- unlocks a semaphore				*/
/****************************************************************************/
void
IpcSemaphoreUnlock(IpcSemaphoreId semId, int sem)
{
	int			errStatus;
	struct sembuf sops;

	sops.sem_op = 1;			/* increment */
	sops.sem_flg = 0;
	sops.sem_num = sem;


	/* ----------------
	 *	Note: if errStatus is -1 and errno == EINTR then it means we
	 *		  returned from the operation prematurely because we were
	 *		  sent a signal.  So we try and unlock the semaphore again.
	 *		  Not clear this can really happen, but might as well cope.
	 * ----------------
	 */
	do
	{
		errStatus = semop(semId, &sops, 1);
	} while (errStatus == -1 && errno == EINTR);

	if (errStatus == -1)
	{
		fprintf(stderr, "IpcSemaphoreUnlock: semop(id=%d) failed: %s\n",
				semId, strerror(errno));
		proc_exit(255);
	}
}

/****************************************************************************/
/*	 IpcSemaphoreTryLock(semId, sem)	- conditionally locks a semaphore	*/
/* Lock the semaphore if it's free, but don't block.						*/
/****************************************************************************/
bool
IpcSemaphoreTryLock(IpcSemaphoreId semId, int sem)
{
	int			errStatus;
	struct sembuf sops;

	sops.sem_op = -1;			/* decrement */
	sops.sem_flg = IPC_NOWAIT;	/* but don't block */
	sops.sem_num = sem;

	/* ----------------
	 *	Note: if errStatus is -1 and errno == EINTR then it means we
	 *		  returned from the operation prematurely because we were
	 *		  sent a signal.  So we try and lock the semaphore again.
	 * ----------------
	 */
	do
	{
		errStatus = semop(semId, &sops, 1);
	} while (errStatus == -1 && errno == EINTR);

	if (errStatus == -1)
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
        fprintf(stderr, "IpcSemaphoreTryLock: semop(id=%d) failed: %s\n",
				semId, strerror(errno));
		proc_exit(255);
	}

	return true;
}

/* Get the current value (semval) of the semaphore */
int
IpcSemaphoreGetValue(IpcSemaphoreId semId, int sem)
{
	union semun dummy;			/* for Solaris */
	dummy.val = 0;		/* unused */

	return semctl(semId, sem, GETVAL, dummy);
}

/* Get the PID of the last process to do semop() on the semaphore */
static pid_t
IpcSemaphoreGetLastPID(IpcSemaphoreId semId, int sem)
{
	union semun dummy;			/* for Solaris */
	dummy.val = 0;		/* unused */

	return semctl(semId, sem, GETPID, dummy);
}


/* ----------------------------------------------------------------
 *						Shared memory support
 *
 * These routines represent a fairly thin layer on top of SysV shared
 * memory functionality.
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *	InternalIpcMemoryCreate(memKey, size, permission)
 *
 * Attempt to create a new shared memory segment with the specified key.
 * Will fail (return NULL) if such a segment already exists.  If successful,
 * attach the segment to the current process and return its attached address.
 * On success, callbacks are registered with on_shmem_exit to detach and
 * delete the segment when on_shmem_exit is called.
 *
 * If we fail with a failure code other than collision-with-existing-segment,
 * print out an error and abort.  Other types of errors are not recoverable.
 * ----------------------------------------------------------------
 */
static void *
InternalIpcMemoryCreate(IpcMemoryKey memKey, uint32 size, int permission)
{
	IpcMemoryId shmid;
	void	   *memAddress;

	shmid = shmget(memKey, size, IPC_CREAT | IPC_EXCL | permission);

	if (shmid < 0)
	{
		/*
		 * Fail quietly if error indicates a collision with existing segment.
		 * One would expect EEXIST, given that we said IPC_EXCL, but perhaps
		 * we could get a permission violation instead?  Also, EIDRM might
		 * occur if an old seg is slated for destruction but not gone yet.
		 */
		if (errno == EEXIST || errno == EACCES
#ifdef EIDRM
			|| errno == EIDRM
#endif
			)
			return NULL;
		/*
		 * Else complain and abort
		 */
		fprintf(stderr, "IpcMemoryCreate: shmget(key=%d, size=%u, 0%o) failed: %s\n",
				(int) memKey, size, (IPC_CREAT | IPC_EXCL | permission),
				strerror(errno));

		if (errno == EINVAL)
			fprintf(stderr,
					"\nThis error can be caused by one of three things:\n\n"
					"1. The maximum size for shared memory segments on your system was\n"
					"   exceeded.  You need to raise the SHMMAX parameter in your kernel\n"
					"   to be at least %u bytes.\n\n"
					"2. The requested shared memory segment was too small for your system.\n"
					"   You need to lower the SHMMIN parameter in your kernel.\n\n"
					"3. The requested shared memory segment already exists but is of the\n"
					"   wrong size.  This is most likely the case if an old version of\n"
					"   PostgreSQL crashed and didn't clean up.  The `ipcclean' utility\n"
					"   can be used to remedy this.\n\n"
					"The PostgreSQL Administrator's Guide contains more information about\n"
					"shared memory configuration.\n\n",
					size);

		else if (errno == ENOSPC)
			fprintf(stderr,
					"\nThis error does *not* mean that you have run out of disk space.\n\n"
					"It occurs either if all available shared memory ids have been taken,\n"
					"in which case you need to raise the SHMMNI parameter in your kernel,\n"
					"or because the system's overall limit for shared memory has been\n"
					"reached.  The PostgreSQL Administrator's Guide contains more\n"
					"information about shared memory configuration.\n\n");

		proc_exit(1);
	}

	/* Register on-exit routine to delete the new segment */
	on_shmem_exit(IpcMemoryDelete, Int32GetDatum(shmid));

	/* OK, should be able to attach to the segment */
	memAddress = shmat(shmid, 0, 0);

	if (memAddress == (void *) -1)
	{
        fprintf(stderr, "IpcMemoryCreate: shmat(id=%d) failed: %s\n",
				shmid, strerror(errno));
		proc_exit(1);
	}

	/* Register on-exit routine to detach new segment before deleting */
	on_shmem_exit(IpcMemoryDetach, PointerGetDatum(memAddress));

	return memAddress;
}

/****************************************************************************/
/*	IpcMemoryDetach(status, shmaddr)	removes a shared memory segment		*/
/*										from process' address spaceq		*/
/*	(called as an on_shmem_exit callback, hence funny argument list)		*/
/****************************************************************************/
static void
IpcMemoryDetach(int status, Datum shmaddr)
{
	if (shmdt(DatumGetPointer(shmaddr)) < 0)
		fprintf(stderr, "IpcMemoryDetach: shmdt(%p) failed: %s\n",
				DatumGetPointer(shmaddr), strerror(errno));
	/* We used to report a failure via elog(NOTICE), but that's pretty
	 * pointless considering any client has long since disconnected ...
	 */
}

/****************************************************************************/
/*	IpcMemoryDelete(status, shmId)		deletes a shared memory segment		*/
/*	(called as an on_shmem_exit callback, hence funny argument list)		*/
/****************************************************************************/
static void
IpcMemoryDelete(int status, Datum shmId)
{
	if (shmctl(DatumGetInt32(shmId), IPC_RMID, (struct shmid_ds *) NULL) < 0)
		fprintf(stderr, "IpcMemoryDelete: shmctl(%d, %d, 0) failed: %s\n",
				DatumGetInt32(shmId), IPC_RMID, strerror(errno));
	/* We used to report a failure via elog(NOTICE), but that's pretty
	 * pointless considering any client has long since disconnected ...
	 */
}

/* ----------------------------------------------------------------
 *						private memory support
 *
 * Rather than allocating shmem segments with IPC_PRIVATE key, we
 * just malloc() the requested amount of space.  This code emulates
 * the needed shmem functions.
 * ----------------------------------------------------------------
 */

static void *
PrivateMemoryCreate(uint32 size)
{
	void	   *memAddress;

	memAddress = malloc(size);
	if (!memAddress)
	{
		fprintf(stderr, "PrivateMemoryCreate: malloc(%u) failed\n", size);
		proc_exit(1);
	}
	MemSet(memAddress, 0, size);		/* keep Purify quiet */

	/* Register on-exit routine to release storage */
	on_shmem_exit(PrivateMemoryDelete, PointerGetDatum(memAddress));

	return memAddress;
}

static void
PrivateMemoryDelete(int status, Datum memaddr)
{
	free(DatumGetPointer(memaddr));
}


/* ------------------
 *				Routines to assign keys for new IPC objects
 *
 * The idea here is to detect and re-use keys that may have been assigned
 * by a crashed postmaster or backend.
 * ------------------
 */

static IpcMemoryKey NextShmemSegID = 0;
static IpcSemaphoreKey NextSemaID = 0;

/*
 * (Re) initialize key assignment at startup of postmaster or standalone
 * backend, also at postmaster reset.
 */
void
IpcInitKeyAssignment(int port)
{
	NextShmemSegID = port * 1000;
	NextSemaID = port * 1000;
}

/*
 * Create a shared memory segment of the given size and initialize its
 * standard header.  Dead Postgres segments are recycled if found,
 * but we do not fail upon collision with non-Postgres shmem segments.
 */
PGShmemHeader *
IpcMemoryCreate(uint32 size, bool makePrivate, int permission)
{
	void   *memAddress;
	PGShmemHeader *hdr;

	/* Room for a header? */
	Assert(size > MAXALIGN(sizeof(PGShmemHeader)));

	/* Loop till we find a free IPC key */
	for (NextShmemSegID++ ; ; NextShmemSegID++)
	{
		IpcMemoryId shmid;

		/* Special case if creating a private segment --- just malloc() it */
		if (makePrivate)
		{
			memAddress = PrivateMemoryCreate(size);
			break;
		}

		/* Try to create new segment */
		memAddress = InternalIpcMemoryCreate(NextShmemSegID, size, permission);
		if (memAddress)
			break;				/* successful create and attach */

		/* See if it looks to be leftover from a dead Postgres process */
		shmid = shmget(NextShmemSegID, sizeof(PGShmemHeader), 0);
		if (shmid < 0)
			continue;			/* failed: must be some other app's */
		memAddress = shmat(shmid, 0, 0);
		if (memAddress == (void *) -1)
			continue;			/* failed: must be some other app's */
		hdr = (PGShmemHeader *) memAddress;
		if (hdr->magic != PGShmemMagic)
		{
			shmdt(memAddress);
			continue;			/* segment belongs to a non-Postgres app */
		}
		/*
		 * If the creator PID is my own PID or does not belong to any
		 * extant process, it's safe to zap it.
		 */
		if (hdr->creatorPID != getpid())
		{
			if (kill(hdr->creatorPID, 0) == 0 ||
				errno != ESRCH)
			{
				shmdt(memAddress);
				continue;		/* segment belongs to a live process */
			}
		}
		/*
		 * The segment appears to be from a dead Postgres process, or
		 * from a previous cycle of life in this same process.  Zap it,
		 * if possible.  This probably shouldn't fail, but if it does,
		 * assume the segment belongs to someone else after all,
		 * and continue quietly.
		 */
		shmdt(memAddress);
		if (shmctl(shmid, IPC_RMID, (struct shmid_ds *) NULL) < 0)
			continue;
		/*
		 * Now try again to create the segment.
		 */
		memAddress = InternalIpcMemoryCreate(NextShmemSegID, size, permission);
		if (memAddress)
			break;				/* successful create and attach */
		/*
		 * Can only get here if some other process managed to create the
		 * same shmem key before we did.  Let him have that one,
		 * loop around to try next key.
		 */
	}
	/*
	 * OK, we created a new segment.  Mark it as created by this process.
	 * The order of assignments here is critical so that another Postgres
	 * process can't see the header as valid but belonging to an invalid
	 * PID!
	 */
	hdr = (PGShmemHeader *) memAddress;
	hdr->creatorPID = getpid();
	hdr->magic = PGShmemMagic;
	/*
	 * Initialize space allocation status for segment.
	 */
	hdr->totalsize = size;
	hdr->freeoffset = MAXALIGN(sizeof(PGShmemHeader));

	return hdr;
}

/*
 * Create a semaphore set with the given number of useful semaphores
 * (an additional sema is actually allocated to serve as identifier).
 * Dead Postgres sema sets are recycled if found, but we do not fail
 * upon collision with non-Postgres sema sets.
 */
IpcSemaphoreId
IpcSemaphoreCreate(int numSems, int permission,
				   int semStartValue, bool removeOnExit)
{
	IpcSemaphoreId	semId;
	union semun semun;

	/* Loop till we find a free IPC key */
	for (NextSemaID++ ; ; NextSemaID++)
	{
		pid_t	creatorPID;

		/* Try to create new semaphore set */
		semId = InternalIpcSemaphoreCreate(NextSemaID, numSems+1,
										   permission, semStartValue,
										   removeOnExit);
		if (semId >= 0)
			break;				/* successful create */

		/* See if it looks to be leftover from a dead Postgres process */
		semId = semget(NextSemaID, numSems+1, 0);
		if (semId < 0)
			continue;			/* failed: must be some other app's */
		if (IpcSemaphoreGetValue(semId, numSems) != PGSemaMagic)
			continue;			/* sema belongs to a non-Postgres app */
		/*
		 * If the creator PID is my own PID or does not belong to any
		 * extant process, it's safe to zap it.
		 */
		creatorPID = IpcSemaphoreGetLastPID(semId, numSems);
		if (creatorPID <= 0)
			continue;			/* oops, GETPID failed */
		if (creatorPID != getpid())
		{
			if (kill(creatorPID, 0) == 0 ||
				errno != ESRCH)
				continue;		/* sema belongs to a live process */
		}
		/*
		 * The sema set appears to be from a dead Postgres process, or
		 * from a previous cycle of life in this same process.  Zap it,
		 * if possible.  This probably shouldn't fail, but if it does,
		 * assume the sema set belongs to someone else after all,
		 * and continue quietly.
		 */
		semun.val = 0;			/* unused, but keep compiler quiet */
		if (semctl(semId, 0, IPC_RMID, semun) < 0)
			continue;
		/*
		 * Now try again to create the sema set.
		 */
		semId = InternalIpcSemaphoreCreate(NextSemaID, numSems+1,
										   permission, semStartValue,
										   removeOnExit);
		if (semId >= 0)
			break;				/* successful create */
		/*
		 * Can only get here if some other process managed to create the
		 * same sema key before we did.  Let him have that one,
		 * loop around to try next key.
		 */
	}
	/*
	 * OK, we created a new sema set.  Mark it as created by this process.
	 * We do this by setting the spare semaphore to PGSemaMagic-1 and then
	 * incrementing it with semop().  That leaves it with value PGSemaMagic
	 * and sempid referencing this process.
	 */
	semun.val = PGSemaMagic-1;
	if (semctl(semId, numSems, SETVAL, semun) < 0)
	{
		fprintf(stderr, "IpcSemaphoreCreate: semctl(id=%d, %d, SETVAL, %d) failed: %s\n",
				semId, numSems, PGSemaMagic-1, strerror(errno));

		if (errno == ERANGE)
			fprintf(stderr,
					"You possibly need to raise your kernel's SEMVMX value to be at least\n"
					"%d.  Look into the PostgreSQL documentation for details.\n",
					PGSemaMagic);

		proc_exit(1);
	}
	IpcSemaphoreUnlock(semId, numSems);

	return semId;
}
