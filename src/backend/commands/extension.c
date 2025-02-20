/*-------------------------------------------------------------------------
 *
 * extension.c
 *	  Commands to manipulate extensions
 *
 * Extensions in PostgreSQL allow management of collections of SQL objects.
 *
 * All we need internally to manage an extension is an OID so that the
 * dependent objects can be associated with it.  An extension is created by
 * populating the pg_extension catalog from a "control" file.
 * The extension control file is parsed with the same parser we use for
 * postgresql.conf.  An extension also has an installation script file,
 * containing SQL commands to create the extension's objects.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/extension.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <dirent.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_database.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "commands/alter.h"
#include "commands/comment.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "commands/schemacmds.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/queryjumble.h"
#include "storage/fd.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/conffiles.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/varlena.h"


/* Globally visible state variables */
bool		creating_extension = false;
Oid			CurrentExtensionObject = InvalidOid;

/*
 * Internal data structure to hold the results of parsing a control file
 */
typedef struct ExtensionControlFile
{
	char	   *name;			/* name of the extension */
	char	   *directory;		/* directory for script files */
	char	   *default_version;	/* default install target version, if any */
	char	   *module_pathname;	/* string to substitute for
									 * MODULE_PATHNAME */
	char	   *comment;		/* comment, if any */
	char	   *schema;			/* target schema (allowed if !relocatable) */
	bool		relocatable;	/* is ALTER EXTENSION SET SCHEMA supported? */
	bool		superuser;		/* must be superuser to install? */
	bool		trusted;		/* allow becoming superuser on the fly? */
	int			encoding;		/* encoding of the script file, or -1 */
	List	   *requires;		/* names of prerequisite extensions */
	List	   *no_relocate;	/* names of prerequisite extensions that
								 * should not be relocated */
} ExtensionControlFile;

/*
 * Internal data structure for update path information
 */
typedef struct ExtensionVersionInfo
{
	char	   *name;			/* name of the starting version */
	List	   *reachable;		/* List of ExtensionVersionInfo's */
	bool		installable;	/* does this version have an install script? */
	/* working state for Dijkstra's algorithm: */
	bool		distance_known; /* is distance from start known yet? */
	int			distance;		/* current worst-case distance estimate */
	struct ExtensionVersionInfo *previous;	/* current best predecessor */
} ExtensionVersionInfo;

/*
 * Information for script_error_callback()
 */
typedef struct
{
	const char *sql;			/* entire script file contents */
	const char *filename;		/* script file pathname */
	ParseLoc	stmt_location;	/* current stmt start loc, or -1 if unknown */
	ParseLoc	stmt_len;		/* length in bytes; 0 means "rest of string" */
} script_error_callback_arg;

/* Local functions */
static List *find_update_path(List *evi_list,
							  ExtensionVersionInfo *evi_start,
							  ExtensionVersionInfo *evi_target,
							  bool reject_indirect,
							  bool reinitialize);
static Oid	get_required_extension(char *reqExtensionName,
								   char *extensionName,
								   char *origSchemaName,
								   bool cascade,
								   List *parents,
								   bool is_create);
static void get_available_versions_for_extension(ExtensionControlFile *pcontrol,
												 Tuplestorestate *tupstore,
												 TupleDesc tupdesc);
static Datum convert_requires_to_datum(List *requires);
static void ApplyExtensionUpdates(Oid extensionOid,
								  ExtensionControlFile *pcontrol,
								  const char *initialVersion,
								  List *updateVersions,
								  char *origSchemaName,
								  bool cascade,
								  bool is_create);
static void ExecAlterExtensionContentsRecurse(AlterExtensionContentsStmt *stmt,
											  ObjectAddress extension,
											  ObjectAddress object);
static char *read_whole_file(const char *filename, int *length);


/*
 * get_extension_oid - given an extension name, look up the OID
 *
 * If missing_ok is false, throw an error if extension name not found.  If
 * true, just return InvalidOid.
 */
Oid
get_extension_oid(const char *extname, bool missing_ok)
{
	Oid			result;

	result = GetSysCacheOid1(EXTENSIONNAME, Anum_pg_extension_oid,
							 CStringGetDatum(extname));

	if (!OidIsValid(result) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("extension \"%s\" does not exist",
						extname)));

	return result;
}

/*
 * get_extension_name - given an extension OID, look up the name
 *
 * Returns a palloc'd string, or NULL if no such extension.
 */
char *
get_extension_name(Oid ext_oid)
{
	char	   *result;
	HeapTuple	tuple;

	tuple = SearchSysCache1(EXTENSIONOID, ObjectIdGetDatum(ext_oid));

	if (!HeapTupleIsValid(tuple))
		return NULL;

	result = pstrdup(NameStr(((Form_pg_extension) GETSTRUCT(tuple))->extname));
	ReleaseSysCache(tuple);

	return result;
}

/*
 * get_extension_schema - given an extension OID, fetch its extnamespace
 *
 * Returns InvalidOid if no such extension.
 */
Oid
get_extension_schema(Oid ext_oid)
{
	Oid			result;
	HeapTuple	tuple;

	tuple = SearchSysCache1(EXTENSIONOID, ObjectIdGetDatum(ext_oid));

	if (!HeapTupleIsValid(tuple))
		return InvalidOid;

	result = ((Form_pg_extension) GETSTRUCT(tuple))->extnamespace;
	ReleaseSysCache(tuple);

	return result;
}

/*
 * Utility functions to check validity of extension and version names
 */
static void
check_valid_extension_name(const char *extensionname)
{
	int			namelen = strlen(extensionname);

	/*
	 * Disallow empty names (the parser rejects empty identifiers anyway, but
	 * let's check).
	 */
	if (namelen == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension name: \"%s\"", extensionname),
				 errdetail("Extension names must not be empty.")));

	/*
	 * No double dashes, since that would make script filenames ambiguous.
	 */
	if (strstr(extensionname, "--"))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension name: \"%s\"", extensionname),
				 errdetail("Extension names must not contain \"--\".")));

	/*
	 * No leading or trailing dash either.  (We could probably allow this, but
	 * it would require much care in filename parsing and would make filenames
	 * visually if not formally ambiguous.  Since there's no real-world use
	 * case, let's just forbid it.)
	 */
	if (extensionname[0] == '-' || extensionname[namelen - 1] == '-')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension name: \"%s\"", extensionname),
				 errdetail("Extension names must not begin or end with \"-\".")));

	/*
	 * No directory separators either (this is sufficient to prevent ".."
	 * style attacks).
	 */
	if (first_dir_separator(extensionname) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension name: \"%s\"", extensionname),
				 errdetail("Extension names must not contain directory separator characters.")));
}

static void
check_valid_version_name(const char *versionname)
{
	int			namelen = strlen(versionname);

	/*
	 * Disallow empty names (we could possibly allow this, but there seems
	 * little point).
	 */
	if (namelen == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension version name: \"%s\"", versionname),
				 errdetail("Version names must not be empty.")));

	/*
	 * No double dashes, since that would make script filenames ambiguous.
	 */
	if (strstr(versionname, "--"))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension version name: \"%s\"", versionname),
				 errdetail("Version names must not contain \"--\".")));

	/*
	 * No leading or trailing dash either.
	 */
	if (versionname[0] == '-' || versionname[namelen - 1] == '-')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension version name: \"%s\"", versionname),
				 errdetail("Version names must not begin or end with \"-\".")));

	/*
	 * No directory separators either (this is sufficient to prevent ".."
	 * style attacks).
	 */
	if (first_dir_separator(versionname) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension version name: \"%s\"", versionname),
				 errdetail("Version names must not contain directory separator characters.")));
}

/*
 * Utility functions to handle extension-related path names
 */
static bool
is_extension_control_filename(const char *filename)
{
	const char *extension = strrchr(filename, '.');

	return (extension != NULL) && (strcmp(extension, ".control") == 0);
}

static bool
is_extension_script_filename(const char *filename)
{
	const char *extension = strrchr(filename, '.');

	return (extension != NULL) && (strcmp(extension, ".sql") == 0);
}

static char *
get_extension_control_directory(void)
{
	char		sharepath[MAXPGPATH];
	char	   *result;

	get_share_path(my_exec_path, sharepath);
	result = (char *) palloc(MAXPGPATH);
	snprintf(result, MAXPGPATH, "%s/extension", sharepath);

	return result;
}

static char *
get_extension_control_filename(const char *extname)
{
	char		sharepath[MAXPGPATH];
	char	   *result;

	get_share_path(my_exec_path, sharepath);
	result = (char *) palloc(MAXPGPATH);
	snprintf(result, MAXPGPATH, "%s/extension/%s.control",
			 sharepath, extname);

	return result;
}

static char *
get_extension_script_directory(ExtensionControlFile *control)
{
	char		sharepath[MAXPGPATH];
	char	   *result;

	/*
	 * The directory parameter can be omitted, absolute, or relative to the
	 * installation's share directory.
	 */
	if (!control->directory)
		return get_extension_control_directory();

	if (is_absolute_path(control->directory))
		return pstrdup(control->directory);

	get_share_path(my_exec_path, sharepath);
	result = (char *) palloc(MAXPGPATH);
	snprintf(result, MAXPGPATH, "%s/%s", sharepath, control->directory);

	return result;
}

static char *
get_extension_aux_control_filename(ExtensionControlFile *control,
								   const char *version)
{
	char	   *result;
	char	   *scriptdir;

	scriptdir = get_extension_script_directory(control);

	result = (char *) palloc(MAXPGPATH);
	snprintf(result, MAXPGPATH, "%s/%s--%s.control",
			 scriptdir, control->name, version);

	pfree(scriptdir);

	return result;
}

static char *
get_extension_script_filename(ExtensionControlFile *control,
							  const char *from_version, const char *version)
{
	char	   *result;
	char	   *scriptdir;

	scriptdir = get_extension_script_directory(control);

	result = (char *) palloc(MAXPGPATH);
	if (from_version)
		snprintf(result, MAXPGPATH, "%s/%s--%s--%s.sql",
				 scriptdir, control->name, from_version, version);
	else
		snprintf(result, MAXPGPATH, "%s/%s--%s.sql",
				 scriptdir, control->name, version);

	pfree(scriptdir);

	return result;
}


/*
 * Parse contents of primary or auxiliary control file, and fill in
 * fields of *control.  We parse primary file if version == NULL,
 * else the optional auxiliary file for that version.
 *
 * Control files are supposed to be very short, half a dozen lines,
 * so we don't worry about memory allocation risks here.  Also we don't
 * worry about what encoding it's in; all values are expected to be ASCII.
 */
static void
parse_extension_control_file(ExtensionControlFile *control,
							 const char *version)
{
	char	   *filename;
	FILE	   *file;
	ConfigVariable *item,
			   *head = NULL,
			   *tail = NULL;

	/*
	 * Locate the file to read.  Auxiliary files are optional.
	 */
	if (version)
		filename = get_extension_aux_control_filename(control, version);
	else
		filename = get_extension_control_filename(control->name);

	if ((file = AllocateFile(filename, "r")) == NULL)
	{
		if (errno == ENOENT)
		{
			/* no complaint for missing auxiliary file */
			if (version)
			{
				pfree(filename);
				return;
			}

			/* missing control file indicates extension is not installed */
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("extension \"%s\" is not available", control->name),
					 errdetail("Could not open extension control file \"%s\": %m.",
							   filename),
					 errhint("The extension must first be installed on the system where PostgreSQL is running.")));
		}
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open extension control file \"%s\": %m",
						filename)));
	}

	/*
	 * Parse the file content, using GUC's file parsing code.  We need not
	 * check the return value since any errors will be thrown at ERROR level.
	 */
	(void) ParseConfigFp(file, filename, CONF_FILE_START_DEPTH, ERROR,
						 &head, &tail);

	FreeFile(file);

	/*
	 * Convert the ConfigVariable list into ExtensionControlFile entries.
	 */
	for (item = head; item != NULL; item = item->next)
	{
		if (strcmp(item->name, "directory") == 0)
		{
			if (version)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("parameter \"%s\" cannot be set in a secondary extension control file",
								item->name)));

			control->directory = pstrdup(item->value);
		}
		else if (strcmp(item->name, "default_version") == 0)
		{
			if (version)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("parameter \"%s\" cannot be set in a secondary extension control file",
								item->name)));

			control->default_version = pstrdup(item->value);
		}
		else if (strcmp(item->name, "module_pathname") == 0)
		{
			control->module_pathname = pstrdup(item->value);
		}
		else if (strcmp(item->name, "comment") == 0)
		{
			control->comment = pstrdup(item->value);
		}
		else if (strcmp(item->name, "schema") == 0)
		{
			control->schema = pstrdup(item->value);
		}
		else if (strcmp(item->name, "relocatable") == 0)
		{
			if (!parse_bool(item->value, &control->relocatable))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("parameter \"%s\" requires a Boolean value",
								item->name)));
		}
		else if (strcmp(item->name, "superuser") == 0)
		{
			if (!parse_bool(item->value, &control->superuser))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("parameter \"%s\" requires a Boolean value",
								item->name)));
		}
		else if (strcmp(item->name, "trusted") == 0)
		{
			if (!parse_bool(item->value, &control->trusted))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("parameter \"%s\" requires a Boolean value",
								item->name)));
		}
		else if (strcmp(item->name, "encoding") == 0)
		{
			control->encoding = pg_valid_server_encoding(item->value);
			if (control->encoding < 0)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("\"%s\" is not a valid encoding name",
								item->value)));
		}
		else if (strcmp(item->name, "requires") == 0)
		{
			/* Need a modifiable copy of string */
			char	   *rawnames = pstrdup(item->value);

			/* Parse string into list of identifiers */
			if (!SplitIdentifierString(rawnames, ',', &control->requires))
			{
				/* syntax error in name list */
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("parameter \"%s\" must be a list of extension names",
								item->name)));
			}
		}
		else if (strcmp(item->name, "no_relocate") == 0)
		{
			/* Need a modifiable copy of string */
			char	   *rawnames = pstrdup(item->value);

			/* Parse string into list of identifiers */
			if (!SplitIdentifierString(rawnames, ',', &control->no_relocate))
			{
				/* syntax error in name list */
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("parameter \"%s\" must be a list of extension names",
								item->name)));
			}
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized parameter \"%s\" in file \"%s\"",
							item->name, filename)));
	}

	FreeConfigVariables(head);

	if (control->relocatable && control->schema != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("parameter \"schema\" cannot be specified when \"relocatable\" is true")));

	pfree(filename);
}

