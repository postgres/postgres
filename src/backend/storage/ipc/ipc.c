/*-------------------------------------------------------------------------
 *
 * ipc.c--
 *	  POSTGRES inter-process communication definitions.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/ipc/ipc.c,v 1.15 1997/09/18 14:20:14 momjian Exp $
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
#include <sys/types.h>
#include <sys/file.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>


#include "postgres.h"
#include "storage/ipc.h"
#include "storage/s_lock.h"
/* In Ultrix, sem.h and shm.h must be included AFTER ipc.h */
#include <sys/sem.h>
#include <sys/shm.h>
#include "utils/memutils.h"

#if defined(sparc_solaris)
#include <string.h>
#include <sys/ipc.h>
#endif

#if defined(bsd44)
int			UsePrivateMemory = 1;

#else
int			UsePrivateMemory = 0;

#endif

static void IpcMemoryDetach(int status, char *shmaddr);

/* ----------------------------------------------------------------
 *						exit() handling stuff
 * ----------------------------------------------------------------
 */

#define MAX_ON_EXITS 20

static struct ONEXIT
{
	void		(*function) ();
	caddr_t		arg;
}			onexit_list[MAX_ON_EXITS];

static int	onexit_index;
static void IpcConfigTip(void);

typedef struct _PrivateMemStruct
{
	int			id;
	char	   *memptr;
} PrivateMem;

PrivateMem	IpcPrivateMem[16];

static int
PrivateMemoryCreate(IpcMemoryKey memKey,
					uint32 size)
{
	static int	memid = 0;

	UsePrivateMemory = 1;

	IpcPrivateMem[memid].id = memid;
	IpcPrivateMem[memid].memptr = malloc(size);
	if (IpcPrivateMem[memid].memptr == NULL)
		elog(WARN, "PrivateMemoryCreate: not enough memory to malloc");
	memset(IpcPrivateMem[memid].memptr, 0, size);		/* XXX PURIFY */

	return (memid++);
}

static char *
PrivateMemoryAttach(IpcMemoryId memid)
{
	return (IpcPrivateMem[memid].memptr);
}


/* ----------------------------------------------------------------
 *		exitpg
 *
 *		this function calls all the callbacks registered
 *		for it (to free resources) and then calls exit.
 *		This should be the only function to call exit().
 *		-cim 2/6/90
 * ----------------------------------------------------------------
 */
static int	exitpg_inprogress = 0;

void
exitpg(int code)
{
	int			i;

	/* ----------------
	 *	if exitpg_inprocess is true, then it means that we
	 *	are being invoked from within an on_exit() handler
	 *	and so we return immediately to avoid recursion.
	 * ----------------
	 */
	if (exitpg_inprogress)
		return;

	exitpg_inprogress = 1;

	/* ----------------
	 *	call all the callbacks registered before calling exit().
	 * ----------------
	 */
	for (i = onexit_index - 1; i >= 0; --i)
		(*onexit_list[i].function) (code, onexit_list[i].arg);

	exit(code);
}

/* ------------------
 * Run all of the on_exitpg routines but don't exit in the end.
 * This is used by the postmaster to re-initialize shared memory and
 * semaphores after a backend dies horribly
 * ------------------
 */
void
quasi_exitpg()
{
	int			i;

	/* ----------------
	 *	if exitpg_inprocess is true, then it means that we
	 *	are being invoked from within an on_exit() handler
	 *	and so we return immediately to avoid recursion.
	 * ----------------
	 */
	if (exitpg_inprogress)
		return;

	exitpg_inprogress = 1;

	/* ----------------
	 *	call all the callbacks registered before calling exit().
	 * ----------------
	 */
	for (i = onexit_index - 1; i >= 0; --i)
		(*onexit_list[i].function) (0, onexit_list[i].arg);

	onexit_index = 0;
	exitpg_inprogress = 0;
}

/* ----------------------------------------------------------------
 *		on_exitpg
 *
 *		this function adds a callback function to the list of
 *		functions invoked by exitpg().	-cim 2/6/90
 * ----------------------------------------------------------------
 */
int
			on_exitpg(void (*function) (), caddr_t arg)
{
	if (onexit_index >= MAX_ON_EXITS)
		return (-1);

	onexit_list[onexit_index].function = function;
	onexit_list[onexit_index].arg = arg;

	++onexit_index;

	return (0);
}

/****************************************************************************/
/*	 IPCPrivateSemaphoreKill(status, semId)									*/
/*																			*/
/****************************************************************************/
static void
IPCPrivateSemaphoreKill(int status,
						int semId)		/* caddr_t */
{
	union semun semun;

	semctl(semId, 0, IPC_RMID, semun);
}


/****************************************************************************/
/*	 IPCPrivateMemoryKill(status, shmId)									*/
/*																			*/
/****************************************************************************/
static void
IPCPrivateMemoryKill(int status,
					 int shmId) /* caddr_t */
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


