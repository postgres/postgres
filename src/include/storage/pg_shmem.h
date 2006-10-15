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
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/pg_shmem.h,v 1.21 2006/10/15 22:04:07 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_SHMEM_H
#define PG_SHMEM_H

typedef struct PGShmemHeader	/* standard header for all Postgres shmem */
{
	int32		magic;			/* magic # to identify Postgres segments */
#define PGShmemMagic  679834894
	pid_t		creatorPID;		/* PID of creating process */
	Size		totalsize;		/* total size of segment */
	Size		freeoffset;		/* offset to first free space */
	Size		indexoffset;	/* offset to ShmemIndex table */
#ifndef WIN32					/* Windows doesn't have useful inode#s */
	dev_t		device;			/* device data directory is on */
	ino_t		inode;			/* inode number of data directory */
#endif
} PGShmemHeader;


#ifdef EXEC_BACKEND
extern unsigned long UsedShmemSegID;
extern void *UsedShmemSegAddr;

extern void PGSharedMemoryReAttach(void);
#endif

extern PGShmemHeader *PGSharedMemoryCreate(Size size, bool makePrivate,
					 int port);
extern bool PGSharedMemoryIsInUse(unsigned long id1, unsigned long id2);
extern void PGSharedMemoryDetach(void);

#endif   /* PG_SHMEM_H */
