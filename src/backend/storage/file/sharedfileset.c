/*-------------------------------------------------------------------------
 *
 * sharedfileset.c
 *	  Shared temporary file management.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/sharedfileset.c
 *
 * SharedFileSets provide a temporary namespace (think directory) so that
 * files can be discovered by name, and a shared ownership semantics so that
 * shared files survive until the last user detaches.
 *
 * SharedFileSets can be used by backends when the temporary files need to be
 * opened/closed multiple times and the underlying files need to survive across
 * transactions.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "catalog/pg_tablespace.h"
#include "commands/tablespace.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "storage/dsm.h"
#include "storage/ipc.h"
#include "storage/sharedfileset.h"
#include "utils/builtins.h"

static List *filesetlist = NIL;

static void SharedFileSetOnDetach(dsm_segment *segment, Datum datum);
static void SharedFileSetDeleteOnProcExit(int status, Datum arg);
static void SharedFileSetPath(char *path, SharedFileSet *fileset, Oid tablespace);
static void SharedFilePath(char *path, SharedFileSet *fileset, const char *name);
static Oid	ChooseTablespace(const SharedFileSet *fileset, const char *name);

/*
 * Initialize a space for temporary files that can be opened by other backends.
 * Other backends must attach to it before accessing it.  Associate this
 * SharedFileSet with 'seg'.  Any contained files will be deleted when the
 * last backend detaches.
 *
 * We can also use this interface if the temporary files are used only by
 * single backend but the files need to be opened and closed multiple times
 * and also the underlying files need to survive across transactions.  For
 * such cases, dsm segment 'seg' should be passed as NULL.  Callers are
 * expected to explicitly remove such files by using SharedFileSetDelete/
 * SharedFileSetDeleteAll or we remove such files on proc exit.
 *
 * Files will be distributed over the tablespaces configured in
 * temp_tablespaces.
 *
 * Under the covers the set is one or more directories which will eventually
 * be deleted.
 */
