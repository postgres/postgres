/*-------------------------------------------------------------------------
 *
 * sharedfileset.c
 *	  Shared temporary file management.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/sharedfileset.c
 *
 * SharedFileSets provide a temporary namespace (think directory) so that
 * files can be discovered by name, and a shared ownership semantics so that
 * shared files survive until the last user detaches.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "storage/dsm.h"
#include "storage/sharedfileset.h"

static void SharedFileSetOnDetach(dsm_segment *segment, Datum datum);

/*
 * Initialize a space for temporary files that can be opened by other backends.
 * Other backends must attach to it before accessing it.  Associate this
 * SharedFileSet with 'seg'.  Any contained files will be deleted when the
 * last backend detaches.
 *
 * Under the covers the set is one or more directories which will eventually
 * be deleted.
 */
void
SharedFileSetInit(SharedFileSet *fileset, dsm_segment *seg)
{
	/* Initialize the shared fileset specific members. */
	SpinLockInit(&fileset->mutex);
	fileset->refcnt = 1;

	/* Initialize the fileset. */
	FileSetInit(&fileset->fs);

	/* Register our cleanup callback. */
	if (seg)
		on_dsm_detach(seg, SharedFileSetOnDetach, PointerGetDatum(fileset));
}

/*
 * Attach to a set of directories that was created with SharedFileSetInit.
 */
void
SharedFileSetAttach(SharedFileSet *fileset, dsm_segment *seg)
{
	bool		success;

	SpinLockAcquire(&fileset->mutex);
	if (fileset->refcnt == 0)
		success = false;
	else
	{
		++fileset->refcnt;
		success = true;
	}
	SpinLockRelease(&fileset->mutex);

	if (!success)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("could not attach to a SharedFileSet that is already destroyed")));

	/* Register our cleanup callback. */
	on_dsm_detach(seg, SharedFileSetOnDetach, PointerGetDatum(fileset));
}

/*
 * Delete all files in the set.
 */
void
SharedFileSetDeleteAll(SharedFileSet *fileset)
{
	FileSetDeleteAll(&fileset->fs);
}

/*
 * Callback function that will be invoked when this backend detaches from a
 * DSM segment holding a SharedFileSet that it has created or attached to.  If
 * we are the last to detach, then try to remove the directories and
 * everything in them.  We can't raise an error on failures, because this runs
 * in error cleanup paths.
 */
static void
SharedFileSetOnDetach(dsm_segment *segment, Datum datum)
{
	bool		unlink_all = false;
	SharedFileSet *fileset = (SharedFileSet *) DatumGetPointer(datum);

	SpinLockAcquire(&fileset->mutex);
	Assert(fileset->refcnt > 0);
	if (--fileset->refcnt == 0)
		unlink_all = true;
	SpinLockRelease(&fileset->mutex);

	/*
	 * If we are the last to detach, we delete the directory in all
	 * tablespaces.  Note that we are still actually attached for the rest of
	 * this function so we can safely access its data.
	 */
	if (unlink_all)
		FileSetDeleteAll(&fileset->fs);
}
