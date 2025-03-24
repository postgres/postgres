/*
 * psql - the PostgreSQL interactive terminal
 *
 * Support for the various \d ("describe") commands.  Note that the current
 * expectation is that all functions in this file will succeed when working
 * with servers of versions 9.2 and up.  It's okay to omit irrelevant
 * information for an old server, but not to fail outright.  (But failing
 * against a pre-9.2 server is allowed.)
 *
 * Copyright (c) 2000-2025, PostgreSQL Global Development Group
 *
 * src/bin/psql/describe.c
 */
#include "postgres_fe.h"

#include <ctype.h>

#include "catalog/pg_am_d.h"
#include "catalog/pg_amop_d.h"
#include "catalog/pg_attribute_d.h"
#include "catalog/pg_cast_d.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_constraint_d.h"
#include "catalog/pg_default_acl_d.h"
#include "catalog/pg_proc_d.h"
#include "catalog/pg_publication_d.h"
#include "catalog/pg_statistic_ext_d.h"
#include "catalog/pg_subscription_d.h"
#include "catalog/pg_type_d.h"
#include "common.h"
#include "common/logging.h"
#include "describe.h"
#include "fe_utils/mbprint.h"
#include "fe_utils/print.h"
#include "fe_utils/string_utils.h"
#include "settings.h"

static const char *map_typename_pattern(const char *pattern);
static bool describeOneTableDetails(const char *schemaname,
									const char *relationname,
									const char *oid,
									bool verbose);
static void add_tablespace_footer(printTableContent *const cont, char relkind,
								  Oid tablespace, const bool newline);
static void add_role_attribute(PQExpBuffer buf, const char *const str);
static bool listTSParsersVerbose(const char *pattern);
static bool describeOneTSParser(const char *oid, const char *nspname,
								const char *prsname);
static bool listTSConfigsVerbose(const char *pattern);
static bool describeOneTSConfig(const char *oid, const char *nspname,
								const char *cfgname,
								const char *pnspname, const char *prsname);
static void printACLColumn(PQExpBuffer buf, const char *colname);
static bool listOneExtensionContents(const char *extname, const char *oid);
static bool validateSQLNamePattern(PQExpBuffer buf, const char *pattern,
								   bool have_where, bool force_escape,
								   const char *schemavar, const char *namevar,
								   const char *altnamevar,
								   const char *visibilityrule,
								   bool *added_clause, int maxparts);


/*----------------
 * Handlers for various slash commands displaying some sort of list
 * of things in the database.
 *
 * Note: try to format the queries to look nice in -E output.
 *----------------
 */


/*
 * \da
 * Takes an optional regexp to select particular aggregates
 */
bool
describeAggregates(const char *pattern, bool verbose, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					  "  p.proname AS \"%s\",\n"
					  "  pg_catalog.format_type(p.prorettype, NULL) AS \"%s\",\n"
					  "  CASE WHEN p.pronargs = 0\n"
					  "    THEN CAST('*' AS pg_catalog.text)\n"
					  "    ELSE pg_catalog.pg_get_function_arguments(p.oid)\n"
					  "  END AS \"%s\",\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("Result data type"),
					  gettext_noop("Argument data types"));

	if (pset.sversion >= 110000)
		appendPQExpBuffer(&buf,
						  "  pg_catalog.obj_description(p.oid, 'pg_proc') as \"%s\"\n"
						  "FROM pg_catalog.pg_proc p\n"
						  "     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace\n"
						  "WHERE p.prokind = " CppAsString2(PROKIND_AGGREGATE) "\n",
						  gettext_noop("Description"));
	else
		appendPQExpBuffer(&buf,
						  "  pg_catalog.obj_description(p.oid, 'pg_proc') as \"%s\"\n"
						  "FROM pg_catalog.pg_proc p\n"
						  "     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace\n"
						  "WHERE p.proisagg\n",
						  gettext_noop("Description"));

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"n.nspname", "p.proname", NULL,
								"pg_catalog.pg_function_is_visible(p.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2, 4;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of aggregate functions");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \dA
 * Takes an optional regexp to select particular access methods
 */
bool
describeAccessMethods(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {false, true, false, false};

	if (pset.sversion < 90600)
	{
		char		sverbuf[32];

		pg_log_error("The server (version %s) does not support access methods.",
					 formatPGVersionNumber(pset.sversion, false,
										   sverbuf, sizeof(sverbuf)));
		return true;
	}

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT amname AS \"%s\",\n"
					  "  CASE amtype"
					  " WHEN " CppAsString2(AMTYPE_INDEX) " THEN '%s'"
					  " WHEN " CppAsString2(AMTYPE_TABLE) " THEN '%s'"
					  " END AS \"%s\"",
					  gettext_noop("Name"),
					  gettext_noop("Index"),
					  gettext_noop("Table"),
					  gettext_noop("Type"));

	if (verbose)
	{
		appendPQExpBuffer(&buf,
						  ",\n  amhandler AS \"%s\",\n"
						  "  pg_catalog.obj_description(oid, 'pg_am') AS \"%s\"",
						  gettext_noop("Handler"),
						  gettext_noop("Description"));
	}

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_am\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								NULL, "amname", NULL,
								NULL,
								NULL, 1))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of access methods");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \db
 * Takes an optional regexp to select particular tablespaces
 */
bool
describeTablespaces(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT spcname AS \"%s\",\n"
					  "  pg_catalog.pg_get_userbyid(spcowner) AS \"%s\",\n"
					  "  pg_catalog.pg_tablespace_location(oid) AS \"%s\"",
					  gettext_noop("Name"),
					  gettext_noop("Owner"),
					  gettext_noop("Location"));

	if (verbose)
	{
		appendPQExpBufferStr(&buf, ",\n  ");
		printACLColumn(&buf, "spcacl");
		appendPQExpBuffer(&buf,
						  ",\n  spcoptions AS \"%s\""
						  ",\n  pg_catalog.pg_size_pretty(pg_catalog.pg_tablespace_size(oid)) AS \"%s\""
						  ",\n  pg_catalog.shobj_description(oid, 'pg_tablespace') AS \"%s\"",
						  gettext_noop("Options"),
						  gettext_noop("Size"),
						  gettext_noop("Description"));
	}

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_tablespace\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								NULL, "spcname", NULL,
								NULL,
								NULL, 1))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of tablespaces");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}


/*
 * \df
 * Takes an optional regexp to select particular functions.
 *
 * As with \d, you can specify the kinds of functions you want:
 *
 * a for aggregates
 * n for normal
 * p for procedure
 * t for trigger
 * w for window
 *
 * and you can mix and match these in any order.
 */
bool
describeFunctions(const char *functypes, const char *func_pattern,
				  char **arg_patterns, int num_arg_patterns,
				  bool verbose, bool showSystem)
{
	bool		showAggregate = strchr(functypes, 'a') != NULL;
	bool		showNormal = strchr(functypes, 'n') != NULL;
	bool		showProcedure = strchr(functypes, 'p') != NULL;
	bool		showTrigger = strchr(functypes, 't') != NULL;
	bool		showWindow = strchr(functypes, 'w') != NULL;
	bool		have_where;
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {false, false, false, false, true, true, true, false, true, true, false, false, false, false};

	/* No "Parallel" column before 9.6 */
	static const bool translate_columns_pre_96[] = {false, false, false, false, true, true, false, true, true, false, false, false, false};

	if (strlen(functypes) != strspn(functypes, "anptwSx+"))
	{
		pg_log_error("\\df only takes [anptwSx+] as options");
		return true;
	}

	if (showProcedure && pset.sversion < 110000)
	{
		char		sverbuf[32];

		pg_log_error("\\df does not take a \"%c\" option with server version %s",
					 'p',
					 formatPGVersionNumber(pset.sversion, false,
										   sverbuf, sizeof(sverbuf)));
		return true;
	}

	if (!showAggregate && !showNormal && !showProcedure && !showTrigger && !showWindow)
	{
		showAggregate = showNormal = showTrigger = showWindow = true;
		if (pset.sversion >= 110000)
			showProcedure = true;
	}

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					  "  p.proname as \"%s\",\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"));

	if (pset.sversion >= 110000)
		appendPQExpBuffer(&buf,
						  "  pg_catalog.pg_get_function_result(p.oid) as \"%s\",\n"
						  "  pg_catalog.pg_get_function_arguments(p.oid) as \"%s\",\n"
						  " CASE p.prokind\n"
						  "  WHEN " CppAsString2(PROKIND_AGGREGATE) " THEN '%s'\n"
						  "  WHEN " CppAsString2(PROKIND_WINDOW) " THEN '%s'\n"
						  "  WHEN " CppAsString2(PROKIND_PROCEDURE) " THEN '%s'\n"
						  "  ELSE '%s'\n"
						  " END as \"%s\"",
						  gettext_noop("Result data type"),
						  gettext_noop("Argument data types"),
		/* translator: "agg" is short for "aggregate" */
						  gettext_noop("agg"),
						  gettext_noop("window"),
						  gettext_noop("proc"),
						  gettext_noop("func"),
						  gettext_noop("Type"));
	else
		appendPQExpBuffer(&buf,
						  "  pg_catalog.pg_get_function_result(p.oid) as \"%s\",\n"
						  "  pg_catalog.pg_get_function_arguments(p.oid) as \"%s\",\n"
						  " CASE\n"
						  "  WHEN p.proisagg THEN '%s'\n"
						  "  WHEN p.proiswindow THEN '%s'\n"
						  "  WHEN p.prorettype = 'pg_catalog.trigger'::pg_catalog.regtype THEN '%s'\n"
						  "  ELSE '%s'\n"
						  " END as \"%s\"",
						  gettext_noop("Result data type"),
						  gettext_noop("Argument data types"),
		/* translator: "agg" is short for "aggregate" */
						  gettext_noop("agg"),
						  gettext_noop("window"),
						  gettext_noop("trigger"),
						  gettext_noop("func"),
						  gettext_noop("Type"));

	if (verbose)
	{
		appendPQExpBuffer(&buf,
						  ",\n CASE\n"
						  "  WHEN p.provolatile = "
						  CppAsString2(PROVOLATILE_IMMUTABLE) " THEN '%s'\n"
						  "  WHEN p.provolatile = "
						  CppAsString2(PROVOLATILE_STABLE) " THEN '%s'\n"
						  "  WHEN p.provolatile = "
						  CppAsString2(PROVOLATILE_VOLATILE) " THEN '%s'\n"
						  " END as \"%s\"",
						  gettext_noop("immutable"),
						  gettext_noop("stable"),
						  gettext_noop("volatile"),
						  gettext_noop("Volatility"));
		if (pset.sversion >= 90600)
			appendPQExpBuffer(&buf,
							  ",\n CASE\n"
							  "  WHEN p.proparallel = "
							  CppAsString2(PROPARALLEL_RESTRICTED) " THEN '%s'\n"
							  "  WHEN p.proparallel = "
							  CppAsString2(PROPARALLEL_SAFE) " THEN '%s'\n"
							  "  WHEN p.proparallel = "
							  CppAsString2(PROPARALLEL_UNSAFE) " THEN '%s'\n"
							  " END as \"%s\"",
							  gettext_noop("restricted"),
							  gettext_noop("safe"),
							  gettext_noop("unsafe"),
							  gettext_noop("Parallel"));
		appendPQExpBuffer(&buf,
						  ",\n pg_catalog.pg_get_userbyid(p.proowner) as \"%s\""
						  ",\n CASE WHEN prosecdef THEN '%s' ELSE '%s' END AS \"%s\""
						  ",\n CASE WHEN p.proleakproof THEN '%s' ELSE '%s' END as \"%s\"",
						  gettext_noop("Owner"),
						  gettext_noop("definer"),
						  gettext_noop("invoker"),
						  gettext_noop("Security"),
						  gettext_noop("yes"),
						  gettext_noop("no"),
						  gettext_noop("Leakproof?"));
		appendPQExpBufferStr(&buf, ",\n ");
		printACLColumn(&buf, "p.proacl");
		appendPQExpBuffer(&buf,
						  ",\n l.lanname as \"%s\"",
						  gettext_noop("Language"));
		appendPQExpBuffer(&buf,
						  ",\n CASE WHEN l.lanname IN ('internal', 'c') THEN p.prosrc END as \"%s\"",
						  gettext_noop("Internal name"));
		appendPQExpBuffer(&buf,
						  ",\n pg_catalog.obj_description(p.oid, 'pg_proc') as \"%s\"",
						  gettext_noop("Description"));
	}

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_proc p"
						 "\n     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace\n");

	for (int i = 0; i < num_arg_patterns; i++)
	{
		appendPQExpBuffer(&buf,
						  "     LEFT JOIN pg_catalog.pg_type t%d ON t%d.oid = p.proargtypes[%d]\n"
						  "     LEFT JOIN pg_catalog.pg_namespace nt%d ON nt%d.oid = t%d.typnamespace\n",
						  i, i, i, i, i, i);
	}

	if (verbose)
		appendPQExpBufferStr(&buf,
							 "     LEFT JOIN pg_catalog.pg_language l ON l.oid = p.prolang\n");

	have_where = false;

	/* filter by function type, if requested */
	if (showNormal && showAggregate && showProcedure && showTrigger && showWindow)
		 /* Do nothing */ ;
	else if (showNormal)
	{
		if (!showAggregate)
		{
			if (have_where)
				appendPQExpBufferStr(&buf, "      AND ");
			else
			{
				appendPQExpBufferStr(&buf, "WHERE ");
				have_where = true;
			}
			if (pset.sversion >= 110000)
				appendPQExpBufferStr(&buf, "p.prokind <> "
									 CppAsString2(PROKIND_AGGREGATE) "\n");
			else
				appendPQExpBufferStr(&buf, "NOT p.proisagg\n");
		}
		if (!showProcedure && pset.sversion >= 110000)
		{
			if (have_where)
				appendPQExpBufferStr(&buf, "      AND ");
			else
			{
				appendPQExpBufferStr(&buf, "WHERE ");
				have_where = true;
			}
			appendPQExpBufferStr(&buf, "p.prokind <> "
								 CppAsString2(PROKIND_PROCEDURE) "\n");
		}
		if (!showTrigger)
		{
			if (have_where)
				appendPQExpBufferStr(&buf, "      AND ");
			else
			{
				appendPQExpBufferStr(&buf, "WHERE ");
				have_where = true;
			}
			appendPQExpBufferStr(&buf, "p.prorettype <> 'pg_catalog.trigger'::pg_catalog.regtype\n");
		}
		if (!showWindow)
		{
			if (have_where)
				appendPQExpBufferStr(&buf, "      AND ");
			else
			{
				appendPQExpBufferStr(&buf, "WHERE ");
				have_where = true;
			}
			if (pset.sversion >= 110000)
				appendPQExpBufferStr(&buf, "p.prokind <> "
									 CppAsString2(PROKIND_WINDOW) "\n");
			else
				appendPQExpBufferStr(&buf, "NOT p.proiswindow\n");
		}
	}
	else
	{
		bool		needs_or = false;

		appendPQExpBufferStr(&buf, "WHERE (\n       ");
		have_where = true;
		/* Note: at least one of these must be true ... */
		if (showAggregate)
		{
			if (pset.sversion >= 110000)
				appendPQExpBufferStr(&buf, "p.prokind = "
									 CppAsString2(PROKIND_AGGREGATE) "\n");
			else
				appendPQExpBufferStr(&buf, "p.proisagg\n");
			needs_or = true;
		}
		if (showTrigger)
		{
			if (needs_or)
				appendPQExpBufferStr(&buf, "       OR ");
			appendPQExpBufferStr(&buf,
								 "p.prorettype = 'pg_catalog.trigger'::pg_catalog.regtype\n");
			needs_or = true;
		}
		if (showProcedure)
		{
			if (needs_or)
				appendPQExpBufferStr(&buf, "       OR ");
			appendPQExpBufferStr(&buf, "p.prokind = "
								 CppAsString2(PROKIND_PROCEDURE) "\n");
			needs_or = true;
		}
		if (showWindow)
		{
			if (needs_or)
				appendPQExpBufferStr(&buf, "       OR ");
			if (pset.sversion >= 110000)
				appendPQExpBufferStr(&buf, "p.prokind = "
									 CppAsString2(PROKIND_WINDOW) "\n");
			else
				appendPQExpBufferStr(&buf, "p.proiswindow\n");
		}
		appendPQExpBufferStr(&buf, "      )\n");
	}

	if (!validateSQLNamePattern(&buf, func_pattern, have_where, false,
								"n.nspname", "p.proname", NULL,
								"pg_catalog.pg_function_is_visible(p.oid)",
								NULL, 3))
		goto error_return;

	for (int i = 0; i < num_arg_patterns; i++)
	{
		if (strcmp(arg_patterns[i], "-") != 0)
		{
			/*
			 * Match type-name patterns against either internal or external
			 * name, like \dT.  Unlike \dT, there seems no reason to
			 * discriminate against arrays or composite types.
			 */
			char		nspname[64];
			char		typname[64];
			char		ft[64];
			char		tiv[64];

			snprintf(nspname, sizeof(nspname), "nt%d.nspname", i);
			snprintf(typname, sizeof(typname), "t%d.typname", i);
			snprintf(ft, sizeof(ft),
					 "pg_catalog.format_type(t%d.oid, NULL)", i);
			snprintf(tiv, sizeof(tiv),
					 "pg_catalog.pg_type_is_visible(t%d.oid)", i);
			if (!validateSQLNamePattern(&buf,
										map_typename_pattern(arg_patterns[i]),
										true, false,
										nspname, typname, ft, tiv,
										NULL, 3))
				goto error_return;
		}
		else
		{
			/* "-" pattern specifies no such parameter */
			appendPQExpBuffer(&buf, "  AND t%d.typname IS NULL\n", i);
		}
	}

	if (!showSystem && !func_pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2, 4;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of functions");
	myopt.translate_header = true;
	if (pset.sversion >= 90600)
	{
		myopt.translate_columns = translate_columns;
		myopt.n_translate_columns = lengthof(translate_columns);
	}
	else
	{
		myopt.translate_columns = translate_columns_pre_96;
		myopt.n_translate_columns = lengthof(translate_columns_pre_96);
	}

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;

error_return:
	termPQExpBuffer(&buf);
	return false;
}



/*
 * \dT
 * describe types
 */
bool
describeTypes(const char *pattern, bool verbose, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					  "  pg_catalog.format_type(t.oid, NULL) AS \"%s\",\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"));
	if (verbose)
	{
		appendPQExpBuffer(&buf,
						  "  t.typname AS \"%s\",\n"
						  "  CASE WHEN t.typrelid != 0\n"
						  "      THEN CAST('tuple' AS pg_catalog.text)\n"
						  "    WHEN t.typlen < 0\n"
						  "      THEN CAST('var' AS pg_catalog.text)\n"
						  "    ELSE CAST(t.typlen AS pg_catalog.text)\n"
						  "  END AS \"%s\",\n"
						  "  pg_catalog.array_to_string(\n"
						  "      ARRAY(\n"
						  "          SELECT e.enumlabel\n"
						  "          FROM pg_catalog.pg_enum e\n"
						  "          WHERE e.enumtypid = t.oid\n"
						  "          ORDER BY e.enumsortorder\n"
						  "      ),\n"
						  "      E'\\n'\n"
						  "  ) AS \"%s\",\n"
						  "  pg_catalog.pg_get_userbyid(t.typowner) AS \"%s\",\n",
						  gettext_noop("Internal name"),
						  gettext_noop("Size"),
						  gettext_noop("Elements"),
						  gettext_noop("Owner"));
		printACLColumn(&buf, "t.typacl");
		appendPQExpBufferStr(&buf, ",\n  ");
	}

	appendPQExpBuffer(&buf,
					  "  pg_catalog.obj_description(t.oid, 'pg_type') as \"%s\"\n",
					  gettext_noop("Description"));

	appendPQExpBufferStr(&buf, "FROM pg_catalog.pg_type t\n"
						 "     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = t.typnamespace\n");

	/*
	 * do not include complex types (typrelid!=0) unless they are standalone
	 * composite types
	 */
	appendPQExpBufferStr(&buf, "WHERE (t.typrelid = 0 ");
	appendPQExpBufferStr(&buf, "OR (SELECT c.relkind = " CppAsString2(RELKIND_COMPOSITE_TYPE)
						 " FROM pg_catalog.pg_class c "
						 "WHERE c.oid = t.typrelid))\n");

	/*
	 * do not include array types unless the pattern contains []
	 */
	if (pattern == NULL || strstr(pattern, "[]") == NULL)
		appendPQExpBufferStr(&buf, "  AND NOT EXISTS(SELECT 1 FROM pg_catalog.pg_type el WHERE el.oid = t.typelem AND el.typarray = t.oid)\n");

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	/* Match name pattern against either internal or external name */
	if (!validateSQLNamePattern(&buf, map_typename_pattern(pattern),
								true, false,
								"n.nspname", "t.typname",
								"pg_catalog.format_type(t.oid, NULL)",
								"pg_catalog.pg_type_is_visible(t.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of data types");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * Map some variant type names accepted by the backend grammar into
 * canonical type names.
 *
 * Helper for \dT and other functions that take typename patterns.
 * This doesn't completely mask the fact that these names are special;
 * for example, a pattern of "dec*" won't magically match "numeric".
 * But it goes a long way to reduce the surprise factor.
 */
static const char *
map_typename_pattern(const char *pattern)
{
	static const char *const typename_map[] = {
		/*
		 * These names are accepted by gram.y, although they are neither the
		 * "real" name seen in pg_type nor the canonical name printed by
		 * format_type().
		 */
		"decimal", "numeric",
		"float", "double precision",
		"int", "integer",

		/*
		 * We also have to map the array names for cases where the canonical
		 * name is different from what pg_type says.
		 */
		"bool[]", "boolean[]",
		"decimal[]", "numeric[]",
		"float[]", "double precision[]",
		"float4[]", "real[]",
		"float8[]", "double precision[]",
		"int[]", "integer[]",
		"int2[]", "smallint[]",
		"int4[]", "integer[]",
		"int8[]", "bigint[]",
		"time[]", "time without time zone[]",
		"timetz[]", "time with time zone[]",
		"timestamp[]", "timestamp without time zone[]",
		"timestamptz[]", "timestamp with time zone[]",
		"varbit[]", "bit varying[]",
		"varchar[]", "character varying[]",
		NULL
	};

	if (pattern == NULL)
		return NULL;
	for (int i = 0; typename_map[i] != NULL; i += 2)
	{
		if (pg_strcasecmp(pattern, typename_map[i]) == 0)
			return typename_map[i + 1];
	}
	return pattern;
}


/*
 * \do
 * Describe operators
 */
bool
describeOperators(const char *oper_pattern,
				  char **arg_patterns, int num_arg_patterns,
				  bool verbose, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {false, false, false, false, false, false, true, false};

	initPQExpBuffer(&buf);

	/*
	 * Note: before Postgres 9.1, we did not assign comments to any built-in
	 * operators, preferring to let the comment on the underlying function
	 * suffice.  The coalesce() on the obj_description() calls below supports
	 * this convention by providing a fallback lookup of a comment on the
	 * operator's function.  Since 9.1 there is a policy that every built-in
	 * operator should have a comment; so the coalesce() is no longer
	 * necessary so far as built-in operators are concerned.  We keep it
	 * anyway, for now, because third-party modules may still be following the
	 * old convention.
	 *
	 * The support for postfix operators in this query is dead code as of
	 * Postgres 14, but we need to keep it for as long as we support talking
	 * to pre-v14 servers.
	 */

	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					  "  o.oprname AS \"%s\",\n"
					  "  CASE WHEN o.oprkind='l' THEN NULL ELSE pg_catalog.format_type(o.oprleft, NULL) END AS \"%s\",\n"
					  "  CASE WHEN o.oprkind='r' THEN NULL ELSE pg_catalog.format_type(o.oprright, NULL) END AS \"%s\",\n"
					  "  pg_catalog.format_type(o.oprresult, NULL) AS \"%s\",\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("Left arg type"),
					  gettext_noop("Right arg type"),
					  gettext_noop("Result type"));

	if (verbose)
		appendPQExpBuffer(&buf,
						  "  o.oprcode AS \"%s\",\n"
						  "  CASE WHEN p.proleakproof THEN '%s' ELSE '%s' END AS \"%s\",\n",
						  gettext_noop("Function"),
						  gettext_noop("yes"),
						  gettext_noop("no"),
						  gettext_noop("Leakproof?"));

	appendPQExpBuffer(&buf,
					  "  coalesce(pg_catalog.obj_description(o.oid, 'pg_operator'),\n"
					  "           pg_catalog.obj_description(o.oprcode, 'pg_proc')) AS \"%s\"\n"
					  "FROM pg_catalog.pg_operator o\n"
					  "     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = o.oprnamespace\n",
					  gettext_noop("Description"));

	if (num_arg_patterns >= 2)
	{
		num_arg_patterns = 2;	/* ignore any additional arguments */
		appendPQExpBufferStr(&buf,
							 "     LEFT JOIN pg_catalog.pg_type t0 ON t0.oid = o.oprleft\n"
							 "     LEFT JOIN pg_catalog.pg_namespace nt0 ON nt0.oid = t0.typnamespace\n"
							 "     LEFT JOIN pg_catalog.pg_type t1 ON t1.oid = o.oprright\n"
							 "     LEFT JOIN pg_catalog.pg_namespace nt1 ON nt1.oid = t1.typnamespace\n");
	}
	else if (num_arg_patterns == 1)
	{
		appendPQExpBufferStr(&buf,
							 "     LEFT JOIN pg_catalog.pg_type t0 ON t0.oid = o.oprright\n"
							 "     LEFT JOIN pg_catalog.pg_namespace nt0 ON nt0.oid = t0.typnamespace\n");
	}

	if (verbose)
		appendPQExpBufferStr(&buf,
							 "     LEFT JOIN pg_catalog.pg_proc p ON p.oid = o.oprcode\n");

	if (!showSystem && !oper_pattern)
		appendPQExpBufferStr(&buf, "WHERE n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, oper_pattern,
								!showSystem && !oper_pattern, true,
								"n.nspname", "o.oprname", NULL,
								"pg_catalog.pg_operator_is_visible(o.oid)",
								NULL, 3))
		goto error_return;

	if (num_arg_patterns == 1)
		appendPQExpBufferStr(&buf, "  AND o.oprleft = 0\n");

	for (int i = 0; i < num_arg_patterns; i++)
	{
		if (strcmp(arg_patterns[i], "-") != 0)
		{
			/*
			 * Match type-name patterns against either internal or external
			 * name, like \dT.  Unlike \dT, there seems no reason to
			 * discriminate against arrays or composite types.
			 */
			char		nspname[64];
			char		typname[64];
			char		ft[64];
			char		tiv[64];

			snprintf(nspname, sizeof(nspname), "nt%d.nspname", i);
			snprintf(typname, sizeof(typname), "t%d.typname", i);
			snprintf(ft, sizeof(ft),
					 "pg_catalog.format_type(t%d.oid, NULL)", i);
			snprintf(tiv, sizeof(tiv),
					 "pg_catalog.pg_type_is_visible(t%d.oid)", i);
			if (!validateSQLNamePattern(&buf,
										map_typename_pattern(arg_patterns[i]),
										true, false,
										nspname, typname, ft, tiv,
										NULL, 3))
				goto error_return;
		}
		else
		{
			/* "-" pattern specifies no such parameter */
			appendPQExpBuffer(&buf, "  AND t%d.typname IS NULL\n", i);
		}
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2, 3, 4;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of operators");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;

error_return:
	termPQExpBuffer(&buf);
	return false;
}


/*
 * listAllDbs
 *
 * for \l, \list, and -l switch
 */
