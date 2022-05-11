/*-------------------------------------------------------------------------
 *
 * dfmgr.c
 *	  Dynamic function manager code.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/fmgr/dfmgr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>

#ifdef HAVE_DLOPEN
#include <dlfcn.h>

/*
 * On macOS, <dlfcn.h> insists on including <stdbool.h>.  If we're not
 * using stdbool, undef bool to undo the damage.
 */
#ifndef PG_USE_STDBOOL
#ifdef bool
#undef bool
#endif
#endif
#endif							/* HAVE_DLOPEN */

#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/shmem.h"
#include "utils/hsearch.h"


/* signature for PostgreSQL-specific library init function */
typedef void (*PG_init_t) (void);

/* hashtable entry for rendezvous variables */
typedef struct
{
	char		varName[NAMEDATALEN];	/* hash key (must be first) */
	void	   *varValue;
} rendezvousHashEntry;

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
	char		filename[FLEXIBLE_ARRAY_MEMBER];	/* Full pathname of file */
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

static void *internal_load_library(const char *libname);
static void incompatible_module_error(const char *libname,
									  const Pg_magic_struct *module_magic_data) pg_attribute_noreturn();
static bool file_exists(const char *name);
static char *expand_dynamic_library_name(const char *name);
static void check_restricted_library_name(const char *name);
static char *substitute_libpath_macro(const char *name);
static char *find_in_dynamic_libpath(const char *basename);

/* Magic structure that module needs to match to be accepted */
static const Pg_magic_struct magic_data = PG_MODULE_MAGIC_DATA;


/*
 * Load the specified dynamic-link library file, and look for a function
 * named funcname in it.
 *
 * If the function is not found, we raise an error if signalNotFound is true,
 * else return NULL.  Note that errors in loading the library
 * will provoke ereport() regardless of signalNotFound.
 *
 * If filehandle is not NULL, then *filehandle will be set to a handle
 * identifying the library file.  The filehandle can be used with
 * lookup_external_function to lookup additional functions in the same file
 * at less cost than repeating load_external_function.
 */
void *
load_external_function(const char *filename, const char *funcname,
					   bool signalNotFound, void **filehandle)
{
	char	   *fullname;
	void	   *lib_handle;
	void	   *retval;

	/* Expand the possibly-abbreviated filename to an exact path name */
	fullname = expand_dynamic_library_name(filename);

	/* Load the shared library, unless we already did */
	lib_handle = internal_load_library(fullname);

	/* Return handle if caller wants it */
	if (filehandle)
		*filehandle = lib_handle;

	/* Look up the function within the library. */
	retval = dlsym(lib_handle, funcname);

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
 * function in it.  If the same shlib has previously been loaded,
 * unload and reload it.
 *
 * When 'restricted' is true, only libraries in the presumed-secure
 * directory $libdir/plugins may be referenced.
 */
void
load_file(const char *filename, bool restricted)
{
	char	   *fullname;

	/* Apply security restriction if requested */
	if (restricted)
		check_restricted_library_name(filename);

	/* Expand the possibly-abbreviated filename to an exact path name */
	fullname = expand_dynamic_library_name(filename);

	/* Load the shared library */
	(void) internal_load_library(fullname);

	pfree(fullname);
}

/*
 * Lookup a function whose library file is already loaded.
 * Return NULL if not found.
 */
void *
lookup_external_function(void *filehandle, const char *funcname)
{
	return dlsym(filehandle, funcname);
}


/*
 * Load the specified dynamic-link library file, unless it already is
 * loaded.  Return the pg_dl* handle for the file.
 *
 * Note: libname is expected to be an exact name for the library file.
 *
 * NB: There is presently no way to unload a dynamically loaded file.  We might
 * add one someday if we can convince ourselves we have safe protocols for un-
 * hooking from hook function pointers, releasing custom GUC variables, and
 * perhaps other things that are definitely unsafe currently.
 */
static void *
internal_load_library(const char *libname)
{
	DynamicFileList *file_scanner;
	PGModuleMagicFunction magic_func;
	char	   *load_error;
	struct stat stat_buf;
	PG_init_t	PG_init;

	/*
	 * Scan the list of loaded FILES to see if the file has been loaded.
	 */
	for (file_scanner = file_list;
		 file_scanner != NULL &&
		 strcmp(libname, file_scanner->filename) != 0;
		 file_scanner = file_scanner->next)
		;

	if (file_scanner == NULL)
	{
		/*
		 * Check for same files - different paths (ie, symlink or link)
		 */
		if (stat(libname, &stat_buf) == -1)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access file \"%s\": %m",
							libname)));

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
			malloc(offsetof(DynamicFileList, filename) + strlen(libname) + 1);
		if (file_scanner == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));

		MemSet(file_scanner, 0, offsetof(DynamicFileList, filename));
		strcpy(file_scanner->filename, libname);
		file_scanner->device = stat_buf.st_dev;
#ifndef WIN32
		file_scanner->inode = stat_buf.st_ino;
