/*-------------------------------------------------------------------------
 *
 * ipc.c
 *	  POSTGRES inter-process communication definitions.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/ipc/ipc.c,v 1.52 2000/10/07 14:39:12 momjian Exp $
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
#include "miscadmin.h"
#include "utils/memutils.h"
#include "libpq/libpq.h"

#if defined(solaris_sparc)
#include <sys/ipc.h>
#endif

/*
 * This flag is set during proc_exit() to change elog()'s behavior,
 * so that an elog() from an on_proc_exit routine cannot get us out
 * of the exit procedure.  We do NOT want to go back to the idle loop...
 */
bool		proc_exit_inprogress = false;

static int	UsePrivateMemory = 0;

static void IpcMemoryDetach(int status, char *shmaddr);

/* ----------------------------------------------------------------
 *						exit() handling stuff
 * ----------------------------------------------------------------
 */

#define MAX_ON_EXITS 20

static struct ONEXIT
{
	void		(*function) ();
	Datum		arg;
}			on_proc_exit_list[MAX_ON_EXITS], on_shmem_exit_list[MAX_ON_EXITS];

static int	on_proc_exit_index,
			on_shmem_exit_index;

typedef struct _PrivateMemStruct
{
	int			id;
	char	   *memptr;
} PrivateMem;

static PrivateMem IpcPrivateMem[16];


static int
PrivateMemoryCreate(IpcMemoryKey memKey,
					uint32 size)
{
	static int	memid = 0;

	UsePrivateMemory = 1;

	IpcPrivateMem[memid].id = memid;
	IpcPrivateMem[memid].memptr = malloc(size);
	if (IpcPrivateMem[memid].memptr == NULL)
		elog(ERROR, "PrivateMemoryCreate: not enough memory to malloc");
	MemSet(IpcPrivateMem[memid].memptr, 0, size);		/* XXX PURIFY */

	return memid++;
}

static char *
PrivateMemoryAttach(IpcMemoryId memid)
{
	return IpcPrivateMem[memid].memptr;
}


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
 * Run all of the on_shmem_exit routines but don't exit in the end.
 * This is used by the postmaster to re-initialize shared memory and
 * semaphores after a backend dies horribly
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
int
on_proc_exit(void (*function) (), Datum arg)
{
	if (on_proc_exit_index >= MAX_ON_EXITS)
		return -1;

	on_proc_exit_list[on_proc_exit_index].function = function;
	on_proc_exit_list[on_proc_exit_index].arg = arg;

	++on_proc_exit_index;

	return 0;
}

/* ----------------------------------------------------------------
 *		on_shmem_exit
 *
 *		this function adds a callback function to the list of
 *		functions invoked by shmem_exit().	-cim 2/6/90
 * ----------------------------------------------------------------
 */
int
on_shmem_exit(void (*function) (), Datum arg)
{
	if (on_shmem_exit_index >= MAX_ON_EXITS)
		return -1;

	on_shmem_exit_list[on_shmem_exit_index].function = function;
	on_shmem_exit_list[on_shmem_exit_index].arg = arg;

	++on_shmem_exit_index;

	return 0;
}

/* ----------------------------------------------------------------
 *		on_exit_reset
 *
 *		this function clears all proc_exit() registered functions.
 * ----------------------------------------------------------------
 */
void
on_exit_reset(void)
{
	on_shmem_exit_index = 0;
	on_proc_exit_index = 0;
}

/****************************************************************************/
/*	 IPCPrivateSemaphoreKill(status, semId)									*/
/*																			*/
/****************************************************************************/
static void
IPCPrivateSemaphoreKill(int status, int semId)
{
	union semun semun;
	semun.val = 0;		/* unused */

	if (semctl(semId, 0, IPC_RMID, semun) == -1)
		elog(NOTICE, "IPCPrivateSemaphoreKill: semctl(%d, 0, IPC_RMID, ...) failed: %s",
			 semId, strerror(errno));
}


/****************************************************************************/
/*	 IPCPrivateMemoryKill(status, shmId)									*/
/*																			*/
/****************************************************************************/
static void
IPCPrivateMemoryKill(int status, int shmId)
{
	if (UsePrivateMemory)
	{
		/* free ( IpcPrivateMem[shmId].memptr ); */
	}
	else
	{
		if (shmctl(shmId, IPC_RMID, (struct shmid_ds *) NULL) < 0)
  		{
  			elog(NOTICE, "IPCPrivateMemoryKill: shmctl(%d, %d, 0) failed: %m",
  				 shmId, IPC_RMID);
  		}
    }
}

/*
 * Note:
 * XXX	This should be split into two different calls.	One should
 * XXX	be used to create a semaphore set.	The other to "attach" a
 * XXX	existing set.  It should be an error for the semaphore set
 * XXX	to to already exist or for it not to, respectively.
 *
 *		Currently, the semaphore sets are "attached" and an error
 *		is detected only when a later shared memory attach fails.
 */

