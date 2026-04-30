/*-------------------------------------------------------------------------
 *
 * ddlutils.c
 *		Utility functions for generating DDL statements
 *
 * This file contains the pg_get_*_ddl family of functions that generate
 * DDL statements to recreate database objects such as roles, tablespaces,
 * and databases, along with common infrastructure for option parsing and
 * pretty-printing.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/ddlutils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_database.h"
#include "catalog/pg_db_role_setting.h"
#include "catalog/pg_tablespace.h"
#include "commands/tablespace.h"
#include "common/relpath.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/pg_locale.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/varlena.h"

/* Option value types for DDL option parsing */
typedef enum
{
	DDL_OPT_BOOL,
	DDL_OPT_TEXT,
	DDL_OPT_INT,
} DdlOptType;

/*
 * A single DDL option descriptor: caller fills in name and type,
 * parse_ddl_options fills in isset + the appropriate value field.
 */
typedef struct DdlOption
{
	const char *name;			/* option name (case-insensitive match) */
	DdlOptType	type;			/* expected value type */
	bool		isset;			/* true if caller supplied this option */
	/* fields for specific option types */
	union
	{
		bool		boolval;	/* filled in for DDL_OPT_BOOL */
		char	   *textval;	/* filled in for DDL_OPT_TEXT (palloc'd) */
		int			intval;		/* filled in for DDL_OPT_INT */
	};
} DdlOption;


static void parse_ddl_options(FunctionCallInfo fcinfo, int variadic_start,
							  DdlOption *opts, int nopts);
static void append_ddl_option(StringInfo buf, bool pretty, int indent,
							  const char *fmt,...)
			pg_attribute_printf(4, 5);
static void append_guc_value(StringInfo buf, const char *name,
							 const char *value);
static List *pg_get_role_ddl_internal(Oid roleid, bool pretty,
									  bool memberships);
static List *pg_get_tablespace_ddl_internal(Oid tsid, bool pretty, bool no_owner);
static Datum pg_get_tablespace_ddl_srf(FunctionCallInfo fcinfo, Oid tsid, bool isnull);
static List *pg_get_database_ddl_internal(Oid dbid, bool pretty,
										  bool no_owner, bool no_tablespace);


/*
 * parse_ddl_options
 * 		Parse variadic name/value option pairs
 *
 * Options are passed as alternating key/value text pairs.  The caller
 * provides an array of DdlOption descriptors specifying the accepted
 * option names and their types; this function matches each supplied
 * pair against the array, validates the value, and fills in the
 * result fields.
 */
static void
parse_ddl_options(FunctionCallInfo fcinfo, int variadic_start,
				  DdlOption *opts, int nopts)
{
	Datum	   *args;
	bool	   *nulls;
	Oid		   *types;
	int			nargs;

	/* Clear all output fields */
	for (int i = 0; i < nopts; i++)
	{
		opts[i].isset = false;
		switch (opts[i].type)
		{
			case DDL_OPT_BOOL:
				opts[i].boolval = false;
				break;
			case DDL_OPT_TEXT:
				opts[i].textval = NULL;
				break;
			case DDL_OPT_INT:
				opts[i].intval = 0;
				break;
		}
	}

	nargs = extract_variadic_args(fcinfo, variadic_start, true,
								  &args, &types, &nulls);

	if (nargs <= 0)
		return;

	/* Handle DEFAULT NULL case */
	if (nargs == 1 && nulls[0])
		return;

	if (nargs % 2 != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("variadic arguments must be name/value pairs"),
				 errhint("Provide an even number of variadic arguments that can be divided into pairs.")));

	/*
	 * For each option name/value pair, find corresponding positional option
	 * for the option name, and assign the option value.
	 */
	for (int i = 0; i < nargs; i += 2)
	{
		char	   *name;
		char	   *valstr;
		DdlOption  *opt = NULL;

		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("option name at variadic position %d is null", i + 1)));

		name = TextDatumGetCString(args[i]);

		if (nulls[i + 1])
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("value for option \"%s\" must not be null", name)));

		/* Find matching option descriptor */
		for (int j = 0; j < nopts; j++)
		{
			if (pg_strcasecmp(name, opts[j].name) == 0)
			{
				opt = &opts[j];
				break;
			}
		}

		if (opt == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized option: \"%s\"", name)));

		if (opt->isset)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("option \"%s\" is specified more than once",
							name)));

		valstr = TextDatumGetCString(args[i + 1]);

		switch (opt->type)
		{
			case DDL_OPT_BOOL:
				if (!parse_bool(valstr, &opt->boolval))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for boolean option \"%s\": %s",
									name, valstr)));
				break;

			case DDL_OPT_TEXT:
				opt->textval = valstr;
				valstr = NULL;	/* don't pfree below */
				break;

			case DDL_OPT_INT:
				{
					char	   *endp;
					long		val;

					errno = 0;
					val = strtol(valstr, &endp, 10);
					if (*endp != '\0' || errno == ERANGE ||
						val < PG_INT32_MIN || val > PG_INT32_MAX)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("invalid value for integer option \"%s\": %s",
										name, valstr)));
					opt->intval = (int) val;
				}
				break;
		}

		opt->isset = true;

		if (valstr)
			pfree(valstr);
		pfree(name);
	}
}

