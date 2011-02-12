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
 * postgresql.conf and recovery.conf.  An extension also has an installation
 * script file, containing SQL commands to create the extension's objects.
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
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
#include <unistd.h>

#include "access/sysattr.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "commands/alter.h"
#include "commands/comment.h"
#include "commands/extension.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/tqual.h"


/* Globally visible state variables */
bool			creating_extension = false;
Oid				CurrentExtensionObject = InvalidOid;

/* Character that separates extension & version names in a script filename */
#define EXT_VERSION_SEP  '-'

/*
 * Internal data structure to hold the results of parsing a control file
 */
typedef struct ExtensionControlFile
{
	char	   *name;			/* name of the extension */
	char	   *directory;		/* directory for script files */
	char	   *default_version; /* default install target version, if any */
	char	   *comment;		/* comment, if any */
	char	   *schema;			/* target schema (allowed if !relocatable) */
	bool		relocatable;	/* is ALTER EXTENSION SET SCHEMA supported? */
	int			encoding;		/* encoding of the script file, or -1 */
	List	   *requires;		/* names of prerequisite extensions */
} ExtensionControlFile;

/*
 * Internal data structure for update path information
 */
typedef struct ExtensionVersionInfo
{
	char	   *name;			/* name of the starting version */
	List	   *reachable;		/* List of ExtensionVersionInfo's */
	/* working state for Dijkstra's algorithm: */
	bool		distance_known;	/* is distance from start known yet? */
	int			distance;		/* current worst-case distance estimate */
	struct ExtensionVersionInfo *previous; /* current best predecessor */
} ExtensionVersionInfo;


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
	Relation	rel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	rel = heap_open(ExtensionRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_extension_extname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(extname));

	scandesc = systable_beginscan(rel, ExtensionNameIndexId, true,
								  SnapshotNow, 1, entry);

	tuple = systable_getnext(scandesc);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
		result = HeapTupleGetOid(tuple);
	else
		result = InvalidOid;

	systable_endscan(scandesc);

	heap_close(rel, AccessShareLock);

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
	Relation	rel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	rel = heap_open(ExtensionRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ext_oid));

	scandesc = systable_beginscan(rel, ExtensionOidIndexId, true,
								  SnapshotNow, 1, entry);

	tuple = systable_getnext(scandesc);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
		result = pstrdup(NameStr(((Form_pg_extension) GETSTRUCT(tuple))->extname));
	else
		result = NULL;

	systable_endscan(scandesc);

	heap_close(rel, AccessShareLock);

	return result;
}

/*
 * get_extension_schema - given an extension OID, fetch its extnamespace
 *
 * Returns InvalidOid if no such extension.
 */
static Oid
get_extension_schema(Oid ext_oid)
{
	Oid			result;
	Relation	rel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	rel = heap_open(ExtensionRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ext_oid));

	scandesc = systable_beginscan(rel, ExtensionOidIndexId, true,
								  SnapshotNow, 1, entry);

	tuple = systable_getnext(scandesc);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
		result = ((Form_pg_extension) GETSTRUCT(tuple))->extnamespace;
	else
		result = InvalidOid;

	systable_endscan(scandesc);

	heap_close(rel, AccessShareLock);

	return result;
}

/*
 * Utility functions to check validity of extension and version names
 */
static void
check_valid_extension_name(const char *extensionname)
{
	/*
	 * No directory separators (this is sufficient to prevent ".." style
	 * attacks).
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
	/* No separators --- would risk confusion of install vs update scripts */
	if (strchr(versionname, EXT_VERSION_SEP))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension version name: \"%s\"", versionname),
				 errdetail("Version names must not contain the character \"%c\".",
						   EXT_VERSION_SEP)));
	/*
	 * No directory separators (this is sufficient to prevent ".." style
	 * attacks).
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
	snprintf(result, MAXPGPATH, "%s/%s%c%s.control",
			 scriptdir, control->name, EXT_VERSION_SEP, version);

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
		snprintf(result, MAXPGPATH, "%s/%s%c%s%c%s.sql",
				 scriptdir, control->name, EXT_VERSION_SEP, from_version,
				 EXT_VERSION_SEP, version);
	else
		snprintf(result, MAXPGPATH, "%s/%s%c%s.sql",
				 scriptdir, control->name, EXT_VERSION_SEP, version);

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
		if (version && errno == ENOENT)
		{
			/* no auxiliary file for this version */
			pfree(filename);
			return;
		}
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open extension control file \"%s\": %m",
						filename)));
	}

	/*
	 * Parse the file content, using GUC's file parsing code
	 */
	ParseConfigFp(file, filename, 0, ERROR, &head, &tail);

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
						 errmsg("parameter \"%s\" cannot be set in a per-version extension control file",
								item->name)));

			control->directory = pstrdup(item->value);
		}
		else if (strcmp(item->name, "default_version") == 0)
		{
			if (version)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("parameter \"%s\" cannot be set in a per-version extension control file",
								item->name)));

			control->default_version = pstrdup(item->value);
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
		else if (strcmp(item->name, "encoding") == 0)
		{
			if (version)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("parameter \"%s\" cannot be set in a per-version extension control file",
								item->name)));

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
	control->encoding = -1;

	/*
	 * Parse the primary control file.
	 */
	parse_extension_control_file(control, NULL);

	return control;
}

