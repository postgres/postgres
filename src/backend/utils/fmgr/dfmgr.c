/*-------------------------------------------------------------------------
 *
 * dfmgr.c
 *	  Dynamic function manager code.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/fmgr/dfmgr.c,v 1.41 2000/05/30 00:49:55 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>
#include <sys/stat.h>

#include "postgres.h"

#include "catalog/pg_proc.h"
#include "dynloader.h"
#include "utils/builtins.h"
#include "utils/dynamic_loader.h"
#include "utils/syscache.h"


/*
 * List of dynamically loaded files.
 */

typedef struct df_files
{
	struct df_files *next;		/* List link */
	dev_t		device;			/* Device file is on */
	ino_t		inode;			/* Inode number of file */
	void	   *handle;			/* a handle for pg_dl* functions */
	char		filename[1];	/* Full pathname of file */
	/* we allocate the block big enough for actual length of pathname.
	 * filename[] must be last item in struct!
	 */
} DynamicFileList;

static DynamicFileList *file_list = (DynamicFileList *) NULL;
static DynamicFileList *file_tail = (DynamicFileList *) NULL;

#define SAME_INODE(A,B) ((A).st_ino == (B).inode && (A).st_dev == (B).device)


PGFunction
fmgr_dynamic(Oid functionId)
{
	HeapTuple	procedureTuple;
	Form_pg_proc procedureStruct;
	char	   *proname,
			   *prosrcstring,
			   *probinstring;
	Datum		prosrcattr,
				probinattr;
	PGFunction	user_fn;
	bool		isnull;

	procedureTuple = SearchSysCacheTuple(PROCOID,
										 ObjectIdGetDatum(functionId),
										 0, 0, 0);
	if (!HeapTupleIsValid(procedureTuple))
		elog(ERROR, "fmgr_dynamic: function %u: cache lookup failed",
			 functionId);
	procedureStruct = (Form_pg_proc) GETSTRUCT(procedureTuple);

	proname = NameStr(procedureStruct->proname);

	prosrcattr = SysCacheGetAttr(PROCOID, procedureTuple,
								 Anum_pg_proc_prosrc, &isnull);
	if (isnull || !PointerIsValid(prosrcattr))
	{
		elog(ERROR, "fmgr: Could not extract prosrc for %u from pg_proc",
			 functionId);
	}
	prosrcstring = textout((text *) DatumGetPointer(prosrcattr));

	probinattr = SysCacheGetAttr(PROCOID, procedureTuple,
								 Anum_pg_proc_probin, &isnull);
	if (isnull || !PointerIsValid(probinattr))
	{
		elog(ERROR, "fmgr: Could not extract probin for %u from pg_proc",
			 functionId);
	}
	probinstring = textout((text *) DatumGetPointer(probinattr));

	user_fn = load_external_function(probinstring, prosrcstring);

	pfree(prosrcstring);
	pfree(probinstring);

	return user_fn;
}

PGFunction
load_external_function(char *filename, char *funcname)
{
	DynamicFileList *file_scanner;
	PGFunction	retval;
	char	   *load_error;
	struct stat stat_buf;

	/*
	 * Scan the list of loaded FILES to see if the file has been loaded.
	 */
	for (file_scanner = file_list;
		 file_scanner != (DynamicFileList *) NULL &&
			 strcmp(filename, file_scanner->filename) != 0;
		 file_scanner = file_scanner->next)
		;
	if (file_scanner == (DynamicFileList *) NULL)
	{
		/*
		 * Check for same files - different paths (ie, symlink or link)
		 */
		if (stat(filename, &stat_buf) == -1)
			elog(ERROR, "stat failed on file '%s': %m", filename);

		for (file_scanner = file_list;
			 file_scanner != (DynamicFileList *) NULL &&
				 !SAME_INODE(stat_buf, *file_scanner);
			 file_scanner = file_scanner->next)
			;
	}

	if (file_scanner == (DynamicFileList *) NULL)
	{
		/*
		 * File not loaded yet.
		 */
		file_scanner = (DynamicFileList *)
			malloc(sizeof(DynamicFileList) + strlen(filename));
		if (file_scanner == NULL)
			elog(FATAL, "Out of memory in load_external_function");

		MemSet((char *) file_scanner, 0, sizeof(DynamicFileList));
		strcpy(file_scanner->filename, filename);
		file_scanner->device = stat_buf.st_dev;
		file_scanner->inode = stat_buf.st_ino;
		file_scanner->next = (DynamicFileList *) NULL;

		file_scanner->handle = pg_dlopen(filename);
		if (file_scanner->handle == (void *) NULL)
		{
			load_error = (char *) pg_dlerror();
			free((char *) file_scanner);
			elog(ERROR, "Load of file %s failed: %s", filename, load_error);
		}

		/* OK to link it into list */
		if (file_list == (DynamicFileList *) NULL)
			file_list = file_scanner;
		else
			file_tail->next = file_scanner;
		file_tail = file_scanner;
	}

	/*
	 * If funcname is NULL, we only wanted to load the file.
	 */
	if (funcname == (char *) NULL)
		return (PGFunction) NULL;

	retval = pg_dlsym(file_scanner->handle, funcname);

	if (retval == (PGFunction) NULL)
		elog(ERROR, "Can't find function %s in file %s", funcname, filename);

	return retval;
}

/*
 * This function loads a shlib file without looking up any particular
 * function in it.  If the same shlib has previously been loaded,
 * unload and reload it.
 */
void
load_file(char *filename)
{
	DynamicFileList *file_scanner,
			   *p;
	struct stat stat_buf;

	/*
	 * We need to do stat() in order to determine whether this is the same
	 * file as a previously loaded file; it's also handy so as to give a
	 * good error message if bogus file name given.
	 */
	if (stat(filename, &stat_buf) == -1)
		elog(ERROR, "LOAD: could not open file '%s': %m", filename);

	if (file_list != (DynamicFileList *) NULL)
	{
		if (SAME_INODE(stat_buf, *file_list))
		{
			p = file_list;
			file_list = p->next;
			pg_dlclose(p->handle);
			free((char *) p);
		}
		else
		{
			for (file_scanner = file_list;
				 file_scanner->next != (DynamicFileList *) NULL;
				 file_scanner = file_scanner->next)
			{
				if (SAME_INODE(stat_buf, *(file_scanner->next)))
				{
					p = file_scanner->next;
					file_scanner->next = p->next;
					pg_dlclose(p->handle);
					free((char *) p);
					break;
				}
			}
		}
	}

	load_external_function(filename, (char *) NULL);
}