/*
 * Helper to append a formatted string with optional pretty-printing.
 */
static void
append_ddl_option(StringInfo buf, bool pretty, int indent,
				  const char *fmt,...)
{
	if (pretty)
	{
		appendStringInfoChar(buf, '\n');
		appendStringInfoSpaces(buf, indent);
	}
	else
		appendStringInfoChar(buf, ' ');

	for (;;)
	{
		va_list		args;
		int			needed;

		va_start(args, fmt);
		needed = appendStringInfoVA(buf, fmt, args);
		va_end(args);
		if (needed == 0)
			break;
		enlargeStringInfo(buf, needed);
	}
}

/*
 * append_guc_value
 *		Append a GUC setting value to buf, handling GUC_LIST_QUOTE properly.
 *
 * Variables marked GUC_LIST_QUOTE were already fully quoted before they
 * were stored in the setconfig array.  We break the list value apart
 * and re-quote the elements as string literals.  For all other variables
 * we simply quote the value as a single string literal.
 *
 * The caller has already appended "SET <name> TO " to buf.
 */
static void
append_guc_value(StringInfo buf, const char *name, const char *value)
{
	char	   *rawval;

	rawval = pstrdup(value);

	if (GetConfigOptionFlags(name, true) & GUC_LIST_QUOTE)
	{
		List	   *namelist;
		bool		first = true;

		/* Parse string into list of identifiers */
		if (!SplitGUCList(rawval, ',', &namelist))
		{
			/* this shouldn't fail really */
			elog(ERROR, "invalid list syntax in setconfig item");
		}
		/* Special case: represent an empty list as NULL */
		if (namelist == NIL)
			appendStringInfoString(buf, "NULL");
		foreach_ptr(char, curname, namelist)
		{
			if (first)
				first = false;
			else
				appendStringInfoString(buf, ", ");
			appendStringInfoString(buf, quote_literal_cstr(curname));
		}
		list_free(namelist);
	}
	else
		appendStringInfoString(buf, quote_literal_cstr(rawval));

	pfree(rawval);
}

/*
 * pg_get_role_ddl_internal
 *		Generate DDL statements to recreate a role
 *
 * Returns a List of palloc'd strings, each being a complete SQL statement.
 * The first list element is always the CREATE ROLE statement; subsequent
 * elements are ALTER ROLE SET statements for any role-specific or
 * role-in-database configuration settings.  If memberships is true,
 * GRANT statements for role memberships are appended.
 */
