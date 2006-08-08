/*-------------------------------------------------------------------------
 *
 * dfmgr.c
 *	  Dynamic function manager code.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/fmgr/dfmgr.c,v 1.87 2006/08/08 19:15:08 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>

#ifndef WIN32_ONLY_COMPILER
#include "dynloader.h"
#else
#include "port/dynloader/win32.h"
#endif
#include "miscadmin.h"
#include "utils/dynamic_loader.h"


/* signatures for PostgreSQL-specific library init/fini functions */
typedef void (*PG_init_t)(void);
typedef void (*PG_fini_t)(void);

/*
 * List of dynamically loaded files (kept in malloc'd memory).
 */

typedef struct df_files
{
	struct df_files *next;		/* List link */
	dev_t		device;			/* Device file is on */
#ifndef WIN32					/* ensures we never again depend on this under
								 * win32 */
	ino_t		inode;			/* Inode number of file */
#endif
	void	   *handle;			/* a handle for pg_dl* functions */
	char		filename[1];	/* Full pathname of file */

	/*
	 * we allocate the block big enough for actual length of pathname.
	 * filename[] must be last item in struct!
	 */
} DynamicFileList;

static DynamicFileList *file_list = NULL;
static DynamicFileList *file_tail = NULL;

/* stat() call under Win32 returns an st_ino field, but it has no meaning */
#ifndef WIN32
#define SAME_INODE(A,B) ((A).st_ino == (B).inode && (A).st_dev == (B).device)
#else
#define SAME_INODE(A,B) false
#endif

char	   *Dynamic_library_path;

static bool file_exists(const char *name);
static char *find_in_dynamic_libpath(const char *basename);
static char *expand_dynamic_library_name(const char *name);
static char *substitute_libpath_macro(const char *name);

/* Magic structure that module needs to match to be accepted */
static const Pg_magic_struct magic_data = PG_MODULE_MAGIC_DATA;


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
	PGModuleMagicFunction magic_func;
	char	   *load_error;
	struct stat stat_buf;
	char	   *fullname;
	PG_init_t	PG_init;

	fullname = expand_dynamic_library_name(filename);
	if (!fullname)
		fullname = pstrdup(filename);
	/* at this point fullname is always freshly palloc'd */

	/*
	 * Scan the list of loaded FILES to see if the file has been loaded.
	 */
	for (file_scanner = file_list;
		 file_scanner != NULL &&
		 strcmp(fullname, file_scanner->filename) != 0;
		 file_scanner = file_scanner->next)
		;
	if (file_scanner == NULL)
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
			 file_scanner != NULL &&
			 !SAME_INODE(stat_buf, *file_scanner);
			 file_scanner = file_scanner->next)
			;
	}

	if (file_scanner == NULL)
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

		MemSet(file_scanner, 0, sizeof(DynamicFileList));
		strcpy(file_scanner->filename, fullname);
		file_scanner->device = stat_buf.st_dev;
#ifndef WIN32
		file_scanner->inode = stat_buf.st_ino;
