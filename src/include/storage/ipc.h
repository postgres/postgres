/*-------------------------------------------------------------------------
 *
 * ipc.h
 *	  POSTGRES inter-process communication definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: ipc.h,v 1.47 2001/02/10 02:31:28 tgl Exp $
 *
 * Some files that would normally need to include only sys/ipc.h must
 * instead include this file because on Ultrix, sys/ipc.h is not designed
 * to be included multiple times.  This file (by virtue of the ifndef IPC_H)
 * is.
 *-------------------------------------------------------------------------
 */
#ifndef IPC_H
#define IPC_H

#include <sys/types.h>
#ifdef HAVE_SYS_IPC_H
#include <sys/ipc.h>
#endif /* HAVE_SYS_IPC_H */

#ifndef HAVE_UNION_SEMUN
union semun
{
	int			val;
	struct semid_ds *buf;
	unsigned short *array;
};
#endif

/* generic IPC definitions */

#define IPCProtection	(0600)	/* access/modify by user only */

/* semaphore definitions */

typedef uint32 IpcSemaphoreKey; /* semaphore key passed to semget(2) */
typedef int IpcSemaphoreId;		/* semaphore ID returned by semget(2) */

#define IPC_NMAXSEM		32		/* maximum number of semaphores per semID */

#define PGSemaMagic  537		/* must be less than SEMVMX */

/* shared memory definitions */

typedef uint32 IpcMemoryKey;	/* shared memory key passed to shmget(2) */
typedef int IpcMemoryId;		/* shared memory ID returned by shmget(2) */

typedef struct					/* standard header for all Postgres shmem */
{
	int32		magic;			/* magic # to identify Postgres segments */
#define PGShmemMagic  679834892
	pid_t		creatorPID;		/* PID of creating process */
	uint32		totalsize;		/* total size of segment */
	uint32		freeoffset;		/* offset to first free space */
} PGShmemHeader;


/* spinlock definitions */

typedef enum _LockId_
{
	BUFMGRLOCKID,
	OIDGENLOCKID,
	XIDGENLOCKID,
	CNTLFILELOCKID,
	SHMEMLOCKID,
	SHMEMINDEXLOCKID,
	LOCKMGRLOCKID,
	SINVALLOCKID,
	PROCSTRUCTLOCKID,

#ifdef STABLE_MEMORY_STORAGE
	MMCACHELOCKID,
#endif

	MAX_SPINS					/* must be last item! */
} _LockId_;


/* ipc.c */
extern bool proc_exit_inprogress;

extern void proc_exit(int code);
extern void shmem_exit(int code);
extern void on_proc_exit(void (*function) (), Datum arg);
extern void on_shmem_exit(void (*function) (), Datum arg);
extern void on_exit_reset(void);

extern void IpcInitKeyAssignment(int port);

extern IpcSemaphoreId IpcSemaphoreCreate(int numSems, int permission,
										 int semStartValue,
										 bool removeOnExit);
extern void IpcSemaphoreKill(IpcSemaphoreId semId);
extern void IpcSemaphoreLock(IpcSemaphoreId semId, int sem, bool interruptOK);
extern void IpcSemaphoreUnlock(IpcSemaphoreId semId, int sem);
extern bool IpcSemaphoreTryLock(IpcSemaphoreId semId, int sem);
extern int	IpcSemaphoreGetValue(IpcSemaphoreId semId, int sem);

extern PGShmemHeader *IpcMemoryCreate(uint32 size, bool makePrivate,
									  int permission);

/* ipci.c */
extern void CreateSharedMemoryAndSemaphores(bool makePrivate,
											int maxBackends);

#endif	 /* IPC_H */