static List *
pg_get_role_ddl_internal(Oid roleid, bool pretty, bool memberships)
{
	HeapTuple	tuple;
	Form_pg_authid roleform;
	StringInfoData buf;
	char	   *rolname;
	Datum		rolevaliduntil;
	bool		isnull;
	Relation	rel;
	ScanKeyData scankey;
	SysScanDesc scan;
	List	   *statements = NIL;

	tuple = SearchSysCache1(AUTHOID, ObjectIdGetDatum(roleid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("role with OID %u does not exist", roleid)));

	roleform = (Form_pg_authid) GETSTRUCT(tuple);
	rolname = pstrdup(NameStr(roleform->rolname));

	/* User must have SELECT privilege on pg_authid. */
	if (pg_class_aclcheck(AuthIdRelationId, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
	{
		ReleaseSysCache(tuple);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for role %s", rolname)));
	}

	/*
	 * We don't support generating DDL for system roles.  The primary reason
	 * for this is that users shouldn't be recreating them.
	 */
	if (IsReservedName(rolname))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("role name \"%s\" is reserved", rolname),
				 errdetail("Role names starting with \"pg_\" are reserved for system roles.")));

	initStringInfo(&buf);
	appendStringInfo(&buf, "CREATE ROLE %s", quote_identifier(rolname));

	/*
	 * Append role attributes.  The order here follows the same sequence as
	 * you'd typically write them in a CREATE ROLE command, though any order
	 * is actually acceptable to the parser.
	 */
	append_ddl_option(&buf, pretty, 4, "%s",
					  roleform->rolsuper ? "SUPERUSER" : "NOSUPERUSER");

	append_ddl_option(&buf, pretty, 4, "%s",
					  roleform->rolinherit ? "INHERIT" : "NOINHERIT");

	append_ddl_option(&buf, pretty, 4, "%s",
					  roleform->rolcreaterole ? "CREATEROLE" : "NOCREATEROLE");

	append_ddl_option(&buf, pretty, 4, "%s",
					  roleform->rolcreatedb ? "CREATEDB" : "NOCREATEDB");

	append_ddl_option(&buf, pretty, 4, "%s",
					  roleform->rolcanlogin ? "LOGIN" : "NOLOGIN");

	append_ddl_option(&buf, pretty, 4, "%s",
					  roleform->rolreplication ? "REPLICATION" : "NOREPLICATION");

	append_ddl_option(&buf, pretty, 4, "%s",
					  roleform->rolbypassrls ? "BYPASSRLS" : "NOBYPASSRLS");

	/*
	 * CONNECTION LIMIT is only interesting if it's not -1 (the default,
	 * meaning no limit).
	 */
	if (roleform->rolconnlimit >= 0)
		append_ddl_option(&buf, pretty, 4, "CONNECTION LIMIT %d",
						  roleform->rolconnlimit);

	rolevaliduntil = SysCacheGetAttr(AUTHOID, tuple,
									 Anum_pg_authid_rolvaliduntil,
									 &isnull);
	if (!isnull)
	{
		TimestampTz ts;
		int			tz;
		struct pg_tm tm;
		fsec_t		fsec;
		const char *tzn;
		char		ts_str[MAXDATELEN + 1];

		ts = DatumGetTimestampTz(rolevaliduntil);
		if (TIMESTAMP_NOT_FINITE(ts))
			EncodeSpecialTimestamp(ts, ts_str);
		else if (timestamp2tm(ts, &tz, &tm, &fsec, &tzn, NULL) == 0)
			EncodeDateTime(&tm, fsec, true, tz, tzn, USE_ISO_DATES, ts_str);
		else
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		append_ddl_option(&buf, pretty, 4, "VALID UNTIL %s",
						  quote_literal_cstr(ts_str));
	}

	ReleaseSysCache(tuple);

	/*
	 * We intentionally omit PASSWORD.  There's no way to retrieve the
	 * original password text from the stored hash, and even if we could,
	 * exposing passwords through a SQL function would be a security issue.
	 * Users must set passwords separately after recreating roles.
	 */

	appendStringInfoChar(&buf, ';');

	statements = lappend(statements, pstrdup(buf.data));

	/*
	 * Now scan pg_db_role_setting for ALTER ROLE SET configurations.
	 *
	 * These can be role-wide (setdatabase = 0) or specific to a particular
	 * database (setdatabase = a valid DB OID).  It generates one ALTER
	 * statement per setting.
	 */
	rel = table_open(DbRoleSettingRelationId, AccessShareLock);
	ScanKeyInit(&scankey,
				Anum_pg_db_role_setting_setrole,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(roleid));
	scan = systable_beginscan(rel, DbRoleSettingDatidRolidIndexId, true,
							  NULL, 1, &scankey);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_db_role_setting setting = (Form_pg_db_role_setting) GETSTRUCT(tuple);
		Oid			datid = setting->setdatabase;
		Datum		datum;
		ArrayType  *role_settings;
		Datum	   *settings;
		bool	   *nulls;
		int			nsettings;
		char	   *datname = NULL;

		/*
		 * If setdatabase is valid, this is a role-in-database setting;
		 * otherwise it's a role-wide setting.  Look up the database name once
		 * for all settings in this row.
		 */
		if (OidIsValid(datid))
		{
			datname = get_database_name(datid);
			/* Database has been dropped; skip all settings in this row. */
			if (datname == NULL)
				continue;
		}

		/*
		 * The setconfig column is a text array in "name=value" format. It
		 * should never be null for a valid row, but be defensive.
		 */
		datum = heap_getattr(tuple, Anum_pg_db_role_setting_setconfig,
							 RelationGetDescr(rel), &isnull);
		if (isnull)
			continue;

		role_settings = DatumGetArrayTypePCopy(datum);

		deconstruct_array_builtin(role_settings, TEXTOID, &settings, &nulls, &nsettings);

		for (int i = 0; i < nsettings; i++)
		{
			char	   *s,
					   *p;

			if (nulls[i])
				continue;

			s = TextDatumGetCString(settings[i]);
			p = strchr(s, '=');
			if (p == NULL)
			{
				pfree(s);
				continue;
			}
			*p++ = '\0';

			/* Build a fresh ALTER ROLE statement for this setting */
			resetStringInfo(&buf);
			appendStringInfo(&buf, "ALTER ROLE %s", quote_identifier(rolname));

			if (datname != NULL)
				appendStringInfo(&buf, " IN DATABASE %s",
								 quote_identifier(datname));

			appendStringInfo(&buf, " SET %s TO ",
							 quote_identifier(s));

			append_guc_value(&buf, s, p);

			appendStringInfoChar(&buf, ';');

			statements = lappend(statements, pstrdup(buf.data));

			pfree(s);
		}

		pfree(settings);
		pfree(nulls);
		pfree(role_settings);

		if (datname != NULL)
			pfree(datname);
	}

	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	/*
	 * Scan pg_auth_members for role memberships.  We look for rows where
	 * member = roleid, meaning this role has been granted membership in other
	 * roles.
	 */
	if (memberships)
	{
		rel = table_open(AuthMemRelationId, AccessShareLock);
		ScanKeyInit(&scankey,
					Anum_pg_auth_members_member,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(roleid));
		scan = systable_beginscan(rel, AuthMemMemRoleIndexId, true,
								  NULL, 1, &scankey);

		while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		{
			Form_pg_auth_members memform = (Form_pg_auth_members) GETSTRUCT(tuple);
			char	   *granted_role;
			char	   *grantor;

			granted_role = GetUserNameFromId(memform->roleid, false);
			grantor = GetUserNameFromId(memform->grantor, false);

			resetStringInfo(&buf);
			appendStringInfo(&buf, "GRANT %s TO %s",
							 quote_identifier(granted_role),
							 quote_identifier(rolname));
			appendStringInfo(&buf, " WITH ADMIN %s, INHERIT %s, SET %s",
							 memform->admin_option ? "TRUE" : "FALSE",
							 memform->inherit_option ? "TRUE" : "FALSE",
							 memform->set_option ? "TRUE" : "FALSE");
			appendStringInfo(&buf, " GRANTED BY %s;",
							 quote_identifier(grantor));

			statements = lappend(statements, pstrdup(buf.data));

			pfree(granted_role);
			pfree(grantor);
		}

		systable_endscan(scan);
		table_close(rel, AccessShareLock);
	}

	pfree(buf.data);
	pfree(rolname);

	return statements;
}