#endif
		file_scanner->next = NULL;

		file_scanner->handle = pg_dlopen(fullname);
		if (file_scanner->handle == NULL)
		{
			load_error = (char *) pg_dlerror();
			free((char *) file_scanner);
			/* errcode_for_file_access might not be appropriate here? */
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not load library \"%s\": %s",
							fullname, load_error)));
		}

		/* Check the magic function to determine compatibility */
		magic_func = (PGModuleMagicFunction)
			pg_dlsym(file_scanner->handle, PG_MAGIC_FUNCTION_NAME_STRING);
		if (magic_func)
		{
			const Pg_magic_struct *magic_data_ptr = (*magic_func) ();

			if (magic_data_ptr->len != magic_data.len ||
			    memcmp(magic_data_ptr, &magic_data, magic_data.len) != 0)
			{
				/* copy data block before unlinking library */
				Pg_magic_struct module_magic_data = *magic_data_ptr;

				/* try to unlink library */
				pg_dlclose(file_scanner->handle);
				free((char *) file_scanner);

				/*
				 * Report suitable error.  It's probably not worth writing
				 * a separate error message for each field; only the most
				 * common case of wrong major version gets its own message.
				 */
				if (module_magic_data.version != magic_data.version)
					ereport(ERROR,
							(errmsg("incompatible library \"%s\": version mismatch",
									fullname),
							 errdetail("Server is version %d.%d, library is version %d.%d.",
									   magic_data.version/100,
									   magic_data.version % 100,
									   module_magic_data.version/100,
									   module_magic_data.version % 100)));
				ereport(ERROR,
						(errmsg("incompatible library \"%s\": magic block mismatch",
								fullname)));
			}
		}
		else
		{
			/* try to unlink library */
			pg_dlclose(file_scanner->handle);
			free((char *) file_scanner);
			/* complain */
			ereport(ERROR,
					(errmsg("incompatible library \"%s\": missing magic block",
							fullname),
					 errhint("Extension libraries are now required to use the PG_MODULE_MAGIC macro.")));
		}

		/*
		 * If the library has a _PG_init() function, call it.
		 */
		PG_init = (PG_init_t) pg_dlsym(file_scanner->handle, "_PG_init");
		if (PG_init)
			(*PG_init)();

		/* OK to link it into list */
		if (file_list == NULL)
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
	if (funcname == NULL)
	{
		pfree(fullname);
		return NULL;
	}

	retval = pg_dlsym(file_scanner->handle, funcname);

	if (retval == NULL && signalNotFound)
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
	PG_fini_t	PG_fini;

	fullname = expand_dynamic_library_name(filename);
	if (!fullname)
		fullname = pstrdup(filename);
	/* at this point fullname is always freshly palloc'd */

	/*
	 * We need to do stat() in order to determine whether this is the same
	 * file as a previously loaded file; it's also handy so as to give a good
	 * error message if bogus file name given.
	 */
	if (stat(fullname, &stat_buf) == -1)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not access file \"%s\": %m", fullname)));

	/*
	 * We have to zap all entries in the list that match on either filename or
	 * inode, else load_external_function() won't do anything.
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
			clear_external_function_hash(file_scanner->handle);

			/*
			 * If the library has a _PG_fini() function, call it.
			 */
			PG_fini = (PG_fini_t) pg_dlsym(file_scanner->handle, "_PG_fini");
			if (PG_fini)
				(*PG_fini)();

			pg_dlclose(file_scanner->handle);
			free((char *) file_scanner);
			/* prv does not change */
		}
		else
			prv = file_scanner;
	}

	load_external_function(fullname, NULL, false, NULL);

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

	have_slash = (first_dir_separator(name) != NULL);

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
	const char *sep_ptr;
	char	   *ret;

	AssertArg(name != NULL);

	if (name[0] != '$')
		return pstrdup(name);

	if ((sep_ptr = first_dir_separator(name)) == NULL)
		sep_ptr = name + strlen(name);

	if (strlen("$libdir") != sep_ptr - name ||
		strncmp(name, "$libdir", strlen("$libdir")) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
			errmsg("invalid macro name in dynamic library path: %s", name)));

	ret = palloc(strlen(pkglib_path) + strlen(sep_ptr) + 1);

	strcpy(ret, pkglib_path);
	strcat(ret, sep_ptr);

	return ret;
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
	AssertArg(first_dir_separator(basename) == NULL);
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

		piece = first_path_separator(p);
		if (piece == p)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_NAME),
					 errmsg("zero-length component in parameter \"dynamic_library_path\"")));

		if (piece == 0)
			len = strlen(p);
		else
			len = piece - p;

		piece = palloc(len + 1);
		strncpy(piece, p, len);
		piece[len] = '\0';

		mangled = substitute_libpath_macro(piece);
		pfree(piece);

		canonicalize_path(mangled);

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