/*
 * Read the primary control file for the specified extension.
 */
static ExtensionControlFile *
read_extension_control_file(const char *extname)
{
	ExtensionControlFile *control;

	/*
	 * Set up default values.  Pointer fields are initially null.
	 */
	control = (ExtensionControlFile *) palloc0(sizeof(ExtensionControlFile));
	control->name = pstrdup(extname);
	control->relocatable = false;
	control->superuser = true;
	control->trusted = false;
	control->encoding = -1;

	/*
	 * Parse the primary control file.
	 */
	parse_extension_control_file(control, NULL);

	return control;
}

/*
 * Read the auxiliary control file for the specified extension and version.
 *
 * Returns a new modified ExtensionControlFile struct; the original struct
 * (reflecting just the primary control file) is not modified.
 */
static ExtensionControlFile *
read_extension_aux_control_file(const ExtensionControlFile *pcontrol,
								const char *version)
{
	ExtensionControlFile *acontrol;

	/*
	 * Flat-copy the struct.  Pointer fields share values with original.
	 */
	acontrol = (ExtensionControlFile *) palloc(sizeof(ExtensionControlFile));
	memcpy(acontrol, pcontrol, sizeof(ExtensionControlFile));

	/*
	 * Parse the auxiliary control file, overwriting struct fields
	 */
	parse_extension_control_file(acontrol, version);

	return acontrol;
}

/*
 * Read an SQL script file into a string, and convert to database encoding
 */
static char *
read_extension_script_file(const ExtensionControlFile *control,
						   const char *filename)
{
	int			src_encoding;
	char	   *src_str;
	char	   *dest_str;
	int			len;

	src_str = read_whole_file(filename, &len);

	/* use database encoding if not given */
	if (control->encoding < 0)
		src_encoding = GetDatabaseEncoding();
	else
		src_encoding = control->encoding;

	/* make sure that source string is valid in the expected encoding */
	(void) pg_verify_mbstr(src_encoding, src_str, len, false);

	/*
	 * Convert the encoding to the database encoding. read_whole_file
	 * null-terminated the string, so if no conversion happens the string is
	 * valid as is.
	 */
	dest_str = pg_any_to_server(src_str, len, src_encoding);

	return dest_str;
}

/*
 * error context callback for failures in script-file execution
 */
static void
script_error_callback(void *arg)
{
	script_error_callback_arg *callback_arg = (script_error_callback_arg *) arg;
	const char *query = callback_arg->sql;
	int			location = callback_arg->stmt_location;
	int			len = callback_arg->stmt_len;
	int			syntaxerrposition;
	const char *lastslash;

	/*
	 * If there is a syntax error position, convert to internal syntax error;
	 * otherwise report the current query as an item of context stack.
	 *
	 * Note: we'll provide no context except the filename if there's neither
	 * an error position nor any known current query.  That shouldn't happen
	 * though: all errors reported during raw parsing should come with an
	 * error position.
	 */
	syntaxerrposition = geterrposition();
	if (syntaxerrposition > 0)
	{
		/*
		 * If we do not know the bounds of the current statement (as would
		 * happen for an error occurring during initial raw parsing), we have
		 * to use a heuristic to decide how much of the script to show.  We'll
		 * also use the heuristic in the unlikely case that syntaxerrposition
		 * is outside what we think the statement bounds are.
		 */
		if (location < 0 || syntaxerrposition < location ||
			(len > 0 && syntaxerrposition > location + len))
		{
			/*
			 * Our heuristic is pretty simple: look for semicolon-newline
			 * sequences, and break at the last one strictly before
			 * syntaxerrposition and the first one strictly after.  It's
			 * certainly possible to fool this with semicolon-newline embedded
			 * in a string literal, but it seems better to do this than to
			 * show the entire extension script.
			 *
			 * Notice we cope with Windows-style newlines (\r\n) regardless of
			 * platform.  This is because there might be such newlines in
			 * script files on other platforms.
			 */
			int			slen = strlen(query);

			location = len = 0;
			for (int loc = 0; loc < slen; loc++)
			{
				if (query[loc] != ';')
					continue;
				if (query[loc + 1] == '\r')
					loc++;
				if (query[loc + 1] == '\n')
				{
					int			bkpt = loc + 2;

					if (bkpt < syntaxerrposition)
						location = bkpt;
					else if (bkpt > syntaxerrposition)
					{
						len = bkpt - location;
						break;	/* no need to keep searching */
					}
				}
			}
		}

		/* Trim leading/trailing whitespace, for consistency */
		query = CleanQuerytext(query, &location, &len);

		/*
		 * Adjust syntaxerrposition.  It shouldn't be pointing into the
		 * whitespace we just trimmed, but cope if it is.
		 */
		syntaxerrposition -= location;
		if (syntaxerrposition < 0)
			syntaxerrposition = 0;
		else if (syntaxerrposition > len)
			syntaxerrposition = len;

		/* And report. */
		errposition(0);
		internalerrposition(syntaxerrposition);
		internalerrquery(pnstrdup(query, len));
	}
	else if (location >= 0)
	{
		/*
		 * Since no syntax cursor will be shown, it's okay and helpful to trim
		 * the reported query string to just the current statement.
		 */
		query = CleanQuerytext(query, &location, &len);
		errcontext("SQL statement \"%.*s\"", len, query);
	}

	/*
	 * Trim the reported file name to remove the path.  We know that
	 * get_extension_script_filename() inserted a '/', regardless of whether
	 * we're on Windows.
	 */
	lastslash = strrchr(callback_arg->filename, '/');
	if (lastslash)
		lastslash++;
	else
		lastslash = callback_arg->filename; /* shouldn't happen, but cope */

	/*
	 * If we have a location (which, as said above, we really always should)
	 * then report a line number to aid in localizing problems in big scripts.
	 */
	if (location >= 0)
	{
		int			linenumber = 1;

		for (query = callback_arg->sql; *query; query++)
		{
			if (--location < 0)
				break;
			if (*query == '\n')
				linenumber++;
		}
		errcontext("extension script file \"%s\", near line %d",
				   lastslash, linenumber);
	}
	else
		errcontext("extension script file \"%s\"", lastslash);
}

/*
 * Execute given SQL string.
 *
 * The filename the string came from is also provided, for error reporting.
 *
 * Note: it's tempting to just use SPI to execute the string, but that does
 * not work very well.  The really serious problem is that SPI will parse,
 * analyze, and plan the whole string before executing any of it; of course
 * this fails if there are any plannable statements referring to objects
 * created earlier in the script.  A lesser annoyance is that SPI insists
 * on printing the whole string as errcontext in case of any error, and that
 * could be very long.
 */
static void
execute_sql_string(const char *sql, const char *filename)
{
	script_error_callback_arg callback_arg;
	ErrorContextCallback scripterrcontext;
	List	   *raw_parsetree_list;
	DestReceiver *dest;
	ListCell   *lc1;

	/*
	 * Setup error traceback support for ereport().
	 */
	callback_arg.sql = sql;
	callback_arg.filename = filename;
	callback_arg.stmt_location = -1;
	callback_arg.stmt_len = -1;

	scripterrcontext.callback = script_error_callback;
	scripterrcontext.arg = (void *) &callback_arg;
	scripterrcontext.previous = error_context_stack;
	error_context_stack = &scripterrcontext;

	/*
	 * Parse the SQL string into a list of raw parse trees.
	 */
	raw_parsetree_list = pg_parse_query(sql);

	/* All output from SELECTs goes to the bit bucket */
	dest = CreateDestReceiver(DestNone);

	/*
	 * Do parse analysis, rule rewrite, planning, and execution for each raw
	 * parsetree.  We must fully execute each query before beginning parse
	 * analysis on the next one, since there may be interdependencies.
	 */
	foreach(lc1, raw_parsetree_list)
	{
		RawStmt    *parsetree = lfirst_node(RawStmt, lc1);
		MemoryContext per_parsetree_context,
					oldcontext;
		List	   *stmt_list;
		ListCell   *lc2;

		/* Report location of this query for error context callback */
		callback_arg.stmt_location = parsetree->stmt_location;
		callback_arg.stmt_len = parsetree->stmt_len;

		/*
		 * We do the work for each parsetree in a short-lived context, to
		 * limit the memory used when there are many commands in the string.
		 */
		per_parsetree_context =
			AllocSetContextCreate(CurrentMemoryContext,
								  "execute_sql_string per-statement context",
								  ALLOCSET_DEFAULT_SIZES);
		oldcontext = MemoryContextSwitchTo(per_parsetree_context);

		/* Be sure parser can see any DDL done so far */
		CommandCounterIncrement();

		stmt_list = pg_analyze_and_rewrite_fixedparams(parsetree,
													   sql,
													   NULL,
													   0,
													   NULL);
		stmt_list = pg_plan_queries(stmt_list, sql, CURSOR_OPT_PARALLEL_OK, NULL);

		foreach(lc2, stmt_list)
		{
			PlannedStmt *stmt = lfirst_node(PlannedStmt, lc2);

			CommandCounterIncrement();

			PushActiveSnapshot(GetTransactionSnapshot());

			if (stmt->utilityStmt == NULL)
			{
				QueryDesc  *qdesc;

				qdesc = CreateQueryDesc(stmt,
										NULL,
										sql,
										GetActiveSnapshot(), NULL,
										dest, NULL, NULL, 0);

				if (!ExecutorStart(qdesc, 0))
					elog(ERROR, "ExecutorStart() failed unexpectedly");
				ExecutorRun(qdesc, ForwardScanDirection, 0);
				ExecutorFinish(qdesc);
				ExecutorEnd(qdesc);

				FreeQueryDesc(qdesc);
			}
			else
			{
				if (IsA(stmt->utilityStmt, TransactionStmt))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("transaction control statements are not allowed within an extension script")));

				ProcessUtility(stmt,
							   sql,
							   false,
							   PROCESS_UTILITY_QUERY,
							   NULL,
							   NULL,
							   dest,
							   NULL);
			}

			PopActiveSnapshot();
		}

		/* Clean up per-parsetree context. */
		MemoryContextSwitchTo(oldcontext);
		MemoryContextDelete(per_parsetree_context);
	}

	error_context_stack = scripterrcontext.previous;

	/* Be sure to advance the command counter after the last script command */
	CommandCounterIncrement();
}

/*
 * Policy function: is the given extension trusted for installation by a
 * non-superuser?
 *
 * (Update the errhint logic below if you change this.)
 */
static bool
extension_is_trusted(ExtensionControlFile *control)
{
	AclResult	aclresult;

	/* Never trust unless extension's control file says it's okay */
	if (!control->trusted)
		return false;
	/* Allow if user has CREATE privilege on current database */
	aclresult = object_aclcheck(DatabaseRelationId, MyDatabaseId, GetUserId(), ACL_CREATE);
	if (aclresult == ACLCHECK_OK)
		return true;
	return false;
}

/*
 * Execute the appropriate script file for installing or updating the extension
 *
 * If from_version isn't NULL, it's an update
 *
 * Note: requiredSchemas must be one-for-one with the control->requires list
 */
