/*-------------------------------------------------------------------------
 *
 * pg_shmem.h
 *	  Platform-independent API for shared memory support.
 *
 * Every port is expected to support shared memory with approximately
 * SysV-ish semantics; in particular, a memory block is not anonymous
 * but has an ID, and we must be able to tell whether there are any
 * remaining processes attached to a block of a specified ID.
 *
 * To simplify life for the SysV implementation, the ID is assumed to
 * consist of two unsigned long values (these are key and ID in SysV
 * terms).	Other platforms may ignore the second value if they need
 * only one ID number.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_shmem.h,v 1.7.4.1 2003/11/07 21:56:02 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_SHMEM_H
#define PG_SHMEM_H

typedef uint32 IpcMemoryKey;	/* shared memory key passed to shmget(2) */

typedef struct PGShmemHeader	/* standard header for all Postgres shmem */
{
	int32		magic;			/* magic # to identify Postgres segments */
#define PGShmemMagic  679834892
	pid_t		creatorPID;		/* PID of creating process */
	uint32		totalsize;		/* total size of segment */
	uint32		freeoffset;		/* offset to first free space */
} PGShmemHeader;


#ifdef EXEC_BACKEND
extern IpcMemoryKey UsedShmemSegID;
extern void *UsedShmemSegAddr;
#endif

extern PGShmemHeader *PGSharedMemoryCreate(uint32 size, bool makePrivate,
					 int port);
extern bool PGSharedMemoryIsInUse(unsigned long id1, unsigned long id2);
extern void PGSharedMemoryDetach(void);

#endif   /* PG_SHMEM_H */