bool
listAllDbs(const char *pattern, bool verbose)
{
	PGresult   *res;
	PQExpBufferData buf;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT\n"
					  "  d.datname as \"%s\",\n"
					  "  pg_catalog.pg_get_userbyid(d.datdba) as \"%s\",\n"
					  "  pg_catalog.pg_encoding_to_char(d.encoding) as \"%s\",\n",
					  gettext_noop("Name"),
					  gettext_noop("Owner"),
					  gettext_noop("Encoding"));
	if (pset.sversion >= 150000)
		appendPQExpBuffer(&buf,
						  "  CASE d.datlocprovider "
						  "WHEN " CppAsString2(COLLPROVIDER_BUILTIN) " THEN 'builtin' "
						  "WHEN " CppAsString2(COLLPROVIDER_LIBC) " THEN 'libc' "
						  "WHEN " CppAsString2(COLLPROVIDER_ICU) " THEN 'icu' "
						  "END AS \"%s\",\n",
						  gettext_noop("Locale Provider"));
	else
		appendPQExpBuffer(&buf,
						  "  'libc' AS \"%s\",\n",
						  gettext_noop("Locale Provider"));
	appendPQExpBuffer(&buf,
					  "  d.datcollate as \"%s\",\n"
					  "  d.datctype as \"%s\",\n",
					  gettext_noop("Collate"),
					  gettext_noop("Ctype"));
	if (pset.sversion >= 170000)
		appendPQExpBuffer(&buf,
						  "  d.datlocale as \"%s\",\n",
						  gettext_noop("Locale"));
	else if (pset.sversion >= 150000)
		appendPQExpBuffer(&buf,
						  "  d.daticulocale as \"%s\",\n",
						  gettext_noop("Locale"));
	else
		appendPQExpBuffer(&buf,
						  "  NULL as \"%s\",\n",
						  gettext_noop("Locale"));
	if (pset.sversion >= 160000)
		appendPQExpBuffer(&buf,
						  "  d.daticurules as \"%s\",\n",
						  gettext_noop("ICU Rules"));
	else
		appendPQExpBuffer(&buf,
						  "  NULL as \"%s\",\n",
						  gettext_noop("ICU Rules"));
	appendPQExpBufferStr(&buf, "  ");
	printACLColumn(&buf, "d.datacl");
	if (verbose)
		appendPQExpBuffer(&buf,
						  ",\n  CASE WHEN pg_catalog.has_database_privilege(d.datname, 'CONNECT')\n"
						  "       THEN pg_catalog.pg_size_pretty(pg_catalog.pg_database_size(d.datname))\n"
						  "       ELSE 'No Access'\n"
						  "  END as \"%s\""
						  ",\n  t.spcname as \"%s\""
						  ",\n  pg_catalog.shobj_description(d.oid, 'pg_database') as \"%s\"",
						  gettext_noop("Size"),
						  gettext_noop("Tablespace"),
						  gettext_noop("Description"));
	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_database d\n");
	if (verbose)
		appendPQExpBufferStr(&buf,
							 "  JOIN pg_catalog.pg_tablespace t on d.dattablespace = t.oid\n");

	if (pattern)
	{
		if (!validateSQLNamePattern(&buf, pattern, false, false,
									NULL, "d.datname", NULL, NULL,
									NULL, 1))
		{
			termPQExpBuffer(&buf);
			return false;
		}
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1;");
	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of databases");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}


/*
 * List Tables' Grant/Revoke Permissions
 * \z (now also \dp -- perhaps more mnemonic)
 */
bool
permissionsList(const char *pattern, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {false, false, true, false, false, false};

	initPQExpBuffer(&buf);

	/*
	 * we ignore indexes and toast tables since they have no meaningful rights
	 */
	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					  "  c.relname as \"%s\",\n"
					  "  CASE c.relkind"
					  " WHEN " CppAsString2(RELKIND_RELATION) " THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_VIEW) " THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_MATVIEW) " THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_SEQUENCE) " THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_FOREIGN_TABLE) " THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_PARTITIONED_TABLE) " THEN '%s'"
					  " END as \"%s\",\n"
					  "  ",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("table"),
					  gettext_noop("view"),
					  gettext_noop("materialized view"),
					  gettext_noop("sequence"),
					  gettext_noop("foreign table"),
					  gettext_noop("partitioned table"),
					  gettext_noop("Type"));

	printACLColumn(&buf, "c.relacl");

	/*
	 * The formatting of attacl should match printACLColumn().  However, we
	 * need no special case for an empty attacl, because the backend always
	 * optimizes that back to NULL.
	 */
	appendPQExpBuffer(&buf,
					  ",\n  pg_catalog.array_to_string(ARRAY(\n"
					  "    SELECT attname || E':\\n  ' || pg_catalog.array_to_string(attacl, E'\\n  ')\n"
					  "    FROM pg_catalog.pg_attribute a\n"
					  "    WHERE attrelid = c.oid AND NOT attisdropped AND attacl IS NOT NULL\n"
					  "  ), E'\\n') AS \"%s\"",
					  gettext_noop("Column privileges"));

	if (pset.sversion >= 90500 && pset.sversion < 100000)
		appendPQExpBuffer(&buf,
						  ",\n  pg_catalog.array_to_string(ARRAY(\n"
						  "    SELECT polname\n"
						  "    || CASE WHEN polcmd != '*' THEN\n"
						  "           E' (' || polcmd::pg_catalog.text || E'):'\n"
						  "       ELSE E':'\n"
						  "       END\n"
						  "    || CASE WHEN polqual IS NOT NULL THEN\n"
						  "           E'\\n  (u): ' || pg_catalog.pg_get_expr(polqual, polrelid)\n"
						  "       ELSE E''\n"
						  "       END\n"
						  "    || CASE WHEN polwithcheck IS NOT NULL THEN\n"
						  "           E'\\n  (c): ' || pg_catalog.pg_get_expr(polwithcheck, polrelid)\n"
						  "       ELSE E''\n"
						  "       END"
						  "    || CASE WHEN polroles <> '{0}' THEN\n"
						  "           E'\\n  to: ' || pg_catalog.array_to_string(\n"
						  "               ARRAY(\n"
						  "                   SELECT rolname\n"
						  "                   FROM pg_catalog.pg_roles\n"
						  "                   WHERE oid = ANY (polroles)\n"
						  "                   ORDER BY 1\n"
						  "               ), E', ')\n"
						  "       ELSE E''\n"
						  "       END\n"
						  "    FROM pg_catalog.pg_policy pol\n"
						  "    WHERE polrelid = c.oid), E'\\n')\n"
						  "    AS \"%s\"",
						  gettext_noop("Policies"));

	if (pset.sversion >= 100000)
		appendPQExpBuffer(&buf,
						  ",\n  pg_catalog.array_to_string(ARRAY(\n"
						  "    SELECT polname\n"
						  "    || CASE WHEN NOT polpermissive THEN\n"
						  "       E' (RESTRICTIVE)'\n"
						  "       ELSE '' END\n"
						  "    || CASE WHEN polcmd != '*' THEN\n"
						  "           E' (' || polcmd::pg_catalog.text || E'):'\n"
						  "       ELSE E':'\n"
						  "       END\n"
						  "    || CASE WHEN polqual IS NOT NULL THEN\n"
						  "           E'\\n  (u): ' || pg_catalog.pg_get_expr(polqual, polrelid)\n"
						  "       ELSE E''\n"
						  "       END\n"
						  "    || CASE WHEN polwithcheck IS NOT NULL THEN\n"
						  "           E'\\n  (c): ' || pg_catalog.pg_get_expr(polwithcheck, polrelid)\n"
						  "       ELSE E''\n"
						  "       END"
						  "    || CASE WHEN polroles <> '{0}' THEN\n"
						  "           E'\\n  to: ' || pg_catalog.array_to_string(\n"
						  "               ARRAY(\n"
						  "                   SELECT rolname\n"
						  "                   FROM pg_catalog.pg_roles\n"
						  "                   WHERE oid = ANY (polroles)\n"
						  "                   ORDER BY 1\n"
						  "               ), E', ')\n"
						  "       ELSE E''\n"
						  "       END\n"
						  "    FROM pg_catalog.pg_policy pol\n"
						  "    WHERE polrelid = c.oid), E'\\n')\n"
						  "    AS \"%s\"",
						  gettext_noop("Policies"));

	appendPQExpBufferStr(&buf, "\nFROM pg_catalog.pg_class c\n"
						 "     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace\n"
						 "WHERE c.relkind IN ("
						 CppAsString2(RELKIND_RELATION) ","
						 CppAsString2(RELKIND_VIEW) ","
						 CppAsString2(RELKIND_MATVIEW) ","
						 CppAsString2(RELKIND_SEQUENCE) ","
						 CppAsString2(RELKIND_FOREIGN_TABLE) ","
						 CppAsString2(RELKIND_PARTITIONED_TABLE) ")\n");

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"n.nspname", "c.relname", NULL,
								"pg_catalog.pg_table_is_visible(c.oid)",
								NULL, 3))
		goto error_return;

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	if (!res)
		goto error_return;

	printfPQExpBuffer(&buf, _("Access privileges"));
	myopt.title = buf.data;
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	termPQExpBuffer(&buf);
	PQclear(res);
	return true;

error_return:
	termPQExpBuffer(&buf);
	return false;
}


/*
 * \ddp
 *
 * List Default ACLs.  The pattern can match either schema or role name.
 */
bool
listDefaultACLs(const char *pattern)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {false, false, true, false};

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT pg_catalog.pg_get_userbyid(d.defaclrole) AS \"%s\",\n"
					  "  n.nspname AS \"%s\",\n"
					  "  CASE d.defaclobjtype WHEN '%c' THEN '%s' WHEN '%c' THEN '%s' WHEN '%c' THEN '%s' WHEN '%c' THEN '%s' WHEN '%c' THEN '%s' END AS \"%s\",\n"
					  "  ",
					  gettext_noop("Owner"),
					  gettext_noop("Schema"),
					  DEFACLOBJ_RELATION,
					  gettext_noop("table"),
					  DEFACLOBJ_SEQUENCE,
					  gettext_noop("sequence"),
					  DEFACLOBJ_FUNCTION,
					  gettext_noop("function"),
					  DEFACLOBJ_TYPE,
					  gettext_noop("type"),
					  DEFACLOBJ_NAMESPACE,
					  gettext_noop("schema"),
					  gettext_noop("Type"));

	printACLColumn(&buf, "d.defaclacl");

	appendPQExpBufferStr(&buf, "\nFROM pg_catalog.pg_default_acl d\n"
						 "     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = d.defaclnamespace\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								NULL,
								"n.nspname",
								"pg_catalog.pg_get_userbyid(d.defaclrole)",
								NULL,
								NULL, 3))
		goto error_return;

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2, 3;");

	res = PSQLexec(buf.data);
	if (!res)
		goto error_return;

	printfPQExpBuffer(&buf, _("Default access privileges"));
	myopt.title = buf.data;
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	termPQExpBuffer(&buf);
	PQclear(res);
	return true;

error_return:
	termPQExpBuffer(&buf);
	return false;
}


/*
 * Get object comments
 *
 * \dd [foo]
 *
 * Note: This command only lists comments for object types which do not have
 * their comments displayed by their own backslash commands. The following
 * types of objects will be displayed: constraint, operator class,
 * operator family, rule, and trigger.
 *
 */
bool
objectDescription(const char *pattern, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {false, false, true, false};

	initPQExpBuffer(&buf);

	appendPQExpBuffer(&buf,
					  "SELECT DISTINCT tt.nspname AS \"%s\", tt.name AS \"%s\", tt.object AS \"%s\", d.description AS \"%s\"\n"
					  "FROM (\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("Object"),
					  gettext_noop("Description"));

	/* Table constraint descriptions */
	appendPQExpBuffer(&buf,
					  "  SELECT pgc.oid as oid, pgc.tableoid AS tableoid,\n"
					  "  n.nspname as nspname,\n"
					  "  CAST(pgc.conname AS pg_catalog.text) as name,"
					  "  CAST('%s' AS pg_catalog.text) as object\n"
					  "  FROM pg_catalog.pg_constraint pgc\n"
					  "    JOIN pg_catalog.pg_class c "
					  "ON c.oid = pgc.conrelid\n"
					  "    LEFT JOIN pg_catalog.pg_namespace n "
					  "    ON n.oid = c.relnamespace\n",
					  gettext_noop("table constraint"));

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "WHERE n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, !showSystem && !pattern,
								false, "n.nspname", "pgc.conname", NULL,
								"pg_catalog.pg_table_is_visible(c.oid)",
								NULL, 3))
		goto error_return;

	/* Domain constraint descriptions */
	appendPQExpBuffer(&buf,
					  "UNION ALL\n"
					  "  SELECT pgc.oid as oid, pgc.tableoid AS tableoid,\n"
					  "  n.nspname as nspname,\n"
					  "  CAST(pgc.conname AS pg_catalog.text) as name,"
					  "  CAST('%s' AS pg_catalog.text) as object\n"
					  "  FROM pg_catalog.pg_constraint pgc\n"
					  "    JOIN pg_catalog.pg_type t "
					  "ON t.oid = pgc.contypid\n"
					  "    LEFT JOIN pg_catalog.pg_namespace n "
					  "    ON n.oid = t.typnamespace\n",
					  gettext_noop("domain constraint"));

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "WHERE n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, !showSystem && !pattern,
								false, "n.nspname", "pgc.conname", NULL,
								"pg_catalog.pg_type_is_visible(t.oid)",
								NULL, 3))
		goto error_return;

	/* Operator class descriptions */
	appendPQExpBuffer(&buf,
					  "UNION ALL\n"
					  "  SELECT o.oid as oid, o.tableoid as tableoid,\n"
					  "  n.nspname as nspname,\n"
					  "  CAST(o.opcname AS pg_catalog.text) as name,\n"
					  "  CAST('%s' AS pg_catalog.text) as object\n"
					  "  FROM pg_catalog.pg_opclass o\n"
					  "    JOIN pg_catalog.pg_am am ON "
					  "o.opcmethod = am.oid\n"
					  "    JOIN pg_catalog.pg_namespace n ON "
					  "n.oid = o.opcnamespace\n",
					  gettext_noop("operator class"));

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"n.nspname", "o.opcname", NULL,
								"pg_catalog.pg_opclass_is_visible(o.oid)",
								NULL, 3))
		goto error_return;

	/* Operator family descriptions */
	appendPQExpBuffer(&buf,
					  "UNION ALL\n"
					  "  SELECT opf.oid as oid, opf.tableoid as tableoid,\n"
					  "  n.nspname as nspname,\n"
					  "  CAST(opf.opfname AS pg_catalog.text) AS name,\n"
					  "  CAST('%s' AS pg_catalog.text) as object\n"
					  "  FROM pg_catalog.pg_opfamily opf\n"
					  "    JOIN pg_catalog.pg_am am "
					  "ON opf.opfmethod = am.oid\n"
					  "    JOIN pg_catalog.pg_namespace n "
					  "ON opf.opfnamespace = n.oid\n",
					  gettext_noop("operator family"));

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"n.nspname", "opf.opfname", NULL,
								"pg_catalog.pg_opfamily_is_visible(opf.oid)",
								NULL, 3))
		goto error_return;

	/* Rule descriptions (ignore rules for views) */
	appendPQExpBuffer(&buf,
					  "UNION ALL\n"
					  "  SELECT r.oid as oid, r.tableoid as tableoid,\n"
					  "  n.nspname as nspname,\n"
					  "  CAST(r.rulename AS pg_catalog.text) as name,"
					  "  CAST('%s' AS pg_catalog.text) as object\n"
					  "  FROM pg_catalog.pg_rewrite r\n"
					  "       JOIN pg_catalog.pg_class c ON c.oid = r.ev_class\n"
					  "       LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace\n"
					  "  WHERE r.rulename != '_RETURN'\n",
					  gettext_noop("rule"));

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"n.nspname", "r.rulename", NULL,
								"pg_catalog.pg_table_is_visible(c.oid)",
								NULL, 3))
		goto error_return;

	/* Trigger descriptions */
	appendPQExpBuffer(&buf,
					  "UNION ALL\n"
					  "  SELECT t.oid as oid, t.tableoid as tableoid,\n"
					  "  n.nspname as nspname,\n"
					  "  CAST(t.tgname AS pg_catalog.text) as name,"
					  "  CAST('%s' AS pg_catalog.text) as object\n"
					  "  FROM pg_catalog.pg_trigger t\n"
					  "       JOIN pg_catalog.pg_class c ON c.oid = t.tgrelid\n"
					  "       LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace\n",
					  gettext_noop("trigger"));

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "WHERE n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, !showSystem && !pattern, false,
								"n.nspname", "t.tgname", NULL,
								"pg_catalog.pg_table_is_visible(c.oid)",
								NULL, 3))
		goto error_return;

	appendPQExpBufferStr(&buf,
						 ") AS tt\n"
						 "  JOIN pg_catalog.pg_description d ON (tt.oid = d.objoid AND tt.tableoid = d.classoid AND d.objsubid = 0)\n");

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2, 3;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("Object descriptions");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;

error_return:
	termPQExpBuffer(&buf);
	return false;
}


/*
 * describeTableDetails (for \d)
 *
 * This routine finds the tables to be displayed, and calls
 * describeOneTableDetails for each one.
 *
 * verbose: if true, this is \d+
 */
bool
describeTableDetails(const char *pattern, bool verbose, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	int			i;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT c.oid,\n"
					  "  n.nspname,\n"
					  "  c.relname\n"
					  "FROM pg_catalog.pg_class c\n"
					  "     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace\n");

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "WHERE n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, !showSystem && !pattern, false,
								"n.nspname", "c.relname", NULL,
								"pg_catalog.pg_table_is_visible(c.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 2, 3;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	if (PQntuples(res) == 0)
	{
		if (!pset.quiet)
		{
			if (pattern)
				pg_log_error("Did not find any relation named \"%s\".",
							 pattern);
			else
				pg_log_error("Did not find any relations.");
		}
		PQclear(res);
		return false;
	}

	for (i = 0; i < PQntuples(res); i++)
	{
		const char *oid;
		const char *nspname;
		const char *relname;

		oid = PQgetvalue(res, i, 0);
		nspname = PQgetvalue(res, i, 1);
		relname = PQgetvalue(res, i, 2);

		if (!describeOneTableDetails(nspname, relname, oid, verbose))
		{
			PQclear(res);
			return false;
		}
		if (cancel_pressed)
		{
			PQclear(res);
			return false;
		}
	}

	PQclear(res);
	return true;
}

/*
 * describeOneTableDetails (for \d)
 *
 * Unfortunately, the information presented here is so complicated that it
 * cannot be done in a single query. So we have to assemble the printed table
 * by hand and pass it to the underlying printTable() function.
 */