static void
execute_extension_script(Oid extensionOid, ExtensionControlFile *control,
						 const char *from_version,
						 const char *version,
						 List *requiredSchemas,
						 const char *schemaName)
{
	bool		switch_to_superuser = false;
	char	   *filename;
	Oid			save_userid = 0;
	int			save_sec_context = 0;
	int			save_nestlevel;
	StringInfoData pathbuf;
	ListCell   *lc;
	ListCell   *lc2;

	/*
	 * Enforce superuser-ness if appropriate.  We postpone these checks until
	 * here so that the control flags are correctly associated with the right
	 * script(s) if they happen to be set in secondary control files.
	 */
	if (control->superuser && !superuser())
	{
		if (extension_is_trusted(control))
			switch_to_superuser = true;
		else if (from_version == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to create extension \"%s\"",
							control->name),
					 control->trusted
					 ? errhint("Must have CREATE privilege on current database to create this extension.")
					 : errhint("Must be superuser to create this extension.")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to update extension \"%s\"",
							control->name),
					 control->trusted
					 ? errhint("Must have CREATE privilege on current database to update this extension.")
					 : errhint("Must be superuser to update this extension.")));
	}

	filename = get_extension_script_filename(control, from_version, version);

	if (from_version == NULL)
		elog(DEBUG1, "executing extension script for \"%s\" version '%s'", control->name, version);
	else
		elog(DEBUG1, "executing extension script for \"%s\" update from version '%s' to '%s'", control->name, from_version, version);

	/*
	 * If installing a trusted extension on behalf of a non-superuser, become
	 * the bootstrap superuser.  (This switch will be cleaned up automatically
	 * if the transaction aborts, as will the GUC changes below.)
	 */
	if (switch_to_superuser)
	{
		GetUserIdAndSecContext(&save_userid, &save_sec_context);
		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID,
							   save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
	}

	/*
	 * Force client_min_messages and log_min_messages to be at least WARNING,
	 * so that we won't spam the user with useless NOTICE messages from common
	 * script actions like creating shell types.
	 *
	 * We use the equivalent of a function SET option to allow the setting to
	 * persist for exactly the duration of the script execution.  guc.c also
	 * takes care of undoing the setting on error.
	 *
	 * log_min_messages can't be set by ordinary users, so for that one we
	 * pretend to be superuser.
	 */
	save_nestlevel = NewGUCNestLevel();

	if (client_min_messages < WARNING)
		(void) set_config_option("client_min_messages", "warning",
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, 0, false);
	if (log_min_messages < WARNING)
		(void) set_config_option_ext("log_min_messages", "warning",
									 PGC_SUSET, PGC_S_SESSION,
									 BOOTSTRAP_SUPERUSERID,
									 GUC_ACTION_SAVE, true, 0, false);

	/*
	 * Similarly disable check_function_bodies, to ensure that SQL functions
	 * won't be parsed during creation.
	 */
	if (check_function_bodies)
		(void) set_config_option("check_function_bodies", "off",
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, 0, false);

	/*
	 * Set up the search path to have the target schema first, making it be
	 * the default creation target namespace.  Then add the schemas of any
	 * prerequisite extensions, unless they are in pg_catalog which would be
	 * searched anyway.  (Listing pg_catalog explicitly in a non-first
	 * position would be bad for security.)  Finally add pg_temp to ensure
	 * that temp objects can't take precedence over others.
	 */
	initStringInfo(&pathbuf);
	appendStringInfoString(&pathbuf, quote_identifier(schemaName));
	foreach(lc, requiredSchemas)
	{
		Oid			reqschema = lfirst_oid(lc);
		char	   *reqname = get_namespace_name(reqschema);

		if (reqname && strcmp(reqname, "pg_catalog") != 0)
			appendStringInfo(&pathbuf, ", %s", quote_identifier(reqname));
	}
	appendStringInfoString(&pathbuf, ", pg_temp");

	(void) set_config_option("search_path", pathbuf.data,
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);

	/*
	 * Set creating_extension and related variables so that
	 * recordDependencyOnCurrentExtension and other functions do the right
	 * things.  On failure, ensure we reset these variables.
	 */
	creating_extension = true;
	CurrentExtensionObject = extensionOid;
	PG_TRY();
	{
		char	   *c_sql = read_extension_script_file(control, filename);
		Datum		t_sql;

		/*
		 * We filter each substitution through quote_identifier().  When the
		 * arg contains one of the following characters, no one collection of
		 * quoting can work inside $$dollar-quoted string literals$$,
		 * 'single-quoted string literals', and outside of any literal.  To
		 * avoid a security snare for extension authors, error on substitution
		 * for arguments containing these.
		 */
		const char *quoting_relevant_chars = "\"$'\\";

		/* We use various functions that want to operate on text datums */
		t_sql = CStringGetTextDatum(c_sql);

		/*
		 * Reduce any lines beginning with "\echo" to empty.  This allows
		 * scripts to contain messages telling people not to run them via
		 * psql, which has been found to be necessary due to old habits.
		 */
		t_sql = DirectFunctionCall4Coll(textregexreplace,
										C_COLLATION_OID,
										t_sql,
										CStringGetTextDatum("^\\\\echo.*$"),
										CStringGetTextDatum(""),
										CStringGetTextDatum("ng"));

		/*
		 * If the script uses @extowner@, substitute the calling username.
		 */
		if (strstr(c_sql, "@extowner@"))
		{
			Oid			uid = switch_to_superuser ? save_userid : GetUserId();
			const char *userName = GetUserNameFromId(uid, false);
			const char *qUserName = quote_identifier(userName);

			t_sql = DirectFunctionCall3Coll(replace_text,
											C_COLLATION_OID,
											t_sql,
											CStringGetTextDatum("@extowner@"),
											CStringGetTextDatum(qUserName));
			if (strpbrk(userName, quoting_relevant_chars))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid character in extension owner: must not contain any of \"%s\"",
								quoting_relevant_chars)));
		}

		/*
		 * If it's not relocatable, substitute the target schema name for
		 * occurrences of @extschema@.
		 *
		 * For a relocatable extension, we needn't do this.  There cannot be
		 * any need for @extschema@, else it wouldn't be relocatable.
		 */
		if (!control->relocatable)
		{
			Datum		old = t_sql;
			const char *qSchemaName = quote_identifier(schemaName);

			t_sql = DirectFunctionCall3Coll(replace_text,
											C_COLLATION_OID,
											t_sql,
											CStringGetTextDatum("@extschema@"),
											CStringGetTextDatum(qSchemaName));
			if (t_sql != old && strpbrk(schemaName, quoting_relevant_chars))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid character in extension \"%s\" schema: must not contain any of \"%s\"",
								control->name, quoting_relevant_chars)));
		}

		/*
		 * Likewise, substitute required extensions' schema names for
		 * occurrences of @extschema:extension_name@.
		 */
		Assert(list_length(control->requires) == list_length(requiredSchemas));
		forboth(lc, control->requires, lc2, requiredSchemas)
		{
			Datum		old = t_sql;
			char	   *reqextname = (char *) lfirst(lc);
			Oid			reqschema = lfirst_oid(lc2);
			char	   *schemaName = get_namespace_name(reqschema);
			const char *qSchemaName = quote_identifier(schemaName);
			char	   *repltoken;

			repltoken = psprintf("@extschema:%s@", reqextname);
			t_sql = DirectFunctionCall3Coll(replace_text,
											C_COLLATION_OID,
											t_sql,
											CStringGetTextDatum(repltoken),
											CStringGetTextDatum(qSchemaName));
			if (t_sql != old && strpbrk(schemaName, quoting_relevant_chars))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid character in extension \"%s\" schema: must not contain any of \"%s\"",
								reqextname, quoting_relevant_chars)));
		}

		/*
		 * If module_pathname was set in the control file, substitute its
		 * value for occurrences of MODULE_PATHNAME.
		 */
		if (control->module_pathname)
		{
			t_sql = DirectFunctionCall3Coll(replace_text,
											C_COLLATION_OID,
											t_sql,
											CStringGetTextDatum("MODULE_PATHNAME"),
											CStringGetTextDatum(control->module_pathname));
		}

		/* And now back to C string */
		c_sql = text_to_cstring(DatumGetTextPP(t_sql));

		execute_sql_string(c_sql, filename);
	}
	PG_FINALLY();
	{
		creating_extension = false;
		CurrentExtensionObject = InvalidOid;
	}
	PG_END_TRY();

	/*
	 * Restore the GUC variables we set above.
	 */
	AtEOXact_GUC(true, save_nestlevel);

	/*
	 * Restore authentication state if needed.
	 */
	if (switch_to_superuser)
		SetUserIdAndSecContext(save_userid, save_sec_context);
}

/*
 * Find or create an ExtensionVersionInfo for the specified version name
 *
 * Currently, we just use a List of the ExtensionVersionInfo's.  Searching
 * for them therefore uses about O(N^2) time when there are N versions of
 * the extension.  We could change the data structure to a hash table if
 * this ever becomes a bottleneck.
 */
static ExtensionVersionInfo *
get_ext_ver_info(const char *versionname, List **evi_list)
{
	ExtensionVersionInfo *evi;
	ListCell   *lc;

	foreach(lc, *evi_list)
	{
		evi = (ExtensionVersionInfo *) lfirst(lc);
		if (strcmp(evi->name, versionname) == 0)
			return evi;
	}

	evi = (ExtensionVersionInfo *) palloc(sizeof(ExtensionVersionInfo));
	evi->name = pstrdup(versionname);
	evi->reachable = NIL;
	evi->installable = false;
	/* initialize for later application of Dijkstra's algorithm */
	evi->distance_known = false;
	evi->distance = INT_MAX;
	evi->previous = NULL;

	*evi_list = lappend(*evi_list, evi);

	return evi;
}

/*
 * Locate the nearest unprocessed ExtensionVersionInfo
 *
 * This part of the algorithm is also about O(N^2).  A priority queue would
 * make it much faster, but for now there's no need.
 */
static ExtensionVersionInfo *
get_nearest_unprocessed_vertex(List *evi_list)
{
	ExtensionVersionInfo *evi = NULL;
	ListCell   *lc;

	foreach(lc, evi_list)
	{
		ExtensionVersionInfo *evi2 = (ExtensionVersionInfo *) lfirst(lc);

		/* only vertices whose distance is still uncertain are candidates */
		if (evi2->distance_known)
			continue;
		/* remember the closest such vertex */
		if (evi == NULL ||
			evi->distance > evi2->distance)
			evi = evi2;
	}

	return evi;
}

/*
 * Obtain information about the set of update scripts available for the
 * specified extension.  The result is a List of ExtensionVersionInfo
 * structs, each with a subsidiary list of the ExtensionVersionInfos for
 * the versions that can be reached in one step from that version.
 */
static List *
get_ext_ver_list(ExtensionControlFile *control)
{
	List	   *evi_list = NIL;
	int			extnamelen = strlen(control->name);
	char	   *location;
	DIR		   *dir;
	struct dirent *de;

	location = get_extension_script_directory(control);
	dir = AllocateDir(location);
	while ((de = ReadDir(dir, location)) != NULL)
	{
		char	   *vername;
		char	   *vername2;
		ExtensionVersionInfo *evi;
		ExtensionVersionInfo *evi2;

		/* must be a .sql file ... */
		if (!is_extension_script_filename(de->d_name))
			continue;

		/* ... matching extension name followed by separator */
		if (strncmp(de->d_name, control->name, extnamelen) != 0 ||
			de->d_name[extnamelen] != '-' ||
			de->d_name[extnamelen + 1] != '-')
			continue;

		/* extract version name(s) from 'extname--something.sql' filename */
		vername = pstrdup(de->d_name + extnamelen + 2);
		*strrchr(vername, '.') = '\0';
		vername2 = strstr(vername, "--");
		if (!vername2)
		{
			/* It's an install, not update, script; record its version name */
			evi = get_ext_ver_info(vername, &evi_list);
			evi->installable = true;
			continue;
		}
		*vername2 = '\0';		/* terminate first version */
		vername2 += 2;			/* and point to second */

		/* if there's a third --, it's bogus, ignore it */
		if (strstr(vername2, "--"))
			continue;

		/* Create ExtensionVersionInfos and link them together */
		evi = get_ext_ver_info(vername, &evi_list);
		evi2 = get_ext_ver_info(vername2, &evi_list);
		evi->reachable = lappend(evi->reachable, evi2);
	}
	FreeDir(dir);

	return evi_list;
}

/*
 * Given an initial and final version name, identify the sequence of update
 * scripts that have to be applied to perform that update.
 *
 * Result is a List of names of versions to transition through (the initial
 * version is *not* included).
 */
static List *
identify_update_path(ExtensionControlFile *control,
					 const char *oldVersion, const char *newVersion)
{
	List	   *result;
	List	   *evi_list;
	ExtensionVersionInfo *evi_start;
	ExtensionVersionInfo *evi_target;

	/* Extract the version update graph from the script directory */
	evi_list = get_ext_ver_list(control);

	/* Initialize start and end vertices */
	evi_start = get_ext_ver_info(oldVersion, &evi_list);
	evi_target = get_ext_ver_info(newVersion, &evi_list);

	/* Find shortest path */
	result = find_update_path(evi_list, evi_start, evi_target, false, false);

	if (result == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("extension \"%s\" has no update path from version \"%s\" to version \"%s\"",
						control->name, oldVersion, newVersion)));

	return result;
}

