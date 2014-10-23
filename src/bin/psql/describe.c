/*
 * psql - the PostgreSQL interactive terminal
 *
 * Support for the various \d ("describe") commands.  Note that the current
 * expectation is that all functions in this file will succeed when working
 * with servers of versions 7.4 and up.  It's okay to omit irrelevant
 * information for an old server, but not to fail outright.
 *
 * Copyright (c) 2000-2014, PostgreSQL Global Development Group
 *
 * src/bin/psql/describe.c
 */
#include "postgres_fe.h"

#include <ctype.h>

#include "catalog/pg_default_acl.h"

#include "common.h"
#include "describe.h"
#include "dumputils.h"
#include "mbprint.h"
#include "print.h"
#include "settings.h"
#include "variables.h"


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


/*----------------
 * Handlers for various slash commands displaying some sort of list
 * of things in the database.
 *
 * Note: try to format the queries to look nice in -E output.
 *----------------
 */


/* \da
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
				 "  pg_catalog.format_type(p.prorettype, NULL) AS \"%s\",\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("Result data type"));

	if (pset.sversion >= 80400)
		appendPQExpBuffer(&buf,
						  "  CASE WHEN p.pronargs = 0\n"
						  "    THEN CAST('*' AS pg_catalog.text)\n"
					 "    ELSE pg_catalog.pg_get_function_arguments(p.oid)\n"
						  "  END AS \"%s\",\n",
						  gettext_noop("Argument data types"));
	else if (pset.sversion >= 80200)
		appendPQExpBuffer(&buf,
						  "  CASE WHEN p.pronargs = 0\n"
						  "    THEN CAST('*' AS pg_catalog.text)\n"
						  "    ELSE\n"
						  "    pg_catalog.array_to_string(ARRAY(\n"
						  "      SELECT\n"
				 "        pg_catalog.format_type(p.proargtypes[s.i], NULL)\n"
						  "      FROM\n"
						  "        pg_catalog.generate_series(0, pg_catalog.array_upper(p.proargtypes, 1)) AS s(i)\n"
						  "    ), ', ')\n"
						  "  END AS \"%s\",\n",
						  gettext_noop("Argument data types"));
	else
		appendPQExpBuffer(&buf,
			 "  pg_catalog.format_type(p.proargtypes[0], NULL) AS \"%s\",\n",
						  gettext_noop("Argument data types"));

	appendPQExpBuffer(&buf,
				 "  pg_catalog.obj_description(p.oid, 'pg_proc') as \"%s\"\n"
					  "FROM pg_catalog.pg_proc p\n"
	   "     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace\n"
					  "WHERE p.proisagg\n",
					  gettext_noop("Description"));

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	processSQLNamePattern(pset.db, &buf, pattern, true, false,
						  "n.nspname", "p.proname", NULL,
						  "pg_catalog.pg_function_is_visible(p.oid)");

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2, 4;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of aggregate functions");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

	PQclear(res);
	return true;
}

/* \db
 * Takes an optional regexp to select particular tablespaces
 */
bool
describeTablespaces(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	if (pset.sversion < 80000)
	{
		psql_error("The server (version %d.%d) does not support tablespaces.\n",
				   pset.sversion / 10000, (pset.sversion / 100) % 100);
		return true;
	}

	initPQExpBuffer(&buf);

	if (pset.sversion >= 90200)
		printfPQExpBuffer(&buf,
						  "SELECT spcname AS \"%s\",\n"
						"  pg_catalog.pg_get_userbyid(spcowner) AS \"%s\",\n"
						"  pg_catalog.pg_tablespace_location(oid) AS \"%s\"",
						  gettext_noop("Name"),
						  gettext_noop("Owner"),
						  gettext_noop("Location"));
	else
		printfPQExpBuffer(&buf,
						  "SELECT spcname AS \"%s\",\n"
						"  pg_catalog.pg_get_userbyid(spcowner) AS \"%s\",\n"
						  "  spclocation AS \"%s\"",
						  gettext_noop("Name"),
						  gettext_noop("Owner"),
						  gettext_noop("Location"));

	if (verbose)
	{
		appendPQExpBufferStr(&buf, ",\n  ");
		printACLColumn(&buf, "spcacl");
	}

	if (verbose && pset.sversion >= 90000)
		appendPQExpBuffer(&buf,
						  ",\n  spcoptions AS \"%s\"",
						  gettext_noop("Options"));

	if (verbose && pset.sversion >= 90200)
		appendPQExpBuffer(&buf,
						  ",\n  pg_catalog.pg_size_pretty(pg_catalog.pg_tablespace_size(oid)) AS \"%s\"",
						  gettext_noop("Size"));

	if (verbose && pset.sversion >= 80200)
		appendPQExpBuffer(&buf,
		 ",\n  pg_catalog.shobj_description(oid, 'pg_tablespace') AS \"%s\"",
						  gettext_noop("Description"));

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_tablespace\n");

	processSQLNamePattern(pset.db, &buf, pattern, false, false,
						  NULL, "spcname", NULL,
						  NULL);

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of tablespaces");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

	PQclear(res);
	return true;
}


/* \df
 * Takes an optional regexp to select particular functions.
 *
 * As with \d, you can specify the kinds of functions you want:
 *
 * a for aggregates
 * n for normal
 * t for trigger
 * w for window
 *
 * and you can mix and match these in any order.
 */
bool
describeFunctions(const char *functypes, const char *pattern, bool verbose, bool showSystem)
{
	bool		showAggregate = strchr(functypes, 'a') != NULL;
	bool		showNormal = strchr(functypes, 'n') != NULL;
	bool		showTrigger = strchr(functypes, 't') != NULL;
	bool		showWindow = strchr(functypes, 'w') != NULL;
	bool		have_where;
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {false, false, false, false, true, true, true, false, false, false, false};

	if (strlen(functypes) != strspn(functypes, "antwS+"))
	{
		psql_error("\\df only takes [antwS+] as options\n");
		return true;
	}

	if (showWindow && pset.sversion < 80400)
	{
		psql_error("\\df does not take a \"w\" option with server version %d.%d\n",
				   pset.sversion / 10000, (pset.sversion / 100) % 100);
		return true;
	}

	if (!showAggregate && !showNormal && !showTrigger && !showWindow)
	{
		showAggregate = showNormal = showTrigger = true;
		if (pset.sversion >= 80400)
			showWindow = true;
	}

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					  "  p.proname as \"%s\",\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"));

	if (pset.sversion >= 80400)
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
						  gettext_noop("normal"),
						  gettext_noop("Type"));
	else if (pset.sversion >= 80100)
		appendPQExpBuffer(&buf,
					 "  CASE WHEN p.proretset THEN 'SETOF ' ELSE '' END ||\n"
				  "  pg_catalog.format_type(p.prorettype, NULL) as \"%s\",\n"
						  "  CASE WHEN proallargtypes IS NOT NULL THEN\n"
						  "    pg_catalog.array_to_string(ARRAY(\n"
						  "      SELECT\n"
						  "        CASE\n"
						  "          WHEN p.proargmodes[s.i] = 'i' THEN ''\n"
					  "          WHEN p.proargmodes[s.i] = 'o' THEN 'OUT '\n"
					"          WHEN p.proargmodes[s.i] = 'b' THEN 'INOUT '\n"
				 "          WHEN p.proargmodes[s.i] = 'v' THEN 'VARIADIC '\n"
						  "        END ||\n"
						  "        CASE\n"
			 "          WHEN COALESCE(p.proargnames[s.i], '') = '' THEN ''\n"
						  "          ELSE p.proargnames[s.i] || ' ' \n"
						  "        END ||\n"
			  "        pg_catalog.format_type(p.proallargtypes[s.i], NULL)\n"
						  "      FROM\n"
						  "        pg_catalog.generate_series(1, pg_catalog.array_upper(p.proallargtypes, 1)) AS s(i)\n"
						  "    ), ', ')\n"
						  "  ELSE\n"
						  "    pg_catalog.array_to_string(ARRAY(\n"
						  "      SELECT\n"
						  "        CASE\n"
		   "          WHEN COALESCE(p.proargnames[s.i+1], '') = '' THEN ''\n"
						  "          ELSE p.proargnames[s.i+1] || ' '\n"
						  "          END ||\n"
				 "        pg_catalog.format_type(p.proargtypes[s.i], NULL)\n"
						  "      FROM\n"
						  "        pg_catalog.generate_series(0, pg_catalog.array_upper(p.proargtypes, 1)) AS s(i)\n"
						  "    ), ', ')\n"
						  "  END AS \"%s\",\n"
						  "  CASE\n"
						  "    WHEN p.proisagg THEN '%s'\n"
						  "    WHEN p.prorettype = 'pg_catalog.trigger'::pg_catalog.regtype THEN '%s'\n"
						  "    ELSE '%s'\n"
						  "  END AS \"%s\"",
						  gettext_noop("Result data type"),
						  gettext_noop("Argument data types"),
		/* translator: "agg" is short for "aggregate" */
						  gettext_noop("agg"),
						  gettext_noop("trigger"),
						  gettext_noop("normal"),
						  gettext_noop("Type"));
	else
		appendPQExpBuffer(&buf,
					 "  CASE WHEN p.proretset THEN 'SETOF ' ELSE '' END ||\n"
				  "  pg_catalog.format_type(p.prorettype, NULL) as \"%s\",\n"
					"  pg_catalog.oidvectortypes(p.proargtypes) as \"%s\",\n"
						  "  CASE\n"
						  "    WHEN p.proisagg THEN '%s'\n"
						  "    WHEN p.prorettype = 'pg_catalog.trigger'::pg_catalog.regtype THEN '%s'\n"
						  "    ELSE '%s'\n"
						  "  END AS \"%s\"",
						  gettext_noop("Result data type"),
						  gettext_noop("Argument data types"),
		/* translator: "agg" is short for "aggregate" */
						  gettext_noop("agg"),
						  gettext_noop("trigger"),
						  gettext_noop("normal"),
						  gettext_noop("Type"));

	if (verbose)
		appendPQExpBuffer(&buf,
				  ",\n CASE WHEN prosecdef THEN '%s' ELSE '%s' END AS \"%s\""
						  ",\n CASE\n"
						  "  WHEN p.provolatile = 'i' THEN '%s'\n"
						  "  WHEN p.provolatile = 's' THEN '%s'\n"
						  "  WHEN p.provolatile = 'v' THEN '%s'\n"
						  " END as \"%s\""
				   ",\n  pg_catalog.pg_get_userbyid(p.proowner) as \"%s\",\n"
						  "  l.lanname as \"%s\",\n"
						  "  p.prosrc as \"%s\",\n"
				  "  pg_catalog.obj_description(p.oid, 'pg_proc') as \"%s\"",
						  gettext_noop("definer"),
						  gettext_noop("invoker"),
						  gettext_noop("Security"),
						  gettext_noop("immutable"),
						  gettext_noop("stable"),
						  gettext_noop("volatile"),
						  gettext_noop("Volatility"),
						  gettext_noop("Owner"),
						  gettext_noop("Language"),
						  gettext_noop("Source code"),
						  gettext_noop("Description"));

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_proc p"
	"\n     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace\n");

	if (verbose)
		appendPQExpBufferStr(&buf,
		   "     LEFT JOIN pg_catalog.pg_language l ON l.oid = p.prolang\n");

	have_where = false;

	/* filter by function type, if requested */
	if (showNormal && showAggregate && showTrigger && showWindow)
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
			appendPQExpBufferStr(&buf, "NOT p.proisagg\n");
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
		if (!showWindow && pset.sversion >= 80400)
		{
			if (have_where)
				appendPQExpBufferStr(&buf, "      AND ");
			else
			{
				appendPQExpBufferStr(&buf, "WHERE ");
				have_where = true;
			}
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
		if (showWindow)
		{
			if (needs_or)
				appendPQExpBufferStr(&buf, "       OR ");
			appendPQExpBufferStr(&buf, "p.proiswindow\n");
			needs_or = true;
		}
		appendPQExpBufferStr(&buf, "      )\n");
	}

	processSQLNamePattern(pset.db, &buf, pattern, have_where, false,
						  "n.nspname", "p.proname", NULL,
						  "pg_catalog.pg_function_is_visible(p.oid)");

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2, 4;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of functions");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

	PQclear(res);
	return true;
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
		appendPQExpBuffer(&buf,
						  "  t.typname AS \"%s\",\n"
						  "  CASE WHEN t.typrelid != 0\n"
						  "      THEN CAST('tuple' AS pg_catalog.text)\n"
						  "    WHEN t.typlen < 0\n"
						  "      THEN CAST('var' AS pg_catalog.text)\n"
						  "    ELSE CAST(t.typlen AS pg_catalog.text)\n"
						  "  END AS \"%s\",\n",
						  gettext_noop("Internal name"),
						  gettext_noop("Size"));
	if (verbose && pset.sversion >= 80300)
	{
		appendPQExpBufferStr(&buf,
							 "  pg_catalog.array_to_string(\n"
							 "      ARRAY(\n"
							 "		     SELECT e.enumlabel\n"
							 "          FROM pg_catalog.pg_enum e\n"
							 "          WHERE e.enumtypid = t.oid\n");

		if (pset.sversion >= 90100)
			appendPQExpBufferStr(&buf,
								 "          ORDER BY e.enumsortorder\n");
		else
			appendPQExpBufferStr(&buf,
								 "          ORDER BY e.oid\n");

		appendPQExpBuffer(&buf,
						  "      ),\n"
						  "      E'\\n'\n"
						  "  ) AS \"%s\",\n",
						  gettext_noop("Elements"));
	}
	if (verbose && pset.sversion >= 90200)
	{
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
	appendPQExpBufferStr(&buf, "OR (SELECT c.relkind = 'c' FROM pg_catalog.pg_class c "
						 "WHERE c.oid = t.typrelid))\n");

	/*
	 * do not include array types (before 8.3 we have to use the assumption
	 * that their names start with underscore)
	 */
	if (pset.sversion >= 80300)
		appendPQExpBufferStr(&buf, "  AND NOT EXISTS(SELECT 1 FROM pg_catalog.pg_type el WHERE el.oid = t.typelem AND el.typarray = t.oid)\n");
	else
		appendPQExpBufferStr(&buf, "  AND t.typname !~ '^_'\n");

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	/* Match name pattern against either internal or external name */
	processSQLNamePattern(pset.db, &buf, pattern, true, false,
						  "n.nspname", "t.typname",
						  "pg_catalog.format_type(t.oid, NULL)",
						  "pg_catalog.pg_type_is_visible(t.oid)");

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of data types");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

	PQclear(res);
	return true;
}


