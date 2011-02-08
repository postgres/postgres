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


bool			creating_extension = false;
Oid				CurrentExtensionObject = InvalidOid;

/*
 * Internal data structure to hold the results of parsing a control file
 */
typedef struct ExtensionControlFile
{
	char	   *name;			/* name of the extension */
	char	   *script;			/* filename of the installation script */
	char	   *version;	    /* version ID, if any */
	char	   *comment;		/* comment, if any */
	char	   *schema;			/* target schema (allowed if !relocatable) */
	bool		relocatable;	/* is ALTER EXTENSION SET SCHEMA supported? */
	int			encoding;		/* encoding of the script file, or -1 */
	List	   *requires;		/* names of prerequisite extensions */
} ExtensionControlFile;


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
 * Utility functions to handle extension-related path names
 */
static bool
is_extension_control_filename(const char *filename)
{
	const char *extension = strrchr(filename, '.');

	return (extension != NULL) && (strcmp(extension, ".control") == 0);
}

static char *
get_extension_control_directory(void)
{
	char		sharepath[MAXPGPATH];
	char	   *result;

	get_share_path(my_exec_path, sharepath);
	result = (char *) palloc(MAXPGPATH);
	snprintf(result, MAXPGPATH, "%s/contrib", sharepath);

	return result;
}

static char *
get_extension_control_filename(const char *extname)
{
	char		sharepath[MAXPGPATH];
	char	   *result;

	get_share_path(my_exec_path, sharepath);
	result = (char *) palloc(MAXPGPATH);
	snprintf(result, MAXPGPATH, "%s/contrib/%s.control", sharepath, extname);

	return result;
}

/*
 * Given a relative pathname such as "name.sql", return the full path to
 * the script file.  If given an absolute name, just return it.
 */
static char *
get_extension_absolute_path(const char *filename)
{
	char		sharepath[MAXPGPATH];
	char	   *result;

	if (is_absolute_path(filename))
		return pstrdup(filename);

	get_share_path(my_exec_path, sharepath);
	result = (char *) palloc(MAXPGPATH);
    snprintf(result, MAXPGPATH, "%s/contrib/%s", sharepath, filename);

	return result;
}


/*
 * Read the control file for the specified extension.
 *
 * The control file is supposed to be very short, half a dozen lines, and
 * reading it is only allowed to superuser, so we don't worry about
 * memory allocation risks here.  Also note that we don't worry about
 * what encoding it's in; all values are expected to be ASCII.
 */