/*
 * Apply Dijkstra's algorithm to find the shortest path from evi_start to
 * evi_target.
 *
 * If reject_indirect is true, ignore paths that go through installable
 * versions.  This saves work when the caller will consider starting from
 * all installable versions anyway.
 *
 * If reinitialize is false, assume the ExtensionVersionInfo list has not
 * been used for this before, and the initialization done by get_ext_ver_info
 * is still good.  Otherwise, reinitialize all transient fields used here.
 *
 * Result is a List of names of versions to transition through (the initial
 * version is *not* included).  Returns NIL if no such path.
 */
static List *
find_update_path(List *evi_list,
				 ExtensionVersionInfo *evi_start,
				 ExtensionVersionInfo *evi_target,
				 bool reject_indirect,
				 bool reinitialize)
{
	List	   *result;
	ExtensionVersionInfo *evi;
	ListCell   *lc;

	/* Caller error if start == target */
	Assert(evi_start != evi_target);
	/* Caller error if reject_indirect and target is installable */
	Assert(!(reject_indirect && evi_target->installable));

	if (reinitialize)
	{
		foreach(lc, evi_list)
		{
			evi = (ExtensionVersionInfo *) lfirst(lc);
			evi->distance_known = false;
			evi->distance = INT_MAX;
			evi->previous = NULL;
		}
	}

	evi_start->distance = 0;

	while ((evi = get_nearest_unprocessed_vertex(evi_list)) != NULL)
	{
		if (evi->distance == INT_MAX)
			break;				/* all remaining vertices are unreachable */
		evi->distance_known = true;
		if (evi == evi_target)
			break;				/* found shortest path to target */
		foreach(lc, evi->reachable)
		{
			ExtensionVersionInfo *evi2 = (ExtensionVersionInfo *) lfirst(lc);
			int			newdist;

			/* if reject_indirect, treat installable versions as unreachable */
			if (reject_indirect && evi2->installable)
				continue;
			newdist = evi->distance + 1;
			if (newdist < evi2->distance)
			{
				evi2->distance = newdist;
				evi2->previous = evi;
			}
			else if (newdist == evi2->distance &&
					 evi2->previous != NULL &&
					 strcmp(evi->name, evi2->previous->name) < 0)
			{
				/*
				 * Break ties in favor of the version name that comes first
				 * according to strcmp().  This behavior is undocumented and
				 * users shouldn't rely on it.  We do it just to ensure that
				 * if there is a tie, the update path that is chosen does not
				 * depend on random factors like the order in which directory
				 * entries get visited.
				 */
				evi2->previous = evi;
			}
		}
	}

	/* Return NIL if target is not reachable from start */
	if (!evi_target->distance_known)
		return NIL;

	/* Build and return list of version names representing the update path */
	result = NIL;
	for (evi = evi_target; evi != evi_start; evi = evi->previous)
		result = lcons(evi->name, result);

	return result;
}

/*
 * Given a target version that is not directly installable, find the
 * best installation sequence starting from a directly-installable version.
 *
 * evi_list: previously-collected version update graph
 * evi_target: member of that list that we want to reach
 *
 * Returns the best starting-point version, or NULL if there is none.
 * On success, *best_path is set to the path from the start point.
 *
 * If there's more than one possible start point, prefer shorter update paths,
 * and break any ties arbitrarily on the basis of strcmp'ing the starting
 * versions' names.
 */
static ExtensionVersionInfo *
find_install_path(List *evi_list, ExtensionVersionInfo *evi_target,
				  List **best_path)
{
	ExtensionVersionInfo *evi_start = NULL;
	ListCell   *lc;

	*best_path = NIL;

	/*
	 * We don't expect to be called for an installable target, but if we are,
	 * the answer is easy: just start from there, with an empty update path.
	 */
	if (evi_target->installable)
		return evi_target;

	/* Consider all installable versions as start points */
	foreach(lc, evi_list)
	{
		ExtensionVersionInfo *evi1 = (ExtensionVersionInfo *) lfirst(lc);
		List	   *path;

		if (!evi1->installable)
			continue;

		/*
		 * Find shortest path from evi1 to evi_target; but no need to consider
		 * paths going through other installable versions.
		 */
		path = find_update_path(evi_list, evi1, evi_target, true, true);
		if (path == NIL)
			continue;

		/* Remember best path */
		if (evi_start == NULL ||
			list_length(path) < list_length(*best_path) ||
			(list_length(path) == list_length(*best_path) &&
			 strcmp(evi_start->name, evi1->name) < 0))
		{
			evi_start = evi1;
			*best_path = path;
		}
	}

	return evi_start;
}

/*
 * CREATE EXTENSION worker
 *
 * When CASCADE is specified, CreateExtensionInternal() recurses if required
 * extensions need to be installed.  To sanely handle cyclic dependencies,
 * the "parents" list contains a list of names of extensions already being
 * installed, allowing us to error out if we recurse to one of those.
 */
static ObjectAddress
CreateExtensionInternal(char *extensionName,
						char *schemaName,
						const char *versionName,
						bool cascade,
						List *parents,
						bool is_create)
{
	char	   *origSchemaName = schemaName;
	Oid			schemaOid = InvalidOid;
	Oid			extowner = GetUserId();
	ExtensionControlFile *pcontrol;
	ExtensionControlFile *control;
	char	   *filename;
	struct stat fst;
	List	   *updateVersions;
	List	   *requiredExtensions;
	List	   *requiredSchemas;
	Oid			extensionOid;
	ObjectAddress address;
	ListCell   *lc;

	/*
	 * Read the primary control file.  Note we assume that it does not contain
	 * any non-ASCII data, so there is no need to worry about encoding at this
	 * point.
	 */
	pcontrol = read_extension_control_file(extensionName);

	/*
	 * Determine the version to install
	 */
	if (versionName == NULL)
	{
		if (pcontrol->default_version)
			versionName = pcontrol->default_version;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("version to install must be specified")));
	}
	check_valid_version_name(versionName);

	/*
	 * Figure out which script(s) we need to run to install the desired
	 * version of the extension.  If we do not have a script that directly
	 * does what is needed, we try to find a sequence of update scripts that
	 * will get us there.
	 */
	filename = get_extension_script_filename(pcontrol, NULL, versionName);
	if (stat(filename, &fst) == 0)
	{
		/* Easy, no extra scripts */
		updateVersions = NIL;
	}
	else
	{
		/* Look for best way to install this version */
		List	   *evi_list;
		ExtensionVersionInfo *evi_start;
		ExtensionVersionInfo *evi_target;

		/* Extract the version update graph from the script directory */
		evi_list = get_ext_ver_list(pcontrol);

		/* Identify the target version */
		evi_target = get_ext_ver_info(versionName, &evi_list);

		/* Identify best path to reach target */
		evi_start = find_install_path(evi_list, evi_target,
									  &updateVersions);

		/* Fail if no path ... */
		if (evi_start == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("extension \"%s\" has no installation script nor update path for version \"%s\"",
							pcontrol->name, versionName)));

		/* Otherwise, install best starting point and then upgrade */
		versionName = evi_start->name;
	}

	/*
	 * Fetch control parameters for installation target version
	 */
	control = read_extension_aux_control_file(pcontrol, versionName);

	/*
	 * Determine the target schema to install the extension into
	 */
	if (schemaName)
	{
		/* If the user is giving us the schema name, it must exist already. */
		schemaOid = get_namespace_oid(schemaName, false);
	}

	if (control->schema != NULL)
	{
		/*
		 * The extension is not relocatable and the author gave us a schema
		 * for it.
		 *
		 * Unless CASCADE parameter was given, it's an error to give a schema
		 * different from control->schema if control->schema is specified.
		 */
		if (schemaName && strcmp(control->schema, schemaName) != 0 &&
			!cascade)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("extension \"%s\" must be installed in schema \"%s\"",
							control->name,
							control->schema)));

		/* Always use the schema from control file for current extension. */
		schemaName = control->schema;

		/* Find or create the schema in case it does not exist. */
		schemaOid = get_namespace_oid(schemaName, true);

		if (!OidIsValid(schemaOid))
		{
			CreateSchemaStmt *csstmt = makeNode(CreateSchemaStmt);

			csstmt->schemaname = schemaName;
			csstmt->authrole = NULL;	/* will be created by current user */
			csstmt->schemaElts = NIL;
			csstmt->if_not_exists = false;
			CreateSchemaCommand(csstmt, "(generated CREATE SCHEMA command)",
								-1, -1);

			/*
			 * CreateSchemaCommand includes CommandCounterIncrement, so new
			 * schema is now visible.
			 */
			schemaOid = get_namespace_oid(schemaName, false);
		}
	}
	else if (!OidIsValid(schemaOid))
	{
		/*
		 * Neither user nor author of the extension specified schema; use the
		 * current default creation namespace, which is the first explicit
		 * entry in the search_path.
		 */
		List	   *search_path = fetch_search_path(false);

		if (search_path == NIL) /* nothing valid in search_path? */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_SCHEMA),
					 errmsg("no schema has been selected to create in")));
		schemaOid = linitial_oid(search_path);
		schemaName = get_namespace_name(schemaOid);
		if (schemaName == NULL) /* recently-deleted namespace? */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_SCHEMA),
					 errmsg("no schema has been selected to create in")));

		list_free(search_path);
	}

	/*
	 * Make note if a temporary namespace has been accessed in this
	 * transaction.
	 */
	if (isTempNamespace(schemaOid))
		MyXactFlags |= XACT_FLAGS_ACCESSEDTEMPNAMESPACE;

	/*
	 * We don't check creation rights on the target namespace here.  If the
	 * extension script actually creates any objects there, it will fail if
	 * the user doesn't have such permissions.  But there are cases such as
	 * procedural languages where it's convenient to set schema = pg_catalog
	 * yet we don't want to restrict the command to users with ACL_CREATE for
	 * pg_catalog.
	 */

	/*
	 * Look up the prerequisite extensions, install them if necessary, and
	 * build lists of their OIDs and the OIDs of their target schemas.
	 */
	requiredExtensions = NIL;
	requiredSchemas = NIL;
	foreach(lc, control->requires)
	{
		char	   *curreq = (char *) lfirst(lc);
		Oid			reqext;
		Oid			reqschema;

		reqext = get_required_extension(curreq,
										extensionName,
										origSchemaName,
										cascade,
										parents,
										is_create);
		reqschema = get_extension_schema(reqext);
		requiredExtensions = lappend_oid(requiredExtensions, reqext);
		requiredSchemas = lappend_oid(requiredSchemas, reqschema);
	}

	/*
	 * Insert new tuple into pg_extension, and create dependency entries.
	 */
	address = InsertExtensionTuple(control->name, extowner,
								   schemaOid, control->relocatable,
								   versionName,
								   PointerGetDatum(NULL),
								   PointerGetDatum(NULL),
								   requiredExtensions);
	extensionOid = address.objectId;

	/*
	 * Apply any control-file comment on extension
	 */
	if (control->comment != NULL)
		CreateComments(extensionOid, ExtensionRelationId, 0, control->comment);

	/*
	 * Execute the installation script file
	 */
	execute_extension_script(extensionOid, control,
							 NULL, versionName,
							 requiredSchemas,
							 schemaName);

	/*
	 * If additional update scripts have to be executed, apply the updates as
	 * though a series of ALTER EXTENSION UPDATE commands were given
	 */
	ApplyExtensionUpdates(extensionOid, pcontrol,
						  versionName, updateVersions,
						  origSchemaName, cascade, is_create);

	return address;
}

/*
 * Get the OID of an extension listed in "requires", possibly creating it.
 */
static Oid
get_required_extension(char *reqExtensionName,
					   char *extensionName,
					   char *origSchemaName,
					   bool cascade,
					   List *parents,
					   bool is_create)
{
	Oid			reqExtensionOid;

	reqExtensionOid = get_extension_oid(reqExtensionName, true);
	if (!OidIsValid(reqExtensionOid))
	{
		if (cascade)
		{
			/* Must install it. */
			ObjectAddress addr;
			List	   *cascade_parents;
			ListCell   *lc;

			/* Check extension name validity before trying to cascade. */
			check_valid_extension_name(reqExtensionName);

			/* Check for cyclic dependency between extensions. */
			foreach(lc, parents)
			{
				char	   *pname = (char *) lfirst(lc);

				if (strcmp(pname, reqExtensionName) == 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_RECURSION),
							 errmsg("cyclic dependency detected between extensions \"%s\" and \"%s\"",
									reqExtensionName, extensionName)));
			}

			ereport(NOTICE,
					(errmsg("installing required extension \"%s\"",
							reqExtensionName)));

			/* Add current extension to list of parents to pass down. */
			cascade_parents = lappend(list_copy(parents), extensionName);

			/*
			 * Create the required extension.  We propagate the SCHEMA option
			 * if any, and CASCADE, but no other options.
			 */
			addr = CreateExtensionInternal(reqExtensionName,
										   origSchemaName,
										   NULL,
										   cascade,
										   cascade_parents,
										   is_create);

			/* Get its newly-assigned OID. */
			reqExtensionOid = addr.objectId;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("required extension \"%s\" is not installed",
							reqExtensionName),
					 is_create ?
					 errhint("Use CREATE EXTENSION ... CASCADE to install required extensions too.") : 0));
	}

	return reqExtensionOid;
}

