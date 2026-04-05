/*-------------------------------------------------------------------------
 *
 * shmem.h
 *	  shared memory management structures
 *
 * This file contains public functions for other core subsystems and
 * extensions to allocate shared memory.  Internal functions for the shmem
 * allocator itself and hooking it to the rest of the system are in
 * shmem_internal.h
 *
 * Historical note:
 * A long time ago, Postgres' shared memory region was allowed to be mapped
 * at a different address in each process, and shared memory "pointers" were
 * passed around as offsets relative to the start of the shared memory region.
 * That is no longer the case: each process must map the shared memory region
 * at the same address.  This means shared memory pointers can be passed
 * around directly between different processes.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/shmem.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SHMEM_H
#define SHMEM_H

#include "utils/hsearch.h"


/* shmem.c */
extern void *ShmemAlloc(Size size);
extern void *ShmemAllocNoError(Size size);
extern bool ShmemAddrIsValid(const void *addr);
extern void *ShmemInitStruct(const char *name, Size size, bool *foundPtr);
extern Size add_size(Size s1, Size s2);
extern Size mul_size(Size s1, Size s2);

extern PGDLLIMPORT Size pg_get_shmem_pagesize(void);

/* shmem_hash.c */
extern HTAB *ShmemInitHash(const char *name, int64 nelems,
						   HASHCTL *infoP, int hash_flags);

/* ipci.c */
extern void RequestAddinShmemSpace(Size size);

#endif							/* SHMEM_H */