#endif
		file_scanner->next = NULL;

		file_scanner->handle = dlopen(file_scanner->filename, RTLD_NOW | RTLD_GLOBAL);
		if (file_scanner->handle == NULL)
		{
			load_error = dlerror();
			free((char *) file_scanner);
			/* errcode_for_file_access might not be appropriate here? */
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not load library \"%s\": %s",
							libname, load_error)));
		}

		/* Check the magic function to determine compatibility */
		magic_func = (PGModuleMagicFunction)
			dlsym(file_scanner->handle, PG_MAGIC_FUNCTION_NAME_STRING);
		if (magic_func)
		{
			const Pg_magic_struct *magic_data_ptr = (*magic_func) ();

			if (magic_data_ptr->len != magic_data.len ||
				memcmp(magic_data_ptr, &magic_data, magic_data.len) != 0)
			{
				/* copy data block before unlinking library */
				Pg_magic_struct module_magic_data = *magic_data_ptr;

				/* try to close library */
				dlclose(file_scanner->handle);
				free((char *) file_scanner);

				/* issue suitable complaint */
				incompatible_module_error(libname, &module_magic_data);
			}
		}
		else
		{
			/* try to close library */
			dlclose(file_scanner->handle);
			free((char *) file_scanner);
			/* complain */
			ereport(ERROR,
					(errmsg("incompatible library \"%s\": missing magic block",
							libname),
					 errhint("Extension libraries are required to use the PG_MODULE_MAGIC macro.")));
		}

		/*
		 * If the library has a _PG_init() function, call it.
		 */
		PG_init = (PG_init_t) dlsym(file_scanner->handle, "_PG_init");
		if (PG_init)
			(*PG_init) ();

		/* OK to link it into list */
		if (file_list == NULL)
			file_list = file_scanner;
		else
			file_tail->next = file_scanner;
		file_tail = file_scanner;
	}

	return file_scanner->handle;
}

/*
 * Report a suitable error for an incompatible magic block.
 */
static void
incompatible_module_error(const char *libname,
						  const Pg_magic_struct *module_magic_data)
{
	StringInfoData details;

	/*
	 * If the version doesn't match, just report that, because the rest of the
	 * block might not even have the fields we expect.
	 */
	if (magic_data.version != module_magic_data->version)
	{
		char		library_version[32];

		if (module_magic_data->version >= 1000)
			snprintf(library_version, sizeof(library_version), "%d",
					 module_magic_data->version / 100);
		else
			snprintf(library_version, sizeof(library_version), "%d.%d",
					 module_magic_data->version / 100,
					 module_magic_data->version % 100);
		ereport(ERROR,
				(errmsg("incompatible library \"%s\": version mismatch",
						libname),
				 errdetail("Server is version %d, library is version %s.",
						   magic_data.version / 100, library_version)));
	}

	/*
	 * Similarly, if the ABI extra field doesn't match, error out.  Other
	 * fields below might also mismatch, but that isn't useful information if
	 * you're using the wrong product altogether.
	 */
	if (strcmp(module_magic_data->abi_extra, magic_data.abi_extra) != 0)
	{
		ereport(ERROR,
				(errmsg("incompatible library \"%s\": ABI mismatch",
						libname),
				 errdetail("Server has ABI \"%s\", library has \"%s\".",
						   magic_data.abi_extra,
						   module_magic_data->abi_extra)));
	}

	/*
	 * Otherwise, spell out which fields don't agree.
	 *
	 * XXX this code has to be adjusted any time the set of fields in a magic
	 * block change!
	 */
	initStringInfo(&details);

	if (module_magic_data->funcmaxargs != magic_data.funcmaxargs)
	{
		if (details.len)
			appendStringInfoChar(&details, '\n');
		appendStringInfo(&details,
						 _("Server has FUNC_MAX_ARGS = %d, library has %d."),
						 magic_data.funcmaxargs,
						 module_magic_data->funcmaxargs);
	}
	if (module_magic_data->indexmaxkeys != magic_data.indexmaxkeys)
	{
		if (details.len)
			appendStringInfoChar(&details, '\n');
		appendStringInfo(&details,
						 _("Server has INDEX_MAX_KEYS = %d, library has %d."),
						 magic_data.indexmaxkeys,
						 module_magic_data->indexmaxkeys);
	}
	if (module_magic_data->namedatalen != magic_data.namedatalen)
	{
		if (details.len)
			appendStringInfoChar(&details, '\n');
		appendStringInfo(&details,
						 _("Server has NAMEDATALEN = %d, library has %d."),
						 magic_data.namedatalen,
						 module_magic_data->namedatalen);
	}
	if (module_magic_data->float8byval != magic_data.float8byval)
	{
		if (details.len)
			appendStringInfoChar(&details, '\n');
		appendStringInfo(&details,
						 _("Server has FLOAT8PASSBYVAL = %s, library has %s."),
						 magic_data.float8byval ? "true" : "false",
						 module_magic_data->float8byval ? "true" : "false");
	}

	if (details.len == 0)
		appendStringInfoString(&details,
							   _("Magic block has unexpected length or padding difference."));

	ereport(ERROR,
			(errmsg("incompatible library \"%s\": magic block mismatch",
					libname),
			 errdetail_internal("%s", details.data)));
}