/*
 * CREATE EXTENSION
 */
ObjectAddress
CreateExtension(ParseState *pstate, CreateExtensionStmt *stmt)
{
	DefElem    *d_schema = NULL;
	DefElem    *d_new_version = NULL;
	DefElem    *d_cascade = NULL;
	char	   *schemaName = NULL;
	char	   *versionName = NULL;
	bool		cascade = false;
	ListCell   *lc;

	/* Check extension name validity before any filesystem access */
	check_valid_extension_name(stmt->extname);

	/*
	 * Check for duplicate extension name.  The unique index on
	 * pg_extension.extname would catch this anyway, and serves as a backstop
	 * in case of race conditions; but this is a friendlier error message, and
	 * besides we need a check to support IF NOT EXISTS.
	 */
	if (get_extension_oid(stmt->extname, true) != InvalidOid)
	{
		if (stmt->if_not_exists)
		{
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("extension \"%s\" already exists, skipping",
							stmt->extname)));
			return InvalidObjectAddress;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("extension \"%s\" already exists",
							stmt->extname)));
	}

	/*
	 * We use global variables to track the extension being created, so we can
	 * create only one extension at the same time.
	 */
	if (creating_extension)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("nested CREATE EXTENSION is not supported")));

	/* Deconstruct the statement option list */
	foreach(lc, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		if (strcmp(defel->defname, "schema") == 0)
		{
			if (d_schema)
				errorConflictingDefElem(defel, pstate);
			d_schema = defel;
			schemaName = defGetString(d_schema);
		}
		else if (strcmp(defel->defname, "new_version") == 0)
		{
			if (d_new_version)
				errorConflictingDefElem(defel, pstate);
			d_new_version = defel;
			versionName = defGetString(d_new_version);
		}
		else if (strcmp(defel->defname, "cascade") == 0)
		{
			if (d_cascade)
				errorConflictingDefElem(defel, pstate);
			d_cascade = defel;
			cascade = defGetBoolean(d_cascade);
		}
		else
			elog(ERROR, "unrecognized option: %s", defel->defname);
	}

	/* Call CreateExtensionInternal to do the real work. */
	return CreateExtensionInternal(stmt->extname,
								   schemaName,
								   versionName,
								   cascade,
								   NIL,
								   true);
}

/*
 * InsertExtensionTuple
 *
 * Insert the new pg_extension row, and create extension's dependency entries.
 * Return the OID assigned to the new row.
 *
 * This is exported for the benefit of pg_upgrade, which has to create a
 * pg_extension entry (and the extension-level dependencies) without
 * actually running the extension's script.
 *
 * extConfig and extCondition should be arrays or PointerGetDatum(NULL).
 * We declare them as plain Datum to avoid needing array.h in extension.h.
 */
ObjectAddress
InsertExtensionTuple(const char *extName, Oid extOwner,
					 Oid schemaOid, bool relocatable, const char *extVersion,
					 Datum extConfig, Datum extCondition,
					 List *requiredExtensions)
{
	Oid			extensionOid;
	Relation	rel;
	Datum		values[Natts_pg_extension];
	bool		nulls[Natts_pg_extension];
	HeapTuple	tuple;
	ObjectAddress myself;
	ObjectAddress nsp;
	ObjectAddresses *refobjs;
	ListCell   *lc;

	/*
	 * Build and insert the pg_extension tuple
	 */
	rel = table_open(ExtensionRelationId, RowExclusiveLock);

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));

	extensionOid = GetNewOidWithIndex(rel, ExtensionOidIndexId,
									  Anum_pg_extension_oid);
	values[Anum_pg_extension_oid - 1] = ObjectIdGetDatum(extensionOid);
	values[Anum_pg_extension_extname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(extName));
	values[Anum_pg_extension_extowner - 1] = ObjectIdGetDatum(extOwner);
	values[Anum_pg_extension_extnamespace - 1] = ObjectIdGetDatum(schemaOid);
	values[Anum_pg_extension_extrelocatable - 1] = BoolGetDatum(relocatable);
	values[Anum_pg_extension_extversion - 1] = CStringGetTextDatum(extVersion);

	if (extConfig == PointerGetDatum(NULL))
		nulls[Anum_pg_extension_extconfig - 1] = true;
	else
		values[Anum_pg_extension_extconfig - 1] = extConfig;

	if (extCondition == PointerGetDatum(NULL))
		nulls[Anum_pg_extension_extcondition - 1] = true;
	else
		values[Anum_pg_extension_extcondition - 1] = extCondition;

	tuple = heap_form_tuple(rel->rd_att, values, nulls);

	CatalogTupleInsert(rel, tuple);

	heap_freetuple(tuple);
	table_close(rel, RowExclusiveLock);

	/*
	 * Record dependencies on owner, schema, and prerequisite extensions
	 */
	recordDependencyOnOwner(ExtensionRelationId, extensionOid, extOwner);

	refobjs = new_object_addresses();

	ObjectAddressSet(myself, ExtensionRelationId, extensionOid);

	ObjectAddressSet(nsp, NamespaceRelationId, schemaOid);
	add_exact_object_address(&nsp, refobjs);

	foreach(lc, requiredExtensions)
	{
		Oid			reqext = lfirst_oid(lc);
		ObjectAddress otherext;

		ObjectAddressSet(otherext, ExtensionRelationId, reqext);
		add_exact_object_address(&otherext, refobjs);
	}

	/* Record all of them (this includes duplicate elimination) */
	record_object_address_dependencies(&myself, refobjs, DEPENDENCY_NORMAL);
	free_object_addresses(refobjs);

	/* Post creation hook for new extension */
	InvokeObjectPostCreateHook(ExtensionRelationId, extensionOid, 0);

	return myself;
}

/*
 * Guts of extension deletion.
 *
 * All we need do here is remove the pg_extension tuple itself.  Everything
 * else is taken care of by the dependency infrastructure.
 */
void
RemoveExtensionById(Oid extId)
{
	Relation	rel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	/*
	 * Disallow deletion of any extension that's currently open for insertion;
	 * else subsequent executions of recordDependencyOnCurrentExtension()
	 * could create dangling pg_depend records that refer to a no-longer-valid
	 * pg_extension OID.  This is needed not so much because we think people
	 * might write "DROP EXTENSION foo" in foo's own script files, as because
	 * errors in dependency management in extension script files could give
	 * rise to cases where an extension is dropped as a result of recursing
	 * from some contained object.  Because of that, we must test for the case
	 * here, not at some higher level of the DROP EXTENSION command.
	 */
	if (extId == CurrentExtensionObject)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot drop extension \"%s\" because it is being modified",
						get_extension_name(extId))));

	rel = table_open(ExtensionRelationId, RowExclusiveLock);

	ScanKeyInit(&entry[0],
				Anum_pg_extension_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(extId));
	scandesc = systable_beginscan(rel, ExtensionOidIndexId, true,
								  NULL, 1, entry);

	tuple = systable_getnext(scandesc);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
		CatalogTupleDelete(rel, &tuple->t_self);

	systable_endscan(scandesc);

	table_close(rel, RowExclusiveLock);
}

/*
 * This function lists the available extensions (one row per primary control
 * file in the control directory).  We parse each control file and report the
 * interesting fields.
 *
 * The system view pg_available_extensions provides a user interface to this
 * SRF, adding information about whether the extensions are installed in the
 * current DB.
 */
Datum
pg_available_extensions(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *location;
	DIR		   *dir;
	struct dirent *de;

	/* Build tuplestore to hold the result rows */
	InitMaterializedSRF(fcinfo, 0);

	location = get_extension_control_directory();
	dir = AllocateDir(location);

	/*
	 * If the control directory doesn't exist, we want to silently return an
	 * empty set.  Any other error will be reported by ReadDir.
	 */
	if (dir == NULL && errno == ENOENT)
	{
		/* do nothing */
	}
	else
	{
		while ((de = ReadDir(dir, location)) != NULL)
		{
			ExtensionControlFile *control;
			char	   *extname;
			Datum		values[3];
			bool		nulls[3];

			if (!is_extension_control_filename(de->d_name))
				continue;

			/* extract extension name from 'name.control' filename */
			extname = pstrdup(de->d_name);
			*strrchr(extname, '.') = '\0';

			/* ignore it if it's an auxiliary control file */
			if (strstr(extname, "--"))
				continue;

			control = read_extension_control_file(extname);

			memset(values, 0, sizeof(values));
			memset(nulls, 0, sizeof(nulls));

			/* name */
			values[0] = DirectFunctionCall1(namein,
											CStringGetDatum(control->name));
			/* default_version */
			if (control->default_version == NULL)
				nulls[1] = true;
			else
				values[1] = CStringGetTextDatum(control->default_version);
			/* comment */
			if (control->comment == NULL)
				nulls[2] = true;
			else
				values[2] = CStringGetTextDatum(control->comment);

			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
								 values, nulls);
		}

		FreeDir(dir);
	}

	return (Datum) 0;
}

/*
 * This function lists the available extension versions (one row per
 * extension installation script).  For each version, we parse the related
 * control file(s) and report the interesting fields.
 *
 * The system view pg_available_extension_versions provides a user interface
 * to this SRF, adding information about which versions are installed in the
 * current DB.
 */
Datum
pg_available_extension_versions(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *location;
	DIR		   *dir;
	struct dirent *de;

	/* Build tuplestore to hold the result rows */
	InitMaterializedSRF(fcinfo, 0);

	location = get_extension_control_directory();
	dir = AllocateDir(location);

	/*
	 * If the control directory doesn't exist, we want to silently return an
	 * empty set.  Any other error will be reported by ReadDir.
	 */
	if (dir == NULL && errno == ENOENT)
	{
		/* do nothing */
	}
	else
	{
		while ((de = ReadDir(dir, location)) != NULL)
		{
			ExtensionControlFile *control;
			char	   *extname;

			if (!is_extension_control_filename(de->d_name))
				continue;

			/* extract extension name from 'name.control' filename */
			extname = pstrdup(de->d_name);
			*strrchr(extname, '.') = '\0';

			/* ignore it if it's an auxiliary control file */
			if (strstr(extname, "--"))
				continue;

			/* read the control file */
			control = read_extension_control_file(extname);

			/* scan extension's script directory for install scripts */
			get_available_versions_for_extension(control, rsinfo->setResult,
												 rsinfo->setDesc);
		}

		FreeDir(dir);
	}

	return (Datum) 0;
}

/*
 * Inner loop for pg_available_extension_versions:
 *		read versions of one extension, add rows to tupstore
 */
static void
get_available_versions_for_extension(ExtensionControlFile *pcontrol,
									 Tuplestorestate *tupstore,
									 TupleDesc tupdesc)
{
	List	   *evi_list;
	ListCell   *lc;

	/* Extract the version update graph from the script directory */
	evi_list = get_ext_ver_list(pcontrol);

	/* For each installable version ... */
	foreach(lc, evi_list)
	{
		ExtensionVersionInfo *evi = (ExtensionVersionInfo *) lfirst(lc);
		ExtensionControlFile *control;
		Datum		values[8];
		bool		nulls[8];
		ListCell   *lc2;

		if (!evi->installable)
			continue;

		/*
		 * Fetch parameters for specific version (pcontrol is not changed)
		 */
		control = read_extension_aux_control_file(pcontrol, evi->name);

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		/* name */
		values[0] = DirectFunctionCall1(namein,
										CStringGetDatum(control->name));
		/* version */
		values[1] = CStringGetTextDatum(evi->name);
		/* superuser */
		values[2] = BoolGetDatum(control->superuser);
		/* trusted */
		values[3] = BoolGetDatum(control->trusted);
		/* relocatable */
		values[4] = BoolGetDatum(control->relocatable);
		/* schema */
		if (control->schema == NULL)
			nulls[5] = true;
		else
			values[5] = DirectFunctionCall1(namein,
											CStringGetDatum(control->schema));
		/* requires */
		if (control->requires == NIL)
			nulls[6] = true;
		else
			values[6] = convert_requires_to_datum(control->requires);
		/* comment */
		if (control->comment == NULL)
			nulls[7] = true;
		else
			values[7] = CStringGetTextDatum(control->comment);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);

		/*
		 * Find all non-directly-installable versions that would be installed
		 * starting from this version, and report them, inheriting the
		 * parameters that aren't changed in updates from this version.
		 */
		foreach(lc2, evi_list)
		{
			ExtensionVersionInfo *evi2 = (ExtensionVersionInfo *) lfirst(lc2);
			List	   *best_path;

			if (evi2->installable)
				continue;
			if (find_install_path(evi_list, evi2, &best_path) == evi)
			{
				/*
				 * Fetch parameters for this version (pcontrol is not changed)
				 */
				control = read_extension_aux_control_file(pcontrol, evi2->name);

				/* name stays the same */
				/* version */
				values[1] = CStringGetTextDatum(evi2->name);
				/* superuser */
				values[2] = BoolGetDatum(control->superuser);
				/* trusted */
				values[3] = BoolGetDatum(control->trusted);
				/* relocatable */
				values[4] = BoolGetDatum(control->relocatable);
				/* schema stays the same */
				/* requires */
				if (control->requires == NIL)
					nulls[6] = true;
				else
				{
					values[6] = convert_requires_to_datum(control->requires);
					nulls[6] = false;
				}
				/* comment stays the same */

				tuplestore_putvalues(tupstore, tupdesc, values, nulls);
			}
		}
	}
}

