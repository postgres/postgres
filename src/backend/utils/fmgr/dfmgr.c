/*-------------------------------------------------------------------------
 *
 * dfmgr.c
 *	  Dynamic function manager code.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/fmgr/dfmgr.c,v 1.49 2001/05/17 17:44:18 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "dynloader.h"
#include "miscadmin.h"
#include "utils/dynamic_loader.h"


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

	/*
	 * we allocate the block big enough for actual length of pathname.
	 * filename[] must be last item in struct!
	 */
} DynamicFileList;

static DynamicFileList *file_list = (DynamicFileList *) NULL;
static DynamicFileList *file_tail = (DynamicFileList *) NULL;

#define SAME_INODE(A,B) ((A).st_ino == (B).inode && (A).st_dev == (B).device)

char * Dynamic_library_path;

static bool file_exists(const char *name);
static char * find_in_dynamic_libpath(const char * basename);
static char * expand_dynamic_library_name(const char *name);


/*
 * Load the specified dynamic-link library file, and look for a function
 * named funcname in it.  If the function is not found, we raise an error
 * if signalNotFound is true, else return (PGFunction) NULL.  Note that
 * errors in loading the library will provoke elog regardless of
 * signalNotFound.
 */
PGFunction
load_external_function(char *filename, char *funcname,
					   bool signalNotFound)
{
	DynamicFileList *file_scanner;
	PGFunction	retval;
	char	   *load_error;
	struct stat stat_buf;
	char	   *fullname;

	fullname = expand_dynamic_library_name(filename);
	if (fullname)
		filename = fullname;

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
			elog(ERROR, "Out of memory in load_external_function");

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

	if (retval == (PGFunction) NULL && signalNotFound)
		elog(ERROR, "Can't find function %s in file %s", funcname, filename);

	return retval;
}

/*
 * This function loads a shlib file without looking up any particular
 * function in it.	If the same shlib has previously been loaded,
 * unload and reload it.
 */
void
load_file(char *filename)
{
	DynamicFileList *file_scanner,
			   *p;
	struct stat stat_buf;
	char	   *fullname;

	fullname = expand_dynamic_library_name(filename);
	if (fullname)
		filename = fullname;

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

	load_external_function(filename, (char *) NULL, false);
}



static bool
file_exists(const char *name)
{
	struct stat st;

	AssertArg(name != NULL);

	if (stat(name, &st) == 0)
		return true;
	else if (!(errno == ENOENT || errno == ENOTDIR || errno == EACCES))
			elog(ERROR, "stat failed on %s: %s", name, strerror(errno));

	return false;
}


/* Example format: ".so" */
#ifndef DLSUFFIX
#error "DLSUFFIX must be defined to compile this file."
#endif

/* Example format: "/usr/local/pgsql/lib" */
#ifndef LIBDIR
#error "LIBDIR needs to be defined to compile this file."
#endif


/*
 * If name contains a slash, check if the file exists, if so return
 * the name.  Else (no slash) try to expand using search path (see
 * find_in_dynamic_libpath below); if that works, return the fully
 * expanded file name.  If the previous failed, append DLSUFFIX and
 * try again.  If all fails, return NULL.  The return value is
 * palloc'ed.
 */
static char *
expand_dynamic_library_name(const char *name)
{
	bool have_slash;
	char * new;
	size_t len;

	AssertArg(name);

	have_slash = (strchr(name, '/') != NULL);

	if (!have_slash)
	{
		char * full;

		full = find_in_dynamic_libpath(name);
		if (full)
			return full;
	}
	else
	{
		if (file_exists(name))
			return pstrdup(name);
	}

	len = strlen(name);

	new = palloc(len + strlen(DLSUFFIX) + 1);
	strcpy(new, name);
	strcpy(new + len, DLSUFFIX);

	if (!have_slash)
	{
		char * full;

		full = find_in_dynamic_libpath(new);
		pfree(new);
		if (full)
			return full;
	}
	else
	{
		if (file_exists(new))
			return new;
	}
		
	return NULL;
}



/*
 * Search for a file called 'basename' in the colon-separated search
 * path 'path'.  If the file is found, the full file name is returned
 * in palloced memory.  The the file is not found, return NULL.
 */
static char *
find_in_dynamic_libpath(const char * basename)
{
	const char *p;
	char *full;
	size_t len;
	size_t baselen;

	AssertArg(basename != NULL);
	AssertArg(strchr(basename, '/') == NULL);
	AssertState(Dynamic_library_path != NULL);

	p = Dynamic_library_path;
	if (strlen(p) == 0)
		return NULL;

	baselen = strlen(basename);

	do {
		len = strcspn(p, ":");

		if (len == 0)
			elog(ERROR, "zero length dynamic_library_path component");

		/* substitute special value */
		if (p[0] == '$')
		{
			size_t varname_len = strcspn(p + 1, "/") + 1;
			const char * replacement = NULL;
			size_t repl_len;

			if (strncmp(p, "$libdir", varname_len)==0)
				replacement = LIBDIR;
			else
				elog(ERROR, "invalid dynamic_library_path specification");

			repl_len = strlen(replacement);

			if (p[varname_len] == '\0')
			{
				full = palloc(repl_len + 1 + baselen + 1);
				snprintf(full, repl_len + 1 + baselen + 1,
						 "%s/%s", replacement, basename);
			}
			else
			{
				full = palloc(repl_len + (len - varname_len) + 1 + baselen + 1);

				strcpy(full, replacement);
				strncat(full, p + varname_len, len - varname_len);
				full[repl_len + (len - varname_len)] = '\0';
				strcat(full, "/");
				strcat(full, basename);
			}
		}

		/* regular case */
		else
		{
			/* only absolute paths */
			if (p[0] != '/')
				elog(ERROR, "dynamic_library_path component is not absolute");

			full = palloc(len + 1 + baselen + 1);
			strncpy(full, p, len);
			full[len] = '/';
			strcpy(full + len + 1, basename);
		}

		if (DebugLvl > 1)
			elog(DEBUG, "find_in_dynamic_libpath: trying %s", full);

		if (file_exists(full))
			return full;

		pfree(full);
		if (p[len] == '\0')
			break;
		else
			p += len + 1;
	} while(1);

	return NULL;
}