/*
 * Read a SQL script file into a string, and convert to database encoding
 */
static char *
read_extension_script_file(const ExtensionControlFile *control,
						   const char *filename)
{
	int         src_encoding;
	int         dest_encoding = GetDatabaseEncoding();
	bytea      *content;
	char	   *src_str;
	char       *dest_str;
	int         len;

	content = read_binary_file(filename, 0, -1);

	/* use database encoding if not given */
	if (control->encoding < 0)
		src_encoding = dest_encoding;
	else
		src_encoding = control->encoding;

	/* make sure that source string is valid in the expected encoding */
	len = VARSIZE_ANY_EXHDR(content);
	src_str = VARDATA_ANY(content);
	pg_verify_mbstr_len(src_encoding, src_str, len, false);

	/* convert the encoding to the database encoding */
	dest_str = (char *) pg_do_encoding_conversion((unsigned char *) src_str,
												  len,
												  src_encoding,
												  dest_encoding);

	/* if no conversion happened, we have to arrange for null termination */
	if (dest_str == src_str)
	{
		dest_str = (char *) palloc(len + 1);
		memcpy(dest_str, src_str, len);
		dest_str[len] = '\0';
	}

	return dest_str;
}

/*
 * Execute given SQL string.
 *
 * filename is used only to report errors.
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
	List	   *raw_parsetree_list;
	DestReceiver *dest;
	ListCell   *lc1;

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
		Node	   *parsetree = (Node *) lfirst(lc1);
		List	   *stmt_list;
		ListCell   *lc2;

		stmt_list = pg_analyze_and_rewrite(parsetree,
										   sql,
										   NULL,
										   0);
		stmt_list = pg_plan_queries(stmt_list, 0, NULL);

		foreach(lc2, stmt_list)
		{
			Node	   *stmt = (Node *) lfirst(lc2);

			if (IsA(stmt, TransactionStmt))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("transaction control statements are not allowed within an extension script")));

			CommandCounterIncrement();

			PushActiveSnapshot(GetTransactionSnapshot());

			if (IsA(stmt, PlannedStmt) &&
				((PlannedStmt *) stmt)->utilityStmt == NULL)
			{
				QueryDesc  *qdesc;

				qdesc = CreateQueryDesc((PlannedStmt *) stmt,
										sql,
										GetActiveSnapshot(), NULL,
										dest, NULL, 0);

				AfterTriggerBeginQuery();
				ExecutorStart(qdesc, 0);
				ExecutorRun(qdesc, ForwardScanDirection, 0);
				AfterTriggerEndQuery(qdesc->estate);
				ExecutorEnd(qdesc);

				FreeQueryDesc(qdesc);
			}
			else
			{
				ProcessUtility(stmt,
							   sql,
							   NULL,
							   false,	/* not top level */
							   dest,
							   NULL);
			}

			PopActiveSnapshot();
		}
	}

	/* Be sure to advance the command counter after the last script command */
	CommandCounterIncrement();
}

/*
 * Execute the appropriate script file for installing or updating the extension
 *
 * If from_version isn't NULL, it's an update
 */
