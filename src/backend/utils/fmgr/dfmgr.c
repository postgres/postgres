/*-------------------------------------------------------------------------
 *
 * dfmgr.c
 *	  Dynamic function manager code.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/fmgr/dfmgr.c,v 1.66 2003/09/25 06:58:05 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>
#include <sys/stat.h>

#include "dynloader.h"
#include "miscadmin.h"
#include "utils/dynamic_loader.h"


/*
 * List of dynamically loaded files (kept in malloc'd memory).
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

char	   *Dynamic_library_path;

static bool file_exists(const char *name);
static char *find_in_dynamic_libpath(const char *basename);
static char *expand_dynamic_library_name(const char *name);
static char *substitute_libpath_macro(const char *name);

/*
 * Load the specified dynamic-link library file, and look for a function
 * named funcname in it.  (funcname can be NULL to just load the file.)
 *
 * If the function is not found, we raise an error if signalNotFound is true,
 * else return (PGFunction) NULL.  Note that errors in loading the library
 * will provoke ereport() regardless of signalNotFound.
 *
 * If filehandle is not NULL, then *filehandle will be set to a handle
 * identifying the library file.  The filehandle can be used with
 * lookup_external_function to lookup additional functions in the same file
 * at less cost than repeating load_external_function.
  */
PGFunction
load_external_function(char *filename, char *funcname,
					   bool signalNotFound, void **filehandle)
{
	DynamicFileList *file_scanner;
	PGFunction	retval;
	char	   *load_error;
	struct stat stat_buf;
	char	   *fullname;

	fullname = expand_dynamic_library_name(filename);
	if (!fullname)
		fullname = pstrdup(filename);
	/* at this point fullname is always freshly palloc'd */

	/*
	 * Scan the list of loaded FILES to see if the file has been loaded.
	 */
	for (file_scanner = file_list;
		 file_scanner != (DynamicFileList *) NULL &&
		 strcmp(fullname, file_scanner->filename) != 0;
		 file_scanner = file_scanner->next)
		;
	if (file_scanner == (DynamicFileList *) NULL)
	{
		/*
		 * Check for same files - different paths (ie, symlink or link)
		 */
		if (stat(fullname, &stat_buf) == -1)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access file \"%s\": %m",
							fullname)));

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
			malloc(sizeof(DynamicFileList) + strlen(fullname));
		if (file_scanner == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));

		MemSet((char *) file_scanner, 0, sizeof(DynamicFileList));
		strcpy(file_scanner->filename, fullname);
		file_scanner->device = stat_buf.st_dev;
		file_scanner->inode = stat_buf.st_ino;
		file_scanner->next = (DynamicFileList *) NULL;

		file_scanner->handle = pg_dlopen(fullname);
		if (file_scanner->handle == (void *) NULL)
		{
			load_error = (char *) pg_dlerror();
			free((char *) file_scanner);
			/* errcode_for_file_access might not be appropriate here? */
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not load library \"%s\": %s",
							fullname, load_error)));
		}

		/* OK to link it into list */
		if (file_list == (DynamicFileList *) NULL)
			file_list = file_scanner;
		else
			file_tail->next = file_scanner;
		file_tail = file_scanner;
	}

	/* Return handle if caller wants it. */
	if (filehandle)
		*filehandle = file_scanner->handle;

	/*
	 * If funcname is NULL, we only wanted to load the file.
	 */
	if (funcname == (char *) NULL)
	{
		pfree(fullname);
		return (PGFunction) NULL;
	}

	retval = pg_dlsym(file_scanner->handle, funcname);

	if (retval == (PGFunction) NULL && signalNotFound)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("could not find function \"%s\" in file \"%s\"",
						funcname, fullname)));

	pfree(fullname);
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
			   *prv,
			   *nxt;
	struct stat stat_buf;
	char	   *fullname;

	fullname = expand_dynamic_library_name(filename);
	if (!fullname)
		fullname = pstrdup(filename);
	/* at this point fullname is always freshly palloc'd */

	/*
	 * We need to do stat() in order to determine whether this is the same
	 * file as a previously loaded file; it's also handy so as to give a
	 * good error message if bogus file name given.
	 */
	if (stat(fullname, &stat_buf) == -1)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not access file \"%s\": %m", fullname)));

	/*
	 * We have to zap all entries in the list that match on either filename
	 * or inode, else load_external_function() won't do anything.
	 */
	prv = NULL;
	for (file_scanner = file_list; file_scanner != NULL; file_scanner = nxt)
	{
		nxt = file_scanner->next;
		if (strcmp(fullname, file_scanner->filename) == 0 ||
			SAME_INODE(stat_buf, *file_scanner))
		{
			if (prv)
				prv->next = nxt;
			else
				file_list = nxt;
			pg_dlclose(file_scanner->handle);
			free((char *) file_scanner);
			/* prv does not change */
		}
		else
			prv = file_scanner;
	}

	load_external_function(fullname, (char *) NULL, false, (void *) NULL);

	pfree(fullname);
}

