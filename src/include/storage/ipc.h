/*-------------------------------------------------------------------------
 *
 * ipc.h--
 *    POSTGRES inter-process communication definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: ipc.h,v 1.2 1996/10/02 20:40:17 scrappy Exp $
 *
 * NOTES
 *    This file is very architecture-specific.  This stuff should actually
 *    be factored into the port/ directories.
 *
 *-------------------------------------------------------------------------
 */
#ifndef	IPC_H
#define IPC_H

#include <sys/types.h>
#ifndef	_IPC_
#define _IPC_
#include <sys/ipc.h>
#endif

#include "c.h"

/*
 * Many architectures have support for user-level spinlocks (i.e., an
 * atomic test-and-set instruction).  However, we have only written
 * spinlock code for the architectures listed.
 */
#if defined(PORTNAME_aix) || \
    defined(PORTNAME_alpha) || \
    defined(PORTNAME_BSD44_derived) || \
    defined(PORTNAME_bsdi) || \
    defined(PORTNAME_bsdi_2_1) || \
    defined(PORTNAME_hpux) || \
    defined(PORTNAME_i386_solaris) || \
    defined(PORTNAME_irix5) || \
    defined(PORTNAME_linux) || \
    defined(PORTNAME_next) || \
    defined(PORTNAME_sparc) || \
    defined(PORTNAME_sparc_solaris)
#define HAS_TEST_AND_SET
#endif

#if defined(HAS_TEST_AND_SET)

#if defined(PORTNAME_aix)
/*
 * The AIX C library has the cs(3) builtin for compare-and-set that 
 * operates on ints.
 */
typedef unsigned int	slock_t;
#else /* aix */

#if defined(PORTNAME_alpha)
#include <sys/mman.h>
typedef msemaphore	slock_t;
#else /* alpha */

#if defined(PORTNAME_hpux)
/*
 * The PA-RISC "semaphore" for the LDWCX instruction is 4 bytes aligned
 * to a 16-byte boundary.
 */
typedef struct { int sem[4]; } slock_t;
#else /* hpux */

#if defined(PORTNAME_irix5)
#include <abi_mutex.h>
typedef abilock_t	slock_t;
#else /* irix5 */

#if defined(PORTNAME_next)
/*
 * Use Mach mutex routines since these are, in effect, test-and-set
 * spinlocks.
 */
#undef NEVER	/* definition in cthreads.h conflicts with parse.h */
#include <mach/cthreads.h>
typedef struct mutex	slock_t;
#else /* next */

/*
 * On all other architectures spinlocks are a single byte.
 */
typedef unsigned char   slock_t;

#endif /* next */
#endif /* irix5 */
#endif /* hpux */
#endif /* alpha */
#endif /* aix */

extern void S_LOCK(slock_t *lock);
extern void S_UNLOCK(slock_t *lock);
extern void S_INIT_LOCK(slock_t *lock);

#if defined(PORTNAME_alpha) || \
    defined(PORTNAME_hpux) || \
    defined(PORTNAME_irix5) || \
    defined(PORTNAME_next)
extern int S_LOCK_FREE(slock_t *lock);
#else
#define S_LOCK_FREE(lock)	((*lock) == 0)
#endif

#endif /* HAS_TEST_AND_SET */

#ifdef NEED_UNION_SEMUN
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};
#endif

typedef uint16	SystemPortAddress;

/* semaphore definitions */

#define IPCProtection	(0600)		/* access/modify by user only */

#define IPC_NMAXSEM	25		/* maximum number of semaphores */
#define IpcSemaphoreDefaultStartValue	255
#define IpcSharedLock					(-1)
#define IpcExclusiveLock			  (-255)

#define IpcUnknownStatus	(-1)
#define IpcInvalidArgument	(-2)
#define IpcSemIdExist		(-3)
#define IpcSemIdNotExist	(-4)

typedef uint32	IpcSemaphoreKey;		/* semaphore key */
typedef int	IpcSemaphoreId;

/* shared memory definitions */ 

#define IpcMemCreationFailed	(-1)
#define IpcMemIdGetFailed	(-2)
#define IpcMemAttachFailed	0

typedef uint32	IPCKey;
#define PrivateIPCKey	IPC_PRIVATE
#define DefaultIPCKey	17317

typedef uint32  IpcMemoryKey;			/* shared memory key */
typedef int	IpcMemoryId;


/* ipc.c */
extern void exitpg(int code);
extern void quasi_exitpg(void);
extern on_exitpg(void (*function)(), caddr_t arg);

extern IpcSemaphoreId IpcSemaphoreCreate(IpcSemaphoreKey semKey,
		int semNum, int permission, int semStartValue,
		int removeOnExit, int *status);