IpcSemaphoreId
IpcSemaphoreCreate(IpcSemaphoreKey semKey,
				   int semNum,
				   int permission,
				   int semStartValue,
				   int removeOnExit)
{
	int			semId;
	int			i;
	int			errStatus;
	u_short		array[IPC_NMAXSEM];
	union semun semun;

	/* check arguments	*/
	if (semNum > IPC_NMAXSEM || semNum <= 0)
		return (-1);

	semId = semget(semKey, 0, 0);

	if (semId == -1)
	{
#ifdef DEBUG_IPC
		fprintf(stderr, "calling semget(%d, %d, 0%o)\n",
			semKey, semNum, (unsigned)(IPC_CREAT|permission));
#endif

		semId = semget(semKey, semNum, IPC_CREAT | permission);

		if (semId < 0)
		{
			fprintf(stderr, "IpcSemaphoreCreate: semget(key=%d, num=%d, 0%o) failed: %s\n",
					semKey, semNum, (unsigned)(permission|IPC_CREAT),
					strerror(errno));

			if (errno == ENOSPC)
				fprintf(stderr,
						"\nThis error does *not* mean that you have run out of disk space.\n\n"
						"It occurs either because system limit for the maximum number of\n"
						"semaphore sets (SEMMNI), or the system wide maximum number of\n"
						"semaphores (SEMMNS), would be exceeded.  You need to raise the\n"
						"respective kernel parameter.  Look into the PostgreSQL documentation\n"
						"for details.\n\n");

			return (-1);
		}
		for (i = 0; i < semNum; i++)
			array[i] = semStartValue;
		semun.array = array;
		errStatus = semctl(semId, 0, SETALL, semun);
		if (errStatus == -1)
		{
			fprintf(stderr, "IpcSemaphoreCreate: semctl(id=%d, 0, SETALL, ...) failed: %s\n",
					semId, strerror(errno));

			if (errno == ERANGE)
				fprintf(stderr,
						"You possibly need to raise your kernel's SEMVMX value to be at least\n"
						"%d.  Look into the PostgreSQL documentation for details.\n",
						semStartValue);

			semctl(semId, 0, IPC_RMID, semun);
			return (-1);
		}

		if (removeOnExit)
			on_shmem_exit(IPCPrivateSemaphoreKill, (Datum) semId);
	}


#ifdef DEBUG_IPC
	fprintf(stderr, "IpcSemaphoreCreate returns %d\n", semId);
	fflush(stdout);
	fflush(stderr);
#endif

	return semId;
}


/****************************************************************************/
/*	 IpcSemaphoreSet()			- sets the initial value of the semaphore	*/
/*																			*/
/*		note: the xxx_return variables are only used for debugging.			*/
/****************************************************************************/
#ifdef NOT_USED
static int	IpcSemaphoreSet_return;

void
IpcSemaphoreSet(int semId, int semno, int value)
{
	int			errStatus;
	union semun semun;

	semun.val = value;
	errStatus = semctl(semId, semno, SETVAL, semun);
	IpcSemaphoreSet_return = errStatus;

	if (errStatus == -1)
        fprintf(stderr, "IpcSemaphoreSet: semctl(id=%d) failed: %s\n",
				semId, strerror(errno));
}

#endif /* NOT_USED */

/****************************************************************************/
/*	 IpcSemaphoreKill(key)		- removes a semaphore						*/
/*																			*/
/****************************************************************************/
void
IpcSemaphoreKill(IpcSemaphoreKey key)
{
	int			semId;
	union semun semun;
	semun.val = 0;		/* unused */

	/* kill semaphore if existent */

	semId = semget(key, 0, 0);
	if (semId != -1)
		semctl(semId, 0, IPC_RMID, semun);
}

/****************************************************************************/
/*	 IpcSemaphoreLock(semId, sem, lock) - locks a semaphore					*/
/*																			*/
/*		note: the xxx_return variables are only used for debugging.			*/
/****************************************************************************/
static int	IpcSemaphoreLock_return;

void
IpcSemaphoreLock(IpcSemaphoreId semId, int sem, int lock)
{
	extern int	errno;
	int			errStatus;
	struct sembuf sops;

	sops.sem_op = lock;
	sops.sem_flg = 0;
	sops.sem_num = sem;

	/* ----------------
	 *	Note: if errStatus is -1 and errno == EINTR then it means we
	 *		  returned from the operation prematurely because we were
	 *		  sent a signal.  So we try and lock the semaphore again.
	 *		  I am not certain this is correct, but the semantics aren't
	 *		  clear it fixes problems with parallel abort synchronization,
	 *		  namely that after processing an abort signal, the semaphore
	 *		  call returns with -1 (and errno == EINTR) before it should.
	 *		  -cim 3/28/90
	 * ----------------
	 */
	do
	{
		errStatus = semop(semId, &sops, 1);
	} while (errStatus == -1 && errno == EINTR);

	IpcSemaphoreLock_return = errStatus;

	if (errStatus == -1)
	{
        fprintf(stderr, "IpcSemaphoreLock: semop(id=%d) failed: %s\n",
				semId, strerror(errno));
		proc_exit(255);
	}
}

