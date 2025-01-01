/*-------------------------------------------------------------------------
 *
 * mem.h
 *	  portability definitions for various memory operations
 *
 * Copyright (c) 2001-2025, PostgreSQL Global Development Group
 *
 * src/include/portability/mem.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MEM_H
#define MEM_H

#define IPCProtection	(0600)	/* access/modify by user only */

#ifdef SHM_SHARE_MMU			/* use intimate shared memory on Solaris */
#define PG_SHMAT_FLAGS			SHM_SHARE_MMU
#else
#define PG_SHMAT_FLAGS			0
#endif

/* Linux prefers MAP_ANONYMOUS, but the flag is called MAP_ANON on other systems. */
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS			MAP_ANON
#endif

/* BSD-derived systems have MAP_HASSEMAPHORE, but it's not present (or needed) on Linux. */
#ifndef MAP_HASSEMAPHORE
#define MAP_HASSEMAPHORE		0
#endif

/*
 * BSD-derived systems use the MAP_NOSYNC flag to prevent dirty mmap(2)
 * pages from being gratuitously flushed to disk.
 */
#ifndef MAP_NOSYNC
#define MAP_NOSYNC			0
#endif

#define PG_MMAP_FLAGS			(MAP_SHARED|MAP_ANONYMOUS|MAP_HASSEMAPHORE)

/* Some really old systems don't define MAP_FAILED. */
#ifndef MAP_FAILED
#define MAP_FAILED ((void *) -1)
#endif

#endif							/* MEM_H */