static void
execute_extension_script(Oid extensionOid, ExtensionControlFile *control,
						 const char *from_version,
						 const char *version,
						 List *requiredSchemas,
						 const char *schemaName, Oid schemaOid)
{
	char       *filename;
	char	   *save_client_min_messages,
			   *save_log_min_messages,
			   *save_search_path;
	StringInfoData pathbuf;
	ListCell   *lc;

	filename = get_extension_script_filename(control, from_version, version);

	/*
	 * Force client_min_messages and log_min_messages to be at least WARNING,
	 * so that we won't spam the user with useless NOTICE messages from common
	 * script actions like creating shell types.
	 *
	 * We use the equivalent of SET LOCAL to ensure the setting is undone
	 * upon error.
	 */
	save_client_min_messages =
		pstrdup(GetConfigOption("client_min_messages", false));
	if (client_min_messages < WARNING)
		(void) set_config_option("client_min_messages", "warning",
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_LOCAL, true);

	save_log_min_messages =
		pstrdup(GetConfigOption("log_min_messages", false));
	if (log_min_messages < WARNING)
		(void) set_config_option("log_min_messages", "warning",
								 PGC_SUSET, PGC_S_SESSION,
								 GUC_ACTION_LOCAL, true);

	/*
	 * Set up the search path to contain the target schema, then the schemas
	 * of any prerequisite extensions, and nothing else.  In particular this
	 * makes the target schema be the default creation target namespace.
	 *
	 * Note: it might look tempting to use PushOverrideSearchPath for this,
	 * but we cannot do that.  We have to actually set the search_path GUC
	 * in case the extension script examines or changes it.
	 */
	save_search_path = pstrdup(GetConfigOption("search_path", false));

	initStringInfo(&pathbuf);
	appendStringInfoString(&pathbuf, quote_identifier(schemaName));
	foreach(lc, requiredSchemas)
	{
		Oid			reqschema = lfirst_oid(lc);
		char	   *reqname = get_namespace_name(reqschema);

		if (reqname)
			appendStringInfo(&pathbuf, ", %s", quote_identifier(reqname));
	}

	(void) set_config_option("search_path", pathbuf.data,
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_LOCAL, true);

	/*
	 * Set creating_extension and related variables so that
	 * recordDependencyOnCurrentExtension and other functions do the right
	 * things.  On failure, ensure we reset these variables.
	 */
	creating_extension = true;
	CurrentExtensionObject = extensionOid;
	PG_TRY();
	{
		char *sql = read_extension_script_file(control, filename);

		/*
		 * If it's not relocatable, substitute the target schema name for
		 * occcurrences of @extschema@.
		 *
		 * For a relocatable extension, we just run the script as-is.
		 * There cannot be any need for @extschema@, else it wouldn't
		 * be relocatable.
		 */
		if (!control->relocatable)
		{
			const char   *qSchemaName = quote_identifier(schemaName);

			sql = text_to_cstring(
				DatumGetTextPP(
					DirectFunctionCall3(replace_text,
										CStringGetTextDatum(sql),
										CStringGetTextDatum("@extschema@"),
										CStringGetTextDatum(qSchemaName))));

		}

		execute_sql_string(sql, filename);
	}
	PG_CATCH();
	{
		creating_extension = false;
		CurrentExtensionObject = InvalidOid;
		PG_RE_THROW();
	}
	PG_END_TRY();

	creating_extension = false;
	CurrentExtensionObject = InvalidOid;

	/*
	 * Restore GUC variables for the remainder of the current transaction.
	 * Again use SET LOCAL, so we won't affect the session value.
	 */
	(void) set_config_option("search_path", save_search_path,
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_LOCAL, true);
	(void) set_config_option("client_min_messages", save_client_min_messages,
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_LOCAL, true);
	(void) set_config_option("log_min_messages", save_log_min_messages,
							 PGC_SUSET, PGC_S_SESSION,
							 GUC_ACTION_LOCAL, true);
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
	dir  = AllocateDir(location);
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
			de->d_name[extnamelen] != EXT_VERSION_SEP)
			continue;

		/* extract version names from 'extname-something.sql' filename */
		vername = pstrdup(de->d_name + extnamelen + 1);
		*strrchr(vername, '.') = '\0';
		vername2 = strchr(vername, EXT_VERSION_SEP);
		if (!vername2)
			continue;			/* it's not an update script */
		*vername2++ = '\0';

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
 * Result is a List of names of versions to transition through.
 */
static List *
identify_update_path(ExtensionControlFile *control,
					 const char *oldVersion, const char *newVersion)
{
	List	   *result;
	List	   *evi_list;
	ExtensionVersionInfo *evi_start;
	ExtensionVersionInfo *evi_target;
	ExtensionVersionInfo *evi;

	if (strcmp(oldVersion, newVersion) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("version to install or update to must be different from old version")));

	/* Extract the version update graph from the script directory */
	evi_list = get_ext_ver_list(control);

	/* Initialize start and end vertices */
	evi_start = get_ext_ver_info(oldVersion, &evi_list);
	evi_target = get_ext_ver_info(newVersion, &evi_list);

	evi_start->distance = 0;

	while ((evi = get_nearest_unprocessed_vertex(evi_list)) != NULL)
	{
		ListCell   *lc;

		if (evi->distance == INT_MAX)
			break;				/* all remaining vertices are unreachable */
		evi->distance_known = true;
		if (evi == evi_target)
			break;				/* found shortest path to target */
		foreach(lc, evi->reachable)
		{
			ExtensionVersionInfo *evi2 = (ExtensionVersionInfo *) lfirst(lc);
			int		newdist;

			newdist = evi->distance + 1;
			if (newdist < evi2->distance)
			{
				evi2->distance = newdist;
				evi2->previous = evi;
			}
		}
	}

	if (!evi_target->distance_known)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("extension \"%s\" has no update path from version \"%s\" to version \"%s\"",
						control->name, oldVersion, newVersion)));

	/* Build and return list of version names representing the update path */
	result = NIL;
	for (evi = evi_target; evi != evi_start; evi = evi->previous)
		result = lcons(evi->name, result);

	return result;
}

/*
 * CREATE EXTENSION
 */
