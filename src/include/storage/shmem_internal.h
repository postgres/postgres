/*-------------------------------------------------------------------------
 *
 * shmem_internal.h
 *	  Internal functions related to shmem allocation
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/shmem_internal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SHMEM_INTERNAL_H
#define SHMEM_INTERNAL_H

#include "storage/shmem.h"
#include "utils/hsearch.h"

typedef struct PGShmemHeader PGShmemHeader; /* avoid including
											 * storage/pg_shmem.h here */
extern void InitShmemAllocator(PGShmemHeader *seghdr);

extern HTAB *shmem_hash_create(void *location, size_t size, bool found,
							   const char *name, int64 nelems, HASHCTL *infoP, int hash_flags);

/* size constants for the shmem index table */
 /* max size of data structure string name */
#define SHMEM_INDEX_KEYSIZE		 (48)
 /* max number of named shmem structures and hash tables */
#define SHMEM_INDEX_SIZE		 (256)

/* this is a hash bucket in the shmem index table */
typedef struct
{
	char		key[SHMEM_INDEX_KEYSIZE];	/* string name */
	void	   *location;		/* location in shared mem */
	Size		size;			/* # bytes requested for the structure */
	Size		allocated_size; /* # bytes actually allocated */
} ShmemIndexEnt;

#endif							/* SHMEM_INTERNAL_H */