static bool
describeOneTableDetails(const char *schemaname,
						const char *relationname,
						const char *oid,
						bool verbose)
{
	bool		retval = false;
	PQExpBufferData buf;
	PGresult   *res = NULL;
	printTableOpt myopt = pset.popt.topt;
	printTableContent cont;
	bool		printTableInitialized = false;
	int			i;
	char	   *view_def = NULL;
	char	   *headers[12];
	PQExpBufferData title;
	PQExpBufferData tmpbuf;
	int			cols;
	int			attname_col = -1,	/* column indexes in "res" */
				atttype_col = -1,
				attrdef_col = -1,
				attnotnull_col = -1,
				attcoll_col = -1,
				attidentity_col = -1,
				attgenerated_col = -1,
				isindexkey_col = -1,
				indexdef_col = -1,
				fdwopts_col = -1,
				attstorage_col = -1,
				attcompression_col = -1,
				attstattarget_col = -1,
				attdescr_col = -1;
	int			numrows;
	struct
	{
		int16		checks;
		char		relkind;
		bool		hasindex;
		bool		hasrules;
		bool		hastriggers;
		bool		rowsecurity;
		bool		forcerowsecurity;
		bool		hasoids;
		bool		ispartition;
		Oid			tablespace;
		char	   *reloptions;
		char	   *reloftype;
		char		relpersistence;
		char		relreplident;
		char	   *relam;
	}			tableinfo;
	bool		show_column_details = false;

	myopt.default_footer = false;
	/* This output looks confusing in expanded mode. */
	myopt.expanded = false;

	initPQExpBuffer(&buf);
	initPQExpBuffer(&title);
	initPQExpBuffer(&tmpbuf);

	/* Get general table info */
	if (pset.sversion >= 120000)
	{
		printfPQExpBuffer(&buf,
						  "SELECT c.relchecks, c.relkind, c.relhasindex, c.relhasrules, "
						  "c.relhastriggers, c.relrowsecurity, c.relforcerowsecurity, "
						  "false AS relhasoids, c.relispartition, %s, c.reltablespace, "
						  "CASE WHEN c.reloftype = 0 THEN '' ELSE c.reloftype::pg_catalog.regtype::pg_catalog.text END, "
						  "c.relpersistence, c.relreplident, am.amname\n"
						  "FROM pg_catalog.pg_class c\n "
						  "LEFT JOIN pg_catalog.pg_class tc ON (c.reltoastrelid = tc.oid)\n"
						  "LEFT JOIN pg_catalog.pg_am am ON (c.relam = am.oid)\n"
						  "WHERE c.oid = '%s';",
						  (verbose ?
						   "pg_catalog.array_to_string(c.reloptions || "
						   "array(select 'toast.' || x from pg_catalog.unnest(tc.reloptions) x), ', ')\n"
						   : "''"),
						  oid);
	}
	else if (pset.sversion >= 100000)
	{
		printfPQExpBuffer(&buf,
						  "SELECT c.relchecks, c.relkind, c.relhasindex, c.relhasrules, "
						  "c.relhastriggers, c.relrowsecurity, c.relforcerowsecurity, "
						  "c.relhasoids, c.relispartition, %s, c.reltablespace, "
						  "CASE WHEN c.reloftype = 0 THEN '' ELSE c.reloftype::pg_catalog.regtype::pg_catalog.text END, "
						  "c.relpersistence, c.relreplident\n"
						  "FROM pg_catalog.pg_class c\n "
						  "LEFT JOIN pg_catalog.pg_class tc ON (c.reltoastrelid = tc.oid)\n"
						  "WHERE c.oid = '%s';",
						  (verbose ?
						   "pg_catalog.array_to_string(c.reloptions || "
						   "array(select 'toast.' || x from pg_catalog.unnest(tc.reloptions) x), ', ')\n"
						   : "''"),
						  oid);
	}
	else if (pset.sversion >= 90500)
	{
		printfPQExpBuffer(&buf,
						  "SELECT c.relchecks, c.relkind, c.relhasindex, c.relhasrules, "
						  "c.relhastriggers, c.relrowsecurity, c.relforcerowsecurity, "
						  "c.relhasoids, false as relispartition, %s, c.reltablespace, "
						  "CASE WHEN c.reloftype = 0 THEN '' ELSE c.reloftype::pg_catalog.regtype::pg_catalog.text END, "
						  "c.relpersistence, c.relreplident\n"
						  "FROM pg_catalog.pg_class c\n "
						  "LEFT JOIN pg_catalog.pg_class tc ON (c.reltoastrelid = tc.oid)\n"
						  "WHERE c.oid = '%s';",
						  (verbose ?
						   "pg_catalog.array_to_string(c.reloptions || "
						   "array(select 'toast.' || x from pg_catalog.unnest(tc.reloptions) x), ', ')\n"
						   : "''"),
						  oid);
	}
	else if (pset.sversion >= 90400)
	{
		printfPQExpBuffer(&buf,
						  "SELECT c.relchecks, c.relkind, c.relhasindex, c.relhasrules, "
						  "c.relhastriggers, false, false, c.relhasoids, "
						  "false as relispartition, %s, c.reltablespace, "
						  "CASE WHEN c.reloftype = 0 THEN '' ELSE c.reloftype::pg_catalog.regtype::pg_catalog.text END, "
						  "c.relpersistence, c.relreplident\n"
						  "FROM pg_catalog.pg_class c\n "
						  "LEFT JOIN pg_catalog.pg_class tc ON (c.reltoastrelid = tc.oid)\n"
						  "WHERE c.oid = '%s';",
						  (verbose ?
						   "pg_catalog.array_to_string(c.reloptions || "
						   "array(select 'toast.' || x from pg_catalog.unnest(tc.reloptions) x), ', ')\n"
						   : "''"),
						  oid);
	}
	else
	{
		printfPQExpBuffer(&buf,
						  "SELECT c.relchecks, c.relkind, c.relhasindex, c.relhasrules, "
						  "c.relhastriggers, false, false, c.relhasoids, "
						  "false as relispartition, %s, c.reltablespace, "
						  "CASE WHEN c.reloftype = 0 THEN '' ELSE c.reloftype::pg_catalog.regtype::pg_catalog.text END, "
						  "c.relpersistence\n"
						  "FROM pg_catalog.pg_class c\n "
						  "LEFT JOIN pg_catalog.pg_class tc ON (c.reltoastrelid = tc.oid)\n"
						  "WHERE c.oid = '%s';",
						  (verbose ?
						   "pg_catalog.array_to_string(c.reloptions || "
						   "array(select 'toast.' || x from pg_catalog.unnest(tc.reloptions) x), ', ')\n"
						   : "''"),
						  oid);
	}

	res = PSQLexec(buf.data);
	if (!res)
		goto error_return;

	/* Did we get anything? */
	if (PQntuples(res) == 0)
	{
		if (!pset.quiet)
			pg_log_error("Did not find any relation with OID %s.", oid);
		goto error_return;
	}

	tableinfo.checks = atoi(PQgetvalue(res, 0, 0));
	tableinfo.relkind = *(PQgetvalue(res, 0, 1));
	tableinfo.hasindex = strcmp(PQgetvalue(res, 0, 2), "t") == 0;
	tableinfo.hasrules = strcmp(PQgetvalue(res, 0, 3), "t") == 0;
	tableinfo.hastriggers = strcmp(PQgetvalue(res, 0, 4), "t") == 0;
	tableinfo.rowsecurity = strcmp(PQgetvalue(res, 0, 5), "t") == 0;
	tableinfo.forcerowsecurity = strcmp(PQgetvalue(res, 0, 6), "t") == 0;
	tableinfo.hasoids = strcmp(PQgetvalue(res, 0, 7), "t") == 0;
	tableinfo.ispartition = strcmp(PQgetvalue(res, 0, 8), "t") == 0;
	tableinfo.reloptions = pg_strdup(PQgetvalue(res, 0, 9));
	tableinfo.tablespace = atooid(PQgetvalue(res, 0, 10));
	tableinfo.reloftype = (strcmp(PQgetvalue(res, 0, 11), "") != 0) ?
		pg_strdup(PQgetvalue(res, 0, 11)) : NULL;
	tableinfo.relpersistence = *(PQgetvalue(res, 0, 12));
	tableinfo.relreplident = (pset.sversion >= 90400) ?
		*(PQgetvalue(res, 0, 13)) : 'd';
	if (pset.sversion >= 120000)
		tableinfo.relam = PQgetisnull(res, 0, 14) ?
			NULL : pg_strdup(PQgetvalue(res, 0, 14));
	else
		tableinfo.relam = NULL;
	PQclear(res);
	res = NULL;

	/*
	 * If it's a sequence, deal with it here separately.
	 */
	if (tableinfo.relkind == RELKIND_SEQUENCE)
	{
		PGresult   *result = NULL;
		printQueryOpt myopt = pset.popt;
		char	   *footers[2] = {NULL, NULL};

		if (pset.sversion >= 100000)
		{
			printfPQExpBuffer(&buf,
							  "SELECT pg_catalog.format_type(seqtypid, NULL) AS \"%s\",\n"
							  "       seqstart AS \"%s\",\n"
							  "       seqmin AS \"%s\",\n"
							  "       seqmax AS \"%s\",\n"
							  "       seqincrement AS \"%s\",\n"
							  "       CASE WHEN seqcycle THEN '%s' ELSE '%s' END AS \"%s\",\n"
							  "       seqcache AS \"%s\"\n",
							  gettext_noop("Type"),
							  gettext_noop("Start"),
							  gettext_noop("Minimum"),
							  gettext_noop("Maximum"),
							  gettext_noop("Increment"),
							  gettext_noop("yes"),
							  gettext_noop("no"),
							  gettext_noop("Cycles?"),
							  gettext_noop("Cache"));
			appendPQExpBuffer(&buf,
							  "FROM pg_catalog.pg_sequence\n"
							  "WHERE seqrelid = '%s';",
							  oid);
		}
		else
		{
			printfPQExpBuffer(&buf,
							  "SELECT 'bigint' AS \"%s\",\n"
							  "       start_value AS \"%s\",\n"
							  "       min_value AS \"%s\",\n"
							  "       max_value AS \"%s\",\n"
							  "       increment_by AS \"%s\",\n"
							  "       CASE WHEN is_cycled THEN '%s' ELSE '%s' END AS \"%s\",\n"
							  "       cache_value AS \"%s\"\n",
							  gettext_noop("Type"),
							  gettext_noop("Start"),
							  gettext_noop("Minimum"),
							  gettext_noop("Maximum"),
							  gettext_noop("Increment"),
							  gettext_noop("yes"),
							  gettext_noop("no"),
							  gettext_noop("Cycles?"),
							  gettext_noop("Cache"));
			appendPQExpBuffer(&buf, "FROM %s", fmtId(schemaname));
			/* must be separate because fmtId isn't reentrant */
			appendPQExpBuffer(&buf, ".%s;", fmtId(relationname));
		}

		res = PSQLexec(buf.data);
		if (!res)
			goto error_return;

		/* Get the column that owns this sequence */
		printfPQExpBuffer(&buf, "SELECT pg_catalog.quote_ident(nspname) || '.' ||"
						  "\n   pg_catalog.quote_ident(relname) || '.' ||"
						  "\n   pg_catalog.quote_ident(attname),"
						  "\n   d.deptype"
						  "\nFROM pg_catalog.pg_class c"
						  "\nINNER JOIN pg_catalog.pg_depend d ON c.oid=d.refobjid"
						  "\nINNER JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace"
						  "\nINNER JOIN pg_catalog.pg_attribute a ON ("
						  "\n a.attrelid=c.oid AND"
						  "\n a.attnum=d.refobjsubid)"
						  "\nWHERE d.classid='pg_catalog.pg_class'::pg_catalog.regclass"
						  "\n AND d.refclassid='pg_catalog.pg_class'::pg_catalog.regclass"
						  "\n AND d.objid='%s'"
						  "\n AND d.deptype IN ('a', 'i')",
						  oid);

		result = PSQLexec(buf.data);

		/*
		 * If we get no rows back, don't show anything (obviously). We should
		 * never get more than one row back, but if we do, just ignore it and
		 * don't print anything.
		 */
		if (!result)
			goto error_return;
		else if (PQntuples(result) == 1)
		{
			switch (PQgetvalue(result, 0, 1)[0])
			{
				case 'a':
					footers[0] = psprintf(_("Owned by: %s"),
										  PQgetvalue(result, 0, 0));
					break;
				case 'i':
					footers[0] = psprintf(_("Sequence for identity column: %s"),
										  PQgetvalue(result, 0, 0));
					break;
			}
		}
		PQclear(result);

		if (tableinfo.relpersistence == RELPERSISTENCE_UNLOGGED)
			printfPQExpBuffer(&title, _("Unlogged sequence \"%s.%s\""),
							  schemaname, relationname);
		else
			printfPQExpBuffer(&title, _("Sequence \"%s.%s\""),
							  schemaname, relationname);

		myopt.footers = footers;
		myopt.topt.default_footer = false;
		myopt.title = title.data;
		myopt.translate_header = true;

		printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

		free(footers[0]);

		retval = true;
		goto error_return;		/* not an error, just return early */
	}

	/* Identify whether we should print collation, nullable, default vals */
	if (tableinfo.relkind == RELKIND_RELATION ||
		tableinfo.relkind == RELKIND_VIEW ||
		tableinfo.relkind == RELKIND_MATVIEW ||
		tableinfo.relkind == RELKIND_FOREIGN_TABLE ||
		tableinfo.relkind == RELKIND_COMPOSITE_TYPE ||
		tableinfo.relkind == RELKIND_PARTITIONED_TABLE)
		show_column_details = true;

	/*
	 * Get per-column info
	 *
	 * Since the set of query columns we need varies depending on relkind and
	 * server version, we compute all the column numbers on-the-fly.  Column
	 * number variables for columns not fetched are left as -1; this avoids
	 * duplicative test logic below.
	 */
	cols = 0;
	printfPQExpBuffer(&buf, "SELECT a.attname");
	attname_col = cols++;
	appendPQExpBufferStr(&buf, ",\n  pg_catalog.format_type(a.atttypid, a.atttypmod)");
	atttype_col = cols++;

	if (show_column_details)
	{
		/* use "pretty" mode for expression to avoid excessive parentheses */
		appendPQExpBufferStr(&buf,
							 ",\n  (SELECT pg_catalog.pg_get_expr(d.adbin, d.adrelid, true)"
							 "\n   FROM pg_catalog.pg_attrdef d"
							 "\n   WHERE d.adrelid = a.attrelid AND d.adnum = a.attnum AND a.atthasdef)"
							 ",\n  a.attnotnull");
		attrdef_col = cols++;
		attnotnull_col = cols++;
		appendPQExpBufferStr(&buf, ",\n  (SELECT c.collname FROM pg_catalog.pg_collation c, pg_catalog.pg_type t\n"
							 "   WHERE c.oid = a.attcollation AND t.oid = a.atttypid AND a.attcollation <> t.typcollation) AS attcollation");
		attcoll_col = cols++;
		if (pset.sversion >= 100000)
			appendPQExpBufferStr(&buf, ",\n  a.attidentity");
		else
			appendPQExpBufferStr(&buf, ",\n  ''::pg_catalog.char AS attidentity");
		attidentity_col = cols++;
		if (pset.sversion >= 120000)
			appendPQExpBufferStr(&buf, ",\n  a.attgenerated");
		else
			appendPQExpBufferStr(&buf, ",\n  ''::pg_catalog.char AS attgenerated");
		attgenerated_col = cols++;
	}
	if (tableinfo.relkind == RELKIND_INDEX ||
		tableinfo.relkind == RELKIND_PARTITIONED_INDEX)
	{
		if (pset.sversion >= 110000)
		{
			appendPQExpBuffer(&buf, ",\n  CASE WHEN a.attnum <= (SELECT i.indnkeyatts FROM pg_catalog.pg_index i WHERE i.indexrelid = '%s') THEN '%s' ELSE '%s' END AS is_key",
							  oid,
							  gettext_noop("yes"),
							  gettext_noop("no"));
			isindexkey_col = cols++;
		}
		appendPQExpBufferStr(&buf, ",\n  pg_catalog.pg_get_indexdef(a.attrelid, a.attnum, TRUE) AS indexdef");
		indexdef_col = cols++;
	}
	/* FDW options for foreign table column */
	if (tableinfo.relkind == RELKIND_FOREIGN_TABLE)
	{
		appendPQExpBufferStr(&buf, ",\n  CASE WHEN attfdwoptions IS NULL THEN '' ELSE "
							 "  '(' || pg_catalog.array_to_string(ARRAY(SELECT pg_catalog.quote_ident(option_name) || ' ' || pg_catalog.quote_literal(option_value)  FROM "
							 "  pg_catalog.pg_options_to_table(attfdwoptions)), ', ') || ')' END AS attfdwoptions");
		fdwopts_col = cols++;
	}
	if (verbose)
	{
		appendPQExpBufferStr(&buf, ",\n  a.attstorage");
		attstorage_col = cols++;

		/* compression info, if relevant to relkind */
		if (pset.sversion >= 140000 &&
			!pset.hide_compression &&
			(tableinfo.relkind == RELKIND_RELATION ||
			 tableinfo.relkind == RELKIND_PARTITIONED_TABLE ||
			 tableinfo.relkind == RELKIND_MATVIEW))
		{
			appendPQExpBufferStr(&buf, ",\n  a.attcompression AS attcompression");
			attcompression_col = cols++;
		}

		/* stats target, if relevant to relkind */
		if (tableinfo.relkind == RELKIND_RELATION ||
			tableinfo.relkind == RELKIND_INDEX ||
			tableinfo.relkind == RELKIND_PARTITIONED_INDEX ||
			tableinfo.relkind == RELKIND_MATVIEW ||
			tableinfo.relkind == RELKIND_FOREIGN_TABLE ||
			tableinfo.relkind == RELKIND_PARTITIONED_TABLE)
		{
			appendPQExpBufferStr(&buf, ",\n  CASE WHEN a.attstattarget=-1 THEN NULL ELSE a.attstattarget END AS attstattarget");
			attstattarget_col = cols++;
		}

		/*
		 * In 9.0+, we have column comments for: relations, views, composite
		 * types, and foreign tables (cf. CommentObject() in comment.c).
		 */
		if (tableinfo.relkind == RELKIND_RELATION ||
			tableinfo.relkind == RELKIND_VIEW ||
			tableinfo.relkind == RELKIND_MATVIEW ||
			tableinfo.relkind == RELKIND_FOREIGN_TABLE ||
			tableinfo.relkind == RELKIND_COMPOSITE_TYPE ||
			tableinfo.relkind == RELKIND_PARTITIONED_TABLE)
		{
			appendPQExpBufferStr(&buf, ",\n  pg_catalog.col_description(a.attrelid, a.attnum)");
			attdescr_col = cols++;
		}
	}

	appendPQExpBufferStr(&buf, "\nFROM pg_catalog.pg_attribute a");
	appendPQExpBuffer(&buf, "\nWHERE a.attrelid = '%s' AND a.attnum > 0 AND NOT a.attisdropped", oid);
	appendPQExpBufferStr(&buf, "\nORDER BY a.attnum;");

	res = PSQLexec(buf.data);
	if (!res)
		goto error_return;
	numrows = PQntuples(res);

	/* Make title */
	switch (tableinfo.relkind)
	{
		case RELKIND_RELATION:
			if (tableinfo.relpersistence == RELPERSISTENCE_UNLOGGED)
				printfPQExpBuffer(&title, _("Unlogged table \"%s.%s\""),
								  schemaname, relationname);
			else
				printfPQExpBuffer(&title, _("Table \"%s.%s\""),
								  schemaname, relationname);
			break;
		case RELKIND_VIEW:
			printfPQExpBuffer(&title, _("View \"%s.%s\""),
							  schemaname, relationname);
			break;
		case RELKIND_MATVIEW:
			printfPQExpBuffer(&title, _("Materialized view \"%s.%s\""),
							  schemaname, relationname);
			break;
		case RELKIND_INDEX:
			if (tableinfo.relpersistence == RELPERSISTENCE_UNLOGGED)
				printfPQExpBuffer(&title, _("Unlogged index \"%s.%s\""),
								  schemaname, relationname);
			else
				printfPQExpBuffer(&title, _("Index \"%s.%s\""),
								  schemaname, relationname);
			break;
		case RELKIND_PARTITIONED_INDEX:
			if (tableinfo.relpersistence == RELPERSISTENCE_UNLOGGED)
				printfPQExpBuffer(&title, _("Unlogged partitioned index \"%s.%s\""),
								  schemaname, relationname);
			else
				printfPQExpBuffer(&title, _("Partitioned index \"%s.%s\""),
								  schemaname, relationname);
			break;
		case RELKIND_TOASTVALUE:
			printfPQExpBuffer(&title, _("TOAST table \"%s.%s\""),
							  schemaname, relationname);
			break;
		case RELKIND_COMPOSITE_TYPE:
			printfPQExpBuffer(&title, _("Composite type \"%s.%s\""),
							  schemaname, relationname);
			break;
		case RELKIND_FOREIGN_TABLE:
			printfPQExpBuffer(&title, _("Foreign table \"%s.%s\""),
							  schemaname, relationname);
			break;
		case RELKIND_PARTITIONED_TABLE:
			if (tableinfo.relpersistence == RELPERSISTENCE_UNLOGGED)
				printfPQExpBuffer(&title, _("Unlogged partitioned table \"%s.%s\""),
								  schemaname, relationname);
			else
				printfPQExpBuffer(&title, _("Partitioned table \"%s.%s\""),
								  schemaname, relationname);
			break;
		default:
			/* untranslated unknown relkind */
			printfPQExpBuffer(&title, "?%c? \"%s.%s\"",
							  tableinfo.relkind, schemaname, relationname);
			break;
	}

	/* Fill headers[] with the names of the columns we will output */
	cols = 0;
	headers[cols++] = gettext_noop("Column");
	headers[cols++] = gettext_noop("Type");
	if (show_column_details)
	{
		headers[cols++] = gettext_noop("Collation");
		headers[cols++] = gettext_noop("Nullable");
		headers[cols++] = gettext_noop("Default");
	}
	if (isindexkey_col >= 0)
		headers[cols++] = gettext_noop("Key?");
	if (indexdef_col >= 0)
		headers[cols++] = gettext_noop("Definition");
	if (fdwopts_col >= 0)
		headers[cols++] = gettext_noop("FDW options");
	if (attstorage_col >= 0)
		headers[cols++] = gettext_noop("Storage");
	if (attcompression_col >= 0)
		headers[cols++] = gettext_noop("Compression");
	if (attstattarget_col >= 0)
		headers[cols++] = gettext_noop("Stats target");
	if (attdescr_col >= 0)
		headers[cols++] = gettext_noop("Description");

	Assert(cols <= lengthof(headers));

	printTableInit(&cont, &myopt, title.data, cols, numrows);
	printTableInitialized = true;

	for (i = 0; i < cols; i++)
		printTableAddHeader(&cont, headers[i], true, 'l');

	/* Generate table cells to be printed */
	for (i = 0; i < numrows; i++)
	{
		/* Column */
		printTableAddCell(&cont, PQgetvalue(res, i, attname_col), false, false);

		/* Type */
		printTableAddCell(&cont, PQgetvalue(res, i, atttype_col), false, false);

		/* Collation, Nullable, Default */
		if (show_column_details)
		{
			char	   *identity;
			char	   *generated;
			char	   *default_str;
			bool		mustfree = false;

			printTableAddCell(&cont, PQgetvalue(res, i, attcoll_col), false, false);

			printTableAddCell(&cont,
							  strcmp(PQgetvalue(res, i, attnotnull_col), "t") == 0 ? "not null" : "",
							  false, false);

			identity = PQgetvalue(res, i, attidentity_col);
			generated = PQgetvalue(res, i, attgenerated_col);

			if (identity[0] == ATTRIBUTE_IDENTITY_ALWAYS)
				default_str = "generated always as identity";
			else if (identity[0] == ATTRIBUTE_IDENTITY_BY_DEFAULT)
				default_str = "generated by default as identity";
			else if (generated[0] == ATTRIBUTE_GENERATED_STORED)
			{
				default_str = psprintf("generated always as (%s) stored",
									   PQgetvalue(res, i, attrdef_col));
				mustfree = true;
			}
			else if (generated[0] == ATTRIBUTE_GENERATED_VIRTUAL)
			{
				default_str = psprintf("generated always as (%s)",
									   PQgetvalue(res, i, attrdef_col));
				mustfree = true;
			}
			else
				default_str = PQgetvalue(res, i, attrdef_col);

			printTableAddCell(&cont, default_str, false, mustfree);
		}

		/* Info for index columns */
		if (isindexkey_col >= 0)
			printTableAddCell(&cont, PQgetvalue(res, i, isindexkey_col), true, false);
		if (indexdef_col >= 0)
			printTableAddCell(&cont, PQgetvalue(res, i, indexdef_col), false, false);

		/* FDW options for foreign table columns */
		if (fdwopts_col >= 0)
			printTableAddCell(&cont, PQgetvalue(res, i, fdwopts_col), false, false);

		/* Storage mode, if relevant */
		if (attstorage_col >= 0)
		{
			char	   *storage = PQgetvalue(res, i, attstorage_col);

			/* these strings are literal in our syntax, so not translated. */
			printTableAddCell(&cont, (storage[0] == TYPSTORAGE_PLAIN ? "plain" :
									  (storage[0] == TYPSTORAGE_MAIN ? "main" :
									   (storage[0] == TYPSTORAGE_EXTENDED ? "extended" :
										(storage[0] == TYPSTORAGE_EXTERNAL ? "external" :
										 "???")))),
							  false, false);
		}

		/* Column compression, if relevant */
		if (attcompression_col >= 0)
		{
			char	   *compression = PQgetvalue(res, i, attcompression_col);

			/* these strings are literal in our syntax, so not translated. */
			printTableAddCell(&cont, (compression[0] == 'p' ? "pglz" :
									  (compression[0] == 'l' ? "lz4" :
									   (compression[0] == '\0' ? "" :
										"???"))),
							  false, false);
		}

		/* Statistics target, if the relkind supports this feature */
		if (attstattarget_col >= 0)
			printTableAddCell(&cont, PQgetvalue(res, i, attstattarget_col),
							  false, false);

		/* Column comments, if the relkind supports this feature */
		if (attdescr_col >= 0)
			printTableAddCell(&cont, PQgetvalue(res, i, attdescr_col),
							  false, false);
	}

	/* Make footers */

	if (tableinfo.ispartition)
	{
		/* Footer information for a partition child table */
		PGresult   *result;

		printfPQExpBuffer(&buf,
						  "SELECT inhparent::pg_catalog.regclass,\n"
						  "  pg_catalog.pg_get_expr(c.relpartbound, c.oid),\n  ");

		appendPQExpBufferStr(&buf,
							 pset.sversion >= 140000 ? "inhdetachpending" :
							 "false as inhdetachpending");

		/* If verbose, also request the partition constraint definition */
		if (verbose)
			appendPQExpBufferStr(&buf,
								 ",\n  pg_catalog.pg_get_partition_constraintdef(c.oid)");
		appendPQExpBuffer(&buf,
						  "\nFROM pg_catalog.pg_class c"
						  " JOIN pg_catalog.pg_inherits i"
						  " ON c.oid = inhrelid"
						  "\nWHERE c.oid = '%s';", oid);
		result = PSQLexec(buf.data);
		if (!result)
			goto error_return;

		if (PQntuples(result) > 0)
		{
			char	   *parent_name = PQgetvalue(result, 0, 0);
			char	   *partdef = PQgetvalue(result, 0, 1);
			char	   *detached = PQgetvalue(result, 0, 2);

			printfPQExpBuffer(&tmpbuf, _("Partition of: %s %s%s"), parent_name,
							  partdef,
							  strcmp(detached, "t") == 0 ? " DETACH PENDING" : "");
			printTableAddFooter(&cont, tmpbuf.data);

			if (verbose)
			{
				char	   *partconstraintdef = NULL;

				if (!PQgetisnull(result, 0, 3))
					partconstraintdef = PQgetvalue(result, 0, 3);
				/* If there isn't any constraint, show that explicitly */
				if (partconstraintdef == NULL || partconstraintdef[0] == '\0')
					printfPQExpBuffer(&tmpbuf, _("No partition constraint"));
				else
					printfPQExpBuffer(&tmpbuf, _("Partition constraint: %s"),
									  partconstraintdef);
				printTableAddFooter(&cont, tmpbuf.data);
			}
		}
		PQclear(result);
	}

	if (tableinfo.relkind == RELKIND_PARTITIONED_TABLE)
	{
		/* Footer information for a partitioned table (partitioning parent) */
		PGresult   *result;

		printfPQExpBuffer(&buf,
						  "SELECT pg_catalog.pg_get_partkeydef('%s'::pg_catalog.oid);",
						  oid);
		result = PSQLexec(buf.data);
		if (!result)
			goto error_return;

		if (PQntuples(result) == 1)
		{
			char	   *partkeydef = PQgetvalue(result, 0, 0);

			printfPQExpBuffer(&tmpbuf, _("Partition key: %s"), partkeydef);
			printTableAddFooter(&cont, tmpbuf.data);
		}
		PQclear(result);
	}

	if (tableinfo.relkind == RELKIND_TOASTVALUE)
	{
		/* For a TOAST table, print name of owning table */
		PGresult   *result;

		printfPQExpBuffer(&buf,
						  "SELECT n.nspname, c.relname\n"
						  "FROM pg_catalog.pg_class c"
						  " JOIN pg_catalog.pg_namespace n"
						  " ON n.oid = c.relnamespace\n"
						  "WHERE reltoastrelid = '%s';", oid);
		result = PSQLexec(buf.data);
		if (!result)
			goto error_return;

		if (PQntuples(result) == 1)
		{
			char	   *schemaname = PQgetvalue(result, 0, 0);
			char	   *relname = PQgetvalue(result, 0, 1);

			printfPQExpBuffer(&tmpbuf, _("Owning table: \"%s.%s\""),
							  schemaname, relname);
			printTableAddFooter(&cont, tmpbuf.data);
		}
		PQclear(result);
	}

	if (tableinfo.relkind == RELKIND_INDEX ||
		tableinfo.relkind == RELKIND_PARTITIONED_INDEX)
	{
		/* Footer information about an index */
		PGresult   *result;

		printfPQExpBuffer(&buf,
						  "SELECT i.indisunique, i.indisprimary, i.indisclustered, "
						  "i.indisvalid,\n"
						  "  (NOT i.indimmediate) AND "
						  "EXISTS (SELECT 1 FROM pg_catalog.pg_constraint "
						  "WHERE conrelid = i.indrelid AND "
						  "conindid = i.indexrelid AND "
						  "contype IN (" CppAsString2(CONSTRAINT_PRIMARY) ","
						  CppAsString2(CONSTRAINT_UNIQUE) ","
						  CppAsString2(CONSTRAINT_EXCLUSION) ") AND "
						  "condeferrable) AS condeferrable,\n"
						  "  (NOT i.indimmediate) AND "
						  "EXISTS (SELECT 1 FROM pg_catalog.pg_constraint "
						  "WHERE conrelid = i.indrelid AND "
						  "conindid = i.indexrelid AND "
						  "contype IN (" CppAsString2(CONSTRAINT_PRIMARY) ","
						  CppAsString2(CONSTRAINT_UNIQUE) ","
						  CppAsString2(CONSTRAINT_EXCLUSION) ") AND "
						  "condeferred) AS condeferred,\n");

		if (pset.sversion >= 90400)
			appendPQExpBufferStr(&buf, "i.indisreplident,\n");
		else
			appendPQExpBufferStr(&buf, "false AS indisreplident,\n");

		if (pset.sversion >= 150000)
			appendPQExpBufferStr(&buf, "i.indnullsnotdistinct,\n");
		else
			appendPQExpBufferStr(&buf, "false AS indnullsnotdistinct,\n");

		appendPQExpBuffer(&buf, "  a.amname, c2.relname, "
						  "pg_catalog.pg_get_expr(i.indpred, i.indrelid, true)\n"
						  "FROM pg_catalog.pg_index i, pg_catalog.pg_class c, pg_catalog.pg_class c2, pg_catalog.pg_am a\n"
						  "WHERE i.indexrelid = c.oid AND c.oid = '%s' AND c.relam = a.oid\n"
						  "AND i.indrelid = c2.oid;",
						  oid);

		result = PSQLexec(buf.data);
		if (!result)
			goto error_return;
		else if (PQntuples(result) != 1)
		{
			PQclear(result);
			goto error_return;
		}
		else
		{
			char	   *indisunique = PQgetvalue(result, 0, 0);
			char	   *indisprimary = PQgetvalue(result, 0, 1);
			char	   *indisclustered = PQgetvalue(result, 0, 2);
			char	   *indisvalid = PQgetvalue(result, 0, 3);
			char	   *deferrable = PQgetvalue(result, 0, 4);
			char	   *deferred = PQgetvalue(result, 0, 5);
			char	   *indisreplident = PQgetvalue(result, 0, 6);
			char	   *indnullsnotdistinct = PQgetvalue(result, 0, 7);
			char	   *indamname = PQgetvalue(result, 0, 8);
			char	   *indtable = PQgetvalue(result, 0, 9);
			char	   *indpred = PQgetvalue(result, 0, 10);

			if (strcmp(indisprimary, "t") == 0)
				printfPQExpBuffer(&tmpbuf, _("primary key, "));
			else if (strcmp(indisunique, "t") == 0)
			{
				printfPQExpBuffer(&tmpbuf, _("unique"));
				if (strcmp(indnullsnotdistinct, "t") == 0)
					appendPQExpBufferStr(&tmpbuf, _(" nulls not distinct"));
				appendPQExpBufferStr(&tmpbuf, _(", "));
			}
			else
				resetPQExpBuffer(&tmpbuf);
			appendPQExpBuffer(&tmpbuf, "%s, ", indamname);

			/* we assume here that index and table are in same schema */
			appendPQExpBuffer(&tmpbuf, _("for table \"%s.%s\""),
							  schemaname, indtable);

			if (strlen(indpred))
				appendPQExpBuffer(&tmpbuf, _(", predicate (%s)"), indpred);

			if (strcmp(indisclustered, "t") == 0)
				appendPQExpBufferStr(&tmpbuf, _(", clustered"));

			if (strcmp(indisvalid, "t") != 0)
				appendPQExpBufferStr(&tmpbuf, _(", invalid"));

			if (strcmp(deferrable, "t") == 0)
				appendPQExpBufferStr(&tmpbuf, _(", deferrable"));

			if (strcmp(deferred, "t") == 0)
				appendPQExpBufferStr(&tmpbuf, _(", initially deferred"));

			if (strcmp(indisreplident, "t") == 0)
				appendPQExpBufferStr(&tmpbuf, _(", replica identity"));

			printTableAddFooter(&cont, tmpbuf.data);

			/*
			 * If it's a partitioned index, we'll print the tablespace below
			 */
			if (tableinfo.relkind == RELKIND_INDEX)
				add_tablespace_footer(&cont, tableinfo.relkind,
									  tableinfo.tablespace, true);
		}

		PQclear(result);
	}
	/* If you add relkinds here, see also "Finish printing..." stanza below */
	else if (tableinfo.relkind == RELKIND_RELATION ||
			 tableinfo.relkind == RELKIND_MATVIEW ||
			 tableinfo.relkind == RELKIND_FOREIGN_TABLE ||
			 tableinfo.relkind == RELKIND_PARTITIONED_TABLE ||
			 tableinfo.relkind == RELKIND_PARTITIONED_INDEX ||
			 tableinfo.relkind == RELKIND_TOASTVALUE)
	{
		/* Footer information about a table */
		PGresult   *result = NULL;
		int			tuples = 0;

		/* print indexes */
		if (tableinfo.hasindex)
		{
			printfPQExpBuffer(&buf,
							  "SELECT c2.relname, i.indisprimary, i.indisunique, "
							  "i.indisclustered, i.indisvalid, "
							  "pg_catalog.pg_get_indexdef(i.indexrelid, 0, true),\n  "
							  "pg_catalog.pg_get_constraintdef(con.oid, true), "
							  "contype, condeferrable, condeferred");
			if (pset.sversion >= 90400)
				appendPQExpBufferStr(&buf, ", i.indisreplident");
			else
				appendPQExpBufferStr(&buf, ", false AS indisreplident");
			appendPQExpBufferStr(&buf, ", c2.reltablespace");
			if (pset.sversion >= 180000)
				appendPQExpBufferStr(&buf, ", con.conperiod");
			else
				appendPQExpBufferStr(&buf, ", false AS conperiod");
			appendPQExpBuffer(&buf,
							  "\nFROM pg_catalog.pg_class c, pg_catalog.pg_class c2, pg_catalog.pg_index i\n"
							  "  LEFT JOIN pg_catalog.pg_constraint con ON (conrelid = i.indrelid AND conindid = i.indexrelid AND contype IN ("
							  CppAsString2(CONSTRAINT_PRIMARY) ","
							  CppAsString2(CONSTRAINT_UNIQUE) ","
							  CppAsString2(CONSTRAINT_EXCLUSION) "))\n"
							  "WHERE c.oid = '%s' AND c.oid = i.indrelid AND i.indexrelid = c2.oid\n"
							  "ORDER BY i.indisprimary DESC, c2.relname;",
							  oid);
			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else
				tuples = PQntuples(result);

			if (tuples > 0)
			{
				printTableAddFooter(&cont, _("Indexes:"));
				for (i = 0; i < tuples; i++)
				{
					/* untranslated index name */
					printfPQExpBuffer(&buf, "    \"%s\"",
									  PQgetvalue(result, i, 0));

					/*
					 * If exclusion constraint or PK/UNIQUE constraint WITHOUT
					 * OVERLAPS, print the constraintdef
					 */
					if (strcmp(PQgetvalue(result, i, 7), "x") == 0 ||
						strcmp(PQgetvalue(result, i, 12), "t") == 0)
					{
						appendPQExpBuffer(&buf, " %s",
										  PQgetvalue(result, i, 6));
					}
					else
					{
						const char *indexdef;
						const char *usingpos;

						/* Label as primary key or unique (but not both) */
						if (strcmp(PQgetvalue(result, i, 1), "t") == 0)
							appendPQExpBufferStr(&buf, " PRIMARY KEY,");
						else if (strcmp(PQgetvalue(result, i, 2), "t") == 0)
						{
							if (strcmp(PQgetvalue(result, i, 7), "u") == 0)
								appendPQExpBufferStr(&buf, " UNIQUE CONSTRAINT,");
							else
								appendPQExpBufferStr(&buf, " UNIQUE,");
						}

						/* Everything after "USING" is echoed verbatim */
						indexdef = PQgetvalue(result, i, 5);
						usingpos = strstr(indexdef, " USING ");
						if (usingpos)
							indexdef = usingpos + 7;
						appendPQExpBuffer(&buf, " %s", indexdef);

						/* Need these for deferrable PK/UNIQUE indexes */
						if (strcmp(PQgetvalue(result, i, 8), "t") == 0)
							appendPQExpBufferStr(&buf, " DEFERRABLE");

						if (strcmp(PQgetvalue(result, i, 9), "t") == 0)
							appendPQExpBufferStr(&buf, " INITIALLY DEFERRED");
					}

					/* Add these for all cases */
					if (strcmp(PQgetvalue(result, i, 3), "t") == 0)
						appendPQExpBufferStr(&buf, " CLUSTER");

					if (strcmp(PQgetvalue(result, i, 4), "t") != 0)
						appendPQExpBufferStr(&buf, " INVALID");

					if (strcmp(PQgetvalue(result, i, 10), "t") == 0)
						appendPQExpBufferStr(&buf, " REPLICA IDENTITY");

					printTableAddFooter(&cont, buf.data);

					/* Print tablespace of the index on the same line */
					add_tablespace_footer(&cont, RELKIND_INDEX,
										  atooid(PQgetvalue(result, i, 11)),
										  false);
				}
			}
			PQclear(result);
		}

		/* print table (and column) check constraints */
		if (tableinfo.checks)
		{
			printfPQExpBuffer(&buf,
							  "SELECT r.conname, "
							  "pg_catalog.pg_get_constraintdef(r.oid, true)\n"
							  "FROM pg_catalog.pg_constraint r\n"
							  "WHERE r.conrelid = '%s' "
							  "AND r.contype = " CppAsString2(CONSTRAINT_CHECK) "\n"
							  "ORDER BY 1;",
							  oid);
			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else
				tuples = PQntuples(result);

			if (tuples > 0)
			{
				printTableAddFooter(&cont, _("Check constraints:"));
				for (i = 0; i < tuples; i++)
				{
					/* untranslated constraint name and def */
					printfPQExpBuffer(&buf, "    \"%s\" %s",
									  PQgetvalue(result, i, 0),
									  PQgetvalue(result, i, 1));

					printTableAddFooter(&cont, buf.data);
				}
			}
			PQclear(result);
		}

		/*
		 * Print foreign-key constraints (there are none if no triggers,
		 * except if the table is partitioned, in which case the triggers
		 * appear in the partitions)
		 */
		if (tableinfo.hastriggers ||
			tableinfo.relkind == RELKIND_PARTITIONED_TABLE)
		{
			if (pset.sversion >= 120000 &&
				(tableinfo.ispartition || tableinfo.relkind == RELKIND_PARTITIONED_TABLE))
			{
				/*
				 * Put the constraints defined in this table first, followed
				 * by the constraints defined in ancestor partitioned tables.
				 */
				printfPQExpBuffer(&buf,
								  "SELECT conrelid = '%s'::pg_catalog.regclass AS sametable,\n"
								  "       conname,\n"
								  "       pg_catalog.pg_get_constraintdef(oid, true) AS condef,\n"
								  "       conrelid::pg_catalog.regclass AS ontable\n"
								  "  FROM pg_catalog.pg_constraint,\n"
								  "       pg_catalog.pg_partition_ancestors('%s')\n"
								  " WHERE conrelid = relid AND contype = " CppAsString2(CONSTRAINT_FOREIGN) " AND conparentid = 0\n"
								  "ORDER BY sametable DESC, conname;",
								  oid, oid);
			}
			else
			{
				printfPQExpBuffer(&buf,
								  "SELECT true as sametable, conname,\n"
								  "  pg_catalog.pg_get_constraintdef(r.oid, true) as condef,\n"
								  "  conrelid::pg_catalog.regclass AS ontable\n"
								  "FROM pg_catalog.pg_constraint r\n"
								  "WHERE r.conrelid = '%s' AND r.contype = " CppAsString2(CONSTRAINT_FOREIGN) "\n",
								  oid);

				if (pset.sversion >= 120000)
					appendPQExpBufferStr(&buf, "     AND conparentid = 0\n");
				appendPQExpBufferStr(&buf, "ORDER BY conname");
			}

			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else
				tuples = PQntuples(result);

			if (tuples > 0)
			{
				int			i_sametable = PQfnumber(result, "sametable"),
							i_conname = PQfnumber(result, "conname"),
							i_condef = PQfnumber(result, "condef"),
							i_ontable = PQfnumber(result, "ontable");

				printTableAddFooter(&cont, _("Foreign-key constraints:"));
				for (i = 0; i < tuples; i++)
				{
					/*
					 * Print untranslated constraint name and definition. Use
					 * a "TABLE tab" prefix when the constraint is defined in
					 * a parent partitioned table.
					 */
					if (strcmp(PQgetvalue(result, i, i_sametable), "f") == 0)
						printfPQExpBuffer(&buf, "    TABLE \"%s\" CONSTRAINT \"%s\" %s",
										  PQgetvalue(result, i, i_ontable),
										  PQgetvalue(result, i, i_conname),
										  PQgetvalue(result, i, i_condef));
					else
						printfPQExpBuffer(&buf, "    \"%s\" %s",
										  PQgetvalue(result, i, i_conname),
										  PQgetvalue(result, i, i_condef));

					printTableAddFooter(&cont, buf.data);
				}
			}
			PQclear(result);
		}

		/* print incoming foreign-key references */
		if (tableinfo.hastriggers ||
			tableinfo.relkind == RELKIND_PARTITIONED_TABLE)
		{
			if (pset.sversion >= 120000)
			{
				printfPQExpBuffer(&buf,
								  "SELECT conname, conrelid::pg_catalog.regclass AS ontable,\n"
								  "       pg_catalog.pg_get_constraintdef(oid, true) AS condef\n"
								  "  FROM pg_catalog.pg_constraint c\n"
								  " WHERE confrelid IN (SELECT pg_catalog.pg_partition_ancestors('%s')\n"
								  "                     UNION ALL VALUES ('%s'::pg_catalog.regclass))\n"
								  "       AND contype = " CppAsString2(CONSTRAINT_FOREIGN) " AND conparentid = 0\n"
								  "ORDER BY conname;",
								  oid, oid);
			}
			else
			{
				printfPQExpBuffer(&buf,
								  "SELECT conname, conrelid::pg_catalog.regclass AS ontable,\n"
								  "       pg_catalog.pg_get_constraintdef(oid, true) AS condef\n"
								  "  FROM pg_catalog.pg_constraint\n"
								  " WHERE confrelid = %s AND contype = " CppAsString2(CONSTRAINT_FOREIGN) "\n"
								  "ORDER BY conname;",
								  oid);
			}

			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else
				tuples = PQntuples(result);

			if (tuples > 0)
			{
				int			i_conname = PQfnumber(result, "conname"),
							i_ontable = PQfnumber(result, "ontable"),
							i_condef = PQfnumber(result, "condef");

				printTableAddFooter(&cont, _("Referenced by:"));
				for (i = 0; i < tuples; i++)
				{
					printfPQExpBuffer(&buf, "    TABLE \"%s\" CONSTRAINT \"%s\" %s",
									  PQgetvalue(result, i, i_ontable),
									  PQgetvalue(result, i, i_conname),
									  PQgetvalue(result, i, i_condef));

					printTableAddFooter(&cont, buf.data);
				}
			}
			PQclear(result);
		}

		/* print any row-level policies */
		if (pset.sversion >= 90500)
		{
			printfPQExpBuffer(&buf, "SELECT pol.polname,");
			if (pset.sversion >= 100000)
				appendPQExpBufferStr(&buf,
									 " pol.polpermissive,\n");
			else
				appendPQExpBufferStr(&buf,
									 " 't' as polpermissive,\n");
			appendPQExpBuffer(&buf,
							  "  CASE WHEN pol.polroles = '{0}' THEN NULL ELSE pg_catalog.array_to_string(array(select rolname from pg_catalog.pg_roles where oid = any (pol.polroles) order by 1),',') END,\n"
							  "  pg_catalog.pg_get_expr(pol.polqual, pol.polrelid),\n"
							  "  pg_catalog.pg_get_expr(pol.polwithcheck, pol.polrelid),\n"
							  "  CASE pol.polcmd\n"
							  "    WHEN 'r' THEN 'SELECT'\n"
							  "    WHEN 'a' THEN 'INSERT'\n"
							  "    WHEN 'w' THEN 'UPDATE'\n"
							  "    WHEN 'd' THEN 'DELETE'\n"
							  "    END AS cmd\n"
							  "FROM pg_catalog.pg_policy pol\n"
							  "WHERE pol.polrelid = '%s' ORDER BY 1;",
							  oid);

			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else
				tuples = PQntuples(result);

			/*
			 * Handle cases where RLS is enabled and there are policies, or
			 * there aren't policies, or RLS isn't enabled but there are
			 * policies
			 */
			if (tableinfo.rowsecurity && !tableinfo.forcerowsecurity && tuples > 0)
				printTableAddFooter(&cont, _("Policies:"));

			if (tableinfo.rowsecurity && tableinfo.forcerowsecurity && tuples > 0)
				printTableAddFooter(&cont, _("Policies (forced row security enabled):"));

			if (tableinfo.rowsecurity && !tableinfo.forcerowsecurity && tuples == 0)
				printTableAddFooter(&cont, _("Policies (row security enabled): (none)"));

			if (tableinfo.rowsecurity && tableinfo.forcerowsecurity && tuples == 0)
				printTableAddFooter(&cont, _("Policies (forced row security enabled): (none)"));

			if (!tableinfo.rowsecurity && tuples > 0)
				printTableAddFooter(&cont, _("Policies (row security disabled):"));

			/* Might be an empty set - that's ok */
			for (i = 0; i < tuples; i++)
			{
				printfPQExpBuffer(&buf, "    POLICY \"%s\"",
								  PQgetvalue(result, i, 0));

				if (*(PQgetvalue(result, i, 1)) == 'f')
					appendPQExpBufferStr(&buf, " AS RESTRICTIVE");

				if (!PQgetisnull(result, i, 5))
					appendPQExpBuffer(&buf, " FOR %s",
									  PQgetvalue(result, i, 5));

				if (!PQgetisnull(result, i, 2))
				{
					appendPQExpBuffer(&buf, "\n      TO %s",
									  PQgetvalue(result, i, 2));
				}

				if (!PQgetisnull(result, i, 3))
					appendPQExpBuffer(&buf, "\n      USING (%s)",
									  PQgetvalue(result, i, 3));

				if (!PQgetisnull(result, i, 4))
					appendPQExpBuffer(&buf, "\n      WITH CHECK (%s)",
									  PQgetvalue(result, i, 4));

				printTableAddFooter(&cont, buf.data);
			}
			PQclear(result);
		}

		/* print any extended statistics */
		if (pset.sversion >= 140000)
		{
			printfPQExpBuffer(&buf,
							  "SELECT oid, "
							  "stxrelid::pg_catalog.regclass, "
							  "stxnamespace::pg_catalog.regnamespace::pg_catalog.text AS nsp, "
							  "stxname,\n"
							  "pg_catalog.pg_get_statisticsobjdef_columns(oid) AS columns,\n"
							  "  " CppAsString2(STATS_EXT_NDISTINCT) " = any(stxkind) AS ndist_enabled,\n"
							  "  " CppAsString2(STATS_EXT_DEPENDENCIES) " = any(stxkind) AS deps_enabled,\n"
							  "  " CppAsString2(STATS_EXT_MCV) " = any(stxkind) AS mcv_enabled,\n"
							  "stxstattarget\n"
							  "FROM pg_catalog.pg_statistic_ext\n"
							  "WHERE stxrelid = '%s'\n"
							  "ORDER BY nsp, stxname;",
							  oid);

			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else
				tuples = PQntuples(result);

			if (tuples > 0)
			{
				printTableAddFooter(&cont, _("Statistics objects:"));

				for (i = 0; i < tuples; i++)
				{
					bool		gotone = false;
					bool		has_ndistinct;
					bool		has_dependencies;
					bool		has_mcv;
					bool		has_all;
					bool		has_some;

					has_ndistinct = (strcmp(PQgetvalue(result, i, 5), "t") == 0);
					has_dependencies = (strcmp(PQgetvalue(result, i, 6), "t") == 0);
					has_mcv = (strcmp(PQgetvalue(result, i, 7), "t") == 0);

					printfPQExpBuffer(&buf, "    ");

					/* statistics object name (qualified with namespace) */
					appendPQExpBuffer(&buf, "\"%s.%s\"",
									  PQgetvalue(result, i, 2),
									  PQgetvalue(result, i, 3));

					/*
					 * When printing kinds we ignore expression statistics,
					 * which are used only internally and can't be specified
					 * by user. We don't print the kinds when none are
					 * specified (in which case it has to be statistics on a
					 * single expr) or when all are specified (in which case
					 * we assume it's expanded by CREATE STATISTICS).
					 */
					has_all = (has_ndistinct && has_dependencies && has_mcv);
					has_some = (has_ndistinct || has_dependencies || has_mcv);

					if (has_some && !has_all)
					{
						appendPQExpBufferStr(&buf, " (");

						/* options */
						if (has_ndistinct)
						{
							appendPQExpBufferStr(&buf, "ndistinct");
							gotone = true;
						}

						if (has_dependencies)
						{
							appendPQExpBuffer(&buf, "%sdependencies", gotone ? ", " : "");
							gotone = true;
						}

						if (has_mcv)
						{
							appendPQExpBuffer(&buf, "%smcv", gotone ? ", " : "");
						}

						appendPQExpBufferChar(&buf, ')');
					}

					appendPQExpBuffer(&buf, " ON %s FROM %s",
									  PQgetvalue(result, i, 4),
									  PQgetvalue(result, i, 1));

					/* Show the stats target if it's not default */
					if (!PQgetisnull(result, i, 8) &&
						strcmp(PQgetvalue(result, i, 8), "-1") != 0)
						appendPQExpBuffer(&buf, "; STATISTICS %s",
										  PQgetvalue(result, i, 8));

					printTableAddFooter(&cont, buf.data);
				}
			}
			PQclear(result);
		}
		else if (pset.sversion >= 100000)
		{
			printfPQExpBuffer(&buf,
							  "SELECT oid, "
							  "stxrelid::pg_catalog.regclass, "
							  "stxnamespace::pg_catalog.regnamespace AS nsp, "
							  "stxname,\n"
							  "  (SELECT pg_catalog.string_agg(pg_catalog.quote_ident(attname),', ')\n"
							  "   FROM pg_catalog.unnest(stxkeys) s(attnum)\n"
							  "   JOIN pg_catalog.pg_attribute a ON (stxrelid = a.attrelid AND\n"
							  "        a.attnum = s.attnum AND NOT attisdropped)) AS columns,\n"
							  "  " CppAsString2(STATS_EXT_NDISTINCT) " = any(stxkind) AS ndist_enabled,\n"
							  "  " CppAsString2(STATS_EXT_DEPENDENCIES) " = any(stxkind) AS deps_enabled,\n"
							  "  " CppAsString2(STATS_EXT_MCV) " = any(stxkind) AS mcv_enabled,\n");

			if (pset.sversion >= 130000)
				appendPQExpBufferStr(&buf, "  stxstattarget\n");
			else
				appendPQExpBufferStr(&buf, "  -1 AS stxstattarget\n");
			appendPQExpBuffer(&buf, "FROM pg_catalog.pg_statistic_ext\n"
							  "WHERE stxrelid = '%s'\n"
							  "ORDER BY 1;",
							  oid);

			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else
				tuples = PQntuples(result);

			if (tuples > 0)
			{
				printTableAddFooter(&cont, _("Statistics objects:"));

				for (i = 0; i < tuples; i++)
				{
					bool		gotone = false;

					printfPQExpBuffer(&buf, "    ");

					/* statistics object name (qualified with namespace) */
					appendPQExpBuffer(&buf, "\"%s.%s\" (",
									  PQgetvalue(result, i, 2),
									  PQgetvalue(result, i, 3));

					/* options */
					if (strcmp(PQgetvalue(result, i, 5), "t") == 0)
					{
						appendPQExpBufferStr(&buf, "ndistinct");
						gotone = true;
					}

					if (strcmp(PQgetvalue(result, i, 6), "t") == 0)
					{
						appendPQExpBuffer(&buf, "%sdependencies", gotone ? ", " : "");
						gotone = true;
					}

					if (strcmp(PQgetvalue(result, i, 7), "t") == 0)
					{
						appendPQExpBuffer(&buf, "%smcv", gotone ? ", " : "");
					}

					appendPQExpBuffer(&buf, ") ON %s FROM %s",
									  PQgetvalue(result, i, 4),
									  PQgetvalue(result, i, 1));

					/* Show the stats target if it's not default */
					if (strcmp(PQgetvalue(result, i, 8), "-1") != 0)
						appendPQExpBuffer(&buf, "; STATISTICS %s",
										  PQgetvalue(result, i, 8));

					printTableAddFooter(&cont, buf.data);
				}
			}
			PQclear(result);
		}

		/* print rules */
		if (tableinfo.hasrules && tableinfo.relkind != RELKIND_MATVIEW)
		{
			printfPQExpBuffer(&buf,
							  "SELECT r.rulename, trim(trailing ';' from pg_catalog.pg_get_ruledef(r.oid, true)), "
							  "ev_enabled\n"
							  "FROM pg_catalog.pg_rewrite r\n"
							  "WHERE r.ev_class = '%s' ORDER BY 1;",
							  oid);
			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else
				tuples = PQntuples(result);

			if (tuples > 0)
			{
				bool		have_heading;
				int			category;

				for (category = 0; category < 4; category++)
				{
					have_heading = false;

					for (i = 0; i < tuples; i++)
					{
						const char *ruledef;
						bool		list_rule = false;

						switch (category)
						{
							case 0:
								if (*PQgetvalue(result, i, 2) == 'O')
									list_rule = true;
								break;
							case 1:
								if (*PQgetvalue(result, i, 2) == 'D')
									list_rule = true;
								break;
							case 2:
								if (*PQgetvalue(result, i, 2) == 'A')
									list_rule = true;
								break;
							case 3:
								if (*PQgetvalue(result, i, 2) == 'R')
									list_rule = true;
								break;
						}
						if (!list_rule)
							continue;

						if (!have_heading)
						{
							switch (category)
							{
								case 0:
									printfPQExpBuffer(&buf, _("Rules:"));
									break;
								case 1:
									printfPQExpBuffer(&buf, _("Disabled rules:"));
									break;
								case 2:
									printfPQExpBuffer(&buf, _("Rules firing always:"));
									break;
								case 3:
									printfPQExpBuffer(&buf, _("Rules firing on replica only:"));
									break;
							}
							printTableAddFooter(&cont, buf.data);
							have_heading = true;
						}

						/* Everything after "CREATE RULE" is echoed verbatim */
						ruledef = PQgetvalue(result, i, 1);
						ruledef += 12;
						printfPQExpBuffer(&buf, "    %s", ruledef);
						printTableAddFooter(&cont, buf.data);
					}
				}
			}
			PQclear(result);
		}

		/* print any publications */
		if (pset.sversion >= 100000)
		{
			if (pset.sversion >= 150000)
			{
				printfPQExpBuffer(&buf,
								  "SELECT pubname\n"
								  "     , NULL\n"
								  "     , NULL\n"
								  "FROM pg_catalog.pg_publication p\n"
								  "     JOIN pg_catalog.pg_publication_namespace pn ON p.oid = pn.pnpubid\n"
								  "     JOIN pg_catalog.pg_class pc ON pc.relnamespace = pn.pnnspid\n"
								  "WHERE pc.oid ='%s' and pg_catalog.pg_relation_is_publishable('%s')\n"
								  "UNION\n"
								  "SELECT pubname\n"
								  "     , pg_get_expr(pr.prqual, c.oid)\n"
								  "     , (CASE WHEN pr.prattrs IS NOT NULL THEN\n"
								  "         (SELECT string_agg(attname, ', ')\n"
								  "           FROM pg_catalog.generate_series(0, pg_catalog.array_upper(pr.prattrs::pg_catalog.int2[], 1)) s,\n"
								  "                pg_catalog.pg_attribute\n"
								  "          WHERE attrelid = pr.prrelid AND attnum = prattrs[s])\n"
								  "        ELSE NULL END) "
								  "FROM pg_catalog.pg_publication p\n"
								  "     JOIN pg_catalog.pg_publication_rel pr ON p.oid = pr.prpubid\n"
								  "     JOIN pg_catalog.pg_class c ON c.oid = pr.prrelid\n"
								  "WHERE pr.prrelid = '%s'\n"
								  "UNION\n"
								  "SELECT pubname\n"
								  "     , NULL\n"
								  "     , NULL\n"
								  "FROM pg_catalog.pg_publication p\n"
								  "WHERE p.puballtables AND pg_catalog.pg_relation_is_publishable('%s')\n"
								  "ORDER BY 1;",
								  oid, oid, oid, oid);
			}
			else
			{
				printfPQExpBuffer(&buf,
								  "SELECT pubname\n"
								  "     , NULL\n"
								  "     , NULL\n"
								  "FROM pg_catalog.pg_publication p\n"
								  "JOIN pg_catalog.pg_publication_rel pr ON p.oid = pr.prpubid\n"
								  "WHERE pr.prrelid = '%s'\n"
								  "UNION ALL\n"
								  "SELECT pubname\n"
								  "     , NULL\n"
								  "     , NULL\n"
								  "FROM pg_catalog.pg_publication p\n"
								  "WHERE p.puballtables AND pg_catalog.pg_relation_is_publishable('%s')\n"
								  "ORDER BY 1;",
								  oid, oid);
			}

			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else
				tuples = PQntuples(result);

			if (tuples > 0)
				printTableAddFooter(&cont, _("Publications:"));

			/* Might be an empty set - that's ok */
			for (i = 0; i < tuples; i++)
			{
				printfPQExpBuffer(&buf, "    \"%s\"",
								  PQgetvalue(result, i, 0));

				/* column list (if any) */
				if (!PQgetisnull(result, i, 2))
					appendPQExpBuffer(&buf, " (%s)",
									  PQgetvalue(result, i, 2));

				/* row filter (if any) */
				if (!PQgetisnull(result, i, 1))
					appendPQExpBuffer(&buf, " WHERE %s",
									  PQgetvalue(result, i, 1));

				printTableAddFooter(&cont, buf.data);
			}
			PQclear(result);
		}

		/*
		 * If verbose, print NOT NULL constraints.
		 */
		if (verbose)
		{
			printfPQExpBuffer(&buf,
							  "SELECT c.conname, a.attname, c.connoinherit,\n"
							  "  c.conislocal, c.coninhcount <> 0\n"
							  "FROM pg_catalog.pg_constraint c JOIN\n"
							  "  pg_catalog.pg_attribute a ON\n"
							  "    (a.attrelid = c.conrelid AND a.attnum = c.conkey[1])\n"
							  "WHERE c.contype = " CppAsString2(CONSTRAINT_NOTNULL) " AND\n"
							  "  c.conrelid = '%s'::pg_catalog.regclass\n"
							  "ORDER BY a.attnum",
							  oid);

			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else
				tuples = PQntuples(result);

			if (tuples > 0)
				printTableAddFooter(&cont, _("Not-null constraints:"));

			/* Might be an empty set - that's ok */
			for (i = 0; i < tuples; i++)
			{
				bool		islocal = PQgetvalue(result, i, 3)[0] == 't';
				bool		inherited = PQgetvalue(result, i, 4)[0] == 't';

				printfPQExpBuffer(&buf, "    \"%s\" NOT NULL \"%s\"%s",
								  PQgetvalue(result, i, 0),
								  PQgetvalue(result, i, 1),
								  PQgetvalue(result, i, 2)[0] == 't' ?
								  " NO INHERIT" :
								  islocal && inherited ? _(" (local, inherited)") :
								  inherited ? _(" (inherited)") : "");

				printTableAddFooter(&cont, buf.data);
			}
			PQclear(result);
		}
	}

	/* Get view_def if table is a view or materialized view */
	if ((tableinfo.relkind == RELKIND_VIEW ||
		 tableinfo.relkind == RELKIND_MATVIEW) && verbose)
	{
		PGresult   *result;

		printfPQExpBuffer(&buf,
						  "SELECT pg_catalog.pg_get_viewdef('%s'::pg_catalog.oid, true);",
						  oid);
		result = PSQLexec(buf.data);
		if (!result)
			goto error_return;

		if (PQntuples(result) > 0)
			view_def = pg_strdup(PQgetvalue(result, 0, 0));

		PQclear(result);
	}

	if (view_def)
	{
		PGresult   *result = NULL;

		/* Footer information about a view */
		printTableAddFooter(&cont, _("View definition:"));
		printTableAddFooter(&cont, view_def);

		/* print rules */
		if (tableinfo.hasrules)
		{
			printfPQExpBuffer(&buf,
							  "SELECT r.rulename, trim(trailing ';' from pg_catalog.pg_get_ruledef(r.oid, true))\n"
							  "FROM pg_catalog.pg_rewrite r\n"
							  "WHERE r.ev_class = '%s' AND r.rulename != '_RETURN' ORDER BY 1;",
							  oid);
			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;

			if (PQntuples(result) > 0)
			{
				printTableAddFooter(&cont, _("Rules:"));
				for (i = 0; i < PQntuples(result); i++)
				{
					const char *ruledef;

					/* Everything after "CREATE RULE" is echoed verbatim */
					ruledef = PQgetvalue(result, i, 1);
					ruledef += 12;

					printfPQExpBuffer(&buf, " %s", ruledef);
					printTableAddFooter(&cont, buf.data);
				}
			}
			PQclear(result);
		}
	}

	/*
	 * Print triggers next, if any (but only user-defined triggers).  This
	 * could apply to either a table or a view.
	 */
	if (tableinfo.hastriggers)
	{
		PGresult   *result;
		int			tuples;

		printfPQExpBuffer(&buf,
						  "SELECT t.tgname, "
						  "pg_catalog.pg_get_triggerdef(t.oid, true), "
						  "t.tgenabled, t.tgisinternal,\n");

		/*
		 * Detect whether each trigger is inherited, and if so, get the name
		 * of the topmost table it's inherited from.  We have no easy way to
		 * do that pre-v13, for lack of the tgparentid column.  Even with
		 * tgparentid, a straightforward search for the topmost parent would
		 * require a recursive CTE, which seems unduly expensive.  We cheat a
		 * bit by assuming parent triggers will match by tgname; then, joining
		 * with pg_partition_ancestors() allows the planner to make use of
		 * pg_trigger_tgrelid_tgname_index if it wishes.  We ensure we find
		 * the correct topmost parent by stopping at the first-in-partition-
		 * ancestry-order trigger that has tgparentid = 0.  (There might be
		 * unrelated, non-inherited triggers with the same name further up the
		 * stack, so this is important.)
		 */
		if (pset.sversion >= 130000)
			appendPQExpBufferStr(&buf,
								 "  CASE WHEN t.tgparentid != 0 THEN\n"
								 "    (SELECT u.tgrelid::pg_catalog.regclass\n"
								 "     FROM pg_catalog.pg_trigger AS u,\n"
								 "          pg_catalog.pg_partition_ancestors(t.tgrelid) WITH ORDINALITY AS a(relid, depth)\n"
								 "     WHERE u.tgname = t.tgname AND u.tgrelid = a.relid\n"
								 "           AND u.tgparentid = 0\n"
								 "     ORDER BY a.depth LIMIT 1)\n"
								 "  END AS parent\n");
		else
			appendPQExpBufferStr(&buf, "  NULL AS parent\n");

		appendPQExpBuffer(&buf,
						  "FROM pg_catalog.pg_trigger t\n"
						  "WHERE t.tgrelid = '%s' AND ",
						  oid);

		/*
		 * tgisinternal is set true for inherited triggers of partitions in
		 * servers between v11 and v14, though these must still be shown to
		 * the user.  So we use another property that is true for such
		 * inherited triggers to avoid them being hidden, which is their
		 * dependence on another trigger.
		 */
		if (pset.sversion >= 110000 && pset.sversion < 150000)
			appendPQExpBufferStr(&buf, "(NOT t.tgisinternal OR (t.tgisinternal AND t.tgenabled = 'D') \n"
								 "    OR EXISTS (SELECT 1 FROM pg_catalog.pg_depend WHERE objid = t.oid \n"
								 "        AND refclassid = 'pg_catalog.pg_trigger'::pg_catalog.regclass))");
		else
			/* display/warn about disabled internal triggers */
			appendPQExpBufferStr(&buf, "(NOT t.tgisinternal OR (t.tgisinternal AND t.tgenabled = 'D'))");
		appendPQExpBufferStr(&buf, "\nORDER BY 1;");

		result = PSQLexec(buf.data);
		if (!result)
			goto error_return;
		else
			tuples = PQntuples(result);

		if (tuples > 0)
		{
			bool		have_heading;
			int			category;

			/*
			 * split the output into 4 different categories. Enabled triggers,
			 * disabled triggers and the two special ALWAYS and REPLICA
			 * configurations.
			 */
			for (category = 0; category <= 4; category++)
			{
				have_heading = false;
				for (i = 0; i < tuples; i++)
				{
					bool		list_trigger;
					const char *tgdef;
					const char *usingpos;
					const char *tgenabled;
					const char *tgisinternal;

					/*
					 * Check if this trigger falls into the current category
					 */
					tgenabled = PQgetvalue(result, i, 2);
					tgisinternal = PQgetvalue(result, i, 3);
					list_trigger = false;
					switch (category)
					{
						case 0:
							if (*tgenabled == 'O' || *tgenabled == 't')
								list_trigger = true;
							break;
						case 1:
							if ((*tgenabled == 'D' || *tgenabled == 'f') &&
								*tgisinternal == 'f')
								list_trigger = true;
							break;
						case 2:
							if ((*tgenabled == 'D' || *tgenabled == 'f') &&
								*tgisinternal == 't')
								list_trigger = true;
							break;
						case 3:
							if (*tgenabled == 'A')
								list_trigger = true;
							break;
						case 4:
							if (*tgenabled == 'R')
								list_trigger = true;
							break;
					}
					if (list_trigger == false)
						continue;

					/* Print the category heading once */
					if (have_heading == false)
					{
						switch (category)
						{
							case 0:
								printfPQExpBuffer(&buf, _("Triggers:"));
								break;
							case 1:
								printfPQExpBuffer(&buf, _("Disabled user triggers:"));
								break;
							case 2:
								printfPQExpBuffer(&buf, _("Disabled internal triggers:"));
								break;
							case 3:
								printfPQExpBuffer(&buf, _("Triggers firing always:"));
								break;
							case 4:
								printfPQExpBuffer(&buf, _("Triggers firing on replica only:"));
								break;
						}
						printTableAddFooter(&cont, buf.data);
						have_heading = true;
					}

					/* Everything after "TRIGGER" is echoed verbatim */
					tgdef = PQgetvalue(result, i, 1);
					usingpos = strstr(tgdef, " TRIGGER ");
					if (usingpos)
						tgdef = usingpos + 9;

					printfPQExpBuffer(&buf, "    %s", tgdef);

					/* Visually distinguish inherited triggers */
					if (!PQgetisnull(result, i, 4))
						appendPQExpBuffer(&buf, ", ON TABLE %s",
										  PQgetvalue(result, i, 4));

					printTableAddFooter(&cont, buf.data);
				}
			}
		}
		PQclear(result);
	}

	/*
	 * Finish printing the footer information about a table.
	 */
	if (tableinfo.relkind == RELKIND_RELATION ||
		tableinfo.relkind == RELKIND_MATVIEW ||
		tableinfo.relkind == RELKIND_FOREIGN_TABLE ||
		tableinfo.relkind == RELKIND_PARTITIONED_TABLE ||
		tableinfo.relkind == RELKIND_PARTITIONED_INDEX ||
		tableinfo.relkind == RELKIND_TOASTVALUE)
	{
		bool		is_partitioned;
		PGresult   *result;
		int			tuples;

		/* simplify some repeated tests below */
		is_partitioned = (tableinfo.relkind == RELKIND_PARTITIONED_TABLE ||
						  tableinfo.relkind == RELKIND_PARTITIONED_INDEX);

		/* print foreign server name */
		if (tableinfo.relkind == RELKIND_FOREIGN_TABLE)
		{
			char	   *ftoptions;

			/* Footer information about foreign table */
			printfPQExpBuffer(&buf,
							  "SELECT s.srvname,\n"
							  "  pg_catalog.array_to_string(ARRAY(\n"
							  "    SELECT pg_catalog.quote_ident(option_name)"
							  " || ' ' || pg_catalog.quote_literal(option_value)\n"
							  "    FROM pg_catalog.pg_options_to_table(ftoptions)),  ', ')\n"
							  "FROM pg_catalog.pg_foreign_table f,\n"
							  "     pg_catalog.pg_foreign_server s\n"
							  "WHERE f.ftrelid = '%s' AND s.oid = f.ftserver;",
							  oid);
			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else if (PQntuples(result) != 1)
			{
				PQclear(result);
				goto error_return;
			}

			/* Print server name */
			printfPQExpBuffer(&buf, _("Server: %s"),
							  PQgetvalue(result, 0, 0));
			printTableAddFooter(&cont, buf.data);

			/* Print per-table FDW options, if any */
			ftoptions = PQgetvalue(result, 0, 1);
			if (ftoptions && ftoptions[0] != '\0')
			{
				printfPQExpBuffer(&buf, _("FDW options: (%s)"), ftoptions);
				printTableAddFooter(&cont, buf.data);
			}
			PQclear(result);
		}

		/* print tables inherited from (exclude partitioned parents) */
		printfPQExpBuffer(&buf,
						  "SELECT c.oid::pg_catalog.regclass\n"
						  "FROM pg_catalog.pg_class c, pg_catalog.pg_inherits i\n"
						  "WHERE c.oid = i.inhparent AND i.inhrelid = '%s'\n"
						  "  AND c.relkind != " CppAsString2(RELKIND_PARTITIONED_TABLE)
						  " AND c.relkind != " CppAsString2(RELKIND_PARTITIONED_INDEX)
						  "\nORDER BY inhseqno;",
						  oid);

		result = PSQLexec(buf.data);
		if (!result)
			goto error_return;
		else
		{
			const char *s = _("Inherits");
			int			sw = pg_wcswidth(s, strlen(s), pset.encoding);

			tuples = PQntuples(result);

			for (i = 0; i < tuples; i++)
			{
				if (i == 0)
					printfPQExpBuffer(&buf, "%s: %s",
									  s, PQgetvalue(result, i, 0));
				else
					printfPQExpBuffer(&buf, "%*s  %s",
									  sw, "", PQgetvalue(result, i, 0));
				if (i < tuples - 1)
					appendPQExpBufferChar(&buf, ',');

				printTableAddFooter(&cont, buf.data);
			}

			PQclear(result);
		}

		/* print child tables (with additional info if partitions) */
		if (pset.sversion >= 140000)
			printfPQExpBuffer(&buf,
							  "SELECT c.oid::pg_catalog.regclass, c.relkind,"
							  " inhdetachpending,"
							  " pg_catalog.pg_get_expr(c.relpartbound, c.oid)\n"
							  "FROM pg_catalog.pg_class c, pg_catalog.pg_inherits i\n"
							  "WHERE c.oid = i.inhrelid AND i.inhparent = '%s'\n"
							  "ORDER BY pg_catalog.pg_get_expr(c.relpartbound, c.oid) = 'DEFAULT',"
							  " c.oid::pg_catalog.regclass::pg_catalog.text;",
							  oid);
		else if (pset.sversion >= 100000)
			printfPQExpBuffer(&buf,
							  "SELECT c.oid::pg_catalog.regclass, c.relkind,"
							  " false AS inhdetachpending,"
							  " pg_catalog.pg_get_expr(c.relpartbound, c.oid)\n"
							  "FROM pg_catalog.pg_class c, pg_catalog.pg_inherits i\n"
							  "WHERE c.oid = i.inhrelid AND i.inhparent = '%s'\n"
							  "ORDER BY pg_catalog.pg_get_expr(c.relpartbound, c.oid) = 'DEFAULT',"
							  " c.oid::pg_catalog.regclass::pg_catalog.text;",
							  oid);
		else
			printfPQExpBuffer(&buf,
							  "SELECT c.oid::pg_catalog.regclass, c.relkind,"
							  " false AS inhdetachpending, NULL\n"
							  "FROM pg_catalog.pg_class c, pg_catalog.pg_inherits i\n"
							  "WHERE c.oid = i.inhrelid AND i.inhparent = '%s'\n"
							  "ORDER BY c.oid::pg_catalog.regclass::pg_catalog.text;",
							  oid);

		result = PSQLexec(buf.data);
		if (!result)
			goto error_return;
		tuples = PQntuples(result);

		/*
		 * For a partitioned table with no partitions, always print the number
		 * of partitions as zero, even when verbose output is expected.
		 * Otherwise, we will not print "Partitions" section for a partitioned
		 * table without any partitions.
		 */
		if (is_partitioned && tuples == 0)
		{
			printfPQExpBuffer(&buf, _("Number of partitions: %d"), tuples);
			printTableAddFooter(&cont, buf.data);
		}
		else if (!verbose)
		{
			/* print the number of child tables, if any */
			if (tuples > 0)
			{
				if (is_partitioned)
					printfPQExpBuffer(&buf, _("Number of partitions: %d (Use \\d+ to list them.)"), tuples);
				else
					printfPQExpBuffer(&buf, _("Number of child tables: %d (Use \\d+ to list them.)"), tuples);
				printTableAddFooter(&cont, buf.data);
			}
		}
		else
		{
			/* display the list of child tables */
			const char *ct = is_partitioned ? _("Partitions") : _("Child tables");
			int			ctw = pg_wcswidth(ct, strlen(ct), pset.encoding);

			for (i = 0; i < tuples; i++)
			{
				char		child_relkind = *PQgetvalue(result, i, 1);

				if (i == 0)
					printfPQExpBuffer(&buf, "%s: %s",
									  ct, PQgetvalue(result, i, 0));
				else
					printfPQExpBuffer(&buf, "%*s  %s",
									  ctw, "", PQgetvalue(result, i, 0));
				if (!PQgetisnull(result, i, 3))
					appendPQExpBuffer(&buf, " %s", PQgetvalue(result, i, 3));
				if (child_relkind == RELKIND_PARTITIONED_TABLE ||
					child_relkind == RELKIND_PARTITIONED_INDEX)
					appendPQExpBufferStr(&buf, ", PARTITIONED");
				else if (child_relkind == RELKIND_FOREIGN_TABLE)
					appendPQExpBufferStr(&buf, ", FOREIGN");
				if (strcmp(PQgetvalue(result, i, 2), "t") == 0)
					appendPQExpBufferStr(&buf, " (DETACH PENDING)");
				if (i < tuples - 1)
					appendPQExpBufferChar(&buf, ',');

				printTableAddFooter(&cont, buf.data);
			}
		}
		PQclear(result);

		/* Table type */
		if (tableinfo.reloftype)
		{
			printfPQExpBuffer(&buf, _("Typed table of type: %s"), tableinfo.reloftype);
			printTableAddFooter(&cont, buf.data);
		}

		if (verbose &&
			(tableinfo.relkind == RELKIND_RELATION ||
			 tableinfo.relkind == RELKIND_MATVIEW) &&

		/*
		 * No need to display default values; we already display a REPLICA
		 * IDENTITY marker on indexes.
		 */
			tableinfo.relreplident != REPLICA_IDENTITY_INDEX &&
			((strcmp(schemaname, "pg_catalog") != 0 &&
			  tableinfo.relreplident != REPLICA_IDENTITY_DEFAULT) ||
			 (strcmp(schemaname, "pg_catalog") == 0 &&
			  tableinfo.relreplident != REPLICA_IDENTITY_NOTHING)))
		{
			const char *s = _("Replica Identity");

			printfPQExpBuffer(&buf, "%s: %s",
							  s,
							  tableinfo.relreplident == REPLICA_IDENTITY_FULL ? "FULL" :
							  tableinfo.relreplident == REPLICA_IDENTITY_DEFAULT ? "NOTHING" :
							  "???");

			printTableAddFooter(&cont, buf.data);
		}

		/* OIDs, if verbose and not a materialized view */
		if (verbose && tableinfo.relkind != RELKIND_MATVIEW && tableinfo.hasoids)
			printTableAddFooter(&cont, _("Has OIDs: yes"));

		/* Tablespace info */
		add_tablespace_footer(&cont, tableinfo.relkind, tableinfo.tablespace,
							  true);

		/* Access method info */
		if (verbose && tableinfo.relam != NULL && !pset.hide_tableam)
		{
			printfPQExpBuffer(&buf, _("Access method: %s"), tableinfo.relam);
			printTableAddFooter(&cont, buf.data);
		}
	}

	/* reloptions, if verbose */
	if (verbose &&
		tableinfo.reloptions && tableinfo.reloptions[0] != '\0')
	{
		const char *t = _("Options");

		printfPQExpBuffer(&buf, "%s: %s", t, tableinfo.reloptions);
		printTableAddFooter(&cont, buf.data);
	}

	printTable(&cont, pset.queryFout, false, pset.logfile);

	retval = true;

error_return:

	/* clean up */
	if (printTableInitialized)
		printTableCleanup(&cont);
	termPQExpBuffer(&buf);
	termPQExpBuffer(&title);
	termPQExpBuffer(&tmpbuf);

	free(view_def);

	PQclear(res);

	return retval;
}