void
CreateExtension(CreateExtensionStmt *stmt)
{
	DefElem    *d_schema = NULL;
	DefElem    *d_new_version = NULL;
	DefElem    *d_old_version = NULL;
	char       *schemaName;
	Oid			schemaOid;
	char       *versionName;
	char       *oldVersionName;
	Oid			extowner = GetUserId();
	ExtensionControlFile *control;
	List	   *requiredExtensions;
	List	   *requiredSchemas;
	Oid			extensionOid;
	ListCell   *lc;

	/* Must be super user */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create extension \"%s\"",
						stmt->extname),
				 errhint("Must be superuser to create an extension.")));

	/*
	 * We use global variables to track the extension being created, so we
	 * can create only one extension at the same time.
	 */
	if (creating_extension)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("nested CREATE EXTENSION is not supported")));

	/* Check extension name validity before any filesystem access */
	check_valid_extension_name(stmt->extname);

	/*
	 * Check for duplicate extension name.  The unique index on
	 * pg_extension.extname would catch this anyway, and serves as a backstop
	 * in case of race conditions; but this is a friendlier error message.
	 */
	if (get_extension_oid(stmt->extname, true) != InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("extension \"%s\" already exists", stmt->extname)));

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

		if (strcmp(defel->defname, "schema") == 0)
		{
			if (d_schema)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			d_schema = defel;
		}
		else if (strcmp(defel->defname, "new_version") == 0)
		{
			if (d_new_version)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			d_new_version = defel;
		}
		else if (strcmp(defel->defname, "old_version") == 0)
		{
			if (d_old_version)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			d_old_version = defel;
		}
		else
			elog(ERROR, "unrecognized option: %s", defel->defname);
	}

	/*
	 * Determine the version to install
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
	 * Modify control parameters for specific new version
	 */
	parse_extension_control_file(control, versionName);

	/*
	 * Determine the (unpackaged) version to update from, if any
	 */
	if (d_old_version && d_old_version->arg)
	{
		oldVersionName = strVal(d_old_version->arg);
		check_valid_version_name(oldVersionName);
	}
	else
		oldVersionName = NULL;

	/*
	 * Determine the target schema to install the extension into
	 */
	if (d_schema && d_schema->arg)
	{
		/*
		 * User given schema, CREATE EXTENSION ... WITH SCHEMA ...
		 *
		 * It's an error to give a schema different from control->schema if
		 * control->schema is specified.
		 */
		schemaName = strVal(d_schema->arg);

		if (control->schema != NULL &&
			strcmp(control->schema, schemaName) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("extension \"%s\" must be installed in schema \"%s\"",
							control->name,
							control->schema)));

		/* If the user is giving us the schema name, it must exist already */
		schemaOid = get_namespace_oid(schemaName, false);
	}
	else if (control->schema != NULL)
	{
		/*
		 * The extension is not relocatable and the author gave us a schema
		 * for it.  We create the schema here if it does not already exist.
		 */
		schemaName = control->schema;
		schemaOid = get_namespace_oid(schemaName, true);

		if (schemaOid == InvalidOid)
		{
			schemaOid = NamespaceCreate(schemaName, extowner);
			/* Advance cmd counter to make the namespace visible */
			CommandCounterIncrement();
		}
	}
	else
	{
		/*
		 * Else, use the current default creation namespace, which is the
		 * first explicit entry in the search_path.
		 */
		List *search_path = fetch_search_path(false);

		if (search_path == NIL)				/* probably can't happen */
			elog(ERROR, "there is no default creation target");
		schemaOid = linitial_oid(search_path);
		schemaName = get_namespace_name(schemaOid);
		if (schemaName == NULL)				/* recently-deleted namespace? */
			elog(ERROR, "there is no default creation target");

		list_free(search_path);
	}

	/*
	 * If we didn't already know user is superuser, we would probably want
	 * to do pg_namespace_aclcheck(schemaOid, extowner, ACL_CREATE) here.
	 */

	/*
	 * Look up the prerequisite extensions, and build lists of their OIDs
	 * and the OIDs of their target schemas.
	 */
	requiredExtensions = NIL;
	requiredSchemas = NIL;
	foreach(lc, control->requires)
	{
		char	   *curreq = (char *) lfirst(lc);
		Oid			reqext;
		Oid			reqschema;

		/*
		 * We intentionally don't use get_extension_oid's default error
		 * message here, because it would be confusing in this context.
		 */
		reqext = get_extension_oid(curreq, true);
		if (!OidIsValid(reqext))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("required extension \"%s\" is not installed",
							curreq)));
		reqschema = get_extension_schema(reqext);
		requiredExtensions = lappend_oid(requiredExtensions, reqext);
		requiredSchemas = lappend_oid(requiredSchemas, reqschema);
	}

	/*
	 * Insert new tuple into pg_extension, and create dependency entries.
	 */
	extensionOid = InsertExtensionTuple(control->name, extowner,
										schemaOid, control->relocatable,
										versionName,
										PointerGetDatum(NULL),
										PointerGetDatum(NULL),
										requiredExtensions);

	/*
	 * Apply any comment on extension
	 */
	if (control->comment != NULL)
		CreateComments(extensionOid, ExtensionRelationId, 0, control->comment);

	/*
	 * Finally, execute the extension's script file(s)
	 */
	if (oldVersionName == NULL)
	{
		/* Simple install */
		execute_extension_script(extensionOid, control,
								 oldVersionName, versionName,
								 requiredSchemas,
								 schemaName, schemaOid);
	}
	else
	{
		/* Update from unpackaged objects --- find update-file path */
		List	   *updateVersions;

		updateVersions = identify_update_path(control,
											  oldVersionName,
											  versionName);

		foreach(lc, updateVersions)
		{
			char   *vname = (char *) lfirst(lc);

			execute_extension_script(extensionOid, control,
									 oldVersionName, vname,
									 requiredSchemas,
									 schemaName, schemaOid);
			oldVersionName = vname;
		}
	}
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
Oid
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
	ListCell   *lc;

	/*
	 * Build and insert the pg_extension tuple
	 */
	rel = heap_open(ExtensionRelationId, RowExclusiveLock);

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));

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

	extensionOid = simple_heap_insert(rel, tuple);
	CatalogUpdateIndexes(rel, tuple);

	heap_freetuple(tuple);
	heap_close(rel, RowExclusiveLock);

	/*
	 * Record dependencies on owner, schema, and prerequisite extensions
	 */
	recordDependencyOnOwner(ExtensionRelationId, extensionOid, extOwner);

	myself.classId = ExtensionRelationId;
	myself.objectId = extensionOid;
	myself.objectSubId = 0;

	nsp.classId = NamespaceRelationId;
	nsp.objectId = schemaOid;
	nsp.objectSubId = 0;

	recordDependencyOn(&myself, &nsp, DEPENDENCY_NORMAL);

	foreach(lc, requiredExtensions)
	{
		Oid			reqext = lfirst_oid(lc);
		ObjectAddress otherext;

		otherext.classId = ExtensionRelationId;
		otherext.objectId = reqext;
		otherext.objectSubId = 0;

		recordDependencyOn(&myself, &otherext, DEPENDENCY_NORMAL);
	}

	return extensionOid;
}