void
SharedFileSetInit(SharedFileSet *fileset, dsm_segment *seg)
{
	static uint32 counter = 0;

	SpinLockInit(&fileset->mutex);
	fileset->refcnt = 1;
	fileset->creator_pid = MyProcPid;
	fileset->number = counter;
	counter = (counter + 1) % INT_MAX;

	/* Capture the tablespace OIDs so that all backends agree on them. */
	PrepareTempTablespaces();
	fileset->ntablespaces =
		GetTempTablespaces(&fileset->tablespaces[0],
						   lengthof(fileset->tablespaces));
	if (fileset->ntablespaces == 0)
	{
		/* If the GUC is empty, use current database's default tablespace */
		fileset->tablespaces[0] = MyDatabaseTableSpace;
		fileset->ntablespaces = 1;
	}
	else
	{
		int			i;

		/*
		 * An entry of InvalidOid means use the default tablespace for the
		 * current database.  Replace that now, to be sure that all users of
		 * the SharedFileSet agree on what to do.
		 */
		for (i = 0; i < fileset->ntablespaces; i++)
		{
			if (fileset->tablespaces[i] == InvalidOid)
				fileset->tablespaces[i] = MyDatabaseTableSpace;
		}
	}

	/* Register our cleanup callback. */
	if (seg)
		on_dsm_detach(seg, SharedFileSetOnDetach, PointerGetDatum(fileset));
	else
	{
		static bool registered_cleanup = false;

		if (!registered_cleanup)
		{
			/*
			 * We must not have registered any fileset before registering the
			 * fileset clean up.
			 */
			Assert(filesetlist == NIL);
			on_proc_exit(SharedFileSetDeleteOnProcExit, 0);
			registered_cleanup = true;
		}

		filesetlist = lcons((void *) fileset, filesetlist);
	}
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
 * Create a new file in the given set.
 */
File
SharedFileSetCreate(SharedFileSet *fileset, const char *name)
{
	char		path[MAXPGPATH];
	File		file;

	SharedFilePath(path, fileset, name);
	file = PathNameCreateTemporaryFile(path, false);

	/* If we failed, see if we need to create the directory on demand. */
	if (file <= 0)
	{
		char		tempdirpath[MAXPGPATH];
		char		filesetpath[MAXPGPATH];
		Oid			tablespace = ChooseTablespace(fileset, name);

		TempTablespacePath(tempdirpath, tablespace);
		SharedFileSetPath(filesetpath, fileset, tablespace);
		PathNameCreateTemporaryDir(tempdirpath, filesetpath);
		file = PathNameCreateTemporaryFile(path, true);
	}

	return file;
}

/*
 * Open a file that was created with SharedFileSetCreate(), possibly in
 * another backend.
 */
File
SharedFileSetOpen(SharedFileSet *fileset, const char *name, int mode)
{
	char		path[MAXPGPATH];
	File		file;

	SharedFilePath(path, fileset, name);
	file = PathNameOpenTemporaryFile(path, mode);

	return file;
}

/*
 * Delete a file that was created with SharedFileSetCreate().
 * Return true if the file existed, false if didn't.
 */
bool
SharedFileSetDelete(SharedFileSet *fileset, const char *name,
					bool error_on_failure)
{
	char		path[MAXPGPATH];

	SharedFilePath(path, fileset, name);

	return PathNameDeleteTemporaryFile(path, error_on_failure);
}

/*
 * Delete all files in the set.
 */
void
SharedFileSetDeleteAll(SharedFileSet *fileset)
{
	char		dirpath[MAXPGPATH];
	int			i;

	/*
	 * Delete the directory we created in each tablespace.  Doesn't fail
	 * because we use this in error cleanup paths, but can generate LOG
	 * message on IO error.
	 */
	for (i = 0; i < fileset->ntablespaces; ++i)
	{
		SharedFileSetPath(dirpath, fileset, fileset->tablespaces[i]);
		PathNameDeleteTemporaryDir(dirpath);
	}

	/* Unregister the shared fileset */
	SharedFileSetUnregister(fileset);
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
		SharedFileSetDeleteAll(fileset);
}

/*
 * Callback function that will be invoked on the process exit.  This will
 * process the list of all the registered sharedfilesets and delete the
 * underlying files.
 */
static void
SharedFileSetDeleteOnProcExit(int status, Datum arg)
{
	/*
	 * Remove all the pending shared fileset entries. We don't use foreach() here
	 * because SharedFileSetDeleteAll will remove the current element in
	 * filesetlist. Though we have used foreach_delete_current() to remove the
	 * element from filesetlist it could only fix up the state of one of the
	 * loops, see SharedFileSetUnregister.
	 */
	while (list_length(filesetlist) > 0)
	{
		SharedFileSet *fileset = (SharedFileSet *) linitial(filesetlist);

		SharedFileSetDeleteAll(fileset);
	}

	filesetlist = NIL;
}

/*
 * Unregister the shared fileset entry registered for cleanup on proc exit.
 */
void
SharedFileSetUnregister(SharedFileSet *input_fileset)
{
	ListCell   *l;

	/*
	 * If the caller is following the dsm based cleanup then we don't maintain
	 * the filesetlist so return.
	 */
	if (filesetlist == NIL)
		return;

	foreach(l, filesetlist)
	{
		SharedFileSet *fileset = (SharedFileSet *) lfirst(l);

		/* Remove the entry from the list */
		if (input_fileset == fileset)
		{
			filesetlist = foreach_delete_current(filesetlist, l);
			return;
		}
	}

	/* Should have found a match */
	Assert(false);
}

/*
 * Build the path for the directory holding the files backing a SharedFileSet
 * in a given tablespace.
 */
static void
SharedFileSetPath(char *path, SharedFileSet *fileset, Oid tablespace)
{
	char		tempdirpath[MAXPGPATH];

	TempTablespacePath(tempdirpath, tablespace);
	snprintf(path, MAXPGPATH, "%s/%s%lu.%u.sharedfileset",
			 tempdirpath, PG_TEMP_FILE_PREFIX,
			 (unsigned long) fileset->creator_pid, fileset->number);
}

/*
 * Sorting hat to determine which tablespace a given shared temporary file
 * belongs in.
 */
static Oid
ChooseTablespace(const SharedFileSet *fileset, const char *name)
{
	uint32		hash = hash_any((const unsigned char *) name, strlen(name));

	return fileset->tablespaces[hash % fileset->ntablespaces];
}

/*
 * Compute the full path of a file in a SharedFileSet.
 */
static void
SharedFilePath(char *path, SharedFileSet *fileset, const char *name)
{
	char		dirpath[MAXPGPATH];

	SharedFileSetPath(dirpath, fileset, ChooseTablespace(fileset, name));
	snprintf(path, MAXPGPATH, "%s/%s", dirpath, name);
}