/*
 * pg_get_role_ddl
 *		Return DDL to recreate a role as a set of text rows.
 *
 * Each row is a complete SQL statement.  The first row is always the
 * CREATE ROLE statement; subsequent rows are ALTER ROLE SET statements
 * and optionally GRANT statements for role memberships.
 * Returns no rows if the role argument is NULL.
 */
Datum
pg_get_role_ddl(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	List	   *statements;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		Oid			roleid;
		DdlOption	opts[] = {
			{"pretty", DDL_OPT_BOOL},
			{"memberships", DDL_OPT_BOOL},
		};

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (PG_ARGISNULL(0))
		{
			MemoryContextSwitchTo(oldcontext);
			SRF_RETURN_DONE(funcctx);
		}

		roleid = PG_GETARG_OID(0);
		parse_ddl_options(fcinfo, 1, opts, lengthof(opts));

		statements = pg_get_role_ddl_internal(roleid,
											  opts[0].isset && opts[0].boolval,
											  !opts[1].isset || opts[1].boolval);
		funcctx->user_fctx = statements;
		funcctx->max_calls = list_length(statements);

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	statements = (List *) funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		char	   *stmt;

		stmt = list_nth(statements, funcctx->call_cntr);

		SRF_RETURN_NEXT(funcctx, CStringGetTextDatum(stmt));
	}
	else
	{
		list_free_deep(statements);
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * pg_get_tablespace_ddl_internal
 *		Generate DDL statements to recreate a tablespace.
 *
 * Returns a List of palloc'd strings.  The first element is the
 * CREATE TABLESPACE statement; if the tablespace has reloptions,
 * a second element with ALTER TABLESPACE SET (...) is appended.
 */
static List *
pg_get_tablespace_ddl_internal(Oid tsid, bool pretty, bool no_owner)
{
	HeapTuple	tuple;
	Form_pg_tablespace tspForm;
	StringInfoData buf;
	char	   *spcname;
	char	   *spcowner;
	char	   *path;
	bool		isNull;
	Datum		datum;
	List	   *statements = NIL;

	tuple = SearchSysCache1(TABLESPACEOID, ObjectIdGetDatum(tsid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace with OID %u does not exist",
						tsid)));

	tspForm = (Form_pg_tablespace) GETSTRUCT(tuple);
	spcname = pstrdup(NameStr(tspForm->spcname));

	/* User must have SELECT privilege on pg_tablespace. */
	if (pg_class_aclcheck(TableSpaceRelationId, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
	{
		ReleaseSysCache(tuple);
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLESPACE, spcname);
	}

	/*
	 * We don't support generating DDL for system tablespaces.  The primary
	 * reason for this is that users shouldn't be recreating them.
	 */
	if (IsReservedName(spcname))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("tablespace name \"%s\" is reserved", spcname),
				 errdetail("Tablespace names starting with \"pg_\" are reserved for system tablespaces.")));

	initStringInfo(&buf);

	/* Start building the CREATE TABLESPACE statement */
	appendStringInfo(&buf, "CREATE TABLESPACE %s", quote_identifier(spcname));

	/* Add OWNER clause */
	if (!no_owner)
	{
		spcowner = GetUserNameFromId(tspForm->spcowner, false);
		append_ddl_option(&buf, pretty, 4, "OWNER %s",
						  quote_identifier(spcowner));
		pfree(spcowner);
	}

	/* Find tablespace directory path */
	path = get_tablespace_location(tsid);

	/* Add directory LOCATION (path), if it exists */
	if (path[0] != '\0')
	{
		/*
		 * Special case: if the tablespace was created with GUC
		 * "allow_in_place_tablespaces = true" and "LOCATION ''", path will
		 * begin with "pg_tblspc/". In that case, show "LOCATION ''" as the
		 * user originally specified.
		 */
		if (strncmp(PG_TBLSPC_DIR_SLASH, path, strlen(PG_TBLSPC_DIR_SLASH)) == 0)
			append_ddl_option(&buf, pretty, 4, "LOCATION ''");
		else
			append_ddl_option(&buf, pretty, 4, "LOCATION %s",
							  quote_literal_cstr(path));
	}
	pfree(path);

	appendStringInfoChar(&buf, ';');
	statements = lappend(statements, pstrdup(buf.data));

	/* Check for tablespace options */
	datum = SysCacheGetAttr(TABLESPACEOID, tuple,
							Anum_pg_tablespace_spcoptions, &isNull);
	if (!isNull)
	{
		resetStringInfo(&buf);
		appendStringInfo(&buf, "ALTER TABLESPACE %s SET (",
						 quote_identifier(spcname));
		get_reloptions(&buf, datum);
		appendStringInfoString(&buf, ");");
		statements = lappend(statements, pstrdup(buf.data));
	}

	ReleaseSysCache(tuple);
	pfree(spcname);
	pfree(buf.data);

	return statements;
}