/* \do
 * Describe operators
 */
bool
describeOperators(const char *pattern, bool verbose, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	/*
	 * Note: before Postgres 9.1, we did not assign comments to any built-in
	 * operators, preferring to let the comment on the underlying function
	 * suffice.  The coalesce() on the obj_description() calls below supports
	 * this convention by providing a fallback lookup of a comment on the
	 * operator's function.  As of 9.1 there is a policy that every built-in
	 * operator should have a comment; so the coalesce() is no longer
	 * necessary so far as built-in operators are concerned.  We keep it
	 * anyway, for now, because (1) third-party modules may still be following
	 * the old convention, and (2) we'd need to do it anyway when talking to a
	 * pre-9.1 server.
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
						  "  o.oprcode AS \"%s\",\n",
						  gettext_noop("Function"));

	appendPQExpBuffer(&buf,
			 "  coalesce(pg_catalog.obj_description(o.oid, 'pg_operator'),\n"
	"           pg_catalog.obj_description(o.oprcode, 'pg_proc')) AS \"%s\"\n"
					  "FROM pg_catalog.pg_operator o\n"
	  "     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = o.oprnamespace\n",
					  gettext_noop("Description"));

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "WHERE n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	processSQLNamePattern(pset.db, &buf, pattern, !showSystem && !pattern, true,
						  "n.nspname", "o.oprname", NULL,
						  "pg_catalog.pg_operator_is_visible(o.oid)");

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2, 3, 4;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of operators");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

	PQclear(res);
	return true;
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
					  "SELECT d.datname as \"%s\",\n"
				   "       pg_catalog.pg_get_userbyid(d.datdba) as \"%s\",\n"
			"       pg_catalog.pg_encoding_to_char(d.encoding) as \"%s\",\n",
					  gettext_noop("Name"),
					  gettext_noop("Owner"),
					  gettext_noop("Encoding"));
	if (pset.sversion >= 80400)
		appendPQExpBuffer(&buf,
						  "       d.datcollate as \"%s\",\n"
						  "       d.datctype as \"%s\",\n",
						  gettext_noop("Collate"),
						  gettext_noop("Ctype"));
	appendPQExpBufferStr(&buf, "       ");
	printACLColumn(&buf, "d.datacl");
	if (verbose && pset.sversion >= 80200)
		appendPQExpBuffer(&buf,
						  ",\n       CASE WHEN pg_catalog.has_database_privilege(d.datname, 'CONNECT')\n"
						  "            THEN pg_catalog.pg_size_pretty(pg_catalog.pg_database_size(d.datname))\n"
						  "            ELSE 'No Access'\n"
						  "       END as \"%s\"",
						  gettext_noop("Size"));
	if (verbose && pset.sversion >= 80000)
		appendPQExpBuffer(&buf,
						  ",\n       t.spcname as \"%s\"",
						  gettext_noop("Tablespace"));
	if (verbose && pset.sversion >= 80200)
		appendPQExpBuffer(&buf,
						  ",\n       pg_catalog.shobj_description(d.oid, 'pg_database') as \"%s\"",
						  gettext_noop("Description"));
	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_database d\n");
	if (verbose && pset.sversion >= 80000)
		appendPQExpBufferStr(&buf,
		   "  JOIN pg_catalog.pg_tablespace t on d.dattablespace = t.oid\n");

	if (pattern)
		processSQLNamePattern(pset.db, &buf, pattern, false, false,
							  NULL, "d.datname", NULL, NULL);

	appendPQExpBufferStr(&buf, "ORDER BY 1;");
	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of databases");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

	PQclear(res);
	return true;
}


/*
 * List Tables' Grant/Revoke Permissions
 * \z (now also \dp -- perhaps more mnemonic)
 */
bool
permissionsList(const char *pattern)
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
					  " WHEN 'r' THEN '%s'"
					  " WHEN 'v' THEN '%s'"
					  " WHEN 'm' THEN '%s'"
					  " WHEN 'S' THEN '%s'"
					  " WHEN 'f' THEN '%s'"
					  " END as \"%s\",\n"
					  "  ",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("table"),
					  gettext_noop("view"),
					  gettext_noop("materialized view"),
					  gettext_noop("sequence"),
					  gettext_noop("foreign table"),
					  gettext_noop("Type"));

	printACLColumn(&buf, "c.relacl");

	if (pset.sversion >= 80400)
		appendPQExpBuffer(&buf,
						  ",\n  pg_catalog.array_to_string(ARRAY(\n"
						  "    SELECT attname || E':\\n  ' || pg_catalog.array_to_string(attacl, E'\\n  ')\n"
						  "    FROM pg_catalog.pg_attribute a\n"
						  "    WHERE attrelid = c.oid AND NOT attisdropped AND attacl IS NOT NULL\n"
						  "  ), E'\\n') AS \"%s\"",
						  gettext_noop("Column privileges"));

	if (pset.sversion >= 90500)
		appendPQExpBuffer(&buf,
						  ",\n  pg_catalog.array_to_string(ARRAY(\n"
						  "    SELECT rsecpolname\n"
						  "    || CASE WHEN rseccmd IS NOT NULL THEN\n"
						  "           E' (' || rseccmd || E')'\n"
						  "       ELSE E':' \n"
						  "       END\n"
						  "    || CASE WHEN rs.rsecqual IS NOT NULL THEN\n"
						  "           E'\\n  (u): ' || pg_catalog.pg_get_expr(rsecqual, rsecrelid)\n"
						  "       ELSE E''\n"
						  "       END\n"
						  "    || CASE WHEN rsecwithcheck IS NOT NULL THEN\n"
						  "           E'\\n  (c): ' || pg_catalog.pg_get_expr(rsecwithcheck, rsecrelid)\n"
						  "       ELSE E''\n"
						  "       END"
						  "    || CASE WHEN rs.rsecroles <> '{0}' THEN\n"
						  "           E'\\n  to: ' || pg_catalog.array_to_string(\n"
						  "               ARRAY(\n"
						  "                   SELECT rolname\n"
						  "                   FROM pg_catalog.pg_roles\n"
						  "                   WHERE oid = ANY (rs.rsecroles)\n"
						  "                   ORDER BY 1\n"
						  "               ), E', ')\n"
						  "       ELSE E''\n"
						  "       END\n"
						  "    FROM pg_catalog.pg_rowsecurity rs\n"
						  "    WHERE rsecrelid = c.oid), E'\\n')\n"
						  "    AS \"%s\"",
						  gettext_noop("Policies"));

	appendPQExpBufferStr(&buf, "\nFROM pg_catalog.pg_class c\n"
	   "     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace\n"
						 "WHERE c.relkind IN ('r', 'v', 'm', 'S', 'f')\n");

	/*
	 * Unless a schema pattern is specified, we suppress system and temp
	 * tables, since they normally aren't very interesting from a permissions
	 * point of view.  You can see 'em by explicit request though, eg with \z
	 * pg_catalog.*
	 */
	processSQLNamePattern(pset.db, &buf, pattern, true, false,
						  "n.nspname", "c.relname", NULL,
			"n.nspname !~ '^pg_' AND pg_catalog.pg_table_is_visible(c.oid)");

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	if (!res)
	{
		termPQExpBuffer(&buf);
		return false;
	}

	myopt.nullPrint = NULL;
	printfPQExpBuffer(&buf, _("Access privileges"));
	myopt.title = buf.data;
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

	termPQExpBuffer(&buf);
	PQclear(res);
	return true;
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

	if (pset.sversion < 90000)
	{
		psql_error("The server (version %d.%d) does not support altering default privileges.\n",
				   pset.sversion / 10000, (pset.sversion / 100) % 100);
		return true;
	}

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
			   "SELECT pg_catalog.pg_get_userbyid(d.defaclrole) AS \"%s\",\n"
					  "  n.nspname AS \"%s\",\n"
					  "  CASE d.defaclobjtype WHEN '%c' THEN '%s' WHEN '%c' THEN '%s' WHEN '%c' THEN '%s' WHEN '%c' THEN '%s' END AS \"%s\",\n"
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
					  gettext_noop("Type"));

	printACLColumn(&buf, "d.defaclacl");

	appendPQExpBufferStr(&buf, "\nFROM pg_catalog.pg_default_acl d\n"
						 "     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = d.defaclnamespace\n");

	processSQLNamePattern(pset.db, &buf, pattern, false, false,
						  NULL,
						  "n.nspname",
						  "pg_catalog.pg_get_userbyid(d.defaclrole)",
						  NULL);

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2, 3;");

	res = PSQLexec(buf.data);
	if (!res)
	{
		termPQExpBuffer(&buf);
		return false;
	}

	myopt.nullPrint = NULL;
	printfPQExpBuffer(&buf, _("Default access privileges"));
	myopt.title = buf.data;
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

	termPQExpBuffer(&buf);
	PQclear(res);
	return true;
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

	/* Constraint descriptions */
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
					  gettext_noop("constraint"));

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "WHERE n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	processSQLNamePattern(pset.db, &buf, pattern, !showSystem && !pattern,
						  false, "n.nspname", "pgc.conname", NULL,
						  "pg_catalog.pg_table_is_visible(c.oid)");

	/*
	 * pg_opclass.opcmethod only available in 8.3+
	 */
	if (pset.sversion >= 80300)
	{
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

		processSQLNamePattern(pset.db, &buf, pattern, true, false,
							  "n.nspname", "o.opcname", NULL,
							  "pg_catalog.pg_opclass_is_visible(o.oid)");
	}

	/*
	 * although operator family comments have been around since 8.3,
	 * pg_opfamily_is_visible is only available in 9.2+
	 */
	if (pset.sversion >= 90200)
	{
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

		processSQLNamePattern(pset.db, &buf, pattern, true, false,
							  "n.nspname", "opf.opfname", NULL,
							  "pg_catalog.pg_opfamily_is_visible(opf.oid)");
	}

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

	processSQLNamePattern(pset.db, &buf, pattern, true, false,
						  "n.nspname", "r.rulename", NULL,
						  "pg_catalog.pg_table_is_visible(c.oid)");

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

	processSQLNamePattern(pset.db, &buf, pattern, !showSystem && !pattern, false,
						  "n.nspname", "t.tgname", NULL,
						  "pg_catalog.pg_table_is_visible(c.oid)");

	appendPQExpBufferStr(&buf,
						 ") AS tt\n"
						 "  JOIN pg_catalog.pg_description d ON (tt.oid = d.objoid AND tt.tableoid = d.classoid AND d.objsubid = 0)\n");

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2, 3;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("Object descriptions");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

	PQclear(res);
	return true;
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

	processSQLNamePattern(pset.db, &buf, pattern, !showSystem && !pattern, false,
						  "n.nspname", "c.relname", NULL,
						  "pg_catalog.pg_table_is_visible(c.oid)");

	appendPQExpBufferStr(&buf, "ORDER BY 2, 3;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	if (PQntuples(res) == 0)
	{
		if (!pset.quiet)
			psql_error("Did not find any relation named \"%s\".\n",
					   pattern);
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
	PQExpBufferData buf;
	PGresult   *res = NULL;
	printTableOpt myopt = pset.popt.topt;
	printTableContent cont;
	bool		printTableInitialized = false;
	int			i;
	char	   *view_def = NULL;
	char	   *headers[9];
	char	  **seq_values = NULL;
	char	  **modifiers = NULL;
	char	  **ptr;
	PQExpBufferData title;
	PQExpBufferData tmpbuf;
	int			cols;
	int			numrows = 0;
	struct
	{
		int16		checks;
		char		relkind;
		bool		hasindex;
		bool		hasrules;
		bool		hastriggers;
		bool		rowsecurity;
		bool		hasoids;
		Oid			tablespace;
		char	   *reloptions;
		char	   *reloftype;
		char		relpersistence;
		char		relreplident;
	}			tableinfo;
	bool		show_modifiers = false;
	bool		retval;

	retval = false;

	myopt.default_footer = false;
	/* This output looks confusing in expanded mode. */
	myopt.expanded = false;

	initPQExpBuffer(&buf);
	initPQExpBuffer(&title);
	initPQExpBuffer(&tmpbuf);

	/* Get general table info */
	if (pset.sversion >= 90500)
	{
		printfPQExpBuffer(&buf,
			  "SELECT c.relchecks, c.relkind, c.relhasindex, c.relhasrules, "
						  "c.relhastriggers, c.relrowsecurity, c.relhasoids, "
						  "%s, c.reltablespace, "
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
						  "c.relhastriggers, false, c.relhasoids, "
						  "%s, c.reltablespace, "
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
	else if (pset.sversion >= 90100)
	{
		printfPQExpBuffer(&buf,
			  "SELECT c.relchecks, c.relkind, c.relhasindex, c.relhasrules, "
						  "c.relhastriggers, false, c.relhasoids, "
						  "%s, c.reltablespace, "
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
	else if (pset.sversion >= 90000)
	{
		printfPQExpBuffer(&buf,
			  "SELECT c.relchecks, c.relkind, c.relhasindex, c.relhasrules, "
						  "c.relhastriggers, false, c.relhasoids, "
						  "%s, c.reltablespace, "
						  "CASE WHEN c.reloftype = 0 THEN '' ELSE c.reloftype::pg_catalog.regtype::pg_catalog.text END\n"
						  "FROM pg_catalog.pg_class c\n "
		   "LEFT JOIN pg_catalog.pg_class tc ON (c.reltoastrelid = tc.oid)\n"
						  "WHERE c.oid = '%s';",
						  (verbose ?
						   "pg_catalog.array_to_string(c.reloptions || "
						   "array(select 'toast.' || x from pg_catalog.unnest(tc.reloptions) x), ', ')\n"
						   : "''"),
						  oid);
	}
	else if (pset.sversion >= 80400)
	{
		printfPQExpBuffer(&buf,
			  "SELECT c.relchecks, c.relkind, c.relhasindex, c.relhasrules, "
						  "c.relhastriggers, false, c.relhasoids, "
						  "%s, c.reltablespace\n"
						  "FROM pg_catalog.pg_class c\n "
		   "LEFT JOIN pg_catalog.pg_class tc ON (c.reltoastrelid = tc.oid)\n"
						  "WHERE c.oid = '%s';",
						  (verbose ?
						   "pg_catalog.array_to_string(c.reloptions || "
						   "array(select 'toast.' || x from pg_catalog.unnest(tc.reloptions) x), ', ')\n"
						   : "''"),
						  oid);
	}
	else if (pset.sversion >= 80200)
	{
		printfPQExpBuffer(&buf,
					  "SELECT relchecks, relkind, relhasindex, relhasrules, "
						  "reltriggers <> 0, false, relhasoids, "
						  "%s, reltablespace\n"
						  "FROM pg_catalog.pg_class WHERE oid = '%s';",
						  (verbose ?
					 "pg_catalog.array_to_string(reloptions, E', ')" : "''"),
						  oid);
	}
	else if (pset.sversion >= 80000)
	{
		printfPQExpBuffer(&buf,
					  "SELECT relchecks, relkind, relhasindex, relhasrules, "
						  "reltriggers <> 0, false, relhasoids, "
						  "'', reltablespace\n"
						  "FROM pg_catalog.pg_class WHERE oid = '%s';",
						  oid);
	}
	else
	{
		printfPQExpBuffer(&buf,
					  "SELECT relchecks, relkind, relhasindex, relhasrules, "
						  "reltriggers <> 0, false, relhasoids, "
						  "'', ''\n"
						  "FROM pg_catalog.pg_class WHERE oid = '%s';",
						  oid);
	}

	res = PSQLexec(buf.data);
	if (!res)
		goto error_return;

	/* Did we get anything? */
	if (PQntuples(res) == 0)
	{
		if (!pset.quiet)
			psql_error("Did not find any relation with OID %s.\n", oid);
		goto error_return;
	}

	tableinfo.checks = atoi(PQgetvalue(res, 0, 0));
	tableinfo.relkind = *(PQgetvalue(res, 0, 1));
	tableinfo.hasindex = strcmp(PQgetvalue(res, 0, 2), "t") == 0;
	tableinfo.hasrules = strcmp(PQgetvalue(res, 0, 3), "t") == 0;
	tableinfo.hastriggers = strcmp(PQgetvalue(res, 0, 4), "t") == 0;
	tableinfo.rowsecurity = strcmp(PQgetvalue(res, 0, 5), "t") == 0;
	tableinfo.hasoids = strcmp(PQgetvalue(res, 0, 6), "t") == 0;
	tableinfo.reloptions = (pset.sversion >= 80200) ?
		pg_strdup(PQgetvalue(res, 0, 7)) : NULL;
	tableinfo.tablespace = (pset.sversion >= 80000) ?
		atooid(PQgetvalue(res, 0, 8)) : 0;
	tableinfo.reloftype = (pset.sversion >= 90000 &&
						   strcmp(PQgetvalue(res, 0, 9), "") != 0) ?
		pg_strdup(PQgetvalue(res, 0, 9)) : NULL;
	tableinfo.relpersistence = (pset.sversion >= 90100) ?
		*(PQgetvalue(res, 0, 10)) : 0;
	tableinfo.relreplident = (pset.sversion >= 90400) ?
		*(PQgetvalue(res, 0, 11)) : 'd';
	PQclear(res);
	res = NULL;

	/*
	 * If it's a sequence, fetch its values and store into an array that will
	 * be used later.
	 */
	if (tableinfo.relkind == 'S')
	{
		printfPQExpBuffer(&buf, "SELECT * FROM %s", fmtId(schemaname));
		/* must be separate because fmtId isn't reentrant */
		appendPQExpBuffer(&buf, ".%s;", fmtId(relationname));

		res = PSQLexec(buf.data);
		if (!res)
			goto error_return;

		seq_values = pg_malloc((PQnfields(res) + 1) * sizeof(*seq_values));

		for (i = 0; i < PQnfields(res); i++)
			seq_values[i] = pg_strdup(PQgetvalue(res, 0, i));
		seq_values[i] = NULL;

		PQclear(res);
		res = NULL;
	}

	/*
	 * Get column info
	 *
	 * You need to modify value of "firstvcol" which will be defined below if
	 * you are adding column(s) preceding to verbose-only columns.
	 */
	printfPQExpBuffer(&buf, "SELECT a.attname,");
	appendPQExpBufferStr(&buf, "\n  pg_catalog.format_type(a.atttypid, a.atttypmod),"
						 "\n  (SELECT substring(pg_catalog.pg_get_expr(d.adbin, d.adrelid) for 128)"
						 "\n   FROM pg_catalog.pg_attrdef d"
						 "\n   WHERE d.adrelid = a.attrelid AND d.adnum = a.attnum AND a.atthasdef),"
						 "\n  a.attnotnull, a.attnum,");
	if (pset.sversion >= 90100)
		appendPQExpBufferStr(&buf, "\n  (SELECT c.collname FROM pg_catalog.pg_collation c, pg_catalog.pg_type t\n"
							 "   WHERE c.oid = a.attcollation AND t.oid = a.atttypid AND a.attcollation <> t.typcollation) AS attcollation");
	else
		appendPQExpBufferStr(&buf, "\n  NULL AS attcollation");
	if (tableinfo.relkind == 'i')
		appendPQExpBufferStr(&buf, ",\n  pg_catalog.pg_get_indexdef(a.attrelid, a.attnum, TRUE) AS indexdef");
	else
		appendPQExpBufferStr(&buf, ",\n  NULL AS indexdef");
	if (tableinfo.relkind == 'f' && pset.sversion >= 90200)
		appendPQExpBufferStr(&buf, ",\n  CASE WHEN attfdwoptions IS NULL THEN '' ELSE "
							 "  '(' || array_to_string(ARRAY(SELECT quote_ident(option_name) ||  ' ' || quote_literal(option_value)  FROM "
							 "  pg_options_to_table(attfdwoptions)), ', ') || ')' END AS attfdwoptions");
	else
		appendPQExpBufferStr(&buf, ",\n  NULL AS attfdwoptions");
	if (verbose)
	{
		appendPQExpBufferStr(&buf, ",\n  a.attstorage");
		appendPQExpBufferStr(&buf, ",\n  CASE WHEN a.attstattarget=-1 THEN NULL ELSE a.attstattarget END AS attstattarget");

		/*
		 * In 9.0+, we have column comments for: relations, views, composite
		 * types, and foreign tables (c.f. CommentObject() in comment.c).
		 */
		if (tableinfo.relkind == 'r' || tableinfo.relkind == 'v' ||
			tableinfo.relkind == 'm' ||
			tableinfo.relkind == 'f' || tableinfo.relkind == 'c')
			appendPQExpBufferStr(&buf, ", pg_catalog.col_description(a.attrelid, a.attnum)");
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
		case 'r':
			if (tableinfo.relpersistence == 'u')
				printfPQExpBuffer(&title, _("Unlogged table \"%s.%s\""),
								  schemaname, relationname);
			else
				printfPQExpBuffer(&title, _("Table \"%s.%s\""),
								  schemaname, relationname);
			break;
		case 'v':
			printfPQExpBuffer(&title, _("View \"%s.%s\""),
							  schemaname, relationname);
			break;
		case 'm':
			if (tableinfo.relpersistence == 'u')
				printfPQExpBuffer(&title, _("Unlogged materialized view \"%s.%s\""),
								  schemaname, relationname);
			else
				printfPQExpBuffer(&title, _("Materialized view \"%s.%s\""),
								  schemaname, relationname);
			break;
		case 'S':
			printfPQExpBuffer(&title, _("Sequence \"%s.%s\""),
							  schemaname, relationname);
			break;
		case 'i':
			if (tableinfo.relpersistence == 'u')
				printfPQExpBuffer(&title, _("Unlogged index \"%s.%s\""),
								  schemaname, relationname);
			else
				printfPQExpBuffer(&title, _("Index \"%s.%s\""),
								  schemaname, relationname);
			break;
		case 's':
			/* not used as of 8.2, but keep it for backwards compatibility */
			printfPQExpBuffer(&title, _("Special relation \"%s.%s\""),
							  schemaname, relationname);
			break;
		case 't':
			printfPQExpBuffer(&title, _("TOAST table \"%s.%s\""),
							  schemaname, relationname);
			break;
		case 'c':
			printfPQExpBuffer(&title, _("Composite type \"%s.%s\""),
							  schemaname, relationname);
			break;
		case 'f':
			printfPQExpBuffer(&title, _("Foreign table \"%s.%s\""),
							  schemaname, relationname);
			break;
		default:
			/* untranslated unknown relkind */
			printfPQExpBuffer(&title, "?%c? \"%s.%s\"",
							  tableinfo.relkind, schemaname, relationname);
			break;
	}

	/* Set the number of columns, and their names */
	headers[0] = gettext_noop("Column");
	headers[1] = gettext_noop("Type");
	cols = 2;

	if (tableinfo.relkind == 'r' || tableinfo.relkind == 'v' ||
		tableinfo.relkind == 'm' ||
		tableinfo.relkind == 'f' || tableinfo.relkind == 'c')
	{
		show_modifiers = true;
		headers[cols++] = gettext_noop("Modifiers");
		modifiers = pg_malloc0((numrows + 1) * sizeof(*modifiers));
	}

	if (tableinfo.relkind == 'S')
		headers[cols++] = gettext_noop("Value");

	if (tableinfo.relkind == 'i')
		headers[cols++] = gettext_noop("Definition");

	if (tableinfo.relkind == 'f' && pset.sversion >= 90200)
		headers[cols++] = gettext_noop("FDW Options");

	if (verbose)
	{
		headers[cols++] = gettext_noop("Storage");
		if (tableinfo.relkind == 'r' || tableinfo.relkind == 'm' ||
			tableinfo.relkind == 'f')
			headers[cols++] = gettext_noop("Stats target");
		/* Column comments, if the relkind supports this feature. */
		if (tableinfo.relkind == 'r' || tableinfo.relkind == 'v' ||
			tableinfo.relkind == 'm' ||
			tableinfo.relkind == 'c' || tableinfo.relkind == 'f')
			headers[cols++] = gettext_noop("Description");
	}

	printTableInit(&cont, &myopt, title.data, cols, numrows);
	printTableInitialized = true;

	for (i = 0; i < cols; i++)
		printTableAddHeader(&cont, headers[i], true, 'l');

	/* Check if table is a view or materialized view */
	if ((tableinfo.relkind == 'v' || tableinfo.relkind == 'm') && verbose)
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

	/* Generate table cells to be printed */
	for (i = 0; i < numrows; i++)
	{
		/* Column */
		printTableAddCell(&cont, PQgetvalue(res, i, 0), false, false);

		/* Type */
		printTableAddCell(&cont, PQgetvalue(res, i, 1), false, false);

		/* Modifiers: collate, not null, default */
		if (show_modifiers)
		{
			resetPQExpBuffer(&tmpbuf);

			if (!PQgetisnull(res, i, 5))
			{
				if (tmpbuf.len > 0)
					appendPQExpBufferStr(&tmpbuf, " ");
				appendPQExpBuffer(&tmpbuf, _("collate %s"),
								  PQgetvalue(res, i, 5));
			}

			if (strcmp(PQgetvalue(res, i, 3), "t") == 0)
			{
				if (tmpbuf.len > 0)
					appendPQExpBufferStr(&tmpbuf, " ");
				appendPQExpBufferStr(&tmpbuf, _("not null"));
			}

			/* handle "default" here */
			/* (note: above we cut off the 'default' string at 128) */
			if (strlen(PQgetvalue(res, i, 2)) != 0)
			{
				if (tmpbuf.len > 0)
					appendPQExpBufferStr(&tmpbuf, " ");
				/* translator: default values of column definitions */
				appendPQExpBuffer(&tmpbuf, _("default %s"),
								  PQgetvalue(res, i, 2));
			}

			modifiers[i] = pg_strdup(tmpbuf.data);
			printTableAddCell(&cont, modifiers[i], false, false);
		}

		/* Value: for sequences only */
		if (tableinfo.relkind == 'S')
			printTableAddCell(&cont, seq_values[i], false, false);

		/* Expression for index column */
		if (tableinfo.relkind == 'i')
			printTableAddCell(&cont, PQgetvalue(res, i, 6), false, false);

		/* FDW options for foreign table column, only for 9.2 or later */
		if (tableinfo.relkind == 'f' && pset.sversion >= 90200)
			printTableAddCell(&cont, PQgetvalue(res, i, 7), false, false);

		/* Storage and Description */
		if (verbose)
		{
			int			firstvcol = 8;
			char	   *storage = PQgetvalue(res, i, firstvcol);

			/* these strings are literal in our syntax, so not translated. */
			printTableAddCell(&cont, (storage[0] == 'p' ? "plain" :
									  (storage[0] == 'm' ? "main" :
									   (storage[0] == 'x' ? "extended" :
										(storage[0] == 'e' ? "external" :
										 "???")))),
							  false, false);

			/* Statistics target, if the relkind supports this feature */
			if (tableinfo.relkind == 'r' || tableinfo.relkind == 'm' ||
				tableinfo.relkind == 'f')
			{
				printTableAddCell(&cont, PQgetvalue(res, i, firstvcol + 1),
								  false, false);
			}

			/* Column comments, if the relkind supports this feature. */
			if (tableinfo.relkind == 'r' || tableinfo.relkind == 'v' ||
				tableinfo.relkind == 'm' ||
				tableinfo.relkind == 'c' || tableinfo.relkind == 'f')
				printTableAddCell(&cont, PQgetvalue(res, i, firstvcol + 2),
								  false, false);
		}
	}

	/* Make footers */
	if (tableinfo.relkind == 'i')
	{
		/* Footer information about an index */
		PGresult   *result;

		printfPQExpBuffer(&buf,
				 "SELECT i.indisunique, i.indisprimary, i.indisclustered, ");
		if (pset.sversion >= 80200)
			appendPQExpBufferStr(&buf, "i.indisvalid,\n");
		else
			appendPQExpBufferStr(&buf, "true AS indisvalid,\n");
		if (pset.sversion >= 90000)
			appendPQExpBufferStr(&buf,
								 "  (NOT i.indimmediate) AND "
							"EXISTS (SELECT 1 FROM pg_catalog.pg_constraint "
								 "WHERE conrelid = i.indrelid AND "
								 "conindid = i.indexrelid AND "
								 "contype IN ('p','u','x') AND "
								 "condeferrable) AS condeferrable,\n"
								 "  (NOT i.indimmediate) AND "
							"EXISTS (SELECT 1 FROM pg_catalog.pg_constraint "
								 "WHERE conrelid = i.indrelid AND "
								 "conindid = i.indexrelid AND "
								 "contype IN ('p','u','x') AND "
								 "condeferred) AS condeferred,\n");
		else
			appendPQExpBufferStr(&buf,
						"  false AS condeferrable, false AS condeferred,\n");

		if (pset.sversion >= 90400)
			appendPQExpBuffer(&buf, "i.indisreplident,\n");
		else
			appendPQExpBuffer(&buf, "false AS indisreplident,\n");

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
			char	   *indamname = PQgetvalue(result, 0, 7);
			char	   *indtable = PQgetvalue(result, 0, 8);
			char	   *indpred = PQgetvalue(result, 0, 9);

			if (strcmp(indisprimary, "t") == 0)
				printfPQExpBuffer(&tmpbuf, _("primary key, "));
			else if (strcmp(indisunique, "t") == 0)
				printfPQExpBuffer(&tmpbuf, _("unique, "));
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
				appendPQExpBuffer(&tmpbuf, _(", replica identity"));

			printTableAddFooter(&cont, tmpbuf.data);
			add_tablespace_footer(&cont, tableinfo.relkind,
								  tableinfo.tablespace, true);
		}

		PQclear(result);
	}
	else if (tableinfo.relkind == 'S')
	{
		/* Footer information about a sequence */
		PGresult   *result = NULL;

		/* Get the column that owns this sequence */
		printfPQExpBuffer(&buf, "SELECT pg_catalog.quote_ident(nspname) || '.' ||"
						  "\n   pg_catalog.quote_ident(relname) || '.' ||"
						  "\n   pg_catalog.quote_ident(attname)"
						  "\nFROM pg_catalog.pg_class c"
					"\nINNER JOIN pg_catalog.pg_depend d ON c.oid=d.refobjid"
			 "\nINNER JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace"
						  "\nINNER JOIN pg_catalog.pg_attribute a ON ("
						  "\n a.attrelid=c.oid AND"
						  "\n a.attnum=d.refobjsubid)"
			   "\nWHERE d.classid='pg_catalog.pg_class'::pg_catalog.regclass"
			 "\n AND d.refclassid='pg_catalog.pg_class'::pg_catalog.regclass"
						  "\n AND d.objid=%s"
						  "\n AND d.deptype='a'",
						  oid);

		result = PSQLexec(buf.data);
		if (!result)
			goto error_return;
		else if (PQntuples(result) == 1)
		{
			printfPQExpBuffer(&buf, _("Owned by: %s"),
							  PQgetvalue(result, 0, 0));
			printTableAddFooter(&cont, buf.data);
		}

		/*
		 * If we get no rows back, don't show anything (obviously). We should
		 * never get more than one row back, but if we do, just ignore it and
		 * don't print anything.
		 */
		PQclear(result);
	}
	else if (tableinfo.relkind == 'r' || tableinfo.relkind == 'm' ||
			 tableinfo.relkind == 'f')
	{
		/* Footer information about a table */
		PGresult   *result = NULL;
		int			tuples = 0;

		/* print indexes */
		if (tableinfo.hasindex)
		{
			printfPQExpBuffer(&buf,
							  "SELECT c2.relname, i.indisprimary, i.indisunique, i.indisclustered, ");
			if (pset.sversion >= 80200)
				appendPQExpBufferStr(&buf, "i.indisvalid, ");
			else
				appendPQExpBufferStr(&buf, "true as indisvalid, ");
			appendPQExpBufferStr(&buf, "pg_catalog.pg_get_indexdef(i.indexrelid, 0, true),\n  ");
			if (pset.sversion >= 90000)
				appendPQExpBufferStr(&buf,
						   "pg_catalog.pg_get_constraintdef(con.oid, true), "
									 "contype, condeferrable, condeferred");
			else
				appendPQExpBufferStr(&buf,
								   "null AS constraintdef, null AS contype, "
							 "false AS condeferrable, false AS condeferred");
			if (pset.sversion >= 90400)
				appendPQExpBufferStr(&buf, ", i.indisreplident");
			else
				appendPQExpBufferStr(&buf, ", false AS indisreplident");
			if (pset.sversion >= 80000)
				appendPQExpBufferStr(&buf, ", c2.reltablespace");
			appendPQExpBufferStr(&buf,
								 "\nFROM pg_catalog.pg_class c, pg_catalog.pg_class c2, pg_catalog.pg_index i\n");
			if (pset.sversion >= 90000)
				appendPQExpBufferStr(&buf,
									 "  LEFT JOIN pg_catalog.pg_constraint con ON (conrelid = i.indrelid AND conindid = i.indexrelid AND contype IN ('p','u','x'))\n");
			appendPQExpBuffer(&buf,
							  "WHERE c.oid = '%s' AND c.oid = i.indrelid AND i.indexrelid = c2.oid\n"
			 "ORDER BY i.indisprimary DESC, i.indisunique DESC, c2.relname;",
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

					/* If exclusion constraint, print the constraintdef */
					if (strcmp(PQgetvalue(result, i, 7), "x") == 0)
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
						appendPQExpBuffer(&buf, " REPLICA IDENTITY");

					printTableAddFooter(&cont, buf.data);

					/* Print tablespace of the index on the same line */
					if (pset.sversion >= 80000)
						add_tablespace_footer(&cont, 'i',
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
							  "WHERE r.conrelid = '%s' AND r.contype = 'c'\n"
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
					/* untranslated contraint name and def */
					printfPQExpBuffer(&buf, "    \"%s\" %s",
									  PQgetvalue(result, i, 0),
									  PQgetvalue(result, i, 1));

					printTableAddFooter(&cont, buf.data);
				}
			}
			PQclear(result);
		}

		/* print foreign-key constraints (there are none if no triggers) */
		if (tableinfo.hastriggers)
		{
			printfPQExpBuffer(&buf,
							  "SELECT conname,\n"
				 "  pg_catalog.pg_get_constraintdef(r.oid, true) as condef\n"
							  "FROM pg_catalog.pg_constraint r\n"
				   "WHERE r.conrelid = '%s' AND r.contype = 'f' ORDER BY 1;",
							  oid);
			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else
				tuples = PQntuples(result);

			if (tuples > 0)
			{
				printTableAddFooter(&cont, _("Foreign-key constraints:"));
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

		/* print incoming foreign-key references (none if no triggers) */
		if (tableinfo.hastriggers)
		{
			printfPQExpBuffer(&buf,
						   "SELECT conname, conrelid::pg_catalog.regclass,\n"
				 "  pg_catalog.pg_get_constraintdef(c.oid, true) as condef\n"
							  "FROM pg_catalog.pg_constraint c\n"
				  "WHERE c.confrelid = '%s' AND c.contype = 'f' ORDER BY 1;",
							  oid);
			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else
				tuples = PQntuples(result);

			if (tuples > 0)
			{
				printTableAddFooter(&cont, _("Referenced by:"));
				for (i = 0; i < tuples; i++)
				{
					printfPQExpBuffer(&buf, "    TABLE \"%s\" CONSTRAINT \"%s\" %s",
									  PQgetvalue(result, i, 1),
									  PQgetvalue(result, i, 0),
									  PQgetvalue(result, i, 2));

					printTableAddFooter(&cont, buf.data);
				}
			}
			PQclear(result);
		}

		/* print any row-level policies */
		if (pset.sversion >= 90500)
		{
			appendPQExpBuffer(&buf,
				",\n pg_catalog.pg_get_expr(rs.rsecqual, c.oid) as \"%s\"",
				gettext_noop("Row-security"));

			if (verbose)
				appendPQExpBuffer(&buf,
					"\n     LEFT JOIN pg_rowsecurity rs ON rs.rsecrelid = c.oid");

			printfPQExpBuffer(&buf,
						   "SELECT rs.rsecpolname,\n"
						   "CASE WHEN rs.rsecroles = '{0}' THEN NULL ELSE array_to_string(array(select rolname from pg_roles where oid = any (rs.rsecroles) order by 1),',') END,\n"
						   "pg_catalog.pg_get_expr(rs.rsecqual, rs.rsecrelid),\n"
						   "pg_catalog.pg_get_expr(rs.rsecwithcheck, rs.rsecrelid),\n"
						   "CASE rs.rseccmd \n"
						   "WHEN 'r' THEN 'SELECT'\n"
						   "WHEN 'u' THEN 'UPDATE'\n"
						   "WHEN 'a' THEN 'INSERT'\n"
						   "WHEN 'd' THEN 'DELETE'\n"
						   "END AS cmd\n"
							  "FROM pg_catalog.pg_rowsecurity rs\n"
				  "WHERE rs.rsecrelid = '%s' ORDER BY 1;",
							  oid);

			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else
				tuples = PQntuples(result);

			/*
			 * Handle cases where RLS is enabled and there are policies,
			 * or there aren't policies, or RLS isn't enabled but there
			 * are policies
			 */
			if (tableinfo.rowsecurity && tuples > 0)
				printTableAddFooter(&cont, _("Policies:"));

			if (tableinfo.rowsecurity && tuples == 0)
				printTableAddFooter(&cont, _("Policies (Row Security Enabled): (None)"));

			if (!tableinfo.rowsecurity && tuples > 0)
				printTableAddFooter(&cont, _("Policies (Row Security Disabled):"));

			/* Might be an empty set - that's ok */
			for (i = 0; i < tuples; i++)
			{
				printfPQExpBuffer(&buf, "    POLICY \"%s\"",
									  PQgetvalue(result, i, 0));

				if (!PQgetisnull(result, i, 4))
					appendPQExpBuffer(&buf, " FOR %s",
									  PQgetvalue(result, i, 4));

				if (!PQgetisnull(result, i, 1))
				{
					appendPQExpBuffer(&buf, "\n      TO %s",
									  PQgetvalue(result, i, 1));
				}

				if (!PQgetisnull(result, i, 2))
					appendPQExpBuffer(&buf, "\n      USING %s",
									  PQgetvalue(result, i, 2));

				if (!PQgetisnull(result, i, 3))
					appendPQExpBuffer(&buf, "\n      WITH CHECK %s",
									  PQgetvalue(result, i, 3));

				printTableAddFooter(&cont, buf.data);

			}
			PQclear(result);
		}

		/* print rules */
		if (tableinfo.hasrules && tableinfo.relkind != 'm')
		{
			if (pset.sversion >= 80300)
			{
				printfPQExpBuffer(&buf,
								  "SELECT r.rulename, trim(trailing ';' from pg_catalog.pg_get_ruledef(r.oid, true)), "
								  "ev_enabled\n"
								  "FROM pg_catalog.pg_rewrite r\n"
								  "WHERE r.ev_class = '%s' ORDER BY 1;",
								  oid);
			}
			else
			{
				printfPQExpBuffer(&buf,
								  "SELECT r.rulename, trim(trailing ';' from pg_catalog.pg_get_ruledef(r.oid, true)), "
								  "'O'::char AS ev_enabled\n"
								  "FROM pg_catalog.pg_rewrite r\n"
								  "WHERE r.ev_class = '%s' ORDER BY 1;",
								  oid);
			}
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
						  "pg_catalog.pg_get_triggerdef(t.oid%s), "
						  "t.tgenabled, %s\n"
						  "FROM pg_catalog.pg_trigger t\n"
						  "WHERE t.tgrelid = '%s' AND ",
						  (pset.sversion >= 90000 ? ", true" : ""),
						  (pset.sversion >= 90000 ? "t.tgisinternal" :
						   pset.sversion >= 80300 ?
						   "t.tgconstraint <> 0 AS tgisinternal" :
						   "false AS tgisinternal"), oid);
		if (pset.sversion >= 90000)
			/* display/warn about disabled internal triggers */
			appendPQExpBuffer(&buf, "(NOT t.tgisinternal OR (t.tgisinternal AND t.tgenabled = 'D'))");
		else if (pset.sversion >= 80300)
			appendPQExpBufferStr(&buf, "(t.tgconstraint = 0 OR (t.tgconstraint <> 0 AND t.tgenabled = 'D'))");
		else
			appendPQExpBufferStr(&buf,
								 "(NOT tgisconstraint "
								 " OR NOT EXISTS"
								 "  (SELECT 1 FROM pg_catalog.pg_depend d "
								 "   JOIN pg_catalog.pg_constraint c ON (d.refclassid = c.tableoid AND d.refobjid = c.oid) "
								 "   WHERE d.classid = t.tableoid AND d.objid = t.oid AND d.deptype = 'i' AND c.contype = 'f'))");
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
								if (pset.sversion >= 80300)
									printfPQExpBuffer(&buf, _("Disabled user triggers:"));
								else
									printfPQExpBuffer(&buf, _("Disabled triggers:"));
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
					printTableAddFooter(&cont, buf.data);
				}
			}
		}
		PQclear(result);
	}

	/*
	 * Finish printing the footer information about a table.
	 */
	if (tableinfo.relkind == 'r' || tableinfo.relkind == 'm' ||
		tableinfo.relkind == 'f')
	{
		PGresult   *result;
		int			tuples;

		/* print foreign server name */
		if (tableinfo.relkind == 'f')
		{
			char	   *ftoptions;

			/* Footer information about foreign table */
			printfPQExpBuffer(&buf,
							  "SELECT s.srvname,\n"
							  "       array_to_string(ARRAY(SELECT "
							  "       quote_ident(option_name) ||  ' ' || "
							  "       quote_literal(option_value)  FROM "
							"       pg_options_to_table(ftoptions)),  ', ') "
							  "FROM pg_catalog.pg_foreign_table f,\n"
							  "     pg_catalog.pg_foreign_server s\n"
							  "WHERE f.ftrelid = %s AND s.oid = f.ftserver;",
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
			printfPQExpBuffer(&buf, "Server: %s",
							  PQgetvalue(result, 0, 0));
			printTableAddFooter(&cont, buf.data);

			/* Print per-table FDW options, if any */
			ftoptions = PQgetvalue(result, 0, 1);
			if (ftoptions && ftoptions[0] != '\0')
			{
				printfPQExpBuffer(&buf, "FDW Options: (%s)", ftoptions);
				printTableAddFooter(&cont, buf.data);
			}
			PQclear(result);
		}

		/* print inherited tables */
		printfPQExpBuffer(&buf, "SELECT c.oid::pg_catalog.regclass FROM pg_catalog.pg_class c, pg_catalog.pg_inherits i WHERE c.oid=i.inhparent AND i.inhrelid = '%s' ORDER BY inhseqno;", oid);

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
					appendPQExpBufferStr(&buf, ",");

				printTableAddFooter(&cont, buf.data);
			}

			PQclear(result);
		}

		/* print child tables */
		if (pset.sversion >= 80300)
			printfPQExpBuffer(&buf, "SELECT c.oid::pg_catalog.regclass FROM pg_catalog.pg_class c, pg_catalog.pg_inherits i WHERE c.oid=i.inhrelid AND i.inhparent = '%s' ORDER BY c.oid::pg_catalog.regclass::pg_catalog.text;", oid);
		else
			printfPQExpBuffer(&buf, "SELECT c.oid::pg_catalog.regclass FROM pg_catalog.pg_class c, pg_catalog.pg_inherits i WHERE c.oid=i.inhrelid AND i.inhparent = '%s' ORDER BY c.relname;", oid);

		result = PSQLexec(buf.data);
		if (!result)
			goto error_return;
		else
			tuples = PQntuples(result);

		if (!verbose)
		{
			/* print the number of child tables, if any */
			if (tuples > 0)
			{
				printfPQExpBuffer(&buf, _("Number of child tables: %d (Use \\d+ to list them.)"), tuples);
				printTableAddFooter(&cont, buf.data);
			}
		}
		else
		{
			/* display the list of child tables */
			const char *ct = _("Child tables");
			int			ctw = pg_wcswidth(ct, strlen(ct), pset.encoding);

			for (i = 0; i < tuples; i++)
			{
				if (i == 0)
					printfPQExpBuffer(&buf, "%s: %s",
									  ct, PQgetvalue(result, i, 0));
				else
					printfPQExpBuffer(&buf, "%*s  %s",
									  ctw, "", PQgetvalue(result, i, 0));
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

		if (verbose && (tableinfo.relkind == 'r' || tableinfo.relkind == 'm') &&

		/*
		 * No need to display default values;  we already display a REPLICA
		 * IDENTITY marker on indexes.
		 */
			tableinfo.relreplident != 'i' &&
			((strcmp(schemaname, "pg_catalog") != 0 && tableinfo.relreplident != 'd') ||
			 (strcmp(schemaname, "pg_catalog") == 0 && tableinfo.relreplident != 'n')))
		{
			const char *s = _("Replica Identity");

			printfPQExpBuffer(&buf, "%s: %s",
							  s,
							  tableinfo.relreplident == 'f' ? "FULL" :
							  tableinfo.relreplident == 'n' ? "NOTHING" :
							  "???");

			printTableAddFooter(&cont, buf.data);
		}

		/* OIDs, if verbose and not a materialized view */
		if (verbose && tableinfo.relkind != 'm' && tableinfo.hasoids)
			printTableAddFooter(&cont, _("Has OIDs: yes"));

		/* Tablespace info */
		add_tablespace_footer(&cont, tableinfo.relkind, tableinfo.tablespace,
							  true);
	}

	/* reloptions, if verbose */
	if (verbose &&
		tableinfo.reloptions && tableinfo.reloptions[0] != '\0')
	{
		const char *t = _("Options");

		printfPQExpBuffer(&buf, "%s: %s", t, tableinfo.reloptions);
		printTableAddFooter(&cont, buf.data);
	}

	printTable(&cont, pset.queryFout, pset.logfile);
	printTableCleanup(&cont);

	retval = true;

error_return:

	/* clean up */
	if (printTableInitialized)
		printTableCleanup(&cont);
	termPQExpBuffer(&buf);
	termPQExpBuffer(&title);
	termPQExpBuffer(&tmpbuf);

	if (seq_values)
	{
		for (ptr = seq_values; *ptr; ptr++)
			free(*ptr);
		free(seq_values);
	}

	if (modifiers)
	{
		for (ptr = modifiers; *ptr; ptr++)
			free(*ptr);
		free(modifiers);
	}

	if (view_def)
		free(view_def);

	if (res)
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
	if (relkind == 'r' || relkind == 'm' || relkind == 'i')
	{
		/*
		 * We ignore the database default tablespace so that users not using
		 * tablespaces don't need to know about them.  This case also covers
		 * pre-8.0 servers, for which tablespace will always be 0.
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
				return;
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
describeRoles(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printTableContent cont;
	printTableOpt myopt = pset.popt.topt;
	int			ncols = 3;
	int			nrows = 0;
	int			i;
	int			conns;
	const char	align = 'l';
	char	  **attr;

	myopt.default_footer = false;

	initPQExpBuffer(&buf);

	if (pset.sversion >= 80100)
	{
		printfPQExpBuffer(&buf,
						  "SELECT r.rolname, r.rolsuper, r.rolinherit,\n"
						  "  r.rolcreaterole, r.rolcreatedb, r.rolcanlogin,\n"
						  "  r.rolconnlimit, r.rolvaliduntil,\n"
						  "  ARRAY(SELECT b.rolname\n"
						  "        FROM pg_catalog.pg_auth_members m\n"
				 "        JOIN pg_catalog.pg_roles b ON (m.roleid = b.oid)\n"
						  "        WHERE m.member = r.oid) as memberof");

		if (verbose && pset.sversion >= 80200)
		{
			appendPQExpBufferStr(&buf, "\n, pg_catalog.shobj_description(r.oid, 'pg_authid') AS description");
			ncols++;
		}
		if (pset.sversion >= 90100)
		{
			appendPQExpBufferStr(&buf, "\n, r.rolreplication");
		}

		if (pset.sversion >= 90500)
		{
			appendPQExpBufferStr(&buf, "\n, r.rolbypassrls");
		}

		appendPQExpBufferStr(&buf, "\nFROM pg_catalog.pg_roles r\n");

		processSQLNamePattern(pset.db, &buf, pattern, false, false,
							  NULL, "r.rolname", NULL, NULL);
	}
	else
	{
		printfPQExpBuffer(&buf,
						  "SELECT u.usename AS rolname,\n"
						  "  u.usesuper AS rolsuper,\n"
						  "  true AS rolinherit, false AS rolcreaterole,\n"
					 "  u.usecreatedb AS rolcreatedb, true AS rolcanlogin,\n"
						  "  -1 AS rolconnlimit,"
						  "  u.valuntil as rolvaliduntil,\n"
						  "  ARRAY(SELECT g.groname FROM pg_catalog.pg_group g WHERE u.usesysid = ANY(g.grolist)) as memberof"
						  "\nFROM pg_catalog.pg_user u\n");

		processSQLNamePattern(pset.db, &buf, pattern, false, false,
							  NULL, "u.usename", NULL, NULL);
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
	printTableAddHeader(&cont, gettext_noop("Member of"), true, align);

	if (verbose && pset.sversion >= 80200)
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

		if (pset.sversion >= 90100)
			if (strcmp(PQgetvalue(res, i, (verbose ? 10 : 9)), "t") == 0)
				add_role_attribute(&buf, _("Replication"));

		if (pset.sversion >= 90500)
			if (strcmp(PQgetvalue(res, i, (verbose ? 11 : 10)), "t") == 0)
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
				appendPQExpBufferStr(&buf, "\n");
			appendPQExpBufferStr(&buf, _("Password valid until "));
			appendPQExpBufferStr(&buf, PQgetvalue(res, i, 7));
		}

		attr[i] = pg_strdup(buf.data);

		printTableAddCell(&cont, attr[i], false, false);

		printTableAddCell(&cont, PQgetvalue(res, i, 8), false, false);

		if (verbose && pset.sversion >= 80200)
			printTableAddCell(&cont, PQgetvalue(res, i, 9), false, false);
	}
	termPQExpBuffer(&buf);

	printTable(&cont, pset.queryFout, pset.logfile);
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

	initPQExpBuffer(&buf);

	if (pset.sversion >= 90000)
	{
		bool		havewhere;

		printfPQExpBuffer(&buf, "SELECT rolname AS \"%s\", datname AS \"%s\",\n"
				  "pg_catalog.array_to_string(setconfig, E'\\n') AS \"%s\"\n"
						  "FROM pg_db_role_setting AS s\n"
				   "LEFT JOIN pg_database ON pg_database.oid = setdatabase\n"
						  "LEFT JOIN pg_roles ON pg_roles.oid = setrole\n",
						  gettext_noop("Role"),
						  gettext_noop("Database"),
						  gettext_noop("Settings"));
		havewhere = processSQLNamePattern(pset.db, &buf, pattern, false, false,
									   NULL, "pg_roles.rolname", NULL, NULL);
		processSQLNamePattern(pset.db, &buf, pattern2, havewhere, false,
							  NULL, "pg_database.datname", NULL, NULL);
		appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");
	}
	else
	{
		fprintf(pset.queryFout,
		_("No per-database role settings support in this server version.\n"));
		return false;
	}

	res = PSQLexec(buf.data);
	if (!res)
		return false;

	if (PQntuples(res) == 0 && !pset.quiet)
	{
		if (pattern)
			fprintf(pset.queryFout, _("No matching settings found.\n"));
		else
			fprintf(pset.queryFout, _("No settings found.\n"));
	}
	else
	{
		myopt.nullPrint = NULL;
		myopt.title = _("List of settings");
		myopt.translate_header = true;

		printQuery(res, &myopt, pset.queryFout, pset.logfile);
	}

	PQclear(res);
	resetPQExpBuffer(&buf);
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
 * If tabtypes is empty, we default to \dtvsE.
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

	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {false, false, true, false, false, false, false};

	if (!(showTables || showIndexes || showViews || showMatViews || showSeq || showForeign))
		showTables = showViews = showMatViews = showSeq = showForeign = true;

	initPQExpBuffer(&buf);

	/*
	 * Note: as of Pg 8.2, we no longer use relkind 's', but we keep it here
	 * for backwards compatibility.
	 */
	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					  "  c.relname as \"%s\",\n"
					  "  CASE c.relkind"
					  " WHEN 'r' THEN '%s'"
					  " WHEN 'v' THEN '%s'"
					  " WHEN 'm' THEN '%s'"
					  " WHEN 'i' THEN '%s'"
					  " WHEN 'S' THEN '%s'"
					  " WHEN 's' THEN '%s'"
					  " WHEN 'f' THEN '%s'"
					  " END as \"%s\",\n"
					  "  pg_catalog.pg_get_userbyid(c.relowner) as \"%s\"",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("table"),
					  gettext_noop("view"),
					  gettext_noop("materialized view"),
					  gettext_noop("index"),
					  gettext_noop("sequence"),
					  gettext_noop("special"),
					  gettext_noop("foreign table"),
					  gettext_noop("Type"),
					  gettext_noop("Owner"));

	if (showIndexes)
		appendPQExpBuffer(&buf,
						  ",\n c2.relname as \"%s\"",
						  gettext_noop("Table"));

	if (verbose)
	{
		/*
		 * As of PostgreSQL 9.0, use pg_table_size() to show a more acurate
		 * size of a table, including FSM, VM and TOAST tables.
		 */
		if (pset.sversion >= 90000)
			appendPQExpBuffer(&buf,
							  ",\n  pg_catalog.pg_size_pretty(pg_catalog.pg_table_size(c.oid)) as \"%s\"",
							  gettext_noop("Size"));
		else if (pset.sversion >= 80100)
			appendPQExpBuffer(&buf,
							  ",\n  pg_catalog.pg_size_pretty(pg_catalog.pg_relation_size(c.oid)) as \"%s\"",
							  gettext_noop("Size"));

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

	appendPQExpBufferStr(&buf, "\nWHERE c.relkind IN (");
	if (showTables)
		appendPQExpBufferStr(&buf, "'r',");
	if (showViews)
		appendPQExpBufferStr(&buf, "'v',");
	if (showMatViews)
		appendPQExpBufferStr(&buf, "'m',");
	if (showIndexes)
		appendPQExpBufferStr(&buf, "'i',");
	if (showSeq)
		appendPQExpBufferStr(&buf, "'S',");
	if (showSystem || pattern)
		appendPQExpBufferStr(&buf, "'s',");		/* was RELKIND_SPECIAL in <=
												 * 8.1 */
	if (showForeign)
		appendPQExpBufferStr(&buf, "'f',");

	appendPQExpBufferStr(&buf, "''");	/* dummy */
	appendPQExpBufferStr(&buf, ")\n");

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	/*
	 * TOAST objects are suppressed unconditionally.  Since we don't provide
	 * any way to select relkind 't' above, we would never show toast tables
	 * in any case; it seems a bit confusing to allow their indexes to be
	 * shown. Use plain \d if you really need to look at a TOAST table/index.
	 */
	appendPQExpBufferStr(&buf, "      AND n.nspname !~ '^pg_toast'\n");

	processSQLNamePattern(pset.db, &buf, pattern, true, false,
						  "n.nspname", "c.relname", NULL,
						  "pg_catalog.pg_table_is_visible(c.oid)");

	appendPQExpBufferStr(&buf, "ORDER BY 1,2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	if (PQntuples(res) == 0 && !pset.quiet)
	{
		if (pattern)
			fprintf(pset.queryFout, _("No matching relations found.\n"));
		else
			fprintf(pset.queryFout, _("No relations found.\n"));
	}
	else
	{
		myopt.nullPrint = NULL;
		myopt.title = _("List of relations");
		myopt.translate_header = true;
		myopt.translate_columns = translate_columns;
		myopt.n_translate_columns = lengthof(translate_columns);

		printQuery(res, &myopt, pset.queryFout, pset.logfile);
	}

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
					  "SELECT l.lanname AS \"%s\",\n",
					  gettext_noop("Name"));
	if (pset.sversion >= 80300)
		appendPQExpBuffer(&buf,
				"       pg_catalog.pg_get_userbyid(l.lanowner) as \"%s\",\n",
						  gettext_noop("Owner"));

	appendPQExpBuffer(&buf,
					  "       l.lanpltrusted AS \"%s\"",
					  gettext_noop("Trusted"));

	if (verbose)
	{
		appendPQExpBuffer(&buf,
						  ",\n       NOT l.lanispl AS \"%s\",\n"
						  "       l.lanplcallfoid::regprocedure AS \"%s\",\n"
				   "       l.lanvalidator::regprocedure AS \"%s\",\n       ",
						  gettext_noop("Internal Language"),
						  gettext_noop("Call Handler"),
						  gettext_noop("Validator"));
		if (pset.sversion >= 90000)
			appendPQExpBuffer(&buf, "l.laninline::regprocedure AS \"%s\",\n       ",
							  gettext_noop("Inline Handler"));
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
		processSQLNamePattern(pset.db, &buf, pattern, false, false,
							  NULL, "l.lanname", NULL, NULL);

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "WHERE l.lanplcallfoid != 0\n");


	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of languages");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

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
					  "       TRIM(LEADING\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("Type"));

	if (pset.sversion >= 90100)
		appendPQExpBufferStr(&buf,
							 "            COALESCE((SELECT ' collate ' || c.collname FROM pg_catalog.pg_collation c, pg_catalog.pg_type bt\n"
							 "                      WHERE c.oid = t.typcollation AND bt.oid = t.typbasetype AND t.typcollation <> bt.typcollation), '') ||\n");
	appendPQExpBuffer(&buf,
	   "            CASE WHEN t.typnotnull THEN ' not null' ELSE '' END ||\n"
					  "            CASE WHEN t.typdefault IS NOT NULL THEN ' default ' || t.typdefault ELSE '' END\n"
					  "       ) as \"%s\",\n"
					  "       pg_catalog.array_to_string(ARRAY(\n"
					  "         SELECT pg_catalog.pg_get_constraintdef(r.oid, true) FROM pg_catalog.pg_constraint r WHERE t.oid = r.contypid\n"
					  "       ), ' ') as \"%s\"",
					  gettext_noop("Modifier"),
					  gettext_noop("Check"));

	if (verbose)
	{
		if (pset.sversion >= 90200)
		{
			appendPQExpBufferStr(&buf, ",\n  ");
			printACLColumn(&buf, "t.typacl");
		}
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

	processSQLNamePattern(pset.db, &buf, pattern, true, false,
						  "n.nspname", "t.typname", NULL,
						  "pg_catalog.pg_type_is_visible(t.oid)");

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of domains");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

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

	processSQLNamePattern(pset.db, &buf, pattern, true, false,
						  "n.nspname", "c.conname", NULL,
						  "pg_catalog.pg_conversion_is_visible(c.oid)");

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of conversions");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

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
					  gettext_noop("Procedure"),
					  gettext_noop("Tags"));
	if (verbose)
		appendPQExpBuffer(&buf,
		",\npg_catalog.obj_description(e.oid, 'pg_event_trigger') as \"%s\"",
						  gettext_noop("Description"));
	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_event_trigger e ");

	processSQLNamePattern(pset.db, &buf, pattern, false, false,
						  NULL, "evtname", NULL, NULL);

	appendPQExpBufferStr(&buf, "ORDER BY 1");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of event triggers");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

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
	static const bool translate_columns[] = {false, false, false, true, false};

	initPQExpBuffer(&buf);

	/*
	 * We need a left join to pg_proc for binary casts; the others are just
	 * paranoia.  Also note that we don't attempt to localize '(binary
	 * coercible)', because there's too much risk of gettext translating a
	 * function name that happens to match some string in the PO database.
	 */
	printfPQExpBuffer(&buf,
			   "SELECT pg_catalog.format_type(castsource, NULL) AS \"%s\",\n"
			   "       pg_catalog.format_type(casttarget, NULL) AS \"%s\",\n"
				  "       CASE WHEN castfunc = 0 THEN '(binary coercible)'\n"
					  "            ELSE p.proname\n"
					  "       END as \"%s\",\n"
					  "       CASE WHEN c.castcontext = 'e' THEN '%s'\n"
					  "            WHEN c.castcontext = 'a' THEN '%s'\n"
					  "            ELSE '%s'\n"
					  "       END as \"%s\"",
					  gettext_noop("Source type"),
					  gettext_noop("Target type"),
					  gettext_noop("Function"),
					  gettext_noop("no"),
					  gettext_noop("in assignment"),
					  gettext_noop("yes"),
					  gettext_noop("Implicit?"));

	if (verbose)
		appendPQExpBuffer(&buf,
						  ",\n       d.description AS \"%s\"\n",
						  gettext_noop("Description"));

	appendPQExpBufferStr(&buf,
				 "FROM pg_catalog.pg_cast c LEFT JOIN pg_catalog.pg_proc p\n"
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
	processSQLNamePattern(pset.db, &buf, pattern, true, false,
						  "ns.nspname", "ts.typname",
						  "pg_catalog.format_type(ts.oid, NULL)",
						  "pg_catalog.pg_type_is_visible(ts.oid)");

	appendPQExpBufferStr(&buf, ") OR (true");

	processSQLNamePattern(pset.db, &buf, pattern, true, false,
						  "nt.nspname", "tt.typname",
						  "pg_catalog.format_type(tt.oid, NULL)",
						  "pg_catalog.pg_type_is_visible(tt.oid)");

	appendPQExpBufferStr(&buf, ") )\nORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of casts");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

	PQclear(res);
	return true;
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
	static const bool translate_columns[] = {false, false, false, false, false};

	if (pset.sversion < 90100)
	{
		psql_error("The server (version %d.%d) does not support collations.\n",
				   pset.sversion / 10000, (pset.sversion / 100) % 100);
		return true;
	}

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT n.nspname AS \"%s\",\n"
					  "       c.collname AS \"%s\",\n"
					  "       c.collcollate AS \"%s\",\n"
					  "       c.collctype AS \"%s\"",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("Collate"),
					  gettext_noop("Ctype"));

	if (verbose)
		appendPQExpBuffer(&buf,
						  ",\n       pg_catalog.obj_description(c.oid, 'pg_collation') AS \"%s\"",
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

	processSQLNamePattern(pset.db, &buf, pattern, true, false,
						  "n.nspname", "c.collname", NULL,
						  "pg_catalog.pg_collation_is_visible(c.oid)");

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of collations");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

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

	appendPQExpBuffer(&buf,
					  "\nFROM pg_catalog.pg_namespace n\n");

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf,
		"WHERE n.nspname !~ '^pg_' AND n.nspname <> 'information_schema'\n");

	processSQLNamePattern(pset.db, &buf, pattern,
						  !showSystem && !pattern, false,
						  NULL, "n.nspname", NULL,
						  NULL);

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of schemas");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

	PQclear(res);
	return true;
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

	if (pset.sversion < 80300)
	{
		psql_error("The server (version %d.%d) does not support full text search.\n",
				   pset.sversion / 10000, (pset.sversion / 100) % 100);
		return true;
	}

	if (verbose)
		return listTSParsersVerbose(pattern);

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT \n"
					  "  n.nspname as \"%s\",\n"
					  "  p.prsname as \"%s\",\n"
			"  pg_catalog.obj_description(p.oid, 'pg_ts_parser') as \"%s\"\n"
					  "FROM pg_catalog.pg_ts_parser p \n"
		   "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = p.prsnamespace\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("Description")
		);

	processSQLNamePattern(pset.db, &buf, pattern, false, false,
						  "n.nspname", "p.prsname", NULL,
						  "pg_catalog.pg_ts_parser_is_visible(p.oid)");

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of text search parsers");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

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
					  "SELECT p.oid, \n"
					  "  n.nspname, \n"
					  "  p.prsname \n"
					  "FROM pg_catalog.pg_ts_parser p\n"
			"LEFT JOIN pg_catalog.pg_namespace n ON n.oid = p.prsnamespace\n"
		);

	processSQLNamePattern(pset.db, &buf, pattern, false, false,
						  "n.nspname", "p.prsname", NULL,
						  "pg_catalog.pg_ts_parser_is_visible(p.oid)");

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	if (PQntuples(res) == 0)
	{
		if (!pset.quiet)
			psql_error("Did not find any text search parser named \"%s\".\n",
					   pattern);
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
	char		title[1024];
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {true, false, false};

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT '%s' AS \"%s\", \n"
					  "   p.prsstart::pg_catalog.regproc AS \"%s\", \n"
		  "   pg_catalog.obj_description(p.prsstart, 'pg_proc') as \"%s\" \n"
					  " FROM pg_catalog.pg_ts_parser p \n"
					  " WHERE p.oid = '%s' \n"
					  "UNION ALL \n"
					  "SELECT '%s', \n"
					  "   p.prstoken::pg_catalog.regproc, \n"
					"   pg_catalog.obj_description(p.prstoken, 'pg_proc') \n"
					  " FROM pg_catalog.pg_ts_parser p \n"
					  " WHERE p.oid = '%s' \n"
					  "UNION ALL \n"
					  "SELECT '%s', \n"
					  "   p.prsend::pg_catalog.regproc, \n"
					  "   pg_catalog.obj_description(p.prsend, 'pg_proc') \n"
					  " FROM pg_catalog.pg_ts_parser p \n"
					  " WHERE p.oid = '%s' \n"
					  "UNION ALL \n"
					  "SELECT '%s', \n"
					  "   p.prsheadline::pg_catalog.regproc, \n"
				 "   pg_catalog.obj_description(p.prsheadline, 'pg_proc') \n"
					  " FROM pg_catalog.pg_ts_parser p \n"
					  " WHERE p.oid = '%s' \n"
					  "UNION ALL \n"
					  "SELECT '%s', \n"
					  "   p.prslextype::pg_catalog.regproc, \n"
				  "   pg_catalog.obj_description(p.prslextype, 'pg_proc') \n"
					  " FROM pg_catalog.pg_ts_parser p \n"
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

	myopt.nullPrint = NULL;
	if (nspname)
		sprintf(title, _("Text search parser \"%s.%s\""), nspname, prsname);
	else
		sprintf(title, _("Text search parser \"%s\""), prsname);
	myopt.title = title;
	myopt.footers = NULL;
	myopt.topt.default_footer = false;
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

	PQclear(res);

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT t.alias as \"%s\", \n"
					  "  t.description as \"%s\" \n"
			  "FROM pg_catalog.ts_token_type( '%s'::pg_catalog.oid ) as t \n"
					  "ORDER BY 1;",
					  gettext_noop("Token name"),
					  gettext_noop("Description"),
					  oid);

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	if (nspname)
		sprintf(title, _("Token types for parser \"%s.%s\""), nspname, prsname);
	else
		sprintf(title, _("Token types for parser \"%s\""), prsname);
	myopt.title = title;
	myopt.footers = NULL;
	myopt.topt.default_footer = true;
	myopt.translate_header = true;
	myopt.translate_columns = NULL;
	myopt.n_translate_columns = 0;

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

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

	if (pset.sversion < 80300)
	{
		psql_error("The server (version %d.%d) does not support full text search.\n",
				   pset.sversion / 10000, (pset.sversion / 100) % 100);
		return true;
	}

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT \n"
					  "  n.nspname as \"%s\",\n"
					  "  d.dictname as \"%s\",\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"));

	if (verbose)
	{
		appendPQExpBuffer(&buf,
						  "  ( SELECT COALESCE(nt.nspname, '(null)')::pg_catalog.text || '.' || t.tmplname FROM \n"
						  "    pg_catalog.pg_ts_template t \n"
						  "			 LEFT JOIN pg_catalog.pg_namespace nt ON nt.oid = t.tmplnamespace \n"
						  "			 WHERE d.dicttemplate = t.oid ) AS  \"%s\", \n"
						  "  d.dictinitoption as \"%s\", \n",
						  gettext_noop("Template"),
						  gettext_noop("Init options"));
	}

	appendPQExpBuffer(&buf,
			 "  pg_catalog.obj_description(d.oid, 'pg_ts_dict') as \"%s\"\n",
					  gettext_noop("Description"));

	appendPQExpBufferStr(&buf, "FROM pg_catalog.pg_ts_dict d\n"
		 "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = d.dictnamespace\n");

	processSQLNamePattern(pset.db, &buf, pattern, false, false,
						  "n.nspname", "d.dictname", NULL,
						  "pg_catalog.pg_ts_dict_is_visible(d.oid)");

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of text search dictionaries");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

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

	if (pset.sversion < 80300)
	{
		psql_error("The server (version %d.%d) does not support full text search.\n",
				   pset.sversion / 10000, (pset.sversion / 100) % 100);
		return true;
	}

	initPQExpBuffer(&buf);

	if (verbose)
		printfPQExpBuffer(&buf,
						  "SELECT \n"
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
						  "SELECT \n"
						  "  n.nspname AS \"%s\",\n"
						  "  t.tmplname AS \"%s\",\n"
		 "  pg_catalog.obj_description(t.oid, 'pg_ts_template') AS \"%s\"\n",
						  gettext_noop("Schema"),
						  gettext_noop("Name"),
						  gettext_noop("Description"));

	appendPQExpBufferStr(&buf, "FROM pg_catalog.pg_ts_template t\n"
		 "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = t.tmplnamespace\n");

	processSQLNamePattern(pset.db, &buf, pattern, false, false,
						  "n.nspname", "t.tmplname", NULL,
						  "pg_catalog.pg_ts_template_is_visible(t.oid)");

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of text search templates");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

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

	if (pset.sversion < 80300)
	{
		psql_error("The server (version %d.%d) does not support full text search.\n",
				   pset.sversion / 10000, (pset.sversion / 100) % 100);
		return true;
	}

	if (verbose)
		return listTSConfigsVerbose(pattern);

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT \n"
					  "   n.nspname as \"%s\",\n"
					  "   c.cfgname as \"%s\",\n"
		   "   pg_catalog.obj_description(c.oid, 'pg_ts_config') as \"%s\"\n"
					  "FROM pg_catalog.pg_ts_config c\n"
		  "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.cfgnamespace \n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("Description")
		);

	processSQLNamePattern(pset.db, &buf, pattern, false, false,
						  "n.nspname", "c.cfgname", NULL,
						  "pg_catalog.pg_ts_config_is_visible(c.oid)");

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of text search configurations");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

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
					  "   n.nspname, \n"
					  "   p.prsname, \n"
					  "   np.nspname as pnspname \n"
					  "FROM pg_catalog.pg_ts_config c \n"
	   "   LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.cfgnamespace, \n"
					  " pg_catalog.pg_ts_parser p \n"
	  "   LEFT JOIN pg_catalog.pg_namespace np ON np.oid = p.prsnamespace \n"
					  "WHERE  p.oid = c.cfgparser\n"
		);

	processSQLNamePattern(pset.db, &buf, pattern, true, false,
						  "n.nspname", "c.cfgname", NULL,
						  "pg_catalog.pg_ts_config_is_visible(c.oid)");

	appendPQExpBufferStr(&buf, "ORDER BY 3, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	if (PQntuples(res) == 0)
	{
		if (!pset.quiet)
			psql_error("Did not find any text search configuration named \"%s\".\n",
					   pattern);
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
					  "SELECT \n"
					  "  ( SELECT t.alias FROM \n"
					  "    pg_catalog.ts_token_type(c.cfgparser) AS t \n"
					  "    WHERE t.tokid = m.maptokentype ) AS \"%s\", \n"
					  "  pg_catalog.btrim( \n"
				  "    ARRAY( SELECT mm.mapdict::pg_catalog.regdictionary \n"
					  "           FROM pg_catalog.pg_ts_config_map AS mm \n"
					  "           WHERE mm.mapcfg = m.mapcfg AND mm.maptokentype = m.maptokentype \n"
					  "           ORDER BY mapcfg, maptokentype, mapseqno \n"
					  "    ) :: pg_catalog.text , \n"
					  "  '{}') AS \"%s\" \n"
	 "FROM pg_catalog.pg_ts_config AS c, pg_catalog.pg_ts_config_map AS m \n"
					  "WHERE c.oid = '%s' AND m.mapcfg = c.oid \n"
					  "GROUP BY m.mapcfg, m.maptokentype, c.cfgparser \n"
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

	myopt.nullPrint = NULL;
	myopt.title = title.data;
	myopt.footers = NULL;
	myopt.topt.default_footer = false;
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

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

	if (pset.sversion < 80400)
	{
		psql_error("The server (version %d.%d) does not support foreign-data wrappers.\n",
				   pset.sversion / 10000, (pset.sversion / 100) % 100);
		return true;
	}

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT fdw.fdwname AS \"%s\",\n"
				   "  pg_catalog.pg_get_userbyid(fdw.fdwowner) AS \"%s\",\n",
					  gettext_noop("Name"),
					  gettext_noop("Owner"));
	if (pset.sversion >= 90100)
		appendPQExpBuffer(&buf,
						  "  fdw.fdwhandler::pg_catalog.regproc AS \"%s\",\n",
						  gettext_noop("Handler"));
	appendPQExpBuffer(&buf,
					  "  fdw.fdwvalidator::pg_catalog.regproc AS \"%s\"",
					  gettext_noop("Validator"));

	if (verbose)
	{
		appendPQExpBufferStr(&buf, ",\n  ");
		printACLColumn(&buf, "fdwacl");
		appendPQExpBuffer(&buf,
						  ",\n CASE WHEN fdwoptions IS NULL THEN '' ELSE "
						  "  '(' || array_to_string(ARRAY(SELECT "
						  "  quote_ident(option_name) ||  ' ' || "
						  "  quote_literal(option_value)  FROM "
						  "  pg_options_to_table(fdwoptions)),  ', ') || ')' "
						  "  END AS \"%s\"",
						  gettext_noop("FDW Options"));

		if (pset.sversion >= 90100)
			appendPQExpBuffer(&buf,
							  ",\n  d.description AS \"%s\" ",
							  gettext_noop("Description"));
	}

	appendPQExpBufferStr(&buf, "\nFROM pg_catalog.pg_foreign_data_wrapper fdw\n");

	if (verbose && pset.sversion >= 90100)
		appendPQExpBufferStr(&buf,
							 "LEFT JOIN pg_catalog.pg_description d\n"
							 "       ON d.classoid = fdw.tableoid "
							 "AND d.objoid = fdw.oid AND d.objsubid = 0\n");

	processSQLNamePattern(pset.db, &buf, pattern, false, false,
						  NULL, "fdwname", NULL, NULL);

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of foreign-data wrappers");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

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

	if (pset.sversion < 80400)
	{
		psql_error("The server (version %d.%d) does not support foreign servers.\n",
				   pset.sversion / 10000, (pset.sversion / 100) % 100);
		return true;
	}

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
						  "  '(' || array_to_string(ARRAY(SELECT "
						  "  quote_ident(option_name) ||  ' ' || "
						  "  quote_literal(option_value)  FROM "
						  "  pg_options_to_table(srvoptions)),  ', ') || ')' "
						  "  END AS \"%s\",\n"
						  "  d.description AS \"%s\"",
						  gettext_noop("Type"),
						  gettext_noop("Version"),
						  gettext_noop("FDW Options"),
						  gettext_noop("Description"));
	}

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_foreign_server s\n"
	   "     JOIN pg_catalog.pg_foreign_data_wrapper f ON f.oid=s.srvfdw\n");

	if (verbose)
		appendPQExpBufferStr(&buf,
							 "LEFT JOIN pg_description d\n       "
						   "ON d.classoid = s.tableoid AND d.objoid = s.oid "
							 "AND d.objsubid = 0\n");

	processSQLNamePattern(pset.db, &buf, pattern, false, false,
						  NULL, "s.srvname", NULL, NULL);

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of foreign servers");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

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

	if (pset.sversion < 80400)
	{
		psql_error("The server (version %d.%d) does not support user mappings.\n",
				   pset.sversion / 10000, (pset.sversion / 100) % 100);
		return true;
	}

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT um.srvname AS \"%s\",\n"
					  "  um.usename AS \"%s\"",
					  gettext_noop("Server"),
					  gettext_noop("User name"));

	if (verbose)
		appendPQExpBuffer(&buf,
						  ",\n CASE WHEN umoptions IS NULL THEN '' ELSE "
						  "  '(' || array_to_string(ARRAY(SELECT "
						  "  quote_ident(option_name) ||  ' ' || "
						  "  quote_literal(option_value)  FROM "
						  "  pg_options_to_table(umoptions)),  ', ') || ')' "
						  "  END AS \"%s\"",
						  gettext_noop("FDW Options"));

	appendPQExpBufferStr(&buf, "\nFROM pg_catalog.pg_user_mappings um\n");

	processSQLNamePattern(pset.db, &buf, pattern, false, false,
						  NULL, "um.srvname", "um.usename", NULL);

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of user mappings");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

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

	if (pset.sversion < 90100)
	{
		psql_error("The server (version %d.%d) does not support foreign tables.\n",
				   pset.sversion / 10000, (pset.sversion / 100) % 100);
		return true;
	}

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
						  "  '(' || array_to_string(ARRAY(SELECT "
						  "  quote_ident(option_name) ||  ' ' || "
						  "  quote_literal(option_value)  FROM "
						  "  pg_options_to_table(ftoptions)),  ', ') || ')' "
						  "  END AS \"%s\",\n"
						  "  d.description AS \"%s\"",
						  gettext_noop("FDW Options"),
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

	processSQLNamePattern(pset.db, &buf, pattern, false, false,
						  NULL, "n.nspname", "c.relname", NULL);

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of foreign tables");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

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

	if (pset.sversion < 90100)
	{
		psql_error("The server (version %d.%d) does not support extensions.\n",
				   pset.sversion / 10000, (pset.sversion / 100) % 100);
		return true;
	}

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT e.extname AS \"%s\", "
	 "e.extversion AS \"%s\", n.nspname AS \"%s\", c.description AS \"%s\"\n"
					  "FROM pg_catalog.pg_extension e "
			 "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = e.extnamespace "
				 "LEFT JOIN pg_catalog.pg_description c ON c.objoid = e.oid "
		 "AND c.classoid = 'pg_catalog.pg_extension'::pg_catalog.regclass\n",
					  gettext_noop("Name"),
					  gettext_noop("Version"),
					  gettext_noop("Schema"),
					  gettext_noop("Description"));

	processSQLNamePattern(pset.db, &buf, pattern,
						  false, false,
						  NULL, "e.extname", NULL,
						  NULL);

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of installed extensions");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

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

	if (pset.sversion < 90100)
	{
		psql_error("The server (version %d.%d) does not support extensions.\n",
				   pset.sversion / 10000, (pset.sversion / 100) % 100);
		return true;
	}

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT e.extname, e.oid\n"
					  "FROM pg_catalog.pg_extension e\n");

	processSQLNamePattern(pset.db, &buf, pattern,
						  false, false,
						  NULL, "e.extname", NULL,
						  NULL);

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
				psql_error("Did not find any extension named \"%s\".\n",
						   pattern);
			else
				psql_error("Did not find any extensions.\n");
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
	char		title[1024];
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
		"SELECT pg_catalog.pg_describe_object(classid, objid, 0) AS \"%s\"\n"
					  "FROM pg_catalog.pg_depend\n"
					  "WHERE refclassid = 'pg_catalog.pg_extension'::pg_catalog.regclass AND refobjid = '%s' AND deptype = 'e'\n"
					  "ORDER BY 1;",
					  gettext_noop("Object Description"),
					  oid);

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	snprintf(title, sizeof(title), _("Objects in extension \"%s\""), extname);
	myopt.title = title;
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * printACLColumn
 *
 * Helper function for consistently formatting ACL (privilege) columns.
 * The proper targetlist entry is appended to buf.  Note lack of any
 * whitespace or comma decoration.
 */
static void
printACLColumn(PQExpBuffer buf, const char *colname)
{
	if (pset.sversion >= 80100)
		appendPQExpBuffer(buf,
						  "pg_catalog.array_to_string(%s, E'\\n') AS \"%s\"",
						  colname, gettext_noop("Access privileges"));
	else
		appendPQExpBuffer(buf,
						  "pg_catalog.array_to_string(%s, '\\n') AS \"%s\"",
						  colname, gettext_noop("Access privileges"));
}