/*
 *	RemoveExtensions
 *		Implements DROP EXTENSION.
 */
void
RemoveExtensions(DropStmt *drop)
{
	ObjectAddresses *objects;
	ListCell   *cell;

	/*
	 * First we identify all the extensions, then we delete them in a single
	 * performMultipleDeletions() call.  This is to avoid unwanted DROP
	 * RESTRICT errors if one of the extensions depends on another.
	 */
	objects = new_object_addresses();

	foreach(cell, drop->objects)
	{
		List	   *names = (List *) lfirst(cell);
		char	   *extensionName;
		Oid			extensionId;
		ObjectAddress object;

		if (list_length(names) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("extension name cannot be qualified")));
		extensionName = strVal(linitial(names));

		extensionId = get_extension_oid(extensionName, drop->missing_ok);

		if (!OidIsValid(extensionId))
		{
			ereport(NOTICE,
					(errmsg("extension \"%s\" does not exist, skipping",
							extensionName)));
			continue;
		}

		/*
		 * Permission check.  For now, insist on superuser-ness; later we
		 * might want to relax that to being owner of the extension.
		 */
		if (!superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to drop extension \"%s\"",
							extensionName),
					 errhint("Must be superuser to drop an extension.")));

		object.classId = ExtensionRelationId;
		object.objectId = extensionId;
		object.objectSubId = 0;

		add_exact_object_address(&object, objects);
	}

	/*
	 * Do the deletions.  Objects contained in the extension(s) are removed by
	 * means of their dependency links to the extensions.
	 */
	performMultipleDeletions(objects, drop->behavior);

	free_object_addresses(objects);
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

	rel = heap_open(ExtensionRelationId, RowExclusiveLock);

	ScanKeyInit(&entry[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(extId));
	scandesc = systable_beginscan(rel, ExtensionOidIndexId, true,
								  SnapshotNow, 1, entry);

	tuple = systable_getnext(scandesc);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
		simple_heap_delete(rel, &tuple->t_self);

	systable_endscan(scandesc);

	heap_close(rel, RowExclusiveLock);
}

/*
 * This function lists the extensions available in the control directory
 * (each of which might or might not actually be installed).  We parse each
 * available control file and report the interesting fields.
 *
 * The system view pg_available_extensions provides a user interface to this
 * SRF, adding information about whether the extensions are installed in the
 * current DB.
 */