/*
 * Add a tablespace description to a footer.  If 'newline' is true, it is added
 * in a new line; otherwise it's appended to the current value of the last
 * footer.
 */
static void
add_tablespace_footer(printTableContent *const cont, char relkind,
					  Oid tablespace, const bool newline)
{
	/* relkinds for which we support tablespaces */
	if (relkind == RELKIND_RELATION ||
		relkind == RELKIND_MATVIEW ||
		relkind == RELKIND_INDEX ||
		relkind == RELKIND_PARTITIONED_TABLE ||
		relkind == RELKIND_PARTITIONED_INDEX ||
		relkind == RELKIND_TOASTVALUE)
	{
		/*
		 * We ignore the database default tablespace so that users not using
		 * tablespaces don't need to know about them.
		 */
		if (tablespace != 0)
		{
			PGresult   *result = NULL;
			PQExpBufferData buf;

			initPQExpBuffer(&buf);
			printfPQExpBuffer(&buf,
							  "SELECT spcname FROM pg_catalog.pg_tablespace\n"
							  "WHERE oid = '%u';", tablespace);
			result = PSQLexec(buf.data);
			if (!result)
			{
				termPQExpBuffer(&buf);
				return;
			}
			/* Should always be the case, but.... */
			if (PQntuples(result) > 0)
			{
				if (newline)
				{
					/* Add the tablespace as a new footer */
					printfPQExpBuffer(&buf, _("Tablespace: \"%s\""),
									  PQgetvalue(result, 0, 0));
					printTableAddFooter(cont, buf.data);
				}
				else
				{
					/* Append the tablespace to the latest footer */
					printfPQExpBuffer(&buf, "%s", cont->footer->data);

					/*-------
					   translator: before this string there's an index description like
					   '"foo_pkey" PRIMARY KEY, btree (a)' */
					appendPQExpBuffer(&buf, _(", tablespace \"%s\""),
									  PQgetvalue(result, 0, 0));
					printTableSetFooter(cont, buf.data);
				}
			}
			PQclear(result);
			termPQExpBuffer(&buf);
		}
	}
}