static ExtensionControlFile *
read_extension_control_file(const char *extname)
{
	char	   *filename = get_extension_control_filename(extname);
	FILE	   *file;
	ExtensionControlFile *control;
	ConfigVariable *item,
				   *head = NULL,
				   *tail = NULL;

	/*
	 * Parse the file content, using GUC's file parsing code
	 */
	if ((file = AllocateFile(filename, "r")) == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open extension control file \"%s\": %m",
						filename)));

	ParseConfigFp(file, filename, 0, ERROR, &head, &tail);

	FreeFile(file);

	/*
	 * Set up default values.  Pointer fields are initially null.
	 */
	control = (ExtensionControlFile *) palloc0(sizeof(ExtensionControlFile));
	control->name = pstrdup(extname);
	control->relocatable = false;
	control->encoding = -1;

	/*
	 * Convert the ConfigVariable list into ExtensionControlFile entries.
	 */
	for (item = head; item != NULL; item = item->next)
	{
		if (strcmp(item->name, "script") == 0)
		{
			control->script = pstrdup(item->value);
		}
		else if (strcmp(item->name, "version") == 0)
		{
			control->version = pstrdup(item->value);
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

	/*
	 * script defaults to ${extension-name}.sql
	 */
	if (control->script == NULL)
	{
		char	script[MAXPGPATH];

		snprintf(script, MAXPGPATH, "%s.sql", control->name);
		control->script = pstrdup(script);
	}

	return control;
}

/*
 * Read the SQL script into a string, and convert to database encoding
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
 * Execute the extension's script file
 */
static void
execute_extension_script(Oid extensionOid, ExtensionControlFile *control,
						 List *requiredSchemas,
						 const char *schemaName, Oid schemaOid)
{
	char       *filename = get_extension_absolute_path(control->script);
	char	   *save_client_min_messages = NULL,
			   *save_log_min_messages = NULL,
			   *save_search_path;
	StringInfoData pathbuf;
	ListCell   *lc;

	/*
	 * Force client_min_messages and log_min_messages to be at least WARNING,
	 * so that we won't spam the user with useless NOTICE messages from common
	 * script actions like creating shell types.
	 *
	 * We use the equivalent of SET LOCAL to ensure the setting is undone
	 * upon error.
	 */
	if (client_min_messages < WARNING)
	{
		save_client_min_messages =
			pstrdup(GetConfigOption("client_min_messages", false));
		(void) set_config_option("client_min_messages", "warning",
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_LOCAL, true);
	}

	if (log_min_messages < WARNING)
	{
		save_log_min_messages =
			pstrdup(GetConfigOption("log_min_messages", false));
		(void) set_config_option("log_min_messages", "warning",
								 PGC_SUSET, PGC_S_SESSION,
								 GUC_ACTION_LOCAL, true);
	}

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

	if (save_client_min_messages != NULL)
		(void) set_config_option("client_min_messages", save_client_min_messages,
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_LOCAL, true);
	if (save_log_min_messages != NULL)
		(void) set_config_option("log_min_messages", save_log_min_messages,
								 PGC_SUSET, PGC_S_SESSION,
								 GUC_ACTION_LOCAL, true);
}

/*
 * CREATE EXTENSION
 */
void
CreateExtension(CreateExtensionStmt *stmt)
{
	DefElem    *d_schema = NULL;
	char       *schemaName;
	Oid			schemaOid;
	Oid			extowner = GetUserId();
	ExtensionControlFile *control;
	List	   *requiredExtensions;
	List	   *requiredSchemas;
	Relation	rel;
	Datum		values[Natts_pg_extension];
	bool		nulls[Natts_pg_extension];
	HeapTuple	tuple;
	Oid			extensionOid;
	ObjectAddress myself;
	ObjectAddress nsp;
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
	 * Read the control file.  Note we assume that it does not contain
	 * any non-ASCII data, so there is no need to worry about encoding
	 * at this point.
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
		else
			elog(ERROR, "unrecognized option: %s", defel->defname);
	}

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
	 * Insert new tuple into pg_extension.
	 */
	rel = heap_open(ExtensionRelationId, RowExclusiveLock);

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));

	values[Anum_pg_extension_extname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(control->name));
	values[Anum_pg_extension_extowner - 1] = ObjectIdGetDatum(extowner);
	values[Anum_pg_extension_extnamespace - 1] = ObjectIdGetDatum(schemaOid);
	values[Anum_pg_extension_extrelocatable - 1] = BoolGetDatum(control->relocatable);

	if (control->version == NULL)
		nulls[Anum_pg_extension_extversion - 1] = true;
	else
		values[Anum_pg_extension_extversion - 1] =
			CStringGetTextDatum(control->version);

	nulls[Anum_pg_extension_extconfig - 1] = true;
	nulls[Anum_pg_extension_extcondition - 1] = true;

	tuple = heap_form_tuple(rel->rd_att, values, nulls);

	extensionOid = simple_heap_insert(rel, tuple);
	CatalogUpdateIndexes(rel, tuple);

	heap_freetuple(tuple);
	heap_close(rel, RowExclusiveLock);

	/*
	 * Apply any comment on extension
	 */
	if (control->comment != NULL)
		CreateComments(extensionOid, ExtensionRelationId, 0, control->comment);

	/*
	 * Record dependencies on owner, schema, and prerequisite extensions
	 */
	recordDependencyOnOwner(ExtensionRelationId, extensionOid, extowner);

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

	/*
	 * Finally, execute the extension script to create the member objects
	 */
	execute_extension_script(extensionOid, control, requiredSchemas,
							 schemaName, schemaOid);
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
			/* version */
			if (control->version == NULL)
				nulls[1] = true;
			else
				values[1] = CStringGetTextDatum(control->version);
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
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
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