Datum
pg_available_extensions(PG_FUNCTION_ARGS)
{
	ReturnSetInfo	   *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc			tupdesc;
	Tuplestorestate	   *tupstore;
	MemoryContext		per_query_ctx;
	MemoryContext		oldcontext;
	char			   *location;
	DIR				   *dir;
	struct dirent	   *de;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to list available extensions"))));

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	location = get_extension_control_directory();
	dir  = AllocateDir(location);

	/*
	 * If the control directory doesn't exist, we want to silently return
	 * an empty set.  Any other error will be reported by ReadDir.
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
			Datum		values[4];
			bool		nulls[4];

			if (!is_extension_control_filename(de->d_name))
				continue;

			/* extract extension name from 'name.control' filename */
			extname = pstrdup(de->d_name);
			*strrchr(extname, '.') = '\0';

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
			/* relocatable */
			values[2] = BoolGetDatum(control->relocatable);
			/* comment */
			if (control->comment == NULL)
				nulls[3] = true;
			else
				values[3] = CStringGetTextDatum(control->comment);

			tuplestore_putvalues(tupstore, tupdesc, values, nulls);
		}

		FreeDir(dir);
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

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
	text	   *wherecond = PG_GETARG_TEXT_P(1);
	char	   *tablename;
	Relation	extRel;
	ScanKeyData	key[1];
	SysScanDesc	extScan;
	HeapTuple	extTup;
	Datum		arrayDatum;
	Datum		elementDatum;
	int			arrayIndex;
	bool		isnull;
	Datum		repl_val[Natts_pg_extension];
	bool		repl_null[Natts_pg_extension];
	bool		repl_repl[Natts_pg_extension];
	ArrayType  *a;

	/*
	 * We only allow this to be called from an extension's SQL script.
	 * We shouldn't need any permissions check beyond that.
	 */
	if (!creating_extension)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_extension_config_dump() can only be called "
						"from a SQL script executed by CREATE EXTENSION")));

	/*
	 * Check that the table exists and is a member of the extension being
	 * created.  This ensures that we don't need to register a dependency
	 * to protect the extconfig entry.
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
	 * Add the table OID and WHERE condition to the extension's extconfig
	 * and extcondition arrays.
	 */

	/* Find the pg_extension tuple */
	extRel = heap_open(ExtensionRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(CurrentExtensionObject));

	extScan = systable_beginscan(extRel, ExtensionOidIndexId, true,
								 SnapshotNow, 1, key);

	extTup = systable_getnext(extScan);

	if (!HeapTupleIsValid(extTup)) /* should not happen */
		elog(ERROR, "extension with oid %u does not exist",
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
		a = construct_array(&elementDatum, 1,
							OIDOID,
							sizeof(Oid), true, 'i');
	}
	else
	{
		a = DatumGetArrayTypeP(arrayDatum);
		Assert(ARR_ELEMTYPE(a) == OIDOID);
		Assert(ARR_NDIM(a) == 1);
		Assert(ARR_LBOUND(a)[0] == 1);

		arrayIndex = ARR_DIMS(a)[0] + 1; /* add after end */

		a = array_set(a, 1, &arrayIndex,
					  elementDatum,
					  false,
					  -1 /* varlena array */ ,
					  sizeof(Oid) /* OID's typlen */ ,
					  true /* OID's typbyval */ ,
					  'i' /* OID's typalign */ );
	}
	repl_val[Anum_pg_extension_extconfig - 1] = PointerGetDatum(a);
	repl_repl[Anum_pg_extension_extconfig - 1] = true;

	/* Build or modify the extcondition value */
	elementDatum = PointerGetDatum(wherecond);

	arrayDatum = heap_getattr(extTup, Anum_pg_extension_extcondition,
							  RelationGetDescr(extRel), &isnull);
	if (isnull)
	{
		a = construct_array(&elementDatum, 1,
							TEXTOID,
							-1, false, 'i');
	}
	else
	{
		a = DatumGetArrayTypeP(arrayDatum);
		Assert(ARR_ELEMTYPE(a) == TEXTOID);
		Assert(ARR_NDIM(a) == 1);
		Assert(ARR_LBOUND(a)[0] == 1);

		arrayIndex = ARR_DIMS(a)[0] + 1; /* add after end */

		a = array_set(a, 1, &arrayIndex,
					  elementDatum,
					  false,
					  -1 /* varlena array */ ,
					  -1 /* TEXT's typlen */ ,
					  false /* TEXT's typbyval */ ,
					  'i' /* TEXT's typalign */ );
	}
	repl_val[Anum_pg_extension_extcondition - 1] = PointerGetDatum(a);
	repl_repl[Anum_pg_extension_extcondition - 1] = true;

	extTup = heap_modify_tuple(extTup, RelationGetDescr(extRel),
							   repl_val, repl_null, repl_repl);

	simple_heap_update(extRel, &extTup->t_self, extTup);
	CatalogUpdateIndexes(extRel, extTup);

	systable_endscan(extScan);

	heap_close(extRel, RowExclusiveLock);

	PG_RETURN_VOID();
}

/*
 * Execute ALTER EXTENSION SET SCHEMA
 */
