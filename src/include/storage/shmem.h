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

/*
 * Options for ShmemRequestStruct()
 *
 * 'name' and 'size' are required.  Initialize any optional fields that you
 * don't use to zeros.
 *
 * After registration, the shmem machinery reserves memory for the area, sets
 * '*ptr' to point to the allocation, and calls the callbacks at the right
 * moments.
 */
typedef struct ShmemStructOpts
{
	const char *name;

	/*
	 * Requested size of the shmem allocation.
	 *
	 * When attaching to an existing allocation, the size must match the size
	 * given when the shmem region was allocated.  This cross-check can be
	 * disabled specifying SHMEM_ATTACH_UNKNOWN_SIZE.
	 */
	ssize_t		size;

	/*
	 * Alignment of the starting address. If not set, defaults to cacheline
	 * boundary.  Must be a power of two.
	 */
	size_t		alignment;

	/*
	 * When the shmem area is initialized or attached to, pointer to it is
	 * stored in *ptr.  It usually points to a global variable, used to access
	 * the shared memory area later.  *ptr is set before the init_fn or
	 * attach_fn callback is called.
	 */
	void	  **ptr;
} ShmemStructOpts;

#define SHMEM_ATTACH_UNKNOWN_SIZE (-1)

/*
 * Options for ShmemRequestHash()
 *
 * Each hash table is backed by a contiguous shmem area.
 */
typedef struct ShmemHashOpts
{
	/* Options for allocating the underlying shmem area; do not touch directly */
	ShmemStructOpts base;

	/*
	 * Name of the shared memory area.  Required.  Must be unique across the
	 * system.
	 */
	const char *name;

	/*
	 * 'nelems' is the max number of elements for the hash table.
	 */
	int64		nelems;

	/*
	 * Hash table options passed to hash_create()
	 *
	 * hash_info and hash_flags must specify at least the entry sizes and key
	 * comparison semantics (see hash_create()).  Flag bits and values
	 * specific to shared-memory hash tables are added implicitly in
	 * ShmemRequestHash(), except that callers may choose to specify
	 * HASH_PARTITION and/or HASH_FIXED_SIZE.
	 */
	HASHCTL		hash_info;
	int			hash_flags;

	/*
	 * When the hash table is initialized or attached to, pointer to its
	 * backend-private handle is stored in *ptr.  It usually points to a
	 * global variable, used to access the hash table later.
	 */
	HTAB	  **ptr;
} ShmemHashOpts;

typedef void (*ShmemRequestCallback) (void *opaque_arg);
typedef void (*ShmemInitCallback) (void *opaque_arg);
typedef void (*ShmemAttachCallback) (void *opaque_arg);

/*
 * Shared memory is reserved and allocated in stages at postmaster startup,
 * and in EXEC_BACKEND mode, there's some extra work done to "attach" to them
 * at backend startup.  ShmemCallbacks holds callback functions that are
 * called at different stages.
 */
typedef struct ShmemCallbacks
{
	/* SHMEM_CALLBACKS_* flags */
	int			flags;

	/*
	 * 'request_fn' is called during postmaster startup, before the shared
	 * memory has been allocated.  The function should call
	 * ShmemRequestStruct() and ShmemRequestHash() to register the subsystem's
	 * shared memory needs.
	 */
	ShmemRequestCallback request_fn;

	/*
	 * Initialization callback function.  This is called after the shared
	 * memory area has been allocated, usually at postmaster startup.
	 */
	ShmemInitCallback init_fn;

	/*
	 * Attachment callback function.  In EXEC_BACKEND mode, this is called at
	 * startup of each backend.  In !EXEC_BACKEND mode, this is only called if
	 * the shared memory area is registered after postmaster startup (see
	 * SHMEM_CALLBACKS_ALLOW_AFTER_STARTUP).
	 */
	ShmemAttachCallback attach_fn;

	/*
	 * Argument passed to the callbacks.  This is opaque to the shmem system,
	 * callbacks can use it for their own purposes.
	 */
	void	   *opaque_arg;
} ShmemCallbacks;

/*
 * Flags to control the behavior of RegisterShmemCallbacks().
 *
 * SHMEM_CALLBACKS_ALLOW_AFTER_STARTUP: Normally, calling
 * RegisterShmemCallbacks() after postmaster startup, e.g. in an add-in
 * library loaded on-demand in a backend, results in an error, because shared
 * memory should generally be requested at postmaster startup time.  But if
 * this flag is set, it is allowed and the callbacks are called immediately to
 * initialize or attach to the requested shared memory areas.  This is not
 * used by any built-in subsystems, but extensions may find it useful.
 */
#define SHMEM_CALLBACKS_ALLOW_AFTER_STARTUP		0x00000001

extern void RegisterShmemCallbacks(const ShmemCallbacks *callbacks);
extern bool ShmemAddrIsValid(const void *addr);

/*
 * These macros provide syntactic sugar for calling the underlying functions
 * with named arguments -like syntax.
 */
#define ShmemRequestStruct(...)  \
	ShmemRequestStructWithOpts(&(ShmemStructOpts){__VA_ARGS__})

#define ShmemRequestHash(...)  \
	ShmemRequestHashWithOpts(&(ShmemHashOpts){__VA_ARGS__})

extern void ShmemRequestStructWithOpts(const ShmemStructOpts *options);
extern void ShmemRequestHashWithOpts(const ShmemHashOpts *options);

/* legacy shmem allocation functions */
extern void *ShmemInitStruct(const char *name, Size size, bool *foundPtr);
extern HTAB *ShmemInitHash(const char *name, int64 nelems,
						   HASHCTL *infoP, int hash_flags);
extern void *ShmemAlloc(Size size);
extern void *ShmemAllocNoError(Size size);

extern Size add_size(Size s1, Size s2);
extern Size mul_size(Size s1, Size s2);

extern PGDLLIMPORT Size pg_get_shmem_pagesize(void);

/* ipci.c */
extern void RequestAddinShmemSpace(Size size);

#endif							/* SHMEM_H */