/*
 * Lookup a function whose library file is already loaded.
 * Return (PGFunction) NULL if not found.
 */
PGFunction
lookup_external_function(void *filehandle, char *funcname)
{
	return pg_dlsym(filehandle, funcname);
}


static bool
file_exists(const char *name)
{
	struct stat st;

	AssertArg(name != NULL);

	if (stat(name, &st) == 0)
		return S_ISDIR(st.st_mode) ? false : true;
	else if (!(errno == ENOENT || errno == ENOTDIR || errno == EACCES))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not access file \"%s\": %m", name)));

	return false;
}


/* Example format: ".so" */
#ifndef DLSUFFIX
#error "DLSUFFIX must be defined to compile this file."
#endif

/* Example format: "/usr/local/pgsql/lib" */
#ifndef PKGLIBDIR
#error "PKGLIBDIR needs to be defined to compile this file."
#endif


/*
 * If name contains a slash, check if the file exists, if so return
 * the name.  Else (no slash) try to expand using search path (see
 * find_in_dynamic_libpath below); if that works, return the fully
 * expanded file name.	If the previous failed, append DLSUFFIX and
 * try again.  If all fails, return NULL.
 *
 * A non-NULL result will always be freshly palloc'd.
 */
static char *
expand_dynamic_library_name(const char *name)
{
	bool		have_slash;
	char	   *new;
	char	   *full;

	AssertArg(name);

	have_slash = (first_path_separator(name) != NULL);

	if (!have_slash)
	{
		full = find_in_dynamic_libpath(name);
		if (full)
			return full;
	}
	else
	{
		full = substitute_libpath_macro(name);
		if (file_exists(full))
			return full;
		pfree(full);
	}

	new = palloc(strlen(name) + strlen(DLSUFFIX) + 1);
	strcpy(new, name);
	strcat(new, DLSUFFIX);

	if (!have_slash)
	{
		full = find_in_dynamic_libpath(new);
		pfree(new);
		if (full)
			return full;
	}
	else
	{
		full = substitute_libpath_macro(new);
		pfree(new);
		if (file_exists(full))
			return full;
		pfree(full);
	}

	return NULL;
}


/*
 * Substitute for any macros appearing in the given string.
 * Result is always freshly palloc'd.
 */
static char *
substitute_libpath_macro(const char *name)
{
	size_t		macroname_len;
	char	   *replacement = NULL;

	AssertArg(name != NULL);

	if (name[0] != '$')
		return pstrdup(name);

#ifndef WIN32
	macroname_len = strcspn(name + 1, "/") + 1;
#else
	macroname_len = strcspn(name + 1, "/\\") + 1;
#endif

	if (strncmp(name, "$libdir", macroname_len) == 0)
		replacement = PKGLIBDIR;
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("invalid macro name in dynamic library path")));

	if (name[macroname_len] == '\0')
		return pstrdup(replacement);
	else
	{
		char	   *new;

		new = palloc(strlen(replacement) + (strlen(name) - macroname_len) + 1);

		strcpy(new, replacement);
		strcat(new, name + macroname_len);

		return new;
	}
}


/*
 * Search for a file called 'basename' in the colon-separated search
 * path Dynamic_library_path.  If the file is found, the full file name
 * is returned in freshly palloc'd memory.  If the file is not found,
 * return NULL.
 */
static char *
find_in_dynamic_libpath(const char *basename)
{
	const char *p;
	size_t		baselen;

	AssertArg(basename != NULL);
	AssertArg(first_path_separator(basename) == NULL);
	AssertState(Dynamic_library_path != NULL);

	p = Dynamic_library_path;
	if (strlen(p) == 0)
		return NULL;

	baselen = strlen(basename);

	for (;;)
	{
		size_t		len;
		char	   *piece;
		char	   *mangled;
		char	   *full;

		len = strcspn(p, ":");

		if (len == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_NAME),
			   errmsg("zero-length component in parameter \"dynamic_library_path\"")));

		piece = palloc(len + 1);
		strncpy(piece, p, len);
		piece[len] = '\0';

		mangled = substitute_libpath_macro(piece);
		pfree(piece);

		/* only absolute paths */
		if (!is_absolute_path(mangled))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_NAME),
			  errmsg("component in parameter \"dynamic_library_path\" is not an absolute path")));

		full = palloc(strlen(mangled) + 1 + baselen + 1);
		sprintf(full, "%s/%s", mangled, basename);
		pfree(mangled);

		elog(DEBUG3, "find_in_dynamic_libpath: trying \"%s\"", full);

		if (file_exists(full))
			return full;

		pfree(full);

		if (p[len] == '\0')
			break;
		else
			p += len + 1;
	}

	return NULL;
}