/*
 * \du or \dg
 *
 * Describes roles.  Any schema portion of the pattern is ignored.
 */
bool
describeRoles(const char *pattern, bool verbose, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printTableContent cont;
	printTableOpt myopt = pset.popt.topt;
	int			ncols = 2;
	int			nrows = 0;
	int			i;
	int			conns;
	const char	align = 'l';
	char	  **attr;

	myopt.default_footer = false;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT r.rolname, r.rolsuper, r.rolinherit,\n"
					  "  r.rolcreaterole, r.rolcreatedb, r.rolcanlogin,\n"
					  "  r.rolconnlimit, r.rolvaliduntil");

	if (verbose)
	{
		appendPQExpBufferStr(&buf, "\n, pg_catalog.shobj_description(r.oid, 'pg_authid') AS description");
		ncols++;
	}
	appendPQExpBufferStr(&buf, "\n, r.rolreplication");

	if (pset.sversion >= 90500)
	{
		appendPQExpBufferStr(&buf, "\n, r.rolbypassrls");
	}

	appendPQExpBufferStr(&buf, "\nFROM pg_catalog.pg_roles r\n");

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "WHERE r.rolname !~ '^pg_'\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								NULL, "r.rolname", NULL, NULL,
								NULL, 1))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	if (!res)
		return false;

	nrows = PQntuples(res);
	attr = pg_malloc0((nrows + 1) * sizeof(*attr));

	printTableInit(&cont, &myopt, _("List of roles"), ncols, nrows);

	printTableAddHeader(&cont, gettext_noop("Role name"), true, align);
	printTableAddHeader(&cont, gettext_noop("Attributes"), true, align);

	if (verbose)
		printTableAddHeader(&cont, gettext_noop("Description"), true, align);

	for (i = 0; i < nrows; i++)
	{
		printTableAddCell(&cont, PQgetvalue(res, i, 0), false, false);

		resetPQExpBuffer(&buf);
		if (strcmp(PQgetvalue(res, i, 1), "t") == 0)
			add_role_attribute(&buf, _("Superuser"));

		if (strcmp(PQgetvalue(res, i, 2), "t") != 0)
			add_role_attribute(&buf, _("No inheritance"));

		if (strcmp(PQgetvalue(res, i, 3), "t") == 0)
			add_role_attribute(&buf, _("Create role"));

		if (strcmp(PQgetvalue(res, i, 4), "t") == 0)
			add_role_attribute(&buf, _("Create DB"));

		if (strcmp(PQgetvalue(res, i, 5), "t") != 0)
			add_role_attribute(&buf, _("Cannot login"));

		if (strcmp(PQgetvalue(res, i, (verbose ? 9 : 8)), "t") == 0)
			add_role_attribute(&buf, _("Replication"));

		if (pset.sversion >= 90500)
			if (strcmp(PQgetvalue(res, i, (verbose ? 10 : 9)), "t") == 0)
				add_role_attribute(&buf, _("Bypass RLS"));

		conns = atoi(PQgetvalue(res, i, 6));
		if (conns >= 0)
		{
			if (buf.len > 0)
				appendPQExpBufferChar(&buf, '\n');

			if (conns == 0)
				appendPQExpBufferStr(&buf, _("No connections"));
			else
				appendPQExpBuffer(&buf, ngettext("%d connection",
												 "%d connections",
												 conns),
								  conns);
		}

		if (strcmp(PQgetvalue(res, i, 7), "") != 0)
		{
			if (buf.len > 0)
				appendPQExpBufferChar(&buf, '\n');
			appendPQExpBufferStr(&buf, _("Password valid until "));
			appendPQExpBufferStr(&buf, PQgetvalue(res, i, 7));
		}

		attr[i] = pg_strdup(buf.data);

		printTableAddCell(&cont, attr[i], false, false);

		if (verbose)
			printTableAddCell(&cont, PQgetvalue(res, i, 8), false, false);
	}
	termPQExpBuffer(&buf);

	printTable(&cont, pset.queryFout, false, pset.logfile);
	printTableCleanup(&cont);

	for (i = 0; i < nrows; i++)
		free(attr[i]);
	free(attr);

	PQclear(res);
	return true;
}

static void
add_role_attribute(PQExpBuffer buf, const char *const str)
{
	if (buf->len > 0)
		appendPQExpBufferStr(buf, ", ");

	appendPQExpBufferStr(buf, str);
}

/*
 * \drds
 */
bool
listDbRoleSettings(const char *pattern, const char *pattern2)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	bool		havewhere;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf, "SELECT rolname AS \"%s\", datname AS \"%s\",\n"
					  "pg_catalog.array_to_string(setconfig, E'\\n') AS \"%s\"\n"
					  "FROM pg_catalog.pg_db_role_setting s\n"
					  "LEFT JOIN pg_catalog.pg_database d ON d.oid = setdatabase\n"
					  "LEFT JOIN pg_catalog.pg_roles r ON r.oid = setrole\n",
					  gettext_noop("Role"),
					  gettext_noop("Database"),
					  gettext_noop("Settings"));
	if (!validateSQLNamePattern(&buf, pattern, false, false,
								NULL, "r.rolname", NULL, NULL, &havewhere, 1))
		goto error_return;
	if (!validateSQLNamePattern(&buf, pattern2, havewhere, false,
								NULL, "d.datname", NULL, NULL,
								NULL, 1))
		goto error_return;
	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	/*
	 * Most functions in this file are content to print an empty table when
	 * there are no matching objects.  We intentionally deviate from that
	 * here, but only in !quiet mode, because of the possibility that the user
	 * is confused about what the two pattern arguments mean.
	 */
	if (PQntuples(res) == 0 && !pset.quiet)
	{
		if (pattern && pattern2)
			pg_log_error("Did not find any settings for role \"%s\" and database \"%s\".",
						 pattern, pattern2);
		else if (pattern)
			pg_log_error("Did not find any settings for role \"%s\".",
						 pattern);
		else
			pg_log_error("Did not find any settings.");
	}
	else
	{
		myopt.title = _("List of settings");
		myopt.translate_header = true;

		printQuery(res, &myopt, pset.queryFout, false, pset.logfile);
	}

	PQclear(res);
	return true;

error_return:
	termPQExpBuffer(&buf);
	return false;
}

/*
 * \drg
 * Describes role grants.
 */
bool
describeRoleGrants(const char *pattern, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT m.rolname AS \"%s\", r.rolname AS \"%s\",\n"
					  "  pg_catalog.concat_ws(', ',\n",
					  gettext_noop("Role name"),
					  gettext_noop("Member of"));

	if (pset.sversion >= 160000)
		appendPQExpBufferStr(&buf,
							 "    CASE WHEN pam.admin_option THEN 'ADMIN' END,\n"
							 "    CASE WHEN pam.inherit_option THEN 'INHERIT' END,\n"
							 "    CASE WHEN pam.set_option THEN 'SET' END\n");
	else
		appendPQExpBufferStr(&buf,
							 "    CASE WHEN pam.admin_option THEN 'ADMIN' END,\n"
							 "    CASE WHEN m.rolinherit THEN 'INHERIT' END,\n"
							 "    'SET'\n");

	appendPQExpBuffer(&buf,
					  "  ) AS \"%s\",\n"
					  "  g.rolname AS \"%s\"\n",
					  gettext_noop("Options"),
					  gettext_noop("Grantor"));

	appendPQExpBufferStr(&buf,
						 "FROM pg_catalog.pg_roles m\n"
						 "     JOIN pg_catalog.pg_auth_members pam ON (pam.member = m.oid)\n"
						 "     LEFT JOIN pg_catalog.pg_roles r ON (pam.roleid = r.oid)\n"
						 "     LEFT JOIN pg_catalog.pg_roles g ON (pam.grantor = g.oid)\n");

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "WHERE m.rolname !~ '^pg_'\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								NULL, "m.rolname", NULL, NULL,
								NULL, 1))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2, 4;\n");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of role grants");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}


/*
 * listTables()
 *
 * handler for \dt, \di, etc.
 *
 * tabtypes is an array of characters, specifying what info is desired:
 * t - tables
 * i - indexes
 * v - views
 * m - materialized views
 * s - sequences
 * E - foreign table (Note: different from 'f', the relkind value)
 * (any order of the above is fine)
 */