/*
 * Test whether the given extension exists (not whether it's installed)
 *
 * This checks for the existence of a matching control file in the extension
 * directory.  That's not a bulletproof check, since the file might be
 * invalid, but this is only used for hints so it doesn't have to be 100%
 * right.
 */
bool
extension_file_exists(const char *extensionName)
{
	bool		result = false;
	char	   *location;
	DIR		   *dir;
	struct dirent *de;

	location = get_extension_control_directory();
	dir = AllocateDir(location);

	/*
	 * If the control directory doesn't exist, we want to silently return
	 * false.  Any other error will be reported by ReadDir.
	 */
	if (dir == NULL && errno == ENOENT)
	{
		/* do nothing */
	}
	else
	{
		while ((de = ReadDir(dir, location)) != NULL)
		{
			char	   *extname;

			if (!is_extension_control_filename(de->d_name))
				continue;

			/* extract extension name from 'name.control' filename */
			extname = pstrdup(de->d_name);
			*strrchr(extname, '.') = '\0';

			/* ignore it if it's an auxiliary control file */
			if (strstr(extname, "--"))
				continue;

			/* done if it matches request */
			if (strcmp(extname, extensionName) == 0)
			{
				result = true;
				break;
			}
		}

		FreeDir(dir);
	}

	return result;
}

/*
 * Convert a list of extension names to a name[] Datum
 */
static Datum
convert_requires_to_datum(List *requires)
{
	Datum	   *datums;
	int			ndatums;
	ArrayType  *a;
	ListCell   *lc;

	ndatums = list_length(requires);
	datums = (Datum *) palloc(ndatums * sizeof(Datum));
	ndatums = 0;
	foreach(lc, requires)
	{
		char	   *curreq = (char *) lfirst(lc);

		datums[ndatums++] =
			DirectFunctionCall1(namein, CStringGetDatum(curreq));
	}
	a = construct_array_builtin(datums, ndatums, NAMEOID);
	return PointerGetDatum(a);
}

/*
 * This function reports the version update paths that exist for the
 * specified extension.
 */
Datum
pg_extension_update_paths(PG_FUNCTION_ARGS)
{
	Name		extname = PG_GETARG_NAME(0);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	List	   *evi_list;
	ExtensionControlFile *control;
	ListCell   *lc1;

	/* Check extension name validity before any filesystem access */
	check_valid_extension_name(NameStr(*extname));

	/* Build tuplestore to hold the result rows */
	InitMaterializedSRF(fcinfo, 0);

	/* Read the extension's control file */
	control = read_extension_control_file(NameStr(*extname));

	/* Extract the version update graph from the script directory */
	evi_list = get_ext_ver_list(control);

	/* Iterate over all pairs of versions */
	foreach(lc1, evi_list)
	{
		ExtensionVersionInfo *evi1 = (ExtensionVersionInfo *) lfirst(lc1);
		ListCell   *lc2;

		foreach(lc2, evi_list)
		{
			ExtensionVersionInfo *evi2 = (ExtensionVersionInfo *) lfirst(lc2);
			List	   *path;
			Datum		values[3];
			bool		nulls[3];

			if (evi1 == evi2)
				continue;

			/* Find shortest path from evi1 to evi2 */
			path = find_update_path(evi_list, evi1, evi2, false, true);

			/* Emit result row */
			memset(values, 0, sizeof(values));
			memset(nulls, 0, sizeof(nulls));

			/* source */
			values[0] = CStringGetTextDatum(evi1->name);
			/* target */
			values[1] = CStringGetTextDatum(evi2->name);
			/* path */
			if (path == NIL)
				nulls[2] = true;
			else
			{
				StringInfoData pathbuf;
				ListCell   *lcv;

				initStringInfo(&pathbuf);
				/* The path doesn't include start vertex, but show it */
				appendStringInfoString(&pathbuf, evi1->name);
				foreach(lcv, path)
				{
					char	   *versionName = (char *) lfirst(lcv);

					appendStringInfoString(&pathbuf, "--");
					appendStringInfoString(&pathbuf, versionName);
				}
				values[2] = CStringGetTextDatum(pathbuf.data);
				pfree(pathbuf.data);
			}

			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
								 values, nulls);
		}
	}

	return (Datum) 0;
}

/*
 * pg_extension_config_dump
 *
 * Record information about a configuration table that belongs to an
 * extension being created, but whose contents should be dumped in whole
 * or in part during pg_dump.
 */
Datum
pg_extension_config_dump(PG_FUNCTION_ARGS)
{
	Oid			tableoid = PG_GETARG_OID(0);
	text	   *wherecond = PG_GETARG_TEXT_PP(1);
	char	   *tablename;
	Relation	extRel;
	ScanKeyData key[1];
	SysScanDesc extScan;
	HeapTuple	extTup;
	Datum		arrayDatum;
	Datum		elementDatum;
	int			arrayLength;
	int			arrayIndex;
	bool		isnull;
	Datum		repl_val[Natts_pg_extension];
	bool		repl_null[Natts_pg_extension];
	bool		repl_repl[Natts_pg_extension];
	ArrayType  *a;

	/*
	 * We only allow this to be called from an extension's SQL script. We
	 * shouldn't need any permissions check beyond that.
	 */
	if (!creating_extension)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("%s can only be called from an SQL script executed by CREATE EXTENSION",
						"pg_extension_config_dump()")));

	/*
	 * Check that the table exists and is a member of the extension being
	 * created.  This ensures that we don't need to register an additional
	 * dependency to protect the extconfig entry.
	 */
	tablename = get_rel_name(tableoid);
	if (tablename == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("OID %u does not refer to a table", tableoid)));
	if (getExtensionOfObject(RelationRelationId, tableoid) !=
		CurrentExtensionObject)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("table \"%s\" is not a member of the extension being created",
						tablename)));

	/*
	 * Add the table OID and WHERE condition to the extension's extconfig and
	 * extcondition arrays.
	 *
	 * If the table is already in extconfig, treat this as an update of the
	 * WHERE condition.
	 */

	/* Find the pg_extension tuple */
	extRel = table_open(ExtensionRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_extension_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(CurrentExtensionObject));

	extScan = systable_beginscan(extRel, ExtensionOidIndexId, true,
								 NULL, 1, key);

	extTup = systable_getnext(extScan);

	if (!HeapTupleIsValid(extTup))	/* should not happen */
		elog(ERROR, "could not find tuple for extension %u",
			 CurrentExtensionObject);

	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	/* Build or modify the extconfig value */
	elementDatum = ObjectIdGetDatum(tableoid);

	arrayDatum = heap_getattr(extTup, Anum_pg_extension_extconfig,
							  RelationGetDescr(extRel), &isnull);
	if (isnull)
	{
		/* Previously empty extconfig, so build 1-element array */
		arrayLength = 0;
		arrayIndex = 1;

		a = construct_array_builtin(&elementDatum, 1, OIDOID);
	}
	else
	{
		/* Modify or extend existing extconfig array */
		Oid		   *arrayData;
		int			i;

		a = DatumGetArrayTypeP(arrayDatum);

		arrayLength = ARR_DIMS(a)[0];
		if (ARR_NDIM(a) != 1 ||
			ARR_LBOUND(a)[0] != 1 ||
			arrayLength < 0 ||
			ARR_HASNULL(a) ||
			ARR_ELEMTYPE(a) != OIDOID)
			elog(ERROR, "extconfig is not a 1-D Oid array");
		arrayData = (Oid *) ARR_DATA_PTR(a);

		arrayIndex = arrayLength + 1;	/* set up to add after end */

		for (i = 0; i < arrayLength; i++)
		{
			if (arrayData[i] == tableoid)
			{
				arrayIndex = i + 1; /* replace this element instead */
				break;
			}
		}

		a = array_set(a, 1, &arrayIndex,
					  elementDatum,
					  false,
					  -1 /* varlena array */ ,
					  sizeof(Oid) /* OID's typlen */ ,
					  true /* OID's typbyval */ ,
					  TYPALIGN_INT /* OID's typalign */ );
	}
	repl_val[Anum_pg_extension_extconfig - 1] = PointerGetDatum(a);
	repl_repl[Anum_pg_extension_extconfig - 1] = true;

	/* Build or modify the extcondition value */
	elementDatum = PointerGetDatum(wherecond);

	arrayDatum = heap_getattr(extTup, Anum_pg_extension_extcondition,
							  RelationGetDescr(extRel), &isnull);
	if (isnull)
	{
		if (arrayLength != 0)
			elog(ERROR, "extconfig and extcondition arrays do not match");

		a = construct_array_builtin(&elementDatum, 1, TEXTOID);
	}
	else
	{
		a = DatumGetArrayTypeP(arrayDatum);

		if (ARR_NDIM(a) != 1 ||
			ARR_LBOUND(a)[0] != 1 ||
			ARR_HASNULL(a) ||
			ARR_ELEMTYPE(a) != TEXTOID)
			elog(ERROR, "extcondition is not a 1-D text array");
		if (ARR_DIMS(a)[0] != arrayLength)
			elog(ERROR, "extconfig and extcondition arrays do not match");

		/* Add or replace at same index as in extconfig */
		a = array_set(a, 1, &arrayIndex,
					  elementDatum,
					  false,
					  -1 /* varlena array */ ,
					  -1 /* TEXT's typlen */ ,
					  false /* TEXT's typbyval */ ,
					  TYPALIGN_INT /* TEXT's typalign */ );
	}
	repl_val[Anum_pg_extension_extcondition - 1] = PointerGetDatum(a);
	repl_repl[Anum_pg_extension_extcondition - 1] = true;

	extTup = heap_modify_tuple(extTup, RelationGetDescr(extRel),
							   repl_val, repl_null, repl_repl);

	CatalogTupleUpdate(extRel, &extTup->t_self, extTup);

	systable_endscan(extScan);

	table_close(extRel, RowExclusiveLock);

	PG_RETURN_VOID();
}

/*
 * extension_config_remove
 *
 * Remove the specified table OID from extension's extconfig, if present.
 * This is not currently exposed as a function, but it could be;
 * for now, we just invoke it from ALTER EXTENSION DROP.
 */