void
AlterExtensionNamespace(List *names, const char *newschema)
{
	char	   *extensionName;
	Oid			extensionOid;
	Oid			nspOid;
	Oid			oldNspOid = InvalidOid;
	Relation	extRel;
	ScanKeyData	key[2];
	SysScanDesc	extScan;
	HeapTuple	extTup;
	Form_pg_extension extForm;
	Relation	depRel;
	SysScanDesc	depScan;
	HeapTuple	depTup;

	if (list_length(names) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("extension name cannot be qualified")));
	extensionName = strVal(linitial(names));

	extensionOid = get_extension_oid(extensionName, false);

	nspOid = LookupCreationNamespace(newschema);

	/* this might later become an ownership test */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use ALTER EXTENSION"))));

	/* Locate the pg_extension tuple */
	extRel = heap_open(ExtensionRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(extensionOid));

	extScan = systable_beginscan(extRel, ExtensionOidIndexId, true,
								 SnapshotNow, 1, key);

	extTup = systable_getnext(extScan);

	if (!HeapTupleIsValid(extTup)) /* should not happen */
		elog(ERROR, "extension with oid %u does not exist", extensionOid);

	/* Copy tuple so we can modify it below */
	extTup = heap_copytuple(extTup);
	extForm = (Form_pg_extension) GETSTRUCT(extTup);

	systable_endscan(extScan);

	/*
	 * If the extension is already in the target schema, just silently
	 * do nothing.
	 */
	if (extForm->extnamespace == nspOid)
	{
		heap_close(extRel, RowExclusiveLock);
		return;
	}

	/* Check extension is supposed to be relocatable */
	if (!extForm->extrelocatable)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("extension \"%s\" does not support SET SCHEMA",
						NameStr(extForm->extname))));

	/*
	 * Scan pg_depend to find objects that depend directly on the extension,
	 * and alter each one's schema.
	 */
	depRel = heap_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ExtensionRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(extensionOid));

	depScan = systable_beginscan(depRel, DependReferenceIndexId, true,
								 SnapshotNow, 2, key);

	while (HeapTupleIsValid(depTup = systable_getnext(depScan)))
	{
		Form_pg_depend pg_depend = (Form_pg_depend) GETSTRUCT(depTup);
		ObjectAddress dep;
		Oid dep_oldNspOid;

		/*
		 * Ignore non-membership dependencies.  (Currently, the only other
		 * case we could see here is a normal dependency from another
		 * extension.)
		 */
		if (pg_depend->deptype != DEPENDENCY_EXTENSION)
			continue;

		dep.classId = pg_depend->classid;
		dep.objectId = pg_depend->objid;
		dep.objectSubId = pg_depend->objsubid;

		if (dep.objectSubId != 0)		/* should not happen */
			elog(ERROR, "extension should not have a sub-object dependency");

		dep_oldNspOid = AlterObjectNamespace_oid(dep.classId,
												 dep.objectId,
												 nspOid);

		/*
		 * Remember previous namespace of first object that has one
		 */
		if (oldNspOid == InvalidOid && dep_oldNspOid != InvalidOid)
			oldNspOid = dep_oldNspOid;

		/*
		 * If not all the objects had the same old namespace (ignoring any
		 * that are not in namespaces), complain.
		 */
		if (dep_oldNspOid != InvalidOid && dep_oldNspOid != oldNspOid)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("extension \"%s\" does not support SET SCHEMA",
							NameStr(extForm->extname)),
					 errdetail("%s is not in the extension's schema \"%s\"",
							   getObjectDescription(&dep),
							   get_namespace_name(oldNspOid))));
	}

	systable_endscan(depScan);

	relation_close(depRel, AccessShareLock);

	/* Now adjust pg_extension.extnamespace */
	extForm->extnamespace = nspOid;

	simple_heap_update(extRel, &extTup->t_self, extTup);
	CatalogUpdateIndexes(extRel, extTup);

	heap_close(extRel, RowExclusiveLock);

	/* update dependencies to point to the new schema */
	changeDependencyFor(ExtensionRelationId, extensionOid,
						NamespaceRelationId, oldNspOid, nspOid);
}

/*
 * Execute ALTER EXTENSION UPDATE
 */