bool
listTables(const char *tabtypes, const char *pattern, bool verbose, bool showSystem)
{
	bool		showTables = strchr(tabtypes, 't') != NULL;
	bool		showIndexes = strchr(tabtypes, 'i') != NULL;
	bool		showViews = strchr(tabtypes, 'v') != NULL;
	bool		showMatViews = strchr(tabtypes, 'm') != NULL;
	bool		showSeq = strchr(tabtypes, 's') != NULL;
	bool		showForeign = strchr(tabtypes, 'E') != NULL;

	int			ntypes;
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	int			cols_so_far;
	bool		translate_columns[] = {false, false, true, false, false, false, false, false, false};

	/* Count the number of explicitly-requested relation types */
	ntypes = showTables + showIndexes + showViews + showMatViews +
		showSeq + showForeign;
	/* If none, we default to \dtvmsE (but see also command.c) */
	if (ntypes == 0)
		showTables = showViews = showMatViews = showSeq = showForeign = true;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					  "  c.relname as \"%s\",\n"
					  "  CASE c.relkind"
					  " WHEN " CppAsString2(RELKIND_RELATION) " THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_VIEW) " THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_MATVIEW) " THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_INDEX) " THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_SEQUENCE) " THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_TOASTVALUE) " THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_FOREIGN_TABLE) " THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_PARTITIONED_TABLE) " THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_PARTITIONED_INDEX) " THEN '%s'"
					  " END as \"%s\",\n"
					  "  pg_catalog.pg_get_userbyid(c.relowner) as \"%s\"",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("table"),
					  gettext_noop("view"),
					  gettext_noop("materialized view"),
					  gettext_noop("index"),
					  gettext_noop("sequence"),
					  gettext_noop("TOAST table"),
					  gettext_noop("foreign table"),
					  gettext_noop("partitioned table"),
					  gettext_noop("partitioned index"),
					  gettext_noop("Type"),
					  gettext_noop("Owner"));
	cols_so_far = 4;

	if (showIndexes)
	{
		appendPQExpBuffer(&buf,
						  ",\n  c2.relname as \"%s\"",
						  gettext_noop("Table"));
		cols_so_far++;
	}

	if (verbose)
	{
		/*
		 * Show whether a relation is permanent, temporary, or unlogged.
		 */
		appendPQExpBuffer(&buf,
						  ",\n  CASE c.relpersistence "
						  "WHEN " CppAsString2(RELPERSISTENCE_PERMANENT) " THEN '%s' "
						  "WHEN " CppAsString2(RELPERSISTENCE_TEMP) " THEN '%s' "
						  "WHEN " CppAsString2(RELPERSISTENCE_UNLOGGED) " THEN '%s' "
						  "END as \"%s\"",
						  gettext_noop("permanent"),
						  gettext_noop("temporary"),
						  gettext_noop("unlogged"),
						  gettext_noop("Persistence"));
		translate_columns[cols_so_far] = true;

		/*
		 * We don't bother to count cols_so_far below here, as there's no need
		 * to; this might change with future additions to the output columns.
		 */

		/*
		 * Access methods exist for tables, materialized views and indexes.
		 * This has been introduced in PostgreSQL 12 for tables.
		 */
		if (pset.sversion >= 120000 && !pset.hide_tableam &&
			(showTables || showMatViews || showIndexes))
			appendPQExpBuffer(&buf,
							  ",\n  am.amname as \"%s\"",
							  gettext_noop("Access method"));

		appendPQExpBuffer(&buf,
						  ",\n  pg_catalog.pg_size_pretty(pg_catalog.pg_table_size(c.oid)) as \"%s\""
						  ",\n  pg_catalog.obj_description(c.oid, 'pg_class') as \"%s\"",
						  gettext_noop("Size"),
						  gettext_noop("Description"));
	}

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_class c"
						 "\n     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace");

	if (pset.sversion >= 120000 && !pset.hide_tableam &&
		(showTables || showMatViews || showIndexes))
		appendPQExpBufferStr(&buf,
							 "\n     LEFT JOIN pg_catalog.pg_am am ON am.oid = c.relam");

	if (showIndexes)
		appendPQExpBufferStr(&buf,
							 "\n     LEFT JOIN pg_catalog.pg_index i ON i.indexrelid = c.oid"
							 "\n     LEFT JOIN pg_catalog.pg_class c2 ON i.indrelid = c2.oid");

	appendPQExpBufferStr(&buf, "\nWHERE c.relkind IN (");
	if (showTables)
	{
		appendPQExpBufferStr(&buf, CppAsString2(RELKIND_RELATION) ","
							 CppAsString2(RELKIND_PARTITIONED_TABLE) ",");
		/* with 'S' or a pattern, allow 't' to match TOAST tables too */
		if (showSystem || pattern)
			appendPQExpBufferStr(&buf, CppAsString2(RELKIND_TOASTVALUE) ",");
	}
	if (showViews)
		appendPQExpBufferStr(&buf, CppAsString2(RELKIND_VIEW) ",");
	if (showMatViews)
		appendPQExpBufferStr(&buf, CppAsString2(RELKIND_MATVIEW) ",");
	if (showIndexes)
		appendPQExpBufferStr(&buf, CppAsString2(RELKIND_INDEX) ","
							 CppAsString2(RELKIND_PARTITIONED_INDEX) ",");
	if (showSeq)
		appendPQExpBufferStr(&buf, CppAsString2(RELKIND_SEQUENCE) ",");
	if (showSystem || pattern)
		appendPQExpBufferStr(&buf, "'s',"); /* was RELKIND_SPECIAL */
	if (showForeign)
		appendPQExpBufferStr(&buf, CppAsString2(RELKIND_FOREIGN_TABLE) ",");

	appendPQExpBufferStr(&buf, "''");	/* dummy */
	appendPQExpBufferStr(&buf, ")\n");

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname !~ '^pg_toast'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"n.nspname", "c.relname", NULL,
								"pg_catalog.pg_table_is_visible(c.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1,2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	/*
	 * Most functions in this file are content to print an empty table when
	 * there are no matching objects.  We intentionally deviate from that
	 * here, but only in !quiet mode, for historical reasons.
	 */
	if (PQntuples(res) == 0 && !pset.quiet)
	{
		if (pattern)
		{
			if (ntypes != 1)
				pg_log_error("Did not find any relations named \"%s\".",
							 pattern);
			else if (showTables)
				pg_log_error("Did not find any tables named \"%s\".",
							 pattern);
			else if (showIndexes)
				pg_log_error("Did not find any indexes named \"%s\".",
							 pattern);
			else if (showViews)
				pg_log_error("Did not find any views named \"%s\".",
							 pattern);
			else if (showMatViews)
				pg_log_error("Did not find any materialized views named \"%s\".",
							 pattern);
			else if (showSeq)
				pg_log_error("Did not find any sequences named \"%s\".",
							 pattern);
			else if (showForeign)
				pg_log_error("Did not find any foreign tables named \"%s\".",
							 pattern);
			else				/* should not get here */
				pg_log_error_internal("Did not find any ??? named \"%s\".",
									  pattern);
		}
		else
		{
			if (ntypes != 1)
				pg_log_error("Did not find any relations.");
			else if (showTables)
				pg_log_error("Did not find any tables.");
			else if (showIndexes)
				pg_log_error("Did not find any indexes.");
			else if (showViews)
				pg_log_error("Did not find any views.");
			else if (showMatViews)
				pg_log_error("Did not find any materialized views.");
			else if (showSeq)
				pg_log_error("Did not find any sequences.");
			else if (showForeign)
				pg_log_error("Did not find any foreign tables.");
			else				/* should not get here */
				pg_log_error_internal("Did not find any ??? relations.");
		}
	}
	else
	{
		myopt.title =
			(ntypes != 1) ? _("List of relations") :
			(showTables) ? _("List of tables") :
			(showIndexes) ? _("List of indexes") :
			(showViews) ? _("List of views") :
			(showMatViews) ? _("List of materialized views") :
			(showSeq) ? _("List of sequences") :
			(showForeign) ? _("List of foreign tables") :
			"List of ???";		/* should not get here */
		myopt.translate_header = true;
		myopt.translate_columns = translate_columns;
		myopt.n_translate_columns = lengthof(translate_columns);

		printQuery(res, &myopt, pset.queryFout, false, pset.logfile);
	}

	PQclear(res);
	return true;
}

/*
 * \dP
 * Takes an optional regexp to select particular relations
 *
 * As with \d, you can specify the kinds of relations you want:
 *
 * t for tables
 * i for indexes
 *
 * And there's additional flags:
 *
 * n to list non-leaf partitioned tables
 *
 * and you can mix and match these in any order.
 */
bool
listPartitionedTables(const char *reltypes, const char *pattern, bool verbose)
{
	bool		showTables = strchr(reltypes, 't') != NULL;
	bool		showIndexes = strchr(reltypes, 'i') != NULL;
	bool		showNested = strchr(reltypes, 'n') != NULL;
	PQExpBufferData buf;
	PQExpBufferData title;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	bool		translate_columns[] = {false, false, false, false, false, false, false, false, false, false};
	const char *tabletitle;
	bool		mixed_output = false;

	/*
	 * Note: Declarative table partitioning is only supported as of Pg 10.0.
	 */
	if (pset.sversion < 100000)
	{
		char		sverbuf[32];

		pg_log_error("The server (version %s) does not support declarative table partitioning.",
					 formatPGVersionNumber(pset.sversion, false,
										   sverbuf, sizeof(sverbuf)));
		return true;
	}

	/* If no relation kind was selected, show them all */
	if (!showTables && !showIndexes)
		showTables = showIndexes = true;

	if (showIndexes && !showTables)
		tabletitle = _("List of partitioned indexes");	/* \dPi */
	else if (showTables && !showIndexes)
		tabletitle = _("List of partitioned tables");	/* \dPt */
	else
	{
		/* show all kinds */
		tabletitle = _("List of partitioned relations");
		mixed_output = true;
	}

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					  "  c.relname as \"%s\",\n"
					  "  pg_catalog.pg_get_userbyid(c.relowner) as \"%s\"",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("Owner"));

	if (mixed_output)
	{
		appendPQExpBuffer(&buf,
						  ",\n  CASE c.relkind"
						  " WHEN " CppAsString2(RELKIND_PARTITIONED_TABLE) " THEN '%s'"
						  " WHEN " CppAsString2(RELKIND_PARTITIONED_INDEX) " THEN '%s'"
						  " END as \"%s\"",
						  gettext_noop("partitioned table"),
						  gettext_noop("partitioned index"),
						  gettext_noop("Type"));

		translate_columns[3] = true;
	}

	if (showNested || pattern)
		appendPQExpBuffer(&buf,
						  ",\n  inh.inhparent::pg_catalog.regclass as \"%s\"",
						  gettext_noop("Parent name"));

	if (showIndexes)
		appendPQExpBuffer(&buf,
						  ",\n c2.oid::pg_catalog.regclass as \"%s\"",
						  gettext_noop("Table"));

	if (verbose)
	{
		/*
		 * Table access methods were introduced in v12, and can be set on
		 * partitioned tables since v17.
		 */
		appendPQExpBuffer(&buf, ",\n  am.amname as \"%s\"",
						  gettext_noop("Access method"));

		if (showNested)
		{
			appendPQExpBuffer(&buf,
							  ",\n  s.dps as \"%s\"",
							  gettext_noop("Leaf partition size"));
			appendPQExpBuffer(&buf,
							  ",\n  s.tps as \"%s\"",
							  gettext_noop("Total size"));
		}
		else
			/* Sizes of all partitions are considered in this case. */
			appendPQExpBuffer(&buf,
							  ",\n  s.tps as \"%s\"",
							  gettext_noop("Total size"));

		appendPQExpBuffer(&buf,
						  ",\n  pg_catalog.obj_description(c.oid, 'pg_class') as \"%s\"",
						  gettext_noop("Description"));
	}

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_class c"
						 "\n     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace");

	if (showIndexes)
		appendPQExpBufferStr(&buf,
							 "\n     LEFT JOIN pg_catalog.pg_index i ON i.indexrelid = c.oid"
							 "\n     LEFT JOIN pg_catalog.pg_class c2 ON i.indrelid = c2.oid");

	if (showNested || pattern)
		appendPQExpBufferStr(&buf,
							 "\n     LEFT JOIN pg_catalog.pg_inherits inh ON c.oid = inh.inhrelid");

	if (verbose)
	{
		appendPQExpBufferStr(&buf,
							 "\n     LEFT JOIN pg_catalog.pg_am am ON c.relam = am.oid");

		if (pset.sversion < 120000)
		{
			appendPQExpBufferStr(&buf,
								 ",\n     LATERAL (WITH RECURSIVE d\n"
								 "                AS (SELECT inhrelid AS oid, 1 AS level\n"
								 "                      FROM pg_catalog.pg_inherits\n"
								 "                     WHERE inhparent = c.oid\n"
								 "                    UNION ALL\n"
								 "                    SELECT inhrelid, level + 1\n"
								 "                      FROM pg_catalog.pg_inherits i\n"
								 "                           JOIN d ON i.inhparent = d.oid)\n"
								 "                SELECT pg_catalog.pg_size_pretty(sum(pg_catalog.pg_table_size("
								 "d.oid))) AS tps,\n"
								 "                       pg_catalog.pg_size_pretty(sum("
								 "\n             CASE WHEN d.level = 1"
								 " THEN pg_catalog.pg_table_size(d.oid) ELSE 0 END)) AS dps\n"
								 "               FROM d) s");
		}
		else
		{
			/* PostgreSQL 12 has pg_partition_tree function */
			appendPQExpBufferStr(&buf,
								 ",\n     LATERAL (SELECT pg_catalog.pg_size_pretty(sum("
								 "\n                 CASE WHEN ppt.isleaf AND ppt.level = 1"
								 "\n                      THEN pg_catalog.pg_table_size(ppt.relid)"
								 " ELSE 0 END)) AS dps"
								 ",\n                     pg_catalog.pg_size_pretty(sum("
								 "pg_catalog.pg_table_size(ppt.relid))) AS tps"
								 "\n              FROM pg_catalog.pg_partition_tree(c.oid) ppt) s");
		}
	}

	appendPQExpBufferStr(&buf, "\nWHERE c.relkind IN (");
	if (showTables)
		appendPQExpBufferStr(&buf, CppAsString2(RELKIND_PARTITIONED_TABLE) ",");
	if (showIndexes)
		appendPQExpBufferStr(&buf, CppAsString2(RELKIND_PARTITIONED_INDEX) ",");
	appendPQExpBufferStr(&buf, "''");	/* dummy */
	appendPQExpBufferStr(&buf, ")\n");

	appendPQExpBufferStr(&buf, !showNested && !pattern ?
						 " AND NOT c.relispartition\n" : "");

	if (!pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname !~ '^pg_toast'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"n.nspname", "c.relname", NULL,
								"pg_catalog.pg_table_is_visible(c.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBuffer(&buf, "ORDER BY \"Schema\", %s%s\"Name\";",
					  mixed_output ? "\"Type\" DESC, " : "",
					  showNested || pattern ? "\"Parent name\" NULLS FIRST, " : "");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	initPQExpBuffer(&title);
	appendPQExpBufferStr(&title, tabletitle);

	myopt.title = title.data;
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	termPQExpBuffer(&title);

	PQclear(res);
	return true;
}

/*
 * \dL
 *
 * Describes languages.
 */
bool
listLanguages(const char *pattern, bool verbose, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT l.lanname AS \"%s\",\n"
					  "       pg_catalog.pg_get_userbyid(l.lanowner) as \"%s\",\n"
					  "       l.lanpltrusted AS \"%s\"",
					  gettext_noop("Name"),
					  gettext_noop("Owner"),
					  gettext_noop("Trusted"));

	if (verbose)
	{
		appendPQExpBuffer(&buf,
						  ",\n       NOT l.lanispl AS \"%s\",\n"
						  "       l.lanplcallfoid::pg_catalog.regprocedure AS \"%s\",\n"
						  "       l.lanvalidator::pg_catalog.regprocedure AS \"%s\",\n       "
						  "l.laninline::pg_catalog.regprocedure AS \"%s\",\n       ",
						  gettext_noop("Internal language"),
						  gettext_noop("Call handler"),
						  gettext_noop("Validator"),
						  gettext_noop("Inline handler"));
		printACLColumn(&buf, "l.lanacl");
	}

	appendPQExpBuffer(&buf,
					  ",\n       d.description AS \"%s\""
					  "\nFROM pg_catalog.pg_language l\n"
					  "LEFT JOIN pg_catalog.pg_description d\n"
					  "  ON d.classoid = l.tableoid AND d.objoid = l.oid\n"
					  "  AND d.objsubid = 0\n",
					  gettext_noop("Description"));

	if (pattern)
	{
		if (!validateSQLNamePattern(&buf, pattern, false, false,
									NULL, "l.lanname", NULL, NULL,
									NULL, 2))
		{
			termPQExpBuffer(&buf);
			return false;
		}
	}

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "WHERE l.lanplcallfoid != 0\n");


	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of languages");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}


/*
 * \dD
 *
 * Describes domains.
 */
bool
listDomains(const char *pattern, bool verbose, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					  "       t.typname as \"%s\",\n"
					  "       pg_catalog.format_type(t.typbasetype, t.typtypmod) as \"%s\",\n"
					  "       (SELECT c.collname FROM pg_catalog.pg_collation c, pg_catalog.pg_type bt\n"
					  "        WHERE c.oid = t.typcollation AND bt.oid = t.typbasetype AND t.typcollation <> bt.typcollation) as \"%s\",\n"
					  "       CASE WHEN t.typnotnull THEN 'not null' END as \"%s\",\n"
					  "       t.typdefault as \"%s\",\n"
					  "       pg_catalog.array_to_string(ARRAY(\n"
					  "         SELECT pg_catalog.pg_get_constraintdef(r.oid, true) FROM pg_catalog.pg_constraint r WHERE t.oid = r.contypid AND r.contype = " CppAsString2(CONSTRAINT_CHECK) " ORDER BY r.conname\n"
					  "       ), ' ') as \"%s\"",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("Type"),
					  gettext_noop("Collation"),
					  gettext_noop("Nullable"),
					  gettext_noop("Default"),
					  gettext_noop("Check"));

	if (verbose)
	{
		appendPQExpBufferStr(&buf, ",\n  ");
		printACLColumn(&buf, "t.typacl");
		appendPQExpBuffer(&buf,
						  ",\n       d.description as \"%s\"",
						  gettext_noop("Description"));
	}

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_type t\n"
						 "     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = t.typnamespace\n");

	if (verbose)
		appendPQExpBufferStr(&buf,
							 "     LEFT JOIN pg_catalog.pg_description d "
							 "ON d.classoid = t.tableoid AND d.objoid = t.oid "
							 "AND d.objsubid = 0\n");

	appendPQExpBufferStr(&buf, "WHERE t.typtype = 'd'\n");

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"n.nspname", "t.typname", NULL,
								"pg_catalog.pg_type_is_visible(t.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of domains");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \dc
 *
 * Describes conversions.
 */
bool
listConversions(const char *pattern, bool verbose, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] =
	{false, false, false, false, true, false};

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT n.nspname AS \"%s\",\n"
					  "       c.conname AS \"%s\",\n"
					  "       pg_catalog.pg_encoding_to_char(c.conforencoding) AS \"%s\",\n"
					  "       pg_catalog.pg_encoding_to_char(c.contoencoding) AS \"%s\",\n"
					  "       CASE WHEN c.condefault THEN '%s'\n"
					  "       ELSE '%s' END AS \"%s\"",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("Source"),
					  gettext_noop("Destination"),
					  gettext_noop("yes"), gettext_noop("no"),
					  gettext_noop("Default?"));

	if (verbose)
		appendPQExpBuffer(&buf,
						  ",\n       d.description AS \"%s\"",
						  gettext_noop("Description"));

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_conversion c\n"
						 "     JOIN pg_catalog.pg_namespace n "
						 "ON n.oid = c.connamespace\n");

	if (verbose)
		appendPQExpBufferStr(&buf,
							 "LEFT JOIN pg_catalog.pg_description d "
							 "ON d.classoid = c.tableoid\n"
							 "          AND d.objoid = c.oid "
							 "AND d.objsubid = 0\n");

	appendPQExpBufferStr(&buf, "WHERE true\n");

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "  AND n.nspname <> 'pg_catalog'\n"
							 "  AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"n.nspname", "c.conname", NULL,
								"pg_catalog.pg_conversion_is_visible(c.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of conversions");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \dconfig
 *
 * Describes configuration parameters.
 */
bool
describeConfigurationParameters(const char *pattern, bool verbose,
								bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT s.name AS \"%s\", "
					  "pg_catalog.current_setting(s.name) AS \"%s\"",
					  gettext_noop("Parameter"),
					  gettext_noop("Value"));

	if (verbose)
	{
		appendPQExpBuffer(&buf,
						  ", s.vartype AS \"%s\", s.context AS \"%s\", ",
						  gettext_noop("Type"),
						  gettext_noop("Context"));
		if (pset.sversion >= 150000)
			printACLColumn(&buf, "p.paracl");
		else
			appendPQExpBuffer(&buf, "NULL AS \"%s\"",
							  gettext_noop("Access privileges"));
	}

	appendPQExpBufferStr(&buf, "\nFROM pg_catalog.pg_settings s\n");

	if (verbose && pset.sversion >= 150000)
		appendPQExpBufferStr(&buf,
							 "  LEFT JOIN pg_catalog.pg_parameter_acl p\n"
							 "  ON pg_catalog.lower(s.name) = p.parname\n");

	if (pattern)
		processSQLNamePattern(pset.db, &buf, pattern,
							  false, false,
							  NULL, "pg_catalog.lower(s.name)", NULL,
							  NULL, NULL, NULL);
	else
		appendPQExpBufferStr(&buf, "WHERE s.source <> 'default' AND\n"
							 "      s.setting IS DISTINCT FROM s.boot_val\n");

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	if (pattern)
		myopt.title = _("List of configuration parameters");
	else
		myopt.title = _("List of non-default configuration parameters");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \dy
 *
 * Describes Event Triggers.
 */
bool
listEventTriggers(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] =
	{false, false, false, true, false, false, false};

	if (pset.sversion < 90300)
	{
		char		sverbuf[32];

		pg_log_error("The server (version %s) does not support event triggers.",
					 formatPGVersionNumber(pset.sversion, false,
										   sverbuf, sizeof(sverbuf)));
		return true;
	}

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT evtname as \"%s\", "
					  "evtevent as \"%s\", "
					  "pg_catalog.pg_get_userbyid(e.evtowner) as \"%s\",\n"
					  " case evtenabled when 'O' then '%s'"
					  "  when 'R' then '%s'"
					  "  when 'A' then '%s'"
					  "  when 'D' then '%s' end as \"%s\",\n"
					  " e.evtfoid::pg_catalog.regproc as \"%s\", "
					  "pg_catalog.array_to_string(array(select x"
					  " from pg_catalog.unnest(evttags) as t(x)), ', ') as \"%s\"",
					  gettext_noop("Name"),
					  gettext_noop("Event"),
					  gettext_noop("Owner"),
					  gettext_noop("enabled"),
					  gettext_noop("replica"),
					  gettext_noop("always"),
					  gettext_noop("disabled"),
					  gettext_noop("Enabled"),
					  gettext_noop("Function"),
					  gettext_noop("Tags"));
	if (verbose)
		appendPQExpBuffer(&buf,
						  ",\npg_catalog.obj_description(e.oid, 'pg_event_trigger') as \"%s\"",
						  gettext_noop("Description"));
	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_event_trigger e ");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								NULL, "evtname", NULL, NULL,
								NULL, 1))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of event triggers");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \dX
 *
 * Describes extended statistics.
 */
bool
listExtendedStats(const char *pattern)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	if (pset.sversion < 100000)
	{
		char		sverbuf[32];

		pg_log_error("The server (version %s) does not support extended statistics.",
					 formatPGVersionNumber(pset.sversion, false,
										   sverbuf, sizeof(sverbuf)));
		return true;
	}

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT \n"
					  "es.stxnamespace::pg_catalog.regnamespace::pg_catalog.text AS \"%s\", \n"
					  "es.stxname AS \"%s\", \n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"));

	if (pset.sversion >= 140000)
		appendPQExpBuffer(&buf,
						  "pg_catalog.format('%%s FROM %%s', \n"
						  "  pg_catalog.pg_get_statisticsobjdef_columns(es.oid), \n"
						  "  es.stxrelid::pg_catalog.regclass) AS \"%s\"",
						  gettext_noop("Definition"));
	else
		appendPQExpBuffer(&buf,
						  "pg_catalog.format('%%s FROM %%s', \n"
						  "  (SELECT pg_catalog.string_agg(pg_catalog.quote_ident(a.attname),', ') \n"
						  "   FROM pg_catalog.unnest(es.stxkeys) s(attnum) \n"
						  "   JOIN pg_catalog.pg_attribute a \n"
						  "   ON (es.stxrelid = a.attrelid \n"
						  "   AND a.attnum = s.attnum \n"
						  "   AND NOT a.attisdropped)), \n"
						  "es.stxrelid::pg_catalog.regclass) AS \"%s\"",
						  gettext_noop("Definition"));

	appendPQExpBuffer(&buf,
					  ",\nCASE WHEN " CppAsString2(STATS_EXT_NDISTINCT) " = any(es.stxkind) THEN 'defined' \n"
					  "END AS \"%s\", \n"
					  "CASE WHEN " CppAsString2(STATS_EXT_DEPENDENCIES) " = any(es.stxkind) THEN 'defined' \n"
					  "END AS \"%s\"",
					  gettext_noop("Ndistinct"),
					  gettext_noop("Dependencies"));

	/*
	 * Include the MCV statistics kind.
	 */
	if (pset.sversion >= 120000)
	{
		appendPQExpBuffer(&buf,
						  ",\nCASE WHEN " CppAsString2(STATS_EXT_MCV) " = any(es.stxkind) THEN 'defined' \n"
						  "END AS \"%s\" ",
						  gettext_noop("MCV"));
	}

	appendPQExpBufferStr(&buf,
						 " \nFROM pg_catalog.pg_statistic_ext es \n");

	if (!validateSQLNamePattern(&buf, pattern,
								false, false,
								"es.stxnamespace::pg_catalog.regnamespace::pg_catalog.text", "es.stxname",
								NULL, "pg_catalog.pg_statistics_obj_is_visible(es.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of extended statistics");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \dC
 *
 * Describes casts.
 */
bool
listCasts(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {false, false, false, true, true, false};

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT pg_catalog.format_type(castsource, NULL) AS \"%s\",\n"
					  "       pg_catalog.format_type(casttarget, NULL) AS \"%s\",\n",
					  gettext_noop("Source type"),
					  gettext_noop("Target type"));

	/*
	 * We don't attempt to localize '(binary coercible)' or '(with inout)',
	 * because there's too much risk of gettext translating a function name
	 * that happens to match some string in the PO database.
	 */
	appendPQExpBuffer(&buf,
					  "       CASE WHEN c.castmethod = '%c' THEN '(binary coercible)'\n"
					  "            WHEN c.castmethod = '%c' THEN '(with inout)'\n"
					  "            ELSE p.proname\n"
					  "       END AS \"%s\",\n",
					  COERCION_METHOD_BINARY,
					  COERCION_METHOD_INOUT,
					  gettext_noop("Function"));

	appendPQExpBuffer(&buf,
					  "       CASE WHEN c.castcontext = '%c' THEN '%s'\n"
					  "            WHEN c.castcontext = '%c' THEN '%s'\n"
					  "            ELSE '%s'\n"
					  "       END AS \"%s\"",
					  COERCION_CODE_EXPLICIT,
					  gettext_noop("no"),
					  COERCION_CODE_ASSIGNMENT,
					  gettext_noop("in assignment"),
					  gettext_noop("yes"),
					  gettext_noop("Implicit?"));

	if (verbose)
		appendPQExpBuffer(&buf,
						  ",\n       CASE WHEN p.proleakproof THEN '%s'\n"
						  "            ELSE '%s'\n"
						  "       END AS \"%s\",\n"
						  "       d.description AS \"%s\"",
						  gettext_noop("yes"),
						  gettext_noop("no"),
						  gettext_noop("Leakproof?"),
						  gettext_noop("Description"));

	/*
	 * We need a left join to pg_proc for binary casts; the others are just
	 * paranoia.
	 */
	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_cast c LEFT JOIN pg_catalog.pg_proc p\n"
						 "     ON c.castfunc = p.oid\n"
						 "     LEFT JOIN pg_catalog.pg_type ts\n"
						 "     ON c.castsource = ts.oid\n"
						 "     LEFT JOIN pg_catalog.pg_namespace ns\n"
						 "     ON ns.oid = ts.typnamespace\n"
						 "     LEFT JOIN pg_catalog.pg_type tt\n"
						 "     ON c.casttarget = tt.oid\n"
						 "     LEFT JOIN pg_catalog.pg_namespace nt\n"
						 "     ON nt.oid = tt.typnamespace\n");

	if (verbose)
		appendPQExpBufferStr(&buf,
							 "     LEFT JOIN pg_catalog.pg_description d\n"
							 "     ON d.classoid = c.tableoid AND d.objoid = "
							 "c.oid AND d.objsubid = 0\n");

	appendPQExpBufferStr(&buf, "WHERE ( (true");

	/*
	 * Match name pattern against either internal or external name of either
	 * castsource or casttarget
	 */
	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"ns.nspname", "ts.typname",
								"pg_catalog.format_type(ts.oid, NULL)",
								"pg_catalog.pg_type_is_visible(ts.oid)",
								NULL, 3))
		goto error_return;

	appendPQExpBufferStr(&buf, ") OR (true");

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"nt.nspname", "tt.typname",
								"pg_catalog.format_type(tt.oid, NULL)",
								"pg_catalog.pg_type_is_visible(tt.oid)",
								NULL, 3))
		goto error_return;

	appendPQExpBufferStr(&buf, ") )\nORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of casts");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;

error_return:
	termPQExpBuffer(&buf);
	return false;
}

/*
 * \dO
 *
 * Describes collations.
 */
