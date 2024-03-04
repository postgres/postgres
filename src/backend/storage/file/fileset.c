/*-------------------------------------------------------------------------
 *
 * fileset.c
 *	  Management of named temporary files.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/fileset.c
 *
 * FileSets provide a temporary namespace (think directory) so that files can
 * be discovered by name.
 *
 * FileSets can be used by backends when the temporary files need to be
 * opened/closed multiple times and the underlying files need to survive across
 * transactions.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "commands/tablespace.h"
#include "common/file_utils.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "storage/fileset.h"

static void FileSetPath(char *path, FileSet *fileset, Oid tablespace);
static void FilePath(char *path, FileSet *fileset, const char *name);
static Oid	ChooseTablespace(const FileSet *fileset, const char *name);

/*
 * Initialize a space for temporary files. This API can be used by shared
 * fileset as well as if the temporary files are used only by single backend
 * but the files need to be opened and closed multiple times and also the
 * underlying files need to survive across transactions.
 *
 * The callers are expected to explicitly remove such files by using
 * FileSetDelete/FileSetDeleteAll.
 *
 * Files will be distributed over the tablespaces configured in
 * temp_tablespaces.
 *
 * Under the covers the set is one or more directories which will eventually
 * be deleted.
 */
void
FileSetInit(FileSet *fileset)
{
	static uint32 counter = 0;

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
		 * the FileSet agree on what to do.
		 */
		for (i = 0; i < fileset->ntablespaces; i++)
		{
			if (fileset->tablespaces[i] == InvalidOid)
				fileset->tablespaces[i] = MyDatabaseTableSpace;
		}
	}
}

/*
 * Create a new file in the given set.
 */
File
FileSetCreate(FileSet *fileset, const char *name)
{
	char		path[MAXPGPATH];
	File		file;

	FilePath(path, fileset, name);
	file = PathNameCreateTemporaryFile(path, false);

	/* If we failed, see if we need to create the directory on demand. */
	if (file <= 0)
	{
		char		tempdirpath[MAXPGPATH];
		char		filesetpath[MAXPGPATH];
		Oid			tablespace = ChooseTablespace(fileset, name);

		TempTablespacePath(tempdirpath, tablespace);
		FileSetPath(filesetpath, fileset, tablespace);
		PathNameCreateTemporaryDir(tempdirpath, filesetpath);
		file = PathNameCreateTemporaryFile(path, true);
	}

	return file;
}

/*
 * Open a file that was created with FileSetCreate() */
File
FileSetOpen(FileSet *fileset, const char *name, int mode)
{
	char		path[MAXPGPATH];
	File		file;

	FilePath(path, fileset, name);
	file = PathNameOpenTemporaryFile(path, mode);

	return file;
}

/*
 * Delete a file that was created with FileSetCreate().
 *
 * Return true if the file existed, false if didn't.
 */
bool
FileSetDelete(FileSet *fileset, const char *name,
			  bool error_on_failure)
{
	char		path[MAXPGPATH];

	FilePath(path, fileset, name);

	return PathNameDeleteTemporaryFile(path, error_on_failure);
}

/*
 * Delete all files in the set.
 */
void
FileSetDeleteAll(FileSet *fileset)
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
		FileSetPath(dirpath, fileset, fileset->tablespaces[i]);
		PathNameDeleteTemporaryDir(dirpath);
	}
}

/*
 * Build the path for the directory holding the files backing a FileSet in a
 * given tablespace.
 */
static void
FileSetPath(char *path, FileSet *fileset, Oid tablespace)
{
	char		tempdirpath[MAXPGPATH];

	TempTablespacePath(tempdirpath, tablespace);
	snprintf(path, MAXPGPATH, "%s/%s%lu.%u.fileset",
			 tempdirpath, PG_TEMP_FILE_PREFIX,
			 (unsigned long) fileset->creator_pid, fileset->number);
}

/*
 * Sorting has to determine which tablespace a given temporary file belongs in.
 */
static Oid
ChooseTablespace(const FileSet *fileset, const char *name)
{
	uint32		hash = hash_any((const unsigned char *) name, strlen(name));

	return fileset->tablespaces[hash % fileset->ntablespaces];
}

/*
 * Compute the full path of a file in a FileSet.
 */
static void
FilePath(char *path, FileSet *fileset, const char *name)
{
	char		dirpath[MAXPGPATH];

	FileSetPath(dirpath, fileset, ChooseTablespace(fileset, name));
	snprintf(path, MAXPGPATH, "%s/%s", dirpath, name);
}