static void
extension_config_remove(Oid extensionoid, Oid tableoid)
{
	Relation	extRel;
	ScanKeyData key[1];
	SysScanDesc extScan;
	HeapTuple	extTup;
	Datum		arrayDatum;
	int			arrayLength;
	int			arrayIndex;
	bool		isnull;
	Datum		repl_val[Natts_pg_extension];
	bool		repl_null[Natts_pg_extension];
	bool		repl_repl[Natts_pg_extension];
	ArrayType  *a;

	/* Find the pg_extension tuple */
	extRel = table_open(ExtensionRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_extension_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(extensionoid));

	extScan = systable_beginscan(extRel, ExtensionOidIndexId, true,
								 NULL, 1, key);

	extTup = systable_getnext(extScan);

	if (!HeapTupleIsValid(extTup))	/* should not happen */
		elog(ERROR, "could not find tuple for extension %u",
			 extensionoid);

	/* Search extconfig for the tableoid */
	arrayDatum = heap_getattr(extTup, Anum_pg_extension_extconfig,
							  RelationGetDescr(extRel), &isnull);
	if (isnull)
	{
		/* nothing to do */
		a = NULL;
		arrayLength = 0;
		arrayIndex = -1;
	}
	else
	{
		Oid		   *arrayData;
		int			i;

		a = DatumGetArrayTypeP(arrayDatum);

		arrayLength = ARR_DIMS(a)[0];
		if (ARR_NDIM(a) != 1 ||
			ARR_LBOUND(a)[0] != 1 ||
			arrayLength < 0 ||
			ARR_HASNULL(a) ||
			ARR_ELEMTYPE(a) != OIDOID)
			elog(ERROR, "extconfig is not a 1-D Oid array");
		arrayData = (Oid *) ARR_DATA_PTR(a);

		arrayIndex = -1;		/* flag for no deletion needed */

		for (i = 0; i < arrayLength; i++)
		{
			if (arrayData[i] == tableoid)
			{
				arrayIndex = i; /* index to remove */
				break;
			}
		}
	}

	/* If tableoid is not in extconfig, nothing to do */
	if (arrayIndex < 0)
	{
		systable_endscan(extScan);
		table_close(extRel, RowExclusiveLock);
		return;
	}

	/* Modify or delete the extconfig value */
	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	if (arrayLength <= 1)
	{
		/* removing only element, just set array to null */
		repl_null[Anum_pg_extension_extconfig - 1] = true;
	}
	else
	{
		/* squeeze out the target element */
		Datum	   *dvalues;
		int			nelems;
		int			i;

		/* We already checked there are no nulls */
		deconstruct_array_builtin(a, OIDOID, &dvalues, NULL, &nelems);

		for (i = arrayIndex; i < arrayLength - 1; i++)
			dvalues[i] = dvalues[i + 1];

		a = construct_array_builtin(dvalues, arrayLength - 1, OIDOID);

		repl_val[Anum_pg_extension_extconfig - 1] = PointerGetDatum(a);
	}
	repl_repl[Anum_pg_extension_extconfig - 1] = true;

	/* Modify or delete the extcondition value */
	arrayDatum = heap_getattr(extTup, Anum_pg_extension_extcondition,
							  RelationGetDescr(extRel), &isnull);
	if (isnull)
	{
		elog(ERROR, "extconfig and extcondition arrays do not match");
	}
	else
	{
		a = DatumGetArrayTypeP(arrayDatum);

		if (ARR_NDIM(a) != 1 ||
			ARR_LBOUND(a)[0] != 1 ||
			ARR_HASNULL(a) ||
			ARR_ELEMTYPE(a) != TEXTOID)
			elog(ERROR, "extcondition is not a 1-D text array");
		if (ARR_DIMS(a)[0] != arrayLength)
			elog(ERROR, "extconfig and extcondition arrays do not match");
	}

	if (arrayLength <= 1)
	{
		/* removing only element, just set array to null */
		repl_null[Anum_pg_extension_extcondition - 1] = true;
	}
	else
	{
		/* squeeze out the target element */
		Datum	   *dvalues;
		int			nelems;
		int			i;

		/* We already checked there are no nulls */
		deconstruct_array_builtin(a, TEXTOID, &dvalues, NULL, &nelems);

		for (i = arrayIndex; i < arrayLength - 1; i++)
			dvalues[i] = dvalues[i + 1];

		a = construct_array_builtin(dvalues, arrayLength - 1, TEXTOID);

		repl_val[Anum_pg_extension_extcondition - 1] = PointerGetDatum(a);
	}
	repl_repl[Anum_pg_extension_extcondition - 1] = true;

	extTup = heap_modify_tuple(extTup, RelationGetDescr(extRel),
							   repl_val, repl_null, repl_repl);

	CatalogTupleUpdate(extRel, &extTup->t_self, extTup);

	systable_endscan(extScan);

	table_close(extRel, RowExclusiveLock);
}

/*
 * Execute ALTER EXTENSION SET SCHEMA
 */
ObjectAddress
AlterExtensionNamespace(const char *extensionName, const char *newschema, Oid *oldschema)
{
	Oid			extensionOid;
	Oid			nspOid;
	Oid			oldNspOid;
	AclResult	aclresult;
	Relation	extRel;
	ScanKeyData key[2];
	SysScanDesc extScan;
	HeapTuple	extTup;
	Form_pg_extension extForm;
	Relation	depRel;
	SysScanDesc depScan;
	HeapTuple	depTup;
	ObjectAddresses *objsMoved;
	ObjectAddress extAddr;

	extensionOid = get_extension_oid(extensionName, false);

	nspOid = LookupCreationNamespace(newschema);

	/*
	 * Permission check: must own extension.  Note that we don't bother to
	 * check ownership of the individual member objects ...
	 */
	if (!object_ownercheck(ExtensionRelationId, extensionOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_EXTENSION,
					   extensionName);

	/* Permission check: must have creation rights in target namespace */
	aclresult = object_aclcheck(NamespaceRelationId, nspOid, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA, newschema);

	/*
	 * If the schema is currently a member of the extension, disallow moving
	 * the extension into the schema.  That would create a dependency loop.
	 */
	if (getExtensionOfObject(NamespaceRelationId, nspOid) == extensionOid)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot move extension \"%s\" into schema \"%s\" "
						"because the extension contains the schema",
						extensionName, newschema)));

	/* Locate the pg_extension tuple */
	extRel = table_open(ExtensionRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_extension_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(extensionOid));

	extScan = systable_beginscan(extRel, ExtensionOidIndexId, true,
								 NULL, 1, key);

	extTup = systable_getnext(extScan);

	if (!HeapTupleIsValid(extTup))	/* should not happen */
		elog(ERROR, "could not find tuple for extension %u",
			 extensionOid);

	/* Copy tuple so we can modify it below */
	extTup = heap_copytuple(extTup);
	extForm = (Form_pg_extension) GETSTRUCT(extTup);

	systable_endscan(extScan);

	/*
	 * If the extension is already in the target schema, just silently do
	 * nothing.
	 */
	if (extForm->extnamespace == nspOid)
	{
		table_close(extRel, RowExclusiveLock);
		return InvalidObjectAddress;
	}

	/* Check extension is supposed to be relocatable */
	if (!extForm->extrelocatable)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("extension \"%s\" does not support SET SCHEMA",
						NameStr(extForm->extname))));

	objsMoved = new_object_addresses();

	/* store the OID of the namespace to-be-changed */
	oldNspOid = extForm->extnamespace;

	/*
	 * Scan pg_depend to find objects that depend directly on the extension,
	 * and alter each one's schema.
	 */
	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ExtensionRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(extensionOid));

	depScan = systable_beginscan(depRel, DependReferenceIndexId, true,
								 NULL, 2, key);

	while (HeapTupleIsValid(depTup = systable_getnext(depScan)))
	{
		Form_pg_depend pg_depend = (Form_pg_depend) GETSTRUCT(depTup);
		ObjectAddress dep;
		Oid			dep_oldNspOid;

		/*
		 * If a dependent extension has a no_relocate request for this
		 * extension, disallow SET SCHEMA.  (XXX it's a bit ugly to do this in
		 * the same loop that's actually executing the renames: we may detect
		 * the error condition only after having expended a fair amount of
		 * work.  However, the alternative is to do two scans of pg_depend,
		 * which seems like optimizing for failure cases.  The rename work
		 * will all roll back cleanly enough if we do fail here.)
		 */
		if (pg_depend->deptype == DEPENDENCY_NORMAL &&
			pg_depend->classid == ExtensionRelationId)
		{
			char	   *depextname = get_extension_name(pg_depend->objid);
			ExtensionControlFile *dcontrol;
			ListCell   *lc;

			dcontrol = read_extension_control_file(depextname);
			foreach(lc, dcontrol->no_relocate)
			{
				char	   *nrextname = (char *) lfirst(lc);

				if (strcmp(nrextname, NameStr(extForm->extname)) == 0)
				{
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot SET SCHEMA of extension \"%s\" because other extensions prevent it",
									NameStr(extForm->extname)),
							 errdetail("Extension \"%s\" requests no relocation of extension \"%s\".",
									   depextname,
									   NameStr(extForm->extname))));
				}
			}
		}

		/*
		 * Otherwise, ignore non-membership dependencies.  (Currently, the
		 * only other case we could see here is a normal dependency from
		 * another extension.)
		 */
		if (pg_depend->deptype != DEPENDENCY_EXTENSION)
			continue;

		dep.classId = pg_depend->classid;
		dep.objectId = pg_depend->objid;
		dep.objectSubId = pg_depend->objsubid;

		if (dep.objectSubId != 0)	/* should not happen */
			elog(ERROR, "extension should not have a sub-object dependency");

		/* Relocate the object */
		dep_oldNspOid = AlterObjectNamespace_oid(dep.classId,
												 dep.objectId,
												 nspOid,
												 objsMoved);

		/*
		 * If not all the objects had the same old namespace (ignoring any
		 * that are not in namespaces or are dependent types), complain.
		 */
		if (dep_oldNspOid != InvalidOid && dep_oldNspOid != oldNspOid)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("extension \"%s\" does not support SET SCHEMA",
							NameStr(extForm->extname)),
					 errdetail("%s is not in the extension's schema \"%s\"",
							   getObjectDescription(&dep, false),
							   get_namespace_name(oldNspOid))));
	}

	/* report old schema, if caller wants it */
	if (oldschema)
		*oldschema = oldNspOid;

	systable_endscan(depScan);

	relation_close(depRel, AccessShareLock);

	/* Now adjust pg_extension.extnamespace */
	extForm->extnamespace = nspOid;

	CatalogTupleUpdate(extRel, &extTup->t_self, extTup);

	table_close(extRel, RowExclusiveLock);

	/* update dependency to point to the new schema */
	if (changeDependencyFor(ExtensionRelationId, extensionOid,
							NamespaceRelationId, oldNspOid, nspOid) != 1)
		elog(ERROR, "could not change schema dependency for extension %s",
			 NameStr(extForm->extname));

	InvokeObjectPostAlterHook(ExtensionRelationId, extensionOid, 0);

	ObjectAddressSet(extAddr, ExtensionRelationId, extensionOid);

	return extAddr;
}

/*
 * Execute ALTER EXTENSION UPDATE
 */
ObjectAddress
ExecAlterExtensionStmt(ParseState *pstate, AlterExtensionStmt *stmt)
{
	DefElem    *d_new_version = NULL;
	char	   *versionName;
	char	   *oldVersionName;
	ExtensionControlFile *control;
	Oid			extensionOid;
	Relation	extRel;
	ScanKeyData key[1];
	SysScanDesc extScan;
	HeapTuple	extTup;
	List	   *updateVersions;
	Datum		datum;
	bool		isnull;
	ListCell   *lc;
	ObjectAddress address;

	/*
	 * We use global variables to track the extension being created, so we can
	 * create/update only one extension at the same time.
	 */
	if (creating_extension)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("nested ALTER EXTENSION is not supported")));

	/*
	 * Look up the extension --- it must already exist in pg_extension
	 */
	extRel = table_open(ExtensionRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_extension_extname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->extname));

	extScan = systable_beginscan(extRel, ExtensionNameIndexId, true,
								 NULL, 1, key);

	extTup = systable_getnext(extScan);

	if (!HeapTupleIsValid(extTup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("extension \"%s\" does not exist",
						stmt->extname)));

	extensionOid = ((Form_pg_extension) GETSTRUCT(extTup))->oid;

	/*
	 * Determine the existing version we are updating from
	 */
	datum = heap_getattr(extTup, Anum_pg_extension_extversion,
						 RelationGetDescr(extRel), &isnull);
	if (isnull)
		elog(ERROR, "extversion is null");
	oldVersionName = text_to_cstring(DatumGetTextPP(datum));

	systable_endscan(extScan);

	table_close(extRel, AccessShareLock);

	/* Permission check: must own extension */
	if (!object_ownercheck(ExtensionRelationId, extensionOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_EXTENSION,
					   stmt->extname);

	/*
	 * Read the primary control file.  Note we assume that it does not contain
	 * any non-ASCII data, so there is no need to worry about encoding at this
	 * point.
	 */
	control = read_extension_control_file(stmt->extname);

	/*
	 * Read the statement option list
	 */
	foreach(lc, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		if (strcmp(defel->defname, "new_version") == 0)
		{
			if (d_new_version)
				errorConflictingDefElem(defel, pstate);
			d_new_version = defel;
		}
		else
			elog(ERROR, "unrecognized option: %s", defel->defname);
	}

	/*
	 * Determine the version to update to
	 */
	if (d_new_version && d_new_version->arg)
		versionName = strVal(d_new_version->arg);
	else if (control->default_version)
		versionName = control->default_version;
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("version to install must be specified")));
		versionName = NULL;		/* keep compiler quiet */
	}
	check_valid_version_name(versionName);

	/*
	 * If we're already at that version, just say so
	 */
	if (strcmp(oldVersionName, versionName) == 0)
	{
		ereport(NOTICE,
				(errmsg("version \"%s\" of extension \"%s\" is already installed",
						versionName, stmt->extname)));
		return InvalidObjectAddress;
	}

	/*
	 * Identify the series of update script files we need to execute
	 */
	updateVersions = identify_update_path(control,
										  oldVersionName,
										  versionName);

	/*
	 * Update the pg_extension row and execute the update scripts, one at a
	 * time
	 */
	ApplyExtensionUpdates(extensionOid, control,
						  oldVersionName, updateVersions,
						  NULL, false, false);

	ObjectAddressSet(address, ExtensionRelationId, extensionOid);

	return address;
}

/*
 * Apply a series of update scripts as though individual ALTER EXTENSION
 * UPDATE commands had been given, including altering the pg_extension row
 * and dependencies each time.
 *
 * This might be more work than necessary, but it ensures that old update
 * scripts don't break if newer versions have different control parameters.
 */