bool
listCollations(const char *pattern, bool verbose, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {false, false, false, false, false, false, false, true, false};

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT\n"
					  "  n.nspname AS \"%s\",\n"
					  "  c.collname AS \"%s\",\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"));

	if (pset.sversion >= 100000)
		appendPQExpBuffer(&buf,
						  "  CASE c.collprovider "
						  "WHEN " CppAsString2(COLLPROVIDER_DEFAULT) " THEN 'default' "
						  "WHEN " CppAsString2(COLLPROVIDER_BUILTIN) " THEN 'builtin' "
						  "WHEN " CppAsString2(COLLPROVIDER_LIBC) " THEN 'libc' "
						  "WHEN " CppAsString2(COLLPROVIDER_ICU) " THEN 'icu' "
						  "END AS \"%s\",\n",
						  gettext_noop("Provider"));
	else
		appendPQExpBuffer(&buf,
						  "  'libc' AS \"%s\",\n",
						  gettext_noop("Provider"));

	appendPQExpBuffer(&buf,
					  "  c.collcollate AS \"%s\",\n"
					  "  c.collctype AS \"%s\",\n",
					  gettext_noop("Collate"),
					  gettext_noop("Ctype"));

	if (pset.sversion >= 170000)
		appendPQExpBuffer(&buf,
						  "  c.colllocale AS \"%s\",\n",
						  gettext_noop("Locale"));
	else if (pset.sversion >= 150000)
		appendPQExpBuffer(&buf,
						  "  c.colliculocale AS \"%s\",\n",
						  gettext_noop("Locale"));
	else
		appendPQExpBuffer(&buf,
						  "  c.collcollate AS \"%s\",\n",
						  gettext_noop("Locale"));

	if (pset.sversion >= 160000)
		appendPQExpBuffer(&buf,
						  "  c.collicurules AS \"%s\",\n",
						  gettext_noop("ICU Rules"));
	else
		appendPQExpBuffer(&buf,
						  "  NULL AS \"%s\",\n",
						  gettext_noop("ICU Rules"));

	if (pset.sversion >= 120000)
		appendPQExpBuffer(&buf,
						  "  CASE WHEN c.collisdeterministic THEN '%s' ELSE '%s' END AS \"%s\"",
						  gettext_noop("yes"), gettext_noop("no"),
						  gettext_noop("Deterministic?"));
	else
		appendPQExpBuffer(&buf,
						  "  '%s' AS \"%s\"",
						  gettext_noop("yes"),
						  gettext_noop("Deterministic?"));

	if (verbose)
		appendPQExpBuffer(&buf,
						  ",\n  pg_catalog.obj_description(c.oid, 'pg_collation') AS \"%s\"",
						  gettext_noop("Description"));

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_collation c, pg_catalog.pg_namespace n\n"
						 "WHERE n.oid = c.collnamespace\n");

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	/*
	 * Hide collations that aren't usable in the current database's encoding.
	 * If you think to change this, note that pg_collation_is_visible rejects
	 * unusable collations, so you will need to hack name pattern processing
	 * somehow to avoid inconsistent behavior.
	 */
	appendPQExpBufferStr(&buf, "      AND c.collencoding IN (-1, pg_catalog.pg_char_to_encoding(pg_catalog.getdatabaseencoding()))\n");

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"n.nspname", "c.collname", NULL,
								"pg_catalog.pg_collation_is_visible(c.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of collations");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \dn
 *
 * Describes schemas (namespaces)
 */
bool
listSchemas(const char *pattern, bool verbose, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	int			pub_schema_tuples = 0;
	char	  **footers = NULL;

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT n.nspname AS \"%s\",\n"
					  "  pg_catalog.pg_get_userbyid(n.nspowner) AS \"%s\"",
					  gettext_noop("Name"),
					  gettext_noop("Owner"));

	if (verbose)
	{
		appendPQExpBufferStr(&buf, ",\n  ");
		printACLColumn(&buf, "n.nspacl");
		appendPQExpBuffer(&buf,
						  ",\n  pg_catalog.obj_description(n.oid, 'pg_namespace') AS \"%s\"",
						  gettext_noop("Description"));
	}

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_namespace n\n");

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf,
							 "WHERE n.nspname !~ '^pg_' AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern,
								!showSystem && !pattern, false,
								NULL, "n.nspname", NULL,
								NULL,
								NULL, 2))
		goto error_return;

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	if (!res)
		goto error_return;

	myopt.title = _("List of schemas");
	myopt.translate_header = true;

	if (pattern && pset.sversion >= 150000)
	{
		PGresult   *result;
		int			i;

		printfPQExpBuffer(&buf,
						  "SELECT pubname \n"
						  "FROM pg_catalog.pg_publication p\n"
						  "     JOIN pg_catalog.pg_publication_namespace pn ON p.oid = pn.pnpubid\n"
						  "     JOIN pg_catalog.pg_namespace n ON n.oid = pn.pnnspid \n"
						  "WHERE n.nspname = '%s'\n"
						  "ORDER BY 1",
						  pattern);
		result = PSQLexec(buf.data);
		if (!result)
			goto error_return;
		else
			pub_schema_tuples = PQntuples(result);

		if (pub_schema_tuples > 0)
		{
			/*
			 * Allocate memory for footers. Size of footers will be 1 (for
			 * storing "Publications:" string) + publication schema mapping
			 * count +  1 (for storing NULL).
			 */
			footers = (char **) pg_malloc((1 + pub_schema_tuples + 1) * sizeof(char *));
			footers[0] = pg_strdup(_("Publications:"));

			/* Might be an empty set - that's ok */
			for (i = 0; i < pub_schema_tuples; i++)
			{
				printfPQExpBuffer(&buf, "    \"%s\"",
								  PQgetvalue(result, i, 0));

				footers[i + 1] = pg_strdup(buf.data);
			}

			footers[i + 1] = NULL;
			myopt.footers = footers;
		}

		PQclear(result);
	}

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	termPQExpBuffer(&buf);
	PQclear(res);

	/* Free the memory allocated for the footer */
	if (footers)
	{
		char	  **footer = NULL;

		for (footer = footers; *footer; footer++)
			pg_free(*footer);

		pg_free(footers);
	}

	return true;

error_return:
	termPQExpBuffer(&buf);
	return false;
}


/*
 * \dFp
 * list text search parsers
 */
bool
listTSParsers(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	if (verbose)
		return listTSParsersVerbose(pattern);

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT\n"
					  "  n.nspname as \"%s\",\n"
					  "  p.prsname as \"%s\",\n"
					  "  pg_catalog.obj_description(p.oid, 'pg_ts_parser') as \"%s\"\n"
					  "FROM pg_catalog.pg_ts_parser p\n"
					  "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = p.prsnamespace\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("Description")
		);

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								"n.nspname", "p.prsname", NULL,
								"pg_catalog.pg_ts_parser_is_visible(p.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of text search parsers");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * full description of parsers
 */
static bool
listTSParsersVerbose(const char *pattern)
{
	PQExpBufferData buf;
	PGresult   *res;
	int			i;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT p.oid,\n"
					  "  n.nspname,\n"
					  "  p.prsname\n"
					  "FROM pg_catalog.pg_ts_parser p\n"
					  "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = p.prsnamespace\n"
		);

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								"n.nspname", "p.prsname", NULL,
								"pg_catalog.pg_ts_parser_is_visible(p.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	if (PQntuples(res) == 0)
	{
		if (!pset.quiet)
		{
			if (pattern)
				pg_log_error("Did not find any text search parser named \"%s\".",
							 pattern);
			else
				pg_log_error("Did not find any text search parsers.");
		}
		PQclear(res);
		return false;
	}

	for (i = 0; i < PQntuples(res); i++)
	{
		const char *oid;
		const char *nspname = NULL;
		const char *prsname;

		oid = PQgetvalue(res, i, 0);
		if (!PQgetisnull(res, i, 1))
			nspname = PQgetvalue(res, i, 1);
		prsname = PQgetvalue(res, i, 2);

		if (!describeOneTSParser(oid, nspname, prsname))
		{
			PQclear(res);
			return false;
		}

		if (cancel_pressed)
		{
			PQclear(res);
			return false;
		}
	}

	PQclear(res);
	return true;
}

static bool
describeOneTSParser(const char *oid, const char *nspname, const char *prsname)
{
	PQExpBufferData buf;
	PGresult   *res;
	PQExpBufferData title;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {true, false, false};

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT '%s' AS \"%s\",\n"
					  "   p.prsstart::pg_catalog.regproc AS \"%s\",\n"
					  "   pg_catalog.obj_description(p.prsstart, 'pg_proc') as \"%s\"\n"
					  " FROM pg_catalog.pg_ts_parser p\n"
					  " WHERE p.oid = '%s'\n"
					  "UNION ALL\n"
					  "SELECT '%s',\n"
					  "   p.prstoken::pg_catalog.regproc,\n"
					  "   pg_catalog.obj_description(p.prstoken, 'pg_proc')\n"
					  " FROM pg_catalog.pg_ts_parser p\n"
					  " WHERE p.oid = '%s'\n"
					  "UNION ALL\n"
					  "SELECT '%s',\n"
					  "   p.prsend::pg_catalog.regproc,\n"
					  "   pg_catalog.obj_description(p.prsend, 'pg_proc')\n"
					  " FROM pg_catalog.pg_ts_parser p\n"
					  " WHERE p.oid = '%s'\n"
					  "UNION ALL\n"
					  "SELECT '%s',\n"
					  "   p.prsheadline::pg_catalog.regproc,\n"
					  "   pg_catalog.obj_description(p.prsheadline, 'pg_proc')\n"
					  " FROM pg_catalog.pg_ts_parser p\n"
					  " WHERE p.oid = '%s'\n"
					  "UNION ALL\n"
					  "SELECT '%s',\n"
					  "   p.prslextype::pg_catalog.regproc,\n"
					  "   pg_catalog.obj_description(p.prslextype, 'pg_proc')\n"
					  " FROM pg_catalog.pg_ts_parser p\n"
					  " WHERE p.oid = '%s';",
					  gettext_noop("Start parse"),
					  gettext_noop("Method"),
					  gettext_noop("Function"),
					  gettext_noop("Description"),
					  oid,
					  gettext_noop("Get next token"),
					  oid,
					  gettext_noop("End parse"),
					  oid,
					  gettext_noop("Get headline"),
					  oid,
					  gettext_noop("Get token types"),
					  oid);

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	initPQExpBuffer(&title);
	if (nspname)
		printfPQExpBuffer(&title, _("Text search parser \"%s.%s\""),
						  nspname, prsname);
	else
		printfPQExpBuffer(&title, _("Text search parser \"%s\""), prsname);
	myopt.title = title.data;
	myopt.footers = NULL;
	myopt.topt.default_footer = false;
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT t.alias as \"%s\",\n"
					  "  t.description as \"%s\"\n"
					  "FROM pg_catalog.ts_token_type( '%s'::pg_catalog.oid ) as t\n"
					  "ORDER BY 1;",
					  gettext_noop("Token name"),
					  gettext_noop("Description"),
					  oid);

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
	{
		termPQExpBuffer(&title);
		return false;
	}

	if (nspname)
		printfPQExpBuffer(&title, _("Token types for parser \"%s.%s\""),
						  nspname, prsname);
	else
		printfPQExpBuffer(&title, _("Token types for parser \"%s\""), prsname);
	myopt.title = title.data;
	myopt.footers = NULL;
	myopt.topt.default_footer = true;
	myopt.translate_header = true;
	myopt.translate_columns = NULL;
	myopt.n_translate_columns = 0;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	termPQExpBuffer(&title);
	PQclear(res);
	return true;
}


/*
 * \dFd
 * list text search dictionaries
 */
bool
listTSDictionaries(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT\n"
					  "  n.nspname as \"%s\",\n"
					  "  d.dictname as \"%s\",\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"));

	if (verbose)
	{
		appendPQExpBuffer(&buf,
						  "  ( SELECT COALESCE(nt.nspname, '(null)')::pg_catalog.text || '.' || t.tmplname FROM\n"
						  "    pg_catalog.pg_ts_template t\n"
						  "    LEFT JOIN pg_catalog.pg_namespace nt ON nt.oid = t.tmplnamespace\n"
						  "    WHERE d.dicttemplate = t.oid ) AS  \"%s\",\n"
						  "  d.dictinitoption as \"%s\",\n",
						  gettext_noop("Template"),
						  gettext_noop("Init options"));
	}

	appendPQExpBuffer(&buf,
					  "  pg_catalog.obj_description(d.oid, 'pg_ts_dict') as \"%s\"\n",
					  gettext_noop("Description"));

	appendPQExpBufferStr(&buf, "FROM pg_catalog.pg_ts_dict d\n"
						 "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = d.dictnamespace\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								"n.nspname", "d.dictname", NULL,
								"pg_catalog.pg_ts_dict_is_visible(d.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of text search dictionaries");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}


/*
 * \dFt
 * list text search templates
 */
bool
listTSTemplates(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	if (verbose)
		printfPQExpBuffer(&buf,
						  "SELECT\n"
						  "  n.nspname AS \"%s\",\n"
						  "  t.tmplname AS \"%s\",\n"
						  "  t.tmplinit::pg_catalog.regproc AS \"%s\",\n"
						  "  t.tmpllexize::pg_catalog.regproc AS \"%s\",\n"
						  "  pg_catalog.obj_description(t.oid, 'pg_ts_template') AS \"%s\"\n",
						  gettext_noop("Schema"),
						  gettext_noop("Name"),
						  gettext_noop("Init"),
						  gettext_noop("Lexize"),
						  gettext_noop("Description"));
	else
		printfPQExpBuffer(&buf,
						  "SELECT\n"
						  "  n.nspname AS \"%s\",\n"
						  "  t.tmplname AS \"%s\",\n"
						  "  pg_catalog.obj_description(t.oid, 'pg_ts_template') AS \"%s\"\n",
						  gettext_noop("Schema"),
						  gettext_noop("Name"),
						  gettext_noop("Description"));

	appendPQExpBufferStr(&buf, "FROM pg_catalog.pg_ts_template t\n"
						 "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = t.tmplnamespace\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								"n.nspname", "t.tmplname", NULL,
								"pg_catalog.pg_ts_template_is_visible(t.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of text search templates");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}


/*
 * \dF
 * list text search configurations
 */
bool
listTSConfigs(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	if (verbose)
		return listTSConfigsVerbose(pattern);

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT\n"
					  "   n.nspname as \"%s\",\n"
					  "   c.cfgname as \"%s\",\n"
					  "   pg_catalog.obj_description(c.oid, 'pg_ts_config') as \"%s\"\n"
					  "FROM pg_catalog.pg_ts_config c\n"
					  "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.cfgnamespace\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("Description")
		);

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								"n.nspname", "c.cfgname", NULL,
								"pg_catalog.pg_ts_config_is_visible(c.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of text search configurations");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

static bool
listTSConfigsVerbose(const char *pattern)
{
	PQExpBufferData buf;
	PGresult   *res;
	int			i;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT c.oid, c.cfgname,\n"
					  "   n.nspname,\n"
					  "   p.prsname,\n"
					  "   np.nspname as pnspname\n"
					  "FROM pg_catalog.pg_ts_config c\n"
					  "   LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.cfgnamespace,\n"
					  " pg_catalog.pg_ts_parser p\n"
					  "   LEFT JOIN pg_catalog.pg_namespace np ON np.oid = p.prsnamespace\n"
					  "WHERE  p.oid = c.cfgparser\n"
		);

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"n.nspname", "c.cfgname", NULL,
								"pg_catalog.pg_ts_config_is_visible(c.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 3, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	if (PQntuples(res) == 0)
	{
		if (!pset.quiet)
		{
			if (pattern)
				pg_log_error("Did not find any text search configuration named \"%s\".",
							 pattern);
			else
				pg_log_error("Did not find any text search configurations.");
		}
		PQclear(res);
		return false;
	}

	for (i = 0; i < PQntuples(res); i++)
	{
		const char *oid;
		const char *cfgname;
		const char *nspname = NULL;
		const char *prsname;
		const char *pnspname = NULL;

		oid = PQgetvalue(res, i, 0);
		cfgname = PQgetvalue(res, i, 1);
		if (!PQgetisnull(res, i, 2))
			nspname = PQgetvalue(res, i, 2);
		prsname = PQgetvalue(res, i, 3);
		if (!PQgetisnull(res, i, 4))
			pnspname = PQgetvalue(res, i, 4);

		if (!describeOneTSConfig(oid, nspname, cfgname, pnspname, prsname))
		{
			PQclear(res);
			return false;
		}

		if (cancel_pressed)
		{
			PQclear(res);
			return false;
		}
	}

	PQclear(res);
	return true;
}

static bool
describeOneTSConfig(const char *oid, const char *nspname, const char *cfgname,
					const char *pnspname, const char *prsname)
{
	PQExpBufferData buf,
				title;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT\n"
					  "  ( SELECT t.alias FROM\n"
					  "    pg_catalog.ts_token_type(c.cfgparser) AS t\n"
					  "    WHERE t.tokid = m.maptokentype ) AS \"%s\",\n"
					  "  pg_catalog.btrim(\n"
					  "    ARRAY( SELECT mm.mapdict::pg_catalog.regdictionary\n"
					  "           FROM pg_catalog.pg_ts_config_map AS mm\n"
					  "           WHERE mm.mapcfg = m.mapcfg AND mm.maptokentype = m.maptokentype\n"
					  "           ORDER BY mapcfg, maptokentype, mapseqno\n"
					  "    ) :: pg_catalog.text,\n"
					  "  '{}') AS \"%s\"\n"
					  "FROM pg_catalog.pg_ts_config AS c, pg_catalog.pg_ts_config_map AS m\n"
					  "WHERE c.oid = '%s' AND m.mapcfg = c.oid\n"
					  "GROUP BY m.mapcfg, m.maptokentype, c.cfgparser\n"
					  "ORDER BY 1;",
					  gettext_noop("Token"),
					  gettext_noop("Dictionaries"),
					  oid);

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	initPQExpBuffer(&title);

	if (nspname)
		appendPQExpBuffer(&title, _("Text search configuration \"%s.%s\""),
						  nspname, cfgname);
	else
		appendPQExpBuffer(&title, _("Text search configuration \"%s\""),
						  cfgname);

	if (pnspname)
		appendPQExpBuffer(&title, _("\nParser: \"%s.%s\""),
						  pnspname, prsname);
	else
		appendPQExpBuffer(&title, _("\nParser: \"%s\""),
						  prsname);

	myopt.title = title.data;
	myopt.footers = NULL;
	myopt.topt.default_footer = false;
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	termPQExpBuffer(&title);

	PQclear(res);
	return true;
}


/*
 * \dew
 *
 * Describes foreign-data wrappers
 */
bool
listForeignDataWrappers(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT fdw.fdwname AS \"%s\",\n"
					  "  pg_catalog.pg_get_userbyid(fdw.fdwowner) AS \"%s\",\n"
					  "  fdw.fdwhandler::pg_catalog.regproc AS \"%s\",\n"
					  "  fdw.fdwvalidator::pg_catalog.regproc AS \"%s\"",
					  gettext_noop("Name"),
					  gettext_noop("Owner"),
					  gettext_noop("Handler"),
					  gettext_noop("Validator"));

	if (verbose)
	{
		appendPQExpBufferStr(&buf, ",\n  ");
		printACLColumn(&buf, "fdwacl");
		appendPQExpBuffer(&buf,
						  ",\n CASE WHEN fdwoptions IS NULL THEN '' ELSE "
						  "  '(' || pg_catalog.array_to_string(ARRAY(SELECT "
						  "  pg_catalog.quote_ident(option_name) ||  ' ' || "
						  "  pg_catalog.quote_literal(option_value)  FROM "
						  "  pg_catalog.pg_options_to_table(fdwoptions)),  ', ') || ')' "
						  "  END AS \"%s\""
						  ",\n  d.description AS \"%s\" ",
						  gettext_noop("FDW options"),
						  gettext_noop("Description"));
	}

	appendPQExpBufferStr(&buf, "\nFROM pg_catalog.pg_foreign_data_wrapper fdw\n");

	if (verbose)
		appendPQExpBufferStr(&buf,
							 "LEFT JOIN pg_catalog.pg_description d\n"
							 "       ON d.classoid = fdw.tableoid "
							 "AND d.objoid = fdw.oid AND d.objsubid = 0\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								NULL, "fdwname", NULL, NULL,
								NULL, 1))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of foreign-data wrappers");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \des
 *
 * Describes foreign servers.
 */
bool
listForeignServers(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT s.srvname AS \"%s\",\n"
					  "  pg_catalog.pg_get_userbyid(s.srvowner) AS \"%s\",\n"
					  "  f.fdwname AS \"%s\"",
					  gettext_noop("Name"),
					  gettext_noop("Owner"),
					  gettext_noop("Foreign-data wrapper"));

	if (verbose)
	{
		appendPQExpBufferStr(&buf, ",\n  ");
		printACLColumn(&buf, "s.srvacl");
		appendPQExpBuffer(&buf,
						  ",\n"
						  "  s.srvtype AS \"%s\",\n"
						  "  s.srvversion AS \"%s\",\n"
						  "  CASE WHEN srvoptions IS NULL THEN '' ELSE "
						  "  '(' || pg_catalog.array_to_string(ARRAY(SELECT "
						  "  pg_catalog.quote_ident(option_name) ||  ' ' || "
						  "  pg_catalog.quote_literal(option_value)  FROM "
						  "  pg_catalog.pg_options_to_table(srvoptions)),  ', ') || ')' "
						  "  END AS \"%s\",\n"
						  "  d.description AS \"%s\"",
						  gettext_noop("Type"),
						  gettext_noop("Version"),
						  gettext_noop("FDW options"),
						  gettext_noop("Description"));
	}

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_foreign_server s\n"
						 "     JOIN pg_catalog.pg_foreign_data_wrapper f ON f.oid=s.srvfdw\n");

	if (verbose)
		appendPQExpBufferStr(&buf,
							 "LEFT JOIN pg_catalog.pg_description d\n       "
							 "ON d.classoid = s.tableoid AND d.objoid = s.oid "
							 "AND d.objsubid = 0\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								NULL, "s.srvname", NULL, NULL,
								NULL, 1))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of foreign servers");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \deu
 *
 * Describes user mappings.
 */
bool
listUserMappings(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT um.srvname AS \"%s\",\n"
					  "  um.usename AS \"%s\"",
					  gettext_noop("Server"),
					  gettext_noop("User name"));

	if (verbose)
		appendPQExpBuffer(&buf,
						  ",\n CASE WHEN umoptions IS NULL THEN '' ELSE "
						  "  '(' || pg_catalog.array_to_string(ARRAY(SELECT "
						  "  pg_catalog.quote_ident(option_name) ||  ' ' || "
						  "  pg_catalog.quote_literal(option_value)  FROM "
						  "  pg_catalog.pg_options_to_table(umoptions)),  ', ') || ')' "
						  "  END AS \"%s\"",
						  gettext_noop("FDW options"));

	appendPQExpBufferStr(&buf, "\nFROM pg_catalog.pg_user_mappings um\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								NULL, "um.srvname", "um.usename", NULL,
								NULL, 1))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of user mappings");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \det
 *
 * Describes foreign tables.
 */
bool
listForeignTables(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT n.nspname AS \"%s\",\n"
					  "  c.relname AS \"%s\",\n"
					  "  s.srvname AS \"%s\"",
					  gettext_noop("Schema"),
					  gettext_noop("Table"),
					  gettext_noop("Server"));

	if (verbose)
		appendPQExpBuffer(&buf,
						  ",\n CASE WHEN ftoptions IS NULL THEN '' ELSE "
						  "  '(' || pg_catalog.array_to_string(ARRAY(SELECT "
						  "  pg_catalog.quote_ident(option_name) ||  ' ' || "
						  "  pg_catalog.quote_literal(option_value)  FROM "
						  "  pg_catalog.pg_options_to_table(ftoptions)),  ', ') || ')' "
						  "  END AS \"%s\",\n"
						  "  d.description AS \"%s\"",
						  gettext_noop("FDW options"),
						  gettext_noop("Description"));

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_foreign_table ft\n"
						 "  INNER JOIN pg_catalog.pg_class c"
						 " ON c.oid = ft.ftrelid\n"
						 "  INNER JOIN pg_catalog.pg_namespace n"
						 " ON n.oid = c.relnamespace\n"
						 "  INNER JOIN pg_catalog.pg_foreign_server s"
						 " ON s.oid = ft.ftserver\n");
	if (verbose)
		appendPQExpBufferStr(&buf,
							 "   LEFT JOIN pg_catalog.pg_description d\n"
							 "          ON d.classoid = c.tableoid AND "
							 "d.objoid = c.oid AND d.objsubid = 0\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								"n.nspname", "c.relname", NULL,
								"pg_catalog.pg_table_is_visible(c.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of foreign tables");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \dx
 *
 * Briefly describes installed extensions.
 */
bool
listExtensions(const char *pattern)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT e.extname AS \"%s\", "
					  "e.extversion AS \"%s\", ae.default_version AS \"%s\","
					  "n.nspname AS \"%s\", d.description AS \"%s\"\n"
					  "FROM pg_catalog.pg_extension e "
					  "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = e.extnamespace "
					  "LEFT JOIN pg_catalog.pg_description d ON d.objoid = e.oid "
					  "LEFT JOIN pg_catalog.pg_available_extensions() ae(name, default_version, comment) ON ae.name = e.extname "
					  "AND d.classoid = 'pg_catalog.pg_extension'::pg_catalog.regclass\n",
					  gettext_noop("Name"),
					  gettext_noop("Version"),
					  gettext_noop("Default version"),
					  gettext_noop("Schema"),
					  gettext_noop("Description"));

	if (!validateSQLNamePattern(&buf, pattern,
								false, false,
								NULL, "e.extname", NULL,
								NULL,
								NULL, 1))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of installed extensions");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \dx+
 *
 * List contents of installed extensions.
 */
bool
listExtensionContents(const char *pattern)
{
	PQExpBufferData buf;
	PGresult   *res;
	int			i;

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT e.extname, e.oid\n"
					  "FROM pg_catalog.pg_extension e\n");

	if (!validateSQLNamePattern(&buf, pattern,
								false, false,
								NULL, "e.extname", NULL,
								NULL,
								NULL, 1))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	if (PQntuples(res) == 0)
	{
		if (!pset.quiet)
		{
			if (pattern)
				pg_log_error("Did not find any extension named \"%s\".",
							 pattern);
			else
				pg_log_error("Did not find any extensions.");
		}
		PQclear(res);
		return false;
	}

	for (i = 0; i < PQntuples(res); i++)
	{
		const char *extname;
		const char *oid;

		extname = PQgetvalue(res, i, 0);
		oid = PQgetvalue(res, i, 1);

		if (!listOneExtensionContents(extname, oid))
		{
			PQclear(res);
			return false;
		}
		if (cancel_pressed)
		{
			PQclear(res);
			return false;
		}
	}

	PQclear(res);
	return true;
}

static bool
listOneExtensionContents(const char *extname, const char *oid)
{
	PQExpBufferData buf;
	PGresult   *res;
	PQExpBufferData title;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT pg_catalog.pg_describe_object(classid, objid, 0) AS \"%s\"\n"
					  "FROM pg_catalog.pg_depend\n"
					  "WHERE refclassid = 'pg_catalog.pg_extension'::pg_catalog.regclass AND refobjid = '%s' AND deptype = 'e'\n"
					  "ORDER BY 1;",
					  gettext_noop("Object description"),
					  oid);

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	initPQExpBuffer(&title);
	printfPQExpBuffer(&title, _("Objects in extension \"%s\""), extname);
	myopt.title = title.data;
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	termPQExpBuffer(&title);
	PQclear(res);
	return true;
}

/*
 * validateSQLNamePattern
 *
 * Wrapper around string_utils's processSQLNamePattern which also checks the
 * pattern's validity.  In addition to that function's parameters, takes a
 * 'maxparts' parameter specifying the maximum number of dotted names the
 * pattern is allowed to have, and a 'added_clause' parameter that returns by
 * reference whether a clause was added to 'buf'.  Returns whether the pattern
 * passed validation, after logging any errors.
 */
static bool
validateSQLNamePattern(PQExpBuffer buf, const char *pattern, bool have_where,
					   bool force_escape, const char *schemavar,
					   const char *namevar, const char *altnamevar,
					   const char *visibilityrule, bool *added_clause,
					   int maxparts)
{
	PQExpBufferData dbbuf;
	int			dotcnt;
	bool		added;

	initPQExpBuffer(&dbbuf);
	added = processSQLNamePattern(pset.db, buf, pattern, have_where, force_escape,
								  schemavar, namevar, altnamevar,
								  visibilityrule, &dbbuf, &dotcnt);
	if (added_clause != NULL)
		*added_clause = added;

	if (dotcnt >= maxparts)
	{
		pg_log_error("improper qualified name (too many dotted names): %s",
					 pattern);
		goto error_return;
	}

	if (maxparts > 1 && dotcnt == maxparts - 1)
	{
		if (PQdb(pset.db) == NULL)
		{
			pg_log_error("You are currently not connected to a database.");
			goto error_return;
		}
		if (strcmp(PQdb(pset.db), dbbuf.data) != 0)
		{
			pg_log_error("cross-database references are not implemented: %s",
						 pattern);
			goto error_return;
		}
	}
	termPQExpBuffer(&dbbuf);
	return true;

error_return:
	termPQExpBuffer(&dbbuf);
	return false;
}