void
ExecAlterExtensionStmt(AlterExtensionStmt *stmt)
{
	DefElem    *d_new_version = NULL;
	char       *schemaName;
	Oid			schemaOid;
	char       *versionName;
	char       *oldVersionName;
	ExtensionControlFile *control;
	List	   *requiredExtensions;
	List	   *requiredSchemas;
	Oid			extensionOid;
	Relation	extRel;
	ScanKeyData	key[1];
	SysScanDesc	extScan;
	HeapTuple	extTup;
	Form_pg_extension extForm;
	Datum		values[Natts_pg_extension];
	bool		nulls[Natts_pg_extension];
	bool		repl[Natts_pg_extension];
	List	   *updateVersions;
	ObjectAddress myself;
	Datum		datum;
	bool		isnull;
	ListCell   *lc;

	/*
	 * For now, insist on superuser privilege.  Later we might want to
	 * relax this to ownership of the extension.
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use ALTER EXTENSION"))));

	/*
	 * We use global variables to track the extension being created, so we
	 * can create/alter only one extension at the same time.
	 */
	if (creating_extension)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("nested ALTER EXTENSION is not supported")));

	/* Look up the extension --- it must already exist in pg_extension */
	extRel = heap_open(ExtensionRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_extension_extname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->extname));

	extScan = systable_beginscan(extRel, ExtensionNameIndexId, true,
								 SnapshotNow, 1, key);

	extTup = systable_getnext(extScan);

	if (!HeapTupleIsValid(extTup))
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("extension \"%s\" does not exist",
                        stmt->extname)));

	/* Copy tuple so we can modify it below */
	extTup = heap_copytuple(extTup);
	extForm = (Form_pg_extension) GETSTRUCT(extTup);
	extensionOid = HeapTupleGetOid(extTup);

	systable_endscan(extScan);

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
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			d_new_version = defel;
		}
		else
			elog(ERROR, "unrecognized option: %s", defel->defname);
	}

	/*
	 * Determine the version to install
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
	 * Modify control parameters for specific new version
	 */
	parse_extension_control_file(control, versionName);

	/*
	 * Determine the existing version we are upgrading from
	 */
	datum = heap_getattr(extTup, Anum_pg_extension_extversion,
						 RelationGetDescr(extRel), &isnull);
	if (isnull)
		elog(ERROR, "extversion is null");
	oldVersionName = text_to_cstring(DatumGetTextPP(datum));

	/*
	 * Determine the target schema (already set by original install)
	 */
	schemaOid = extForm->extnamespace;
	schemaName = get_namespace_name(schemaOid);

	/*
	 * Look up the prerequisite extensions, and build lists of their OIDs
	 * and the OIDs of their target schemas.  We assume that the requires
	 * list is version-specific, so the dependencies can change across
	 * versions.  But note that only the final version's requires list
	 * is being consulted here!
	 */
	requiredExtensions = NIL;
	requiredSchemas = NIL;
	foreach(lc, control->requires)
	{
		char	   *curreq = (char *) lfirst(lc);
		Oid			reqext;
		Oid			reqschema;

		/*
		 * We intentionally don't use get_extension_oid's default error
		 * message here, because it would be confusing in this context.
		 */
		reqext = get_extension_oid(curreq, true);
		if (!OidIsValid(reqext))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("required extension \"%s\" is not installed",
							curreq)));
		reqschema = get_extension_schema(reqext);
		requiredExtensions = lappend_oid(requiredExtensions, reqext);
		requiredSchemas = lappend_oid(requiredSchemas, reqschema);
	}

	/*
	 * Modify extversion in the pg_extension tuple
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));
	memset(repl, 0, sizeof(repl));

	values[Anum_pg_extension_extversion - 1] = CStringGetTextDatum(versionName);
	repl[Anum_pg_extension_extversion - 1] = true;

	extTup = heap_modify_tuple(extTup, RelationGetDescr(extRel),
							   values, nulls, repl);

	simple_heap_update(extRel, &extTup->t_self, extTup);
	CatalogUpdateIndexes(extRel, extTup);

	heap_close(extRel, RowExclusiveLock);

	/*
	 * Remove and recreate dependencies on prerequisite extensions
	 */
	deleteDependencyRecordsForClass(ExtensionRelationId, extensionOid,
									ExtensionRelationId, DEPENDENCY_NORMAL);

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

	/*
	 * Finally, execute the extension's script file(s)
	 */
	updateVersions = identify_update_path(control,
										  oldVersionName,
										  versionName);

	foreach(lc, updateVersions)
	{
		char   *vname = (char *) lfirst(lc);

		execute_extension_script(extensionOid, control,
								 oldVersionName, vname,
								 requiredSchemas,
								 schemaName, schemaOid);
		oldVersionName = vname;
	}
}

/*
 * Execute ALTER EXTENSION ADD/DROP
 */
void
ExecAlterExtensionContentsStmt(AlterExtensionContentsStmt *stmt)
{
	ObjectAddress	extension;
	ObjectAddress	object;
	Relation		relation;
	Oid				oldExtension;

	/*
	 * For now, insist on superuser privilege.  Later we might want to
	 * relax this to ownership of the target object and the extension.
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use ALTER EXTENSION"))));

	/* Do this next to fail on nonexistent extension */
	extension.classId = ExtensionRelationId;
	extension.objectId = get_extension_oid(stmt->extname, false);
	extension.objectSubId = 0;

	/*
	 * Translate the parser representation that identifies the object into
	 * an ObjectAddress.  get_object_address() will throw an error if the
	 * object does not exist, and will also acquire a lock on the object to
	 * guard against concurrent DROP and ALTER EXTENSION ADD/DROP operations.
	 */
	object = get_object_address(stmt->objtype, stmt->objname, stmt->objargs,
								&relation, ShareUpdateExclusiveLock);

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
							getObjectDescription(&object),
							get_extension_name(oldExtension))));

		/*
		 * OK, add the dependency.
		 */
		recordDependencyOn(&object, &extension, DEPENDENCY_EXTENSION);
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
							getObjectDescription(&object),
							stmt->extname)));

		/*
		 * OK, drop the dependency.
		 */
		if (deleteDependencyRecordsForClass(object.classId, object.objectId,
											ExtensionRelationId,
											DEPENDENCY_EXTENSION) != 1)
			elog(ERROR, "unexpected number of extension dependency records");
	}

	/*
	 * If get_object_address() opened the relation for us, we close it to keep
	 * the reference count correct - but we retain any locks acquired by
	 * get_object_address() until commit time, to guard against concurrent
	 * activity.
	 */
	if (relation != NULL)
		relation_close(relation, NoLock);
}
