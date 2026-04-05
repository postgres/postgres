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

/* Different kinds of shmem areas. */
typedef enum
{
	SHMEM_KIND_STRUCT = 0,		/* plain, contiguous area of memory */
	SHMEM_KIND_HASH,			/* a hash table */
	SHMEM_KIND_SLRU,			/* SLRU buffers and control structures */
} ShmemRequestKind;

/* shmem.c */
typedef struct PGShmemHeader PGShmemHeader; /* avoid including
											 * storage/pg_shmem.h here */
extern void ShmemCallRequestCallbacks(void);
extern void InitShmemAllocator(PGShmemHeader *seghdr);
#ifdef EXEC_BACKEND
extern void AttachShmemAllocator(PGShmemHeader *seghdr);
#endif
extern void ResetShmemAllocator(void);

extern void ShmemRequestInternal(ShmemStructOpts *options, ShmemRequestKind kind);

extern size_t ShmemGetRequestedSize(void);
extern void ShmemInitRequested(void);
#ifdef EXEC_BACKEND
extern void ShmemAttachRequested(void);
#endif

extern PGDLLIMPORT Size pg_get_shmem_pagesize(void);

/* shmem_hash.c */
extern HTAB *shmem_hash_create(void *location, size_t size, bool found,
							   const char *name, int64 nelems, HASHCTL *infoP, int hash_flags);
extern void shmem_hash_init(void *location, ShmemStructOpts *base_options);
extern void shmem_hash_attach(void *location, ShmemStructOpts *base_options);

#endif							/* SHMEM_INTERNAL_H */