/*
 * \dRp
 * Lists publications.
 *
 * Takes an optional regexp to select particular publications
 */
bool
listPublications(const char *pattern)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {false, false, false, false, false, false, false, false, false};

	if (pset.sversion < 100000)
	{
		char		sverbuf[32];

		pg_log_error("The server (version %s) does not support publications.",
					 formatPGVersionNumber(pset.sversion, false,
										   sverbuf, sizeof(sverbuf)));
		return true;
	}

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT pubname AS \"%s\",\n"
					  "  pg_catalog.pg_get_userbyid(pubowner) AS \"%s\",\n"
					  "  puballtables AS \"%s\",\n"
					  "  pubinsert AS \"%s\",\n"
					  "  pubupdate AS \"%s\",\n"
					  "  pubdelete AS \"%s\"",
					  gettext_noop("Name"),
					  gettext_noop("Owner"),
					  gettext_noop("All tables"),
					  gettext_noop("Inserts"),
					  gettext_noop("Updates"),
					  gettext_noop("Deletes"));
	if (pset.sversion >= 110000)
		appendPQExpBuffer(&buf,
						  ",\n  pubtruncate AS \"%s\"",
						  gettext_noop("Truncates"));
	if (pset.sversion >= 180000)
		appendPQExpBuffer(&buf,
						  ",\n (CASE pubgencols\n"
						  "    WHEN '%c' THEN 'none'\n"
						  "    WHEN '%c' THEN 'stored'\n"
						  "   END) AS \"%s\"",
						  PUBLISH_GENCOLS_NONE,
						  PUBLISH_GENCOLS_STORED,
						  gettext_noop("Generated columns"));
	if (pset.sversion >= 130000)
		appendPQExpBuffer(&buf,
						  ",\n  pubviaroot AS \"%s\"",
						  gettext_noop("Via root"));

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_publication\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								NULL, "pubname", NULL,
								NULL,
								NULL, 1))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of publications");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);

	return true;
}

/*
 * Add footer to publication description.
 */
static bool
addFooterToPublicationDesc(PQExpBuffer buf, const char *footermsg,
						   bool as_schema, printTableContent *const cont)
{
	PGresult   *res;
	int			count = 0;
	int			i = 0;

	res = PSQLexec(buf->data);
	if (!res)
		return false;
	else
		count = PQntuples(res);

	if (count > 0)
		printTableAddFooter(cont, footermsg);

	for (i = 0; i < count; i++)
	{
		if (as_schema)
			printfPQExpBuffer(buf, "    \"%s\"", PQgetvalue(res, i, 0));
		else
		{
			printfPQExpBuffer(buf, "    \"%s.%s\"", PQgetvalue(res, i, 0),
							  PQgetvalue(res, i, 1));

			if (!PQgetisnull(res, i, 3))
				appendPQExpBuffer(buf, " (%s)", PQgetvalue(res, i, 3));

			if (!PQgetisnull(res, i, 2))
				appendPQExpBuffer(buf, " WHERE %s", PQgetvalue(res, i, 2));
		}

		printTableAddFooter(cont, buf->data);
	}

	PQclear(res);
	return true;
}

/*
 * \dRp+
 * Describes publications including the contents.
 *
 * Takes an optional regexp to select particular publications
 */
bool
describePublications(const char *pattern)
{
	PQExpBufferData buf;
	int			i;
	PGresult   *res;
	bool		has_pubtruncate;
	bool		has_pubgencols;
	bool		has_pubviaroot;

	PQExpBufferData title;
	printTableContent cont;

	if (pset.sversion < 100000)
	{
		char		sverbuf[32];

		pg_log_error("The server (version %s) does not support publications.",
					 formatPGVersionNumber(pset.sversion, false,
										   sverbuf, sizeof(sverbuf)));
		return true;
	}

	has_pubtruncate = (pset.sversion >= 110000);
	has_pubgencols = (pset.sversion >= 180000);
	has_pubviaroot = (pset.sversion >= 130000);

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT oid, pubname,\n"
					  "  pg_catalog.pg_get_userbyid(pubowner) AS owner,\n"
					  "  puballtables, pubinsert, pubupdate, pubdelete");
	if (has_pubtruncate)
		appendPQExpBufferStr(&buf,
							 ", pubtruncate");
	else
		appendPQExpBufferStr(&buf,
							 ", false AS pubtruncate");

	if (has_pubgencols)
		appendPQExpBuffer(&buf,
						  ", (CASE pubgencols\n"
						  "    WHEN '%c' THEN 'none'\n"
						  "    WHEN '%c' THEN 'stored'\n"
						  "   END) AS \"%s\"\n",
						  PUBLISH_GENCOLS_NONE,
						  PUBLISH_GENCOLS_STORED,
						  gettext_noop("Generated columns"));
	else
		appendPQExpBufferStr(&buf,
							 ", 'none' AS pubgencols");

	if (has_pubviaroot)
		appendPQExpBufferStr(&buf,
							 ", pubviaroot");
	else
		appendPQExpBufferStr(&buf,
							 ", false AS pubviaroot");

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_publication\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								NULL, "pubname", NULL,
								NULL,
								NULL, 1))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 2;");

	res = PSQLexec(buf.data);
	if (!res)
	{
		termPQExpBuffer(&buf);
		return false;
	}

	if (PQntuples(res) == 0)
	{
		if (!pset.quiet)
		{
			if (pattern)
				pg_log_error("Did not find any publication named \"%s\".",
							 pattern);
			else
				pg_log_error("Did not find any publications.");
		}

		termPQExpBuffer(&buf);
		PQclear(res);
		return false;
	}

	for (i = 0; i < PQntuples(res); i++)
	{
		const char	align = 'l';
		int			ncols = 5;
		int			nrows = 1;
		char	   *pubid = PQgetvalue(res, i, 0);
		char	   *pubname = PQgetvalue(res, i, 1);
		bool		puballtables = strcmp(PQgetvalue(res, i, 3), "t") == 0;
		printTableOpt myopt = pset.popt.topt;

		if (has_pubtruncate)
			ncols++;
		if (has_pubgencols)
			ncols++;
		if (has_pubviaroot)
			ncols++;

		initPQExpBuffer(&title);
		printfPQExpBuffer(&title, _("Publication %s"), pubname);
		printTableInit(&cont, &myopt, title.data, ncols, nrows);

		printTableAddHeader(&cont, gettext_noop("Owner"), true, align);
		printTableAddHeader(&cont, gettext_noop("All tables"), true, align);
		printTableAddHeader(&cont, gettext_noop("Inserts"), true, align);
		printTableAddHeader(&cont, gettext_noop("Updates"), true, align);
		printTableAddHeader(&cont, gettext_noop("Deletes"), true, align);
		if (has_pubtruncate)
			printTableAddHeader(&cont, gettext_noop("Truncates"), true, align);
		if (has_pubgencols)
			printTableAddHeader(&cont, gettext_noop("Generated columns"), true, align);
		if (has_pubviaroot)
			printTableAddHeader(&cont, gettext_noop("Via root"), true, align);

		printTableAddCell(&cont, PQgetvalue(res, i, 2), false, false);
		printTableAddCell(&cont, PQgetvalue(res, i, 3), false, false);
		printTableAddCell(&cont, PQgetvalue(res, i, 4), false, false);
		printTableAddCell(&cont, PQgetvalue(res, i, 5), false, false);
		printTableAddCell(&cont, PQgetvalue(res, i, 6), false, false);
		if (has_pubtruncate)
			printTableAddCell(&cont, PQgetvalue(res, i, 7), false, false);
		if (has_pubgencols)
			printTableAddCell(&cont, PQgetvalue(res, i, 8), false, false);
		if (has_pubviaroot)
			printTableAddCell(&cont, PQgetvalue(res, i, 9), false, false);

		if (!puballtables)
		{
			/* Get the tables for the specified publication */
			printfPQExpBuffer(&buf,
							  "SELECT n.nspname, c.relname");
			if (pset.sversion >= 150000)
			{
				appendPQExpBufferStr(&buf,
									 ", pg_get_expr(pr.prqual, c.oid)");
				appendPQExpBufferStr(&buf,
									 ", (CASE WHEN pr.prattrs IS NOT NULL THEN\n"
									 "     pg_catalog.array_to_string("
									 "      ARRAY(SELECT attname\n"
									 "              FROM\n"
									 "                pg_catalog.generate_series(0, pg_catalog.array_upper(pr.prattrs::pg_catalog.int2[], 1)) s,\n"
									 "                pg_catalog.pg_attribute\n"
									 "        WHERE attrelid = c.oid AND attnum = prattrs[s]), ', ')\n"
									 "       ELSE NULL END)");
			}
			else
				appendPQExpBufferStr(&buf,
									 ", NULL, NULL");
			appendPQExpBuffer(&buf,
							  "\nFROM pg_catalog.pg_class c,\n"
							  "     pg_catalog.pg_namespace n,\n"
							  "     pg_catalog.pg_publication_rel pr\n"
							  "WHERE c.relnamespace = n.oid\n"
							  "  AND c.oid = pr.prrelid\n"
							  "  AND pr.prpubid = '%s'\n"
							  "ORDER BY 1,2", pubid);
			if (!addFooterToPublicationDesc(&buf, _("Tables:"), false, &cont))
				goto error_return;

			if (pset.sversion >= 150000)
			{
				/* Get the schemas for the specified publication */
				printfPQExpBuffer(&buf,
								  "SELECT n.nspname\n"
								  "FROM pg_catalog.pg_namespace n\n"
								  "     JOIN pg_catalog.pg_publication_namespace pn ON n.oid = pn.pnnspid\n"
								  "WHERE pn.pnpubid = '%s'\n"
								  "ORDER BY 1", pubid);
				if (!addFooterToPublicationDesc(&buf, _("Tables from schemas:"),
												true, &cont))
					goto error_return;
			}
		}

		printTable(&cont, pset.queryFout, false, pset.logfile);
		printTableCleanup(&cont);

		termPQExpBuffer(&title);
	}

	termPQExpBuffer(&buf);
	PQclear(res);

	return true;

error_return:
	printTableCleanup(&cont);
	PQclear(res);
	termPQExpBuffer(&buf);
	termPQExpBuffer(&title);
	return false;
}

/*
 * \dRs
 * Describes subscriptions.
 *
 * Takes an optional regexp to select particular subscriptions
 */
bool
describeSubscriptions(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {false, false, false, false,
		false, false, false, false, false, false, false, false, false, false,
	false};

	if (pset.sversion < 100000)
	{
		char		sverbuf[32];

		pg_log_error("The server (version %s) does not support subscriptions.",
					 formatPGVersionNumber(pset.sversion, false,
										   sverbuf, sizeof(sverbuf)));
		return true;
	}

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT subname AS \"%s\"\n"
					  ",  pg_catalog.pg_get_userbyid(subowner) AS \"%s\"\n"
					  ",  subenabled AS \"%s\"\n"
					  ",  subpublications AS \"%s\"\n",
					  gettext_noop("Name"),
					  gettext_noop("Owner"),
					  gettext_noop("Enabled"),
					  gettext_noop("Publication"));

	if (verbose)
	{
		/* Binary mode and streaming are only supported in v14 and higher */
		if (pset.sversion >= 140000)
		{
			appendPQExpBuffer(&buf,
							  ", subbinary AS \"%s\"\n",
							  gettext_noop("Binary"));

			if (pset.sversion >= 160000)
				appendPQExpBuffer(&buf,
								  ", (CASE substream\n"
								  "    WHEN " CppAsString2(LOGICALREP_STREAM_OFF) " THEN 'off'\n"
								  "    WHEN " CppAsString2(LOGICALREP_STREAM_ON) " THEN 'on'\n"
								  "    WHEN " CppAsString2(LOGICALREP_STREAM_PARALLEL) " THEN 'parallel'\n"
								  "   END) AS \"%s\"\n",
								  gettext_noop("Streaming"));
			else
				appendPQExpBuffer(&buf,
								  ", substream AS \"%s\"\n",
								  gettext_noop("Streaming"));
		}

		/* Two_phase and disable_on_error are only supported in v15 and higher */
		if (pset.sversion >= 150000)
			appendPQExpBuffer(&buf,
							  ", subtwophasestate AS \"%s\"\n"
							  ", subdisableonerr AS \"%s\"\n",
							  gettext_noop("Two-phase commit"),
							  gettext_noop("Disable on error"));

		if (pset.sversion >= 160000)
			appendPQExpBuffer(&buf,
							  ", suborigin AS \"%s\"\n"
							  ", subpasswordrequired AS \"%s\"\n"
							  ", subrunasowner AS \"%s\"\n",
							  gettext_noop("Origin"),
							  gettext_noop("Password required"),
							  gettext_noop("Run as owner?"));

		if (pset.sversion >= 170000)
			appendPQExpBuffer(&buf,
							  ", subfailover AS \"%s\"\n",
							  gettext_noop("Failover"));

		appendPQExpBuffer(&buf,
						  ",  subsynccommit AS \"%s\"\n"
						  ",  subconninfo AS \"%s\"\n",
						  gettext_noop("Synchronous commit"),
						  gettext_noop("Conninfo"));

		/* Skip LSN is only supported in v15 and higher */
		if (pset.sversion >= 150000)
			appendPQExpBuffer(&buf,
							  ", subskiplsn AS \"%s\"\n",
							  gettext_noop("Skip LSN"));
	}

	/* Only display subscriptions in current database. */
	appendPQExpBufferStr(&buf,
						 "FROM pg_catalog.pg_subscription\n"
						 "WHERE subdbid = (SELECT oid\n"
						 "                 FROM pg_catalog.pg_database\n"
						 "                 WHERE datname = pg_catalog.current_database())");

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								NULL, "subname", NULL,
								NULL,
								NULL, 1))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of subscriptions");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * printACLColumn
 *
 * Helper function for consistently formatting ACL (privilege) columns.
 * The proper targetlist entry is appended to buf.  Note lack of any
 * whitespace or comma decoration.
 *
 * If you change this, see also the handling of attacl in permissionsList(),
 * which can't conveniently use this code.
 */
static void
printACLColumn(PQExpBuffer buf, const char *colname)
{
	appendPQExpBuffer(buf,
					  "CASE"
					  " WHEN pg_catalog.array_length(%s, 1) = 0 THEN '%s'"
					  " ELSE pg_catalog.array_to_string(%s, E'\\n')"
					  " END AS \"%s\"",
					  colname, gettext_noop("(none)"),
					  colname, gettext_noop("Access privileges"));
}

/*
 * \dAc
 * Lists operator classes
 *
 * Takes optional regexps to filter by index access method and input data type.
 */
bool
listOperatorClasses(const char *access_method_pattern,
					const char *type_pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	bool		have_where = false;
	static const bool translate_columns[] = {false, false, false, false, false, false, false};

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT\n"
					  "  am.amname AS \"%s\",\n"
					  "  pg_catalog.format_type(c.opcintype, NULL) AS \"%s\",\n"
					  "  CASE\n"
					  "    WHEN c.opckeytype <> 0 AND c.opckeytype <> c.opcintype\n"
					  "    THEN pg_catalog.format_type(c.opckeytype, NULL)\n"
					  "    ELSE NULL\n"
					  "  END AS \"%s\",\n"
					  "  CASE\n"
					  "    WHEN pg_catalog.pg_opclass_is_visible(c.oid)\n"
					  "    THEN pg_catalog.format('%%I', c.opcname)\n"
					  "    ELSE pg_catalog.format('%%I.%%I', n.nspname, c.opcname)\n"
					  "  END AS \"%s\",\n"
					  "  (CASE WHEN c.opcdefault\n"
					  "    THEN '%s'\n"
					  "    ELSE '%s'\n"
					  "  END) AS \"%s\"",
					  gettext_noop("AM"),
					  gettext_noop("Input type"),
					  gettext_noop("Storage type"),
					  gettext_noop("Operator class"),
					  gettext_noop("yes"),
					  gettext_noop("no"),
					  gettext_noop("Default?"));
	if (verbose)
		appendPQExpBuffer(&buf,
						  ",\n  CASE\n"
						  "    WHEN pg_catalog.pg_opfamily_is_visible(of.oid)\n"
						  "    THEN pg_catalog.format('%%I', of.opfname)\n"
						  "    ELSE pg_catalog.format('%%I.%%I', ofn.nspname, of.opfname)\n"
						  "  END AS \"%s\",\n"
						  " pg_catalog.pg_get_userbyid(c.opcowner) AS \"%s\"\n",
						  gettext_noop("Operator family"),
						  gettext_noop("Owner"));
	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_opclass c\n"
						 "  LEFT JOIN pg_catalog.pg_am am on am.oid = c.opcmethod\n"
						 "  LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.opcnamespace\n"
						 "  LEFT JOIN pg_catalog.pg_type t ON t.oid = c.opcintype\n"
						 "  LEFT JOIN pg_catalog.pg_namespace tn ON tn.oid = t.typnamespace\n");
	if (verbose)
		appendPQExpBufferStr(&buf,
							 "  LEFT JOIN pg_catalog.pg_opfamily of ON of.oid = c.opcfamily\n"
							 "  LEFT JOIN pg_catalog.pg_namespace ofn ON ofn.oid = of.opfnamespace\n");

	if (access_method_pattern)
		if (!validateSQLNamePattern(&buf, access_method_pattern,
									false, false, NULL, "am.amname", NULL, NULL,
									&have_where, 1))
			goto error_return;
	if (type_pattern)
	{
		/* Match type name pattern against either internal or external name */
		if (!validateSQLNamePattern(&buf, type_pattern, have_where, false,
									"tn.nspname", "t.typname",
									"pg_catalog.format_type(t.oid, NULL)",
									"pg_catalog.pg_type_is_visible(t.oid)",
									NULL, 3))
			goto error_return;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2, 4;");
	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of operator classes");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;

error_return:
	termPQExpBuffer(&buf);
	return false;
}

/*
 * \dAf
 * Lists operator families
 *
 * Takes optional regexps to filter by index access method and input data type.
 */
bool
listOperatorFamilies(const char *access_method_pattern,
					 const char *type_pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	bool		have_where = false;
	static const bool translate_columns[] = {false, false, false, false};

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT\n"
					  "  am.amname AS \"%s\",\n"
					  "  CASE\n"
					  "    WHEN pg_catalog.pg_opfamily_is_visible(f.oid)\n"
					  "    THEN pg_catalog.format('%%I', f.opfname)\n"
					  "    ELSE pg_catalog.format('%%I.%%I', n.nspname, f.opfname)\n"
					  "  END AS \"%s\",\n"
					  "  (SELECT\n"
					  "     pg_catalog.string_agg(pg_catalog.format_type(oc.opcintype, NULL), ', ')\n"
					  "   FROM pg_catalog.pg_opclass oc\n"
					  "   WHERE oc.opcfamily = f.oid) \"%s\"",
					  gettext_noop("AM"),
					  gettext_noop("Operator family"),
					  gettext_noop("Applicable types"));
	if (verbose)
		appendPQExpBuffer(&buf,
						  ",\n  pg_catalog.pg_get_userbyid(f.opfowner) AS \"%s\"\n",
						  gettext_noop("Owner"));
	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_opfamily f\n"
						 "  LEFT JOIN pg_catalog.pg_am am on am.oid = f.opfmethod\n"
						 "  LEFT JOIN pg_catalog.pg_namespace n ON n.oid = f.opfnamespace\n");

	if (access_method_pattern)
		if (!validateSQLNamePattern(&buf, access_method_pattern,
									false, false, NULL, "am.amname", NULL, NULL,
									&have_where, 1))
			goto error_return;
	if (type_pattern)
	{
		appendPQExpBuffer(&buf,
						  "  %s EXISTS (\n"
						  "    SELECT 1\n"
						  "    FROM pg_catalog.pg_type t\n"
						  "    JOIN pg_catalog.pg_opclass oc ON oc.opcintype = t.oid\n"
						  "    LEFT JOIN pg_catalog.pg_namespace tn ON tn.oid = t.typnamespace\n"
						  "    WHERE oc.opcfamily = f.oid\n",
						  have_where ? "AND" : "WHERE");
		/* Match type name pattern against either internal or external name */
		if (!validateSQLNamePattern(&buf, type_pattern, true, false,
									"tn.nspname", "t.typname",
									"pg_catalog.format_type(t.oid, NULL)",
									"pg_catalog.pg_type_is_visible(t.oid)",
									NULL, 3))
			goto error_return;
		appendPQExpBufferStr(&buf, "  )\n");
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");
	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of operator families");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;

error_return:
	termPQExpBuffer(&buf);
	return false;
}

/*
 * \dAo
 * Lists operators of operator families
 *
 * Takes optional regexps to filter by index access method and operator
 * family.
 */
bool
listOpFamilyOperators(const char *access_method_pattern,
					  const char *family_pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	bool		have_where = false;

	static const bool translate_columns[] = {false, false, false, false, false, false, true};

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT\n"
					  "  am.amname AS \"%s\",\n"
					  "  CASE\n"
					  "    WHEN pg_catalog.pg_opfamily_is_visible(of.oid)\n"
					  "    THEN pg_catalog.format('%%I', of.opfname)\n"
					  "    ELSE pg_catalog.format('%%I.%%I', nsf.nspname, of.opfname)\n"
					  "  END AS \"%s\",\n"
					  "  o.amopopr::pg_catalog.regoperator AS \"%s\"\n,"
					  "  o.amopstrategy AS \"%s\",\n"
					  "  CASE o.amoppurpose\n"
					  "    WHEN " CppAsString2(AMOP_ORDER) " THEN '%s'\n"
					  "    WHEN " CppAsString2(AMOP_SEARCH) " THEN '%s'\n"
					  "  END AS \"%s\"\n",
					  gettext_noop("AM"),
					  gettext_noop("Operator family"),
					  gettext_noop("Operator"),
					  gettext_noop("Strategy"),
					  gettext_noop("ordering"),
					  gettext_noop("search"),
					  gettext_noop("Purpose"));

	if (verbose)
		appendPQExpBuffer(&buf,
						  ", ofs.opfname AS \"%s\",\n"
						  "  CASE\n"
						  "    WHEN p.proleakproof THEN '%s'\n"
						  "    ELSE '%s'\n"
						  "  END AS \"%s\"\n",
						  gettext_noop("Sort opfamily"),
						  gettext_noop("yes"),
						  gettext_noop("no"),
						  gettext_noop("Leakproof?"));
	appendPQExpBufferStr(&buf,
						 "FROM pg_catalog.pg_amop o\n"
						 "  LEFT JOIN pg_catalog.pg_opfamily of ON of.oid = o.amopfamily\n"
						 "  LEFT JOIN pg_catalog.pg_am am ON am.oid = of.opfmethod AND am.oid = o.amopmethod\n"
						 "  LEFT JOIN pg_catalog.pg_namespace nsf ON of.opfnamespace = nsf.oid\n");
	if (verbose)
		appendPQExpBufferStr(&buf,
							 "  LEFT JOIN pg_catalog.pg_opfamily ofs ON ofs.oid = o.amopsortfamily\n"
							 "  LEFT JOIN pg_catalog.pg_operator op ON op.oid = o.amopopr\n"
							 "  LEFT JOIN pg_catalog.pg_proc p ON p.oid = op.oprcode\n");

	if (access_method_pattern)
	{
		if (!validateSQLNamePattern(&buf, access_method_pattern,
									false, false, NULL, "am.amname",
									NULL, NULL,
									&have_where, 1))
			goto error_return;
	}

	if (family_pattern)
	{
		if (!validateSQLNamePattern(&buf, family_pattern, have_where, false,
									"nsf.nspname", "of.opfname", NULL, NULL,
									NULL, 3))
			goto error_return;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2,\n"
						 "  o.amoplefttype = o.amoprighttype DESC,\n"
						 "  pg_catalog.format_type(o.amoplefttype, NULL),\n"
						 "  pg_catalog.format_type(o.amoprighttype, NULL),\n"
						 "  o.amopstrategy;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of operators of operator families");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;

error_return:
	termPQExpBuffer(&buf);
	return false;
}

/*
 * \dAp
 * Lists support functions of operator families
 *
 * Takes optional regexps to filter by index access method and operator
 * family.
 */
bool
listOpFamilyFunctions(const char *access_method_pattern,
					  const char *family_pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	bool		have_where = false;
	static const bool translate_columns[] = {false, false, false, false, false, false};

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT\n"
					  "  am.amname AS \"%s\",\n"
					  "  CASE\n"
					  "    WHEN pg_catalog.pg_opfamily_is_visible(of.oid)\n"
					  "    THEN pg_catalog.format('%%I', of.opfname)\n"
					  "    ELSE pg_catalog.format('%%I.%%I', ns.nspname, of.opfname)\n"
					  "  END AS \"%s\",\n"
					  "  pg_catalog.format_type(ap.amproclefttype, NULL) AS \"%s\",\n"
					  "  pg_catalog.format_type(ap.amprocrighttype, NULL) AS \"%s\",\n"
					  "  ap.amprocnum AS \"%s\"\n",
					  gettext_noop("AM"),
					  gettext_noop("Operator family"),
					  gettext_noop("Registered left type"),
					  gettext_noop("Registered right type"),
					  gettext_noop("Number"));

	if (!verbose)
		appendPQExpBuffer(&buf,
						  ", p.proname AS \"%s\"\n",
						  gettext_noop("Function"));
	else
		appendPQExpBuffer(&buf,
						  ", ap.amproc::pg_catalog.regprocedure AS \"%s\"\n",
						  gettext_noop("Function"));

	appendPQExpBufferStr(&buf,
						 "FROM pg_catalog.pg_amproc ap\n"
						 "  LEFT JOIN pg_catalog.pg_opfamily of ON of.oid = ap.amprocfamily\n"
						 "  LEFT JOIN pg_catalog.pg_am am ON am.oid = of.opfmethod\n"
						 "  LEFT JOIN pg_catalog.pg_namespace ns ON of.opfnamespace = ns.oid\n"
						 "  LEFT JOIN pg_catalog.pg_proc p ON ap.amproc = p.oid\n");

	if (access_method_pattern)
	{
		if (!validateSQLNamePattern(&buf, access_method_pattern,
									false, false, NULL, "am.amname",
									NULL, NULL,
									&have_where, 1))
			goto error_return;
	}
	if (family_pattern)
	{
		if (!validateSQLNamePattern(&buf, family_pattern, have_where, false,
									"ns.nspname", "of.opfname", NULL, NULL,
									NULL, 3))
			goto error_return;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2,\n"
						 "  ap.amproclefttype = ap.amprocrighttype DESC,\n"
						 "  3, 4, 5;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("List of support functions of operator families");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;

error_return:
	termPQExpBuffer(&buf);
	return false;
}

/*
 * \dl or \lo_list
 * Lists large objects
 */
bool
listLargeObjects(bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT oid as \"%s\",\n"
					  "  pg_catalog.pg_get_userbyid(lomowner) as \"%s\",\n  ",
					  gettext_noop("ID"),
					  gettext_noop("Owner"));

	if (verbose)
	{
		printACLColumn(&buf, "lomacl");
		appendPQExpBufferStr(&buf, ",\n  ");
	}

	appendPQExpBuffer(&buf,
					  "pg_catalog.obj_description(oid, 'pg_largeobject') as \"%s\"\n"
					  "FROM pg_catalog.pg_largeobject_metadata\n"
					  "ORDER BY oid",
					  gettext_noop("Description"));

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.title = _("Large objects");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}