/*
 * pg_get_tablespace_ddl_srf - common SRF logic for tablespace DDL
 */
static Datum
pg_get_tablespace_ddl_srf(FunctionCallInfo fcinfo, Oid tsid, bool isnull)
{
	FuncCallContext *funcctx;
	List	   *statements;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		DdlOption	opts[] = {
			{"pretty", DDL_OPT_BOOL},
			{"owner", DDL_OPT_BOOL},
		};

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (isnull)
		{
			MemoryContextSwitchTo(oldcontext);
			SRF_RETURN_DONE(funcctx);
		}

		parse_ddl_options(fcinfo, 1, opts, lengthof(opts));

		statements = pg_get_tablespace_ddl_internal(tsid,
													opts[0].isset && opts[0].boolval,
													opts[1].isset && !opts[1].boolval);
		funcctx->user_fctx = statements;
		funcctx->max_calls = list_length(statements);

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	statements = (List *) funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		char	   *stmt;

		stmt = (char *) list_nth(statements, funcctx->call_cntr);

		SRF_RETURN_NEXT(funcctx, CStringGetTextDatum(stmt));
	}
	else
	{
		list_free_deep(statements);
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * pg_get_tablespace_ddl_oid
 *		Return DDL to recreate a tablespace, taking OID.
 */
Datum
pg_get_tablespace_ddl_oid(PG_FUNCTION_ARGS)
{
	Oid			tsid = InvalidOid;
	bool		isnull;

	isnull = PG_ARGISNULL(0);
	if (!isnull)
		tsid = PG_GETARG_OID(0);

	return pg_get_tablespace_ddl_srf(fcinfo, tsid, isnull);
}

/*
 * pg_get_tablespace_ddl_name
 *		Return DDL to recreate a tablespace, taking name.
 */
Datum
pg_get_tablespace_ddl_name(PG_FUNCTION_ARGS)
{
	Oid			tsid = InvalidOid;
	Name		tspname;
	bool		isnull;

	isnull = PG_ARGISNULL(0);

	if (!isnull)
	{
		tspname = PG_GETARG_NAME(0);
		tsid = get_tablespace_oid(NameStr(*tspname), false);
	}

	return pg_get_tablespace_ddl_srf(fcinfo, tsid, isnull);
}

/*
 * pg_get_database_ddl_internal
 *		Generate DDL statements to recreate a database.
 *
 * Returns a List of palloc'd strings.  The first element is the
 * CREATE DATABASE statement; subsequent elements are ALTER DATABASE
 * statements for properties and configuration settings.
 */
static List *
pg_get_database_ddl_internal(Oid dbid, bool pretty,
							 bool no_owner, bool no_tablespace)
{
	HeapTuple	tuple;
	Form_pg_database dbform;
	StringInfoData buf;
	bool		isnull;
	Datum		datum;
	const char *encoding;
	char	   *dbname;
	char	   *collate;
	char	   *ctype;
	Relation	rel;
	ScanKeyData scankey[2];
	SysScanDesc scan;
	List	   *statements = NIL;
	AclResult	aclresult;

	tuple = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(dbid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("database with OID %u does not exist", dbid)));

	/* User must have connect privilege for target database. */
	aclresult = object_aclcheck(DatabaseRelationId, dbid, GetUserId(), ACL_CONNECT);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_DATABASE,
					   get_database_name(dbid));

	dbform = (Form_pg_database) GETSTRUCT(tuple);
	dbname = pstrdup(NameStr(dbform->datname));

	/*
	 * Reject invalid databases. Deparsing a pg_database row in invalid state
	 * can produce SQL that is not executable, such as CONNECTION LIMIT = -2.
	 */
	if (database_is_invalid_form(dbform))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot generate DDL for invalid database \"%s\"",
						dbname)));

	/*
	 * We don't support generating DDL for system databases.  The primary
	 * reason for this is that users shouldn't be recreating them.
	 */
	if (strcmp(dbname, "template0") == 0 || strcmp(dbname, "template1") == 0)
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("database \"%s\" is a system database", dbname),
				 errdetail("DDL generation is not supported for template0 and template1.")));

	initStringInfo(&buf);

	/* --- Build CREATE DATABASE statement --- */
	appendStringInfo(&buf, "CREATE DATABASE %s", quote_identifier(dbname));

	/*
	 * Always use template0: the target database already contains the catalog
	 * data from whatever template was used originally, so we must start from
	 * the pristine template to avoid duplication.
	 */
	append_ddl_option(&buf, pretty, 4, "WITH TEMPLATE = template0");

	/* ENCODING */
	encoding = pg_encoding_to_char(dbform->encoding);
	if (strlen(encoding) > 0)
		append_ddl_option(&buf, pretty, 4, "ENCODING = %s",
						  quote_literal_cstr(encoding));

	/* LOCALE_PROVIDER */
	if (dbform->datlocprovider == COLLPROVIDER_BUILTIN ||
		dbform->datlocprovider == COLLPROVIDER_ICU ||
		dbform->datlocprovider == COLLPROVIDER_LIBC)
		append_ddl_option(&buf, pretty, 4, "LOCALE_PROVIDER = %s",
						  collprovider_name(dbform->datlocprovider));
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("unrecognized locale provider: %c",
						dbform->datlocprovider)));

	/* LOCALE, LC_COLLATE, LC_CTYPE */
	datum = SysCacheGetAttr(DATABASEOID, tuple,
							Anum_pg_database_datcollate, &isnull);
	collate = isnull ? NULL : TextDatumGetCString(datum);
	datum = SysCacheGetAttr(DATABASEOID, tuple,
							Anum_pg_database_datctype, &isnull);
	ctype = isnull ? NULL : TextDatumGetCString(datum);
	if (collate != NULL && ctype != NULL && strcmp(collate, ctype) == 0)
	{
		append_ddl_option(&buf, pretty, 4, "LOCALE = %s",
						  quote_literal_cstr(collate));
	}
	else
	{
		if (collate != NULL)
			append_ddl_option(&buf, pretty, 4, "LC_COLLATE = %s",
							  quote_literal_cstr(collate));
		if (ctype != NULL)
			append_ddl_option(&buf, pretty, 4, "LC_CTYPE = %s",
							  quote_literal_cstr(ctype));
	}

	/* LOCALE (provider-specific) */
	datum = SysCacheGetAttr(DATABASEOID, tuple,
							Anum_pg_database_datlocale, &isnull);
	if (!isnull)
	{
		const char *locale = TextDatumGetCString(datum);

		if (dbform->datlocprovider == COLLPROVIDER_BUILTIN)
			append_ddl_option(&buf, pretty, 4, "BUILTIN_LOCALE = %s",
							  quote_literal_cstr(locale));
		else if (dbform->datlocprovider == COLLPROVIDER_ICU)
			append_ddl_option(&buf, pretty, 4, "ICU_LOCALE = %s",
							  quote_literal_cstr(locale));
	}

	/* ICU_RULES */
	datum = SysCacheGetAttr(DATABASEOID, tuple,
							Anum_pg_database_daticurules, &isnull);
	if (!isnull && dbform->datlocprovider == COLLPROVIDER_ICU)
		append_ddl_option(&buf, pretty, 4, "ICU_RULES = %s",
						  quote_literal_cstr(TextDatumGetCString(datum)));

	/* TABLESPACE */
	if (!no_tablespace && OidIsValid(dbform->dattablespace))
	{
		char	   *spcname = get_tablespace_name(dbform->dattablespace);

		if (spcname == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("tablespace with OID %u does not exist",
							dbform->dattablespace),
					 errdetail("It may have been concurrently dropped.")));

		if (pg_strcasecmp(spcname, "pg_default") != 0)
			append_ddl_option(&buf, pretty, 4, "TABLESPACE = %s",
							  quote_identifier(spcname));
	}

	appendStringInfoChar(&buf, ';');
	statements = lappend(statements, pstrdup(buf.data));

	/* OWNER */
	if (!no_owner && OidIsValid(dbform->datdba))
	{
		char	   *owner = GetUserNameFromId(dbform->datdba, false);

		resetStringInfo(&buf);
		appendStringInfo(&buf, "ALTER DATABASE %s OWNER TO %s;",
						 quote_identifier(dbname), quote_identifier(owner));
		pfree(owner);
		statements = lappend(statements, pstrdup(buf.data));
	}

	/* CONNECTION LIMIT */
	if (dbform->datconnlimit != -1)
	{
		resetStringInfo(&buf);
		appendStringInfo(&buf, "ALTER DATABASE %s CONNECTION LIMIT = %d;",
						 quote_identifier(dbname), dbform->datconnlimit);
		statements = lappend(statements, pstrdup(buf.data));
	}

	/* IS_TEMPLATE */
	if (dbform->datistemplate)
	{
		resetStringInfo(&buf);
		appendStringInfo(&buf, "ALTER DATABASE %s IS_TEMPLATE = true;",
						 quote_identifier(dbname));
		statements = lappend(statements, pstrdup(buf.data));
	}

	/* ALLOW_CONNECTIONS */
	if (!dbform->datallowconn)
	{
		resetStringInfo(&buf);
		appendStringInfo(&buf, "ALTER DATABASE %s ALLOW_CONNECTIONS = false;",
						 quote_identifier(dbname));
		statements = lappend(statements, pstrdup(buf.data));
	}

	ReleaseSysCache(tuple);

	/*
	 * Now scan pg_db_role_setting for ALTER DATABASE SET configurations.
	 *
	 * It is only database-wide (setrole = 0). It generates one ALTER
	 * statement per setting.
	 */
	rel = table_open(DbRoleSettingRelationId, AccessShareLock);
	ScanKeyInit(&scankey[0],
				Anum_pg_db_role_setting_setdatabase,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(dbid));
	ScanKeyInit(&scankey[1],
				Anum_pg_db_role_setting_setrole,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));

	scan = systable_beginscan(rel, DbRoleSettingDatidRolidIndexId, true,
							  NULL, 2, scankey);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		ArrayType  *dbconfig;
		Datum	   *settings;
		bool	   *nulls;
		int			nsettings;

		/*
		 * The setconfig column is a text array in "name=value" format. It
		 * should never be null for a valid row, but be defensive.
		 */
		datum = heap_getattr(tuple, Anum_pg_db_role_setting_setconfig,
							 RelationGetDescr(rel), &isnull);
		if (isnull)
			continue;

		dbconfig = DatumGetArrayTypePCopy(datum);

		deconstruct_array_builtin(dbconfig, TEXTOID, &settings, &nulls, &nsettings);

		for (int i = 0; i < nsettings; i++)
		{
			char	   *s,
					   *p;

			if (nulls[i])
				continue;

			s = TextDatumGetCString(settings[i]);
			p = strchr(s, '=');
			if (p == NULL)
			{
				pfree(s);
				continue;
			}
			*p++ = '\0';

			resetStringInfo(&buf);
			appendStringInfo(&buf, "ALTER DATABASE %s SET %s TO ",
							 quote_identifier(dbname),
							 quote_identifier(s));

			append_guc_value(&buf, s, p);

			appendStringInfoChar(&buf, ';');

			statements = lappend(statements, pstrdup(buf.data));

			pfree(s);
		}

		pfree(settings);
		pfree(nulls);
		pfree(dbconfig);
	}

	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	pfree(buf.data);
	pfree(dbname);

	return statements;
}