static void
ApplyExtensionUpdates(Oid extensionOid,
					  ExtensionControlFile *pcontrol,
					  const char *initialVersion,
					  List *updateVersions,
					  char *origSchemaName,
					  bool cascade,
					  bool is_create)
{
	const char *oldVersionName = initialVersion;
	ListCell   *lcv;

	foreach(lcv, updateVersions)
	{
		char	   *versionName = (char *) lfirst(lcv);
		ExtensionControlFile *control;
		char	   *schemaName;
		Oid			schemaOid;
		List	   *requiredExtensions;
		List	   *requiredSchemas;
		Relation	extRel;
		ScanKeyData key[1];
		SysScanDesc extScan;
		HeapTuple	extTup;
		Form_pg_extension extForm;
		Datum		values[Natts_pg_extension];
		bool		nulls[Natts_pg_extension];
		bool		repl[Natts_pg_extension];
		ObjectAddress myself;
		ListCell   *lc;

		/*
		 * Fetch parameters for specific version (pcontrol is not changed)
		 */
		control = read_extension_aux_control_file(pcontrol, versionName);

		/* Find the pg_extension tuple */
		extRel = table_open(ExtensionRelationId, RowExclusiveLock);

		ScanKeyInit(&key[0],
					Anum_pg_extension_oid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(extensionOid));

		extScan = systable_beginscan(extRel, ExtensionOidIndexId, true,
									 NULL, 1, key);

		extTup = systable_getnext(extScan);

		if (!HeapTupleIsValid(extTup))	/* should not happen */
			elog(ERROR, "could not find tuple for extension %u",
				 extensionOid);

		extForm = (Form_pg_extension) GETSTRUCT(extTup);

		/*
		 * Determine the target schema (set by original install)
		 */
		schemaOid = extForm->extnamespace;
		schemaName = get_namespace_name(schemaOid);

		/*
		 * Modify extrelocatable and extversion in the pg_extension tuple
		 */
		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));
		memset(repl, 0, sizeof(repl));

		values[Anum_pg_extension_extrelocatable - 1] =
			BoolGetDatum(control->relocatable);
		repl[Anum_pg_extension_extrelocatable - 1] = true;
		values[Anum_pg_extension_extversion - 1] =
			CStringGetTextDatum(versionName);
		repl[Anum_pg_extension_extversion - 1] = true;

		extTup = heap_modify_tuple(extTup, RelationGetDescr(extRel),
								   values, nulls, repl);

		CatalogTupleUpdate(extRel, &extTup->t_self, extTup);

		systable_endscan(extScan);

		table_close(extRel, RowExclusiveLock);

		/*
		 * Look up the prerequisite extensions for this version, install them
		 * if necessary, and build lists of their OIDs and the OIDs of their
		 * target schemas.
		 */
		requiredExtensions = NIL;
		requiredSchemas = NIL;
		foreach(lc, control->requires)
		{
			char	   *curreq = (char *) lfirst(lc);
			Oid			reqext;
			Oid			reqschema;

			reqext = get_required_extension(curreq,
											control->name,
											origSchemaName,
											cascade,
											NIL,
											is_create);
			reqschema = get_extension_schema(reqext);
			requiredExtensions = lappend_oid(requiredExtensions, reqext);
			requiredSchemas = lappend_oid(requiredSchemas, reqschema);
		}

		/*
		 * Remove and recreate dependencies on prerequisite extensions
		 */
		deleteDependencyRecordsForClass(ExtensionRelationId, extensionOid,
										ExtensionRelationId,
										DEPENDENCY_NORMAL);

		myself.classId = ExtensionRelationId;
		myself.objectId = extensionOid;
		myself.objectSubId = 0;

		foreach(lc, requiredExtensions)
		{
			Oid			reqext = lfirst_oid(lc);
			ObjectAddress otherext;

			otherext.classId = ExtensionRelationId;
			otherext.objectId = reqext;
			otherext.objectSubId = 0;

			recordDependencyOn(&myself, &otherext, DEPENDENCY_NORMAL);
		}

		InvokeObjectPostAlterHook(ExtensionRelationId, extensionOid, 0);

		/*
		 * Finally, execute the update script file
		 */
		execute_extension_script(extensionOid, control,
								 oldVersionName, versionName,
								 requiredSchemas,
								 schemaName);

		/*
		 * Update prior-version name and loop around.  Since
		 * execute_sql_string did a final CommandCounterIncrement, we can
		 * update the pg_extension row again.
		 */
		oldVersionName = versionName;
	}
}

/*
 * Execute ALTER EXTENSION ADD/DROP
 *
 * Return value is the address of the altered extension.
 *
 * objAddr is an output argument which, if not NULL, is set to the address of
 * the added/dropped object.
 */
ObjectAddress
ExecAlterExtensionContentsStmt(AlterExtensionContentsStmt *stmt,
							   ObjectAddress *objAddr)
{
	ObjectAddress extension;
	ObjectAddress object;
	Relation	relation;

	switch (stmt->objtype)
	{
		case OBJECT_DATABASE:
		case OBJECT_EXTENSION:
		case OBJECT_INDEX:
		case OBJECT_PUBLICATION:
		case OBJECT_ROLE:
		case OBJECT_STATISTIC_EXT:
		case OBJECT_SUBSCRIPTION:
		case OBJECT_TABLESPACE:
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cannot add an object of this type to an extension")));
			break;
		default:
			/* OK */
			break;
	}

	/*
	 * Find the extension and acquire a lock on it, to ensure it doesn't get
	 * dropped concurrently.  A sharable lock seems sufficient: there's no
	 * reason not to allow other sorts of manipulations, such as add/drop of
	 * other objects, to occur concurrently.  Concurrently adding/dropping the
	 * *same* object would be bad, but we prevent that by using a non-sharable
	 * lock on the individual object, below.
	 */
	extension = get_object_address(OBJECT_EXTENSION,
								   (Node *) makeString(stmt->extname),
								   &relation, AccessShareLock, false);

	/* Permission check: must own extension */
	if (!object_ownercheck(ExtensionRelationId, extension.objectId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_EXTENSION,
					   stmt->extname);

	/*
	 * Translate the parser representation that identifies the object into an
	 * ObjectAddress.  get_object_address() will throw an error if the object
	 * does not exist, and will also acquire a lock on the object to guard
	 * against concurrent DROP and ALTER EXTENSION ADD/DROP operations.
	 */
	object = get_object_address(stmt->objtype, stmt->object,
								&relation, ShareUpdateExclusiveLock, false);

	Assert(object.objectSubId == 0);
	if (objAddr)
		*objAddr = object;

	/* Permission check: must own target object, too */
	check_object_ownership(GetUserId(), stmt->objtype, object,
						   stmt->object, relation);

	/* Do the update, recursing to any dependent objects */
	ExecAlterExtensionContentsRecurse(stmt, extension, object);

	/* Finish up */
	InvokeObjectPostAlterHook(ExtensionRelationId, extension.objectId, 0);

	/*
	 * If get_object_address() opened the relation for us, we close it to keep
	 * the reference count correct - but we retain any locks acquired by
	 * get_object_address() until commit time, to guard against concurrent
	 * activity.
	 */
	if (relation != NULL)
		relation_close(relation, NoLock);

	return extension;
}

/*
 * ExecAlterExtensionContentsRecurse
 *		Subroutine for ExecAlterExtensionContentsStmt
 *
 * Do the bare alteration of object's membership in extension,
 * without permission checks.  Recurse to dependent objects, if any.
 */
static void
ExecAlterExtensionContentsRecurse(AlterExtensionContentsStmt *stmt,
								  ObjectAddress extension,
								  ObjectAddress object)
{
	Oid			oldExtension;

	/*
	 * Check existing extension membership.
	 */
	oldExtension = getExtensionOfObject(object.classId, object.objectId);

	if (stmt->action > 0)
	{
		/*
		 * ADD, so complain if object is already attached to some extension.
		 */
		if (OidIsValid(oldExtension))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("%s is already a member of extension \"%s\"",
							getObjectDescription(&object, false),
							get_extension_name(oldExtension))));

		/*
		 * Prevent a schema from being added to an extension if the schema
		 * contains the extension.  That would create a dependency loop.
		 */
		if (object.classId == NamespaceRelationId &&
			object.objectId == get_extension_schema(extension.objectId))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot add schema \"%s\" to extension \"%s\" "
							"because the schema contains the extension",
							get_namespace_name(object.objectId),
							stmt->extname)));

		/*
		 * OK, add the dependency.
		 */
		recordDependencyOn(&object, &extension, DEPENDENCY_EXTENSION);

		/*
		 * Also record the initial ACL on the object, if any.
		 *
		 * Note that this will handle the object's ACLs, as well as any ACLs
		 * on object subIds.  (In other words, when the object is a table,
		 * this will record the table's ACL and the ACLs for the columns on
		 * the table, if any).
		 */
		recordExtObjInitPriv(object.objectId, object.classId);
	}
	else
	{
		/*
		 * DROP, so complain if it's not a member.
		 */
		if (oldExtension != extension.objectId)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("%s is not a member of extension \"%s\"",
							getObjectDescription(&object, false),
							stmt->extname)));

		/*
		 * OK, drop the dependency.
		 */
		if (deleteDependencyRecordsForClass(object.classId, object.objectId,
											ExtensionRelationId,
											DEPENDENCY_EXTENSION) != 1)
			elog(ERROR, "unexpected number of extension dependency records");

		/*
		 * If it's a relation, it might have an entry in the extension's
		 * extconfig array, which we must remove.
		 */
		if (object.classId == RelationRelationId)
			extension_config_remove(extension.objectId, object.objectId);

		/*
		 * Remove all the initial ACLs, if any.
		 *
		 * Note that this will remove the object's ACLs, as well as any ACLs
		 * on object subIds.  (In other words, when the object is a table,
		 * this will remove the table's ACL and the ACLs for the columns on
		 * the table, if any).
		 */
		removeExtObjInitPriv(object.objectId, object.classId);
	}

	/*
	 * Recurse to any dependent objects; currently, this includes the array
	 * type of a base type, the multirange type associated with a range type,
	 * and the rowtype of a table.
	 */
	if (object.classId == TypeRelationId)
	{
		ObjectAddress depobject;

		depobject.classId = TypeRelationId;
		depobject.objectSubId = 0;

		/* If it has an array type, update that too */
		depobject.objectId = get_array_type(object.objectId);
		if (OidIsValid(depobject.objectId))
			ExecAlterExtensionContentsRecurse(stmt, extension, depobject);

		/* If it is a range type, update the associated multirange too */
		if (type_is_range(object.objectId))
		{
			depobject.objectId = get_range_multirange(object.objectId);
			if (!OidIsValid(depobject.objectId))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("could not find multirange type for data type %s",
								format_type_be(object.objectId))));
			ExecAlterExtensionContentsRecurse(stmt, extension, depobject);
		}
	}
	if (object.classId == RelationRelationId)
	{
		ObjectAddress depobject;

		depobject.classId = TypeRelationId;
		depobject.objectSubId = 0;

		/* It might not have a rowtype, but if it does, update that */
		depobject.objectId = get_rel_type_id(object.objectId);
		if (OidIsValid(depobject.objectId))
			ExecAlterExtensionContentsRecurse(stmt, extension, depobject);
	}
}

/*
 * Read the whole of file into memory.
 *
 * The file contents are returned as a single palloc'd chunk. For convenience
 * of the callers, an extra \0 byte is added to the end.  That is not counted
 * in the length returned into *length.
 */
static char *
read_whole_file(const char *filename, int *length)
{
	char	   *buf;
	FILE	   *file;
	size_t		bytes_to_read;
	struct stat fst;

	if (stat(filename, &fst) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", filename)));

	if (fst.st_size > (MaxAllocSize - 1))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("file \"%s\" is too large", filename)));
	bytes_to_read = (size_t) fst.st_size;

	if ((file = AllocateFile(filename, PG_BINARY_R)) == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\" for reading: %m",
						filename)));

	buf = (char *) palloc(bytes_to_read + 1);

	bytes_to_read = fread(buf, 1, bytes_to_read, file);

	if (ferror(file))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m", filename)));

	FreeFile(file);

	buf[bytes_to_read] = '\0';

	/*
	 * On Windows, manually convert Windows-style newlines (\r\n) to the Unix
	 * convention of \n only.  This avoids gotchas due to script files
	 * possibly getting converted when being transferred between platforms.
	 * Ideally we'd do this by using text mode to read the file, but that also
	 * causes control-Z to be treated as end-of-file.  Historically we've
	 * allowed control-Z in script files, so breaking that seems unwise.
	 */
#ifdef WIN32
	{
		char	   *s,
				   *d;

		for (s = d = buf; *s; s++)
		{
			if (!(*s == '\r' && s[1] == '\n'))
				*d++ = *s;
		}
		*d = '\0';
		bytes_to_read = d - buf;
	}
#endif

	*length = bytes_to_read;
	return buf;
}
