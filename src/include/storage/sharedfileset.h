/*-------------------------------------------------------------------------
 *
 * sharedfileset.h
 *	  Shared temporary file management.
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/sharedfileset.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef SHAREDFILESET_H
#define SHAREDFILESET_H

#include "storage/dsm.h"
#include "storage/fd.h"
#include "storage/spin.h"

/*
 * A set of temporary files that can be shared by multiple backends.
 */
typedef struct SharedFileSet
{
	pid_t		creator_pid;	/* PID of the creating process */
	uint32		number;			/* per-PID identifier */
	slock_t		mutex;			/* mutex protecting the reference count */
	int			refcnt;			/* number of attached backends */
	int			ntablespaces;	/* number of tablespaces to use */
	Oid			tablespaces[8]; /* OIDs of tablespaces to use. Assumes that
								 * it's rare that there more than temp
								 * tablespaces. */
} SharedFileSet;

extern void SharedFileSetInit(SharedFileSet *fileset, dsm_segment *seg);
extern void SharedFileSetAttach(SharedFileSet *fileset, dsm_segment *seg);
extern File SharedFileSetCreate(SharedFileSet *fileset, const char *name);
extern File SharedFileSetOpen(SharedFileSet *fileset, const char *name);
extern bool SharedFileSetDelete(SharedFileSet *fileset, const char *name,
								bool error_on_failure);
extern void SharedFileSetDeleteAll(SharedFileSet *fileset);

#endif