/****************************************************************************/
/*	 IpcSemaphoreUnlock(semId, sem, lock)		- unlocks a semaphore		*/
/*																			*/
/*		note: the xxx_return variables are only used for debugging.			*/
/****************************************************************************/
static int	IpcSemaphoreUnlock_return;

void
IpcSemaphoreUnlock(IpcSemaphoreId semId, int sem, int lock)
{
	extern int	errno;
	int			errStatus;
	struct sembuf sops;

	sops.sem_op = -lock;
	sops.sem_flg = 0;
	sops.sem_num = sem;


	/* ----------------
	 *	Note: if errStatus is -1 and errno == EINTR then it means we
	 *		  returned from the operation prematurely because we were
	 *		  sent a signal.  So we try and lock the semaphore again.
	 *		  I am not certain this is correct, but the semantics aren't
	 *		  clear it fixes problems with parallel abort synchronization,
	 *		  namely that after processing an abort signal, the semaphore
	 *		  call returns with -1 (and errno == EINTR) before it should.
	 *		  -cim 3/28/90
	 * ----------------
	 */
	do
	{
		errStatus = semop(semId, &sops, 1);
	} while (errStatus == -1 && errno == EINTR);

	IpcSemaphoreUnlock_return = errStatus;

	if (errStatus == -1)
	{
		fprintf(stderr, "IpcSemaphoreUnlock: semop(id=%d) failed: %s\n",
				semId, strerror(errno));
		proc_exit(255);
	}
}

int
IpcSemaphoreGetCount(IpcSemaphoreId semId, int sem)
{
	int			semncnt;
	union semun dummy;			/* for Solaris */
	dummy.val = 0;		/* unused */

	semncnt = semctl(semId, sem, GETNCNT, dummy);
	return semncnt;
}

int
IpcSemaphoreGetValue(IpcSemaphoreId semId, int sem)
{
	int			semval;
	union semun dummy;			/* for Solaris */
	dummy.val = 0;		/* unused */

	semval = semctl(semId, sem, GETVAL, dummy);
	return semval;
}

/****************************************************************************/
/*	 IpcMemoryCreate(memKey)												*/
/*																			*/
/*	  - returns the memory identifier, if creation succeeds					*/
/*		returns IpcMemCreationFailed, if failure							*/
/****************************************************************************/