/****************************************************************************/
/*	 IpcSemaphoreCreate(semKey, semNum, permission, semStartValue)			*/
/*																			*/
/*	  - returns a semaphore identifier:										*/
/*																			*/
/* if key doesn't exist: return a new id,      status:= IpcSemIdNotExist    */
/* if key exists:		 return the old id,    status:= IpcSemIdExist		*/
/* if semNum > MAX :	 return # of argument, status:=IpcInvalidArgument	*/
/*																			*/
/****************************************************************************/

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
				   int removeOnExit,
				   int *status)
{
	int			i;
	int			errStatus;
	int			semId;
	u_short		array[IPC_NMAXSEM];
	union semun semun;

	/* get a semaphore if non-existent */
	/* check arguments	*/
	if (semNum > IPC_NMAXSEM || semNum <= 0)
	{
		*status = IpcInvalidArgument;
		return (2);				/* returns the number of the invalid
								 * argument   */
	}

	semId = semget(semKey, 0, 0);

	if (semId == -1)
	{
		*status = IpcSemIdNotExist;		/* there doesn't exist a semaphore */
#ifdef DEBUG_IPC
		fprintf(stderr, "calling semget with %d, %d , %d\n",
				semKey,
				semNum,
				IPC_CREAT | permission);
#endif
		semId = semget(semKey, semNum, IPC_CREAT | permission);

		if (semId < 0)
		{
			perror("semget");
			IpcConfigTip();
			exitpg(3);
		}
		for (i = 0; i < semNum; i++)
		{
			array[i] = semStartValue;
		}
		semun.array = array;
		errStatus = semctl(semId, 0, SETALL, semun);
		if (errStatus == -1)
		{
			perror("semctl");
			IpcConfigTip();
		}

		if (removeOnExit)
			on_exitpg(IPCPrivateSemaphoreKill, (caddr_t) semId);

	}
	else
	{
		/* there is a semaphore id for this key */
		*status = IpcSemIdExist;
	}

#ifdef DEBUG_IPC
	fprintf(stderr, "\nIpcSemaphoreCreate, status %d, returns %d\n",
			*status,
			semId);
	fflush(stdout);
	fflush(stderr);
#endif
	return (semId);
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
	{
		perror("semctl");
		IpcConfigTip();
	}
}

#endif

/****************************************************************************/
/*	 IpcSemaphoreKill(key)		- removes a semaphore						*/
/*																			*/
/****************************************************************************/
void
IpcSemaphoreKill(IpcSemaphoreKey key)
{
	int			semId;
	union semun semun;

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
		perror("semop");
		IpcConfigTip();
		exitpg(255);
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
		perror("semop");
		IpcConfigTip();
		exitpg(255);
	}
}

int
IpcSemaphoreGetCount(IpcSemaphoreId semId, int sem)
{
	int			semncnt;
	union semun dummy;			/* for Solaris */

	semncnt = semctl(semId, sem, GETNCNT, dummy);
	return semncnt;
}

int
IpcSemaphoreGetValue(IpcSemaphoreId semId, int sem)
{
	int			semval;
	union semun dummy;			/* for Solaris */

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
	{
		shmid = shmget(memKey, size, IPC_CREAT | permission);
	}

	if (shmid < 0)
	{
		fprintf(stderr, "IpcMemoryCreate: memKey=%d , size=%d , permission=%d",
				memKey, size, permission);
		perror("IpcMemoryCreate: shmget(..., create, ...) failed");
		IpcConfigTip();
		return (IpcMemCreationFailed);
	}

	/* if (memKey == PrivateIPCKey) */
	on_exitpg(IPCPrivateMemoryKill, (caddr_t) shmid);

	return (shmid);
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
		fprintf(stderr, "IpcMemoryIdGet: memKey=%d , size=%d , permission=%d",
				memKey, size, 0);
		perror("IpcMemoryIdGet:  shmget() failed");
		IpcConfigTip();
		return (IpcMemIdGetFailed);
	}

	return (shmid);
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
	{
		elog(NOTICE, "IpcMemoryDetach: shmdt(0x%x): %m", shmaddr);
	}
}

/****************************************************************************/
/*	IpcMemoryAttach(memId)	  returns the adress of shared memory			*/
/*							  or IpcMemAttachFailed							*/
/*																			*/
/* CALL IT:  addr = (struct <MemoryStructure> *) IpcMemoryAttach(memId);	*/
/*																			*/
/****************************************************************************/
char	   *
IpcMemoryAttach(IpcMemoryId memId)
{
	char	   *memAddress;

	if (UsePrivateMemory)
	{
		memAddress = (char *) PrivateMemoryAttach(memId);
	}
	else
	{
		memAddress = (char *) shmat(memId, 0, 0);
	}

	/* if ( *memAddress == -1) { XXX ??? */
	if (memAddress == (char *) -1)
	{
		perror("IpcMemoryAttach: shmat() failed");
		IpcConfigTip();
		return (IpcMemAttachFailed);
	}

	if (!UsePrivateMemory)
		on_exitpg(IpcMemoryDetach, (caddr_t) memAddress);

	return ((char *) memAddress);
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
SLock *SLockArray = NULL;

static SLock **FreeSLockPP;
static int *UnusedSLockIP;
static slock_t *SLockMemoryLock;
static IpcMemoryId SLockMemoryId = -1;

struct ipcdummy
{								/* to get alignment/size right */
	SLock	   *free;
	int			unused;
	slock_t		memlock;
	SLock		slocks[NSLOCKS];
};
static int	SLockMemorySize = sizeof(struct ipcdummy);

void
CreateAndInitSLockMemory(IPCKey key)
{
	int			id;
	SLock	   *slckP;

	SLockMemoryId = IpcMemoryCreate(key,
									SLockMemorySize,
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
		SLockMemoryId = IpcMemoryIdGet(key, SLockMemorySize);
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
	return (SLockArray[lockid].flag == NOLOCK);
}
#endif

#endif							/* HAS_TEST_AND_SET */

static void
IpcConfigTip(void)
{
	fprintf(stderr, "This type of error is usually caused by improper\n");
	fprintf(stderr, "shared memory or System V IPC semaphore configuration.\n");
	fprintf(stderr, "See the FAQ for more detailed information\n");
}