/*
 * pg_get_database_ddl
 *		Return DDL to recreate a database as a set of text rows.
 */
Datum
pg_get_database_ddl(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	List	   *statements;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		Oid			dbid;
		DdlOption	opts[] = {
			{"pretty", DDL_OPT_BOOL},
			{"owner", DDL_OPT_BOOL},
			{"tablespace", DDL_OPT_BOOL},
		};

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (PG_ARGISNULL(0))
		{
			MemoryContextSwitchTo(oldcontext);
			SRF_RETURN_DONE(funcctx);
		}

		dbid = PG_GETARG_OID(0);
		parse_ddl_options(fcinfo, 1, opts, lengthof(opts));

		statements = pg_get_database_ddl_internal(dbid,
												  opts[0].isset && opts[0].boolval,
												  opts[1].isset && !opts[1].boolval,
												  opts[2].isset && !opts[2].boolval);
		funcctx->user_fctx = statements;
		funcctx->max_calls = list_length(statements);

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	statements = (List *) funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		char	   *stmt;

		stmt = list_nth(statements, funcctx->call_cntr);

		SRF_RETURN_NEXT(funcctx, CStringGetTextDatum(stmt));
	}
	else
	{
		list_free_deep(statements);
		SRF_RETURN_DONE(funcctx);
	}
}