IpcMemoryId
IpcMemoryCreate(IpcMemoryKey memKey, uint32 size, int permission)
{
	IpcMemoryId shmid;

	if (memKey == PrivateIPCKey)
	{
		/* private */
		shmid = PrivateMemoryCreate(memKey, size);
	}
	else

		shmid = shmget(memKey, size, IPC_CREAT | permission);

	if (shmid < 0)
	{
		fprintf(stderr, "IpcMemoryCreate: shmget(key=%d, size=%d, 0%o) failed: %s\n",
				(int)memKey, size, (unsigned)(IPC_CREAT|permission),
				strerror(errno));

		if (errno == EINVAL)
			fprintf(stderr,
					"\nThis error can be caused by one of three things:\n\n"
					"1. The maximum size for shared memory segments on your system was\n"
					"   exceeded.  You need to raise the SHMMAX parameter in your kernel\n"
					"   to be at least %d bytes.\n\n"
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

		return IpcMemCreationFailed;
	}


	/* if (memKey == PrivateIPCKey) */
	on_shmem_exit(IPCPrivateMemoryKill, (Datum) shmid);

	return shmid;
}

/****************************************************************************/
/*	IpcMemoryIdGet(memKey, size)	returns the shared memory Id			*/
/*									or IpcMemIdGetFailed					*/
/****************************************************************************/
IpcMemoryId
IpcMemoryIdGet(IpcMemoryKey memKey, uint32 size)
{
	IpcMemoryId shmid;

	shmid = shmget(memKey, size, 0);

	if (shmid < 0)
	{
		fprintf(stderr, "IpcMemoryIdGet: shmget(key=%d, size=%d, 0) failed: %s\n",
				memKey, size, strerror(errno));
		return IpcMemIdGetFailed;
	}

	return shmid;
}

/****************************************************************************/
/*	IpcMemoryDetach(status, shmaddr)	removes a shared memory segment		*/
/*										from a backend address space		*/
/*	(only called by backends running under the postmaster)					*/
/****************************************************************************/
static void
IpcMemoryDetach(int status, char *shmaddr)
{
	if (shmdt(shmaddr) < 0)
		elog(NOTICE, "IpcMemoryDetach: shmdt(0x%p) failed: %m", shmaddr);
}

/****************************************************************************/
/*	IpcMemoryAttach(memId)	  returns the adress of shared memory			*/
/*							  or IpcMemAttachFailed							*/
/*																			*/
/* CALL IT:  addr = (struct <MemoryStructure> *) IpcMemoryAttach(memId);	*/
/*																			*/
/****************************************************************************/
char *
IpcMemoryAttach(IpcMemoryId memId)
{
	char	   *memAddress;

	if (UsePrivateMemory)
		memAddress = (char *) PrivateMemoryAttach(memId);
	else
		memAddress = (char *) shmat(memId, 0, 0);

	/* if ( *memAddress == -1) { XXX ??? */
	if (memAddress == (char *) -1)
	{
        fprintf(stderr, "IpcMemoryAttach: shmat(id=%d) failed: %s\n",
				memId, strerror(errno));
		return IpcMemAttachFailed;
	}

	if (!UsePrivateMemory)
		on_shmem_exit(IpcMemoryDetach, PointerGetDatum(memAddress));

	return (char *) memAddress;
}


/****************************************************************************/
/*	IpcMemoryKill(memKey)				removes a shared memory segment		*/
/*	(only called by the postmaster and standalone backends)					*/
/****************************************************************************/
void
IpcMemoryKill(IpcMemoryKey memKey)
{
	IpcMemoryId shmid;

	if (!UsePrivateMemory && (shmid = shmget(memKey, 0, 0)) >= 0)
	{
		if (shmctl(shmid, IPC_RMID, (struct shmid_ds *) NULL) < 0)
		{
			elog(NOTICE, "IpcMemoryKill: shmctl(%d, %d, 0) failed: %m",
				 shmid, IPC_RMID);
		}
	}
}

#ifdef HAS_TEST_AND_SET
/* ------------------
 *	use hardware locks to replace semaphores for sequent machines
 *	to avoid costs of swapping processes and to provide unlimited
 *	supply of locks.
 * ------------------
 */

/* used in spin.c */
SLock	   *SLockArray = NULL;

static SLock **FreeSLockPP;
static int *UnusedSLockIP;
static slock_t *SLockMemoryLock;
static IpcMemoryId SLockMemoryId = -1;

struct ipcdummy
{								/* to get alignment/size right */
	SLock	   *free;
	int			unused;
	slock_t		memlock;
	SLock		slocks[MAX_SPINS + 1];
};

#define SLOCKMEMORYSIZE		sizeof(struct ipcdummy)

void
CreateAndInitSLockMemory(IPCKey key)
{
	int			id;
	SLock	   *slckP;

	SLockMemoryId = IpcMemoryCreate(key,
									SLOCKMEMORYSIZE,
									0700);
	AttachSLockMemory(key);
	*FreeSLockPP = NULL;
	*UnusedSLockIP = (int) FIRSTFREELOCKID;
	for (id = 0; id < (int) FIRSTFREELOCKID; id++)
	{
		slckP = &(SLockArray[id]);
		S_INIT_LOCK(&(slckP->locklock));
		slckP->flag = NOLOCK;
		slckP->nshlocks = 0;
		S_INIT_LOCK(&(slckP->shlock));
		S_INIT_LOCK(&(slckP->exlock));
		S_INIT_LOCK(&(slckP->comlock));
		slckP->next = NULL;
	}
	return;
}

void
AttachSLockMemory(IPCKey key)
{
	struct ipcdummy *slockM;

	if (SLockMemoryId == -1)
		SLockMemoryId = IpcMemoryIdGet(key, SLOCKMEMORYSIZE);
	if (SLockMemoryId == -1)
		elog(FATAL, "SLockMemory not in shared memory");
	slockM = (struct ipcdummy *) IpcMemoryAttach(SLockMemoryId);
	if (slockM == IpcMemAttachFailed)
		elog(FATAL, "AttachSLockMemory: could not attach segment");
	FreeSLockPP = (SLock **) &(slockM->free);
	UnusedSLockIP = (int *) &(slockM->unused);
	SLockMemoryLock = (slock_t *) &(slockM->memlock);
	S_INIT_LOCK(SLockMemoryLock);
	SLockArray = (SLock *) &(slockM->slocks[0]);
	return;
}

#ifdef NOT_USED
bool
LockIsFree(int lockid)
{
	return SLockArray[lockid].flag == NOLOCK;
}

#endif

#endif	 /* HAS_TEST_AND_SET */