static bool
file_exists(const char *name)
{
	struct stat st;

	AssertArg(name != NULL);

	if (stat(name, &st) == 0)
		return !S_ISDIR(st.st_mode);
	else if (!(errno == ENOENT || errno == ENOTDIR || errno == EACCES))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not access file \"%s\": %m", name)));

	return false;
}


/*
 * If name contains a slash, check if the file exists, if so return
 * the name.  Else (no slash) try to expand using search path (see
 * find_in_dynamic_libpath below); if that works, return the fully
 * expanded file name.  If the previous failed, append DLSUFFIX and
 * try again.  If all fails, just return the original name.
 *
 * The result will always be freshly palloc'd.
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

	new = psprintf("%s%s", name, DLSUFFIX);

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

	/*
	 * If we can't find the file, just return the string as-is. The ensuing
	 * load attempt will fail and report a suitable message.
	 */
	return pstrdup(name);
}

/*
 * Check a restricted library name.  It must begin with "$libdir/plugins/"
 * and there must not be any directory separators after that (this is
 * sufficient to prevent ".." style attacks).
 */
static void
check_restricted_library_name(const char *name)
{
	if (strncmp(name, "$libdir/plugins/", 16) != 0 ||
		first_dir_separator(name + 16) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("access to library \"%s\" is not allowed",
						name)));
}

/*
 * Substitute for any macros appearing in the given string.
 * Result is always freshly palloc'd.
 */
static char *
substitute_libpath_macro(const char *name)
{
	const char *sep_ptr;

	AssertArg(name != NULL);

	/* Currently, we only recognize $libdir at the start of the string */
	if (name[0] != '$')
		return pstrdup(name);

	if ((sep_ptr = first_dir_separator(name)) == NULL)
		sep_ptr = name + strlen(name);

	if (strlen("$libdir") != sep_ptr - name ||
		strncmp(name, "$libdir", strlen("$libdir")) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("invalid macro name in dynamic library path: %s",
						name)));

	return psprintf("%s%s", pkglib_path, sep_ptr);
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

		piece = first_path_var_separator(p);
		if (piece == p)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_NAME),
					 errmsg("zero-length component in parameter \"dynamic_library_path\"")));

		if (piece == NULL)
			len = strlen(p);
		else
			len = piece - p;

		piece = palloc(len + 1);
		strlcpy(piece, p, len + 1);

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


/*
 * Find (or create) a rendezvous variable that one dynamically
 * loaded library can use to meet up with another.
 *
 * On the first call of this function for a particular varName,
 * a "rendezvous variable" is created with the given name.
 * The value of the variable is a void pointer (initially set to NULL).
 * Subsequent calls with the same varName just return the address of
 * the existing variable.  Once created, a rendezvous variable lasts
 * for the life of the process.
 *
 * Dynamically loaded libraries can use rendezvous variables
 * to find each other and share information: they just need to agree
 * on the variable name and the data it will point to.
 */
void	  **
find_rendezvous_variable(const char *varName)
{
	static HTAB *rendezvousHash = NULL;

	rendezvousHashEntry *hentry;
	bool		found;

	/* Create a hashtable if we haven't already done so in this process */
	if (rendezvousHash == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = NAMEDATALEN;
		ctl.entrysize = sizeof(rendezvousHashEntry);
		rendezvousHash = hash_create("Rendezvous variable hash",
									 16,
									 &ctl,
									 HASH_ELEM | HASH_STRINGS);
	}

	/* Find or create the hashtable entry for this varName */
	hentry = (rendezvousHashEntry *) hash_search(rendezvousHash,
												 varName,
												 HASH_ENTER,
												 &found);

	/* Initialize to NULL if first time */
	if (!found)
		hentry->varValue = NULL;

	return &hentry->varValue;
}

/*
 * Estimate the amount of space needed to serialize the list of libraries
 * we have loaded.
 */
Size
EstimateLibraryStateSpace(void)
{
	DynamicFileList *file_scanner;
	Size		size = 1;

	for (file_scanner = file_list;
		 file_scanner != NULL;
		 file_scanner = file_scanner->next)
		size = add_size(size, strlen(file_scanner->filename) + 1);

	return size;
}

/*
 * Serialize the list of libraries we have loaded to a chunk of memory.
 */
void
SerializeLibraryState(Size maxsize, char *start_address)
{
	DynamicFileList *file_scanner;

	for (file_scanner = file_list;
		 file_scanner != NULL;
		 file_scanner = file_scanner->next)
	{
		Size		len;

		len = strlcpy(start_address, file_scanner->filename, maxsize) + 1;
		Assert(len < maxsize);
		maxsize -= len;
		start_address += len;
	}
	start_address[0] = '\0';
}

/*
 * Load every library the serializing backend had loaded.
 */
void
RestoreLibraryState(char *start_address)
{
	while (*start_address != '\0')
	{
		internal_load_library(start_address);
		start_address += strlen(start_address) + 1;
	}
}