extern void IpcSemaphoreSet(int semId, int semno, int value);
extern void IpcSemaphoreKill(IpcSemaphoreKey key);
extern void IpcSemaphoreLock(IpcSemaphoreId semId, int sem, int lock);
extern void IpcSemaphoreUnlock(IpcSemaphoreId semId, int sem, int lock);
extern int IpcSemaphoreGetCount(IpcSemaphoreId semId, int sem);
extern int IpcSemaphoreGetValue(IpcSemaphoreId semId, int sem);
extern IpcMemoryId IpcMemoryCreate(IpcMemoryKey memKey, uint32 size,
				   int permission);
extern IpcMemoryId IpcMemoryIdGet(IpcMemoryKey memKey, uint32 size);
extern void IpcMemoryDetach(int status, char *shmaddr);
extern char *IpcMemoryAttach(IpcMemoryId memId);
extern void IpcMemoryKill(IpcMemoryKey memKey);
extern void CreateAndInitSLockMemory(IPCKey key);
extern void AttachSLockMemory(IPCKey key);


#ifdef HAS_TEST_AND_SET

#define NSLOCKS		2048
#define	NOLOCK		0
#define SHAREDLOCK	1
#define EXCLUSIVELOCK	2

typedef enum _LockId_ {
    BUFMGRLOCKID,
    LOCKLOCKID,
    OIDGENLOCKID,
    SHMEMLOCKID,
    BINDINGLOCKID,
    LOCKMGRLOCKID,
    SINVALLOCKID,

#ifdef MAIN_MEMORY
    MMCACHELOCKID,
#endif /* MAIN_MEMORY */

    PROCSTRUCTLOCKID,
    FIRSTFREELOCKID
} _LockId_;

#define MAX_SPINS	FIRSTFREELOCKID

typedef struct slock {
    slock_t		locklock;
    unsigned char	flag;
    short		nshlocks;
    slock_t		shlock;
    slock_t		exlock;
    slock_t		comlock;
    struct slock	*next;
} SLock;

extern void ExclusiveLock(int lockid);
extern void ExclusiveUnlock(int lockid);
extern bool LockIsFree(int lockid);
#else /* HAS_TEST_AND_SET */

typedef enum _LockId_ {
    SHMEMLOCKID,
    BINDINGLOCKID,
    BUFMGRLOCKID,
    LOCKMGRLOCKID,
    SINVALLOCKID,

#ifdef MAIN_MEMORY
    MMCACHELOCKID,
#endif /* MAIN_MEMORY */

    PROCSTRUCTLOCKID,
    OIDGENLOCKID,
    FIRSTFREELOCKID
} _LockId_;

#define MAX_SPINS	FIRSTFREELOCKID

#endif /* HAS_TEST_AND_SET */

/*
 * the following are originally in ipci.h but the prototypes have circular
 * dependencies and most files include both ipci.h and ipc.h anyway, hence
 * combined.
 *
 */

/*
 * Note:
 *	These must not hash to DefaultIPCKey or PrivateIPCKey.
 */
#define SystemPortAddressGetIPCKey(address) \
	(28597 * (address) + 17491)

/*
 * these keys are originally numbered from 1 to 12 consecutively but not
 * all are used. The unused ones are removed.		- ay 4/95.
 */
#define IPCKeyGetBufferMemoryKey(key) \
	((key == PrivateIPCKey) ? key : 1 + (key))

#define IPCKeyGetSIBufferMemoryBlock(key) \
	((key == PrivateIPCKey) ? key : 7 + (key))

#define IPCKeyGetSLockSharedMemoryKey(key) \
	((key == PrivateIPCKey) ? key : 10 + (key))

#define IPCKeyGetSpinLockSemaphoreKey(key) \
	((key == PrivateIPCKey) ? key : 11 + (key))
#define IPCKeyGetWaitIOSemaphoreKey(key) \
	((key == PrivateIPCKey) ? key : 12 + (key))

/* --------------------------
 * NOTE: This macro must always give the highest numbered key as every backend
 * process forked off by the postmaster will be trying to acquire a semaphore
 * with a unique key value starting at key+14 and incrementing up.  Each
 * backend uses the current key value then increments it by one.
 * --------------------------
 */
#define IPCGetProcessSemaphoreInitKey(key) \
	((key == PrivateIPCKey) ? key : 14 + (key))

/* ipci.c */
extern IPCKey SystemPortAddressCreateIPCKey(SystemPortAddress address);
extern void CreateSharedMemoryAndSemaphores(IPCKey key);
extern void AttachSharedMemoryAndSemaphores(IPCKey key);

#endif	/* IPC_H */
