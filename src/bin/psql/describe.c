/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2003, PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/describe.c,v 1.87.2.1 2004/01/11 19:25:44 dennis Exp $
 */
#include "postgres_fe.h"
#include "describe.h"

#include "libpq-fe.h"
#include "pqexpbuffer.h"

#include "common.h"
#include "settings.h"
#include "print.h"
#include "variables.h"

#include <ctype.h>

#ifdef WIN32
/*
 * mbvalidate() is used in function describeOneTableDetails() to make sure
 * all characters of the cells will be printed to the DOS console in a
 * correct way
 */
#include "mbprint.h"
#endif


#define _(x) gettext((x))

static bool describeOneTableDetails(const char *schemaname,
						const char *relationname,
						const char *oid,
						bool verbose);
static void processNamePattern(PQExpBuffer buf, const char *pattern,
				   bool have_where, bool force_escape,
				   const char *schemavar, const char *namevar,
				   const char *altnamevar, const char *visibilityrule);


static void *
xmalloc(size_t size)
{
	void	   *tmp;

	tmp = malloc(size);
	if (!tmp)
	{
		psql_error("out of memory\n");
		exit(EXIT_FAILURE);
	}
	return tmp;
}

static void *
xmalloczero(size_t size)
{
	void	   *tmp;

	tmp = xmalloc(size);
	memset(tmp, 0, size);
	return tmp;
}


/*----------------
 * Handlers for various slash commands displaying some sort of list
 * of things in the database.
 *
 * If you add something here, try to format the query to look nice in -E output.
 *----------------
 */


/* \da
 * Takes an optional regexp to select particular aggregates
 */
bool
describeAggregates(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	/*
	 * There are two kinds of aggregates: ones that work on particular
	 * types and ones that work on all (denoted by input type = "any")
	 */
	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					  "  p.proname AS \"%s\",\n"
					  "  CASE p.proargtypes[0]\n"
					"    WHEN 'pg_catalog.\"any\"'::pg_catalog.regtype\n"
					  "    THEN CAST('%s' AS pg_catalog.text)\n"
			  "    ELSE pg_catalog.format_type(p.proargtypes[0], NULL)\n"
					  "  END AS \"%s\",\n"
			 "  pg_catalog.obj_description(p.oid, 'pg_proc') as \"%s\"\n"
					  "FROM pg_catalog.pg_proc p\n"
	"     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace\n"
					  "WHERE p.proisagg\n",
					  _("Schema"), _("Name"), _("(all types)"),
					  _("Data type"), _("Description"));

	processNamePattern(&buf, pattern, true, false,
					   "n.nspname", "p.proname", NULL,
					   "pg_catalog.pg_function_is_visible(p.oid)");

	appendPQExpBuffer(&buf, "ORDER BY 1, 2, 3;");

	res = PSQLexec(buf.data, false);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of aggregate functions");

	printQuery(res, &myopt, pset.queryFout);

	PQclear(res);
	return true;
}


/* \df
 * Takes an optional regexp to select particular functions
 */
bool
describeFunctions(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
			"SELECT CASE WHEN p.proretset THEN 'setof ' ELSE '' END ||\n"
			  "  pg_catalog.format_type(p.prorettype, NULL) as \"%s\",\n"
					  "  n.nspname as \"%s\",\n"
					  "  p.proname as \"%s\",\n"
				  "  pg_catalog.oidvectortypes(p.proargtypes) as \"%s\"",
					  _("Result data type"), _("Schema"), _("Name"),
					  _("Argument data types"));

	if (verbose)
		appendPQExpBuffer(&buf,
						  ",\n  u.usename as \"%s\",\n"
						  "  l.lanname as \"%s\",\n"
						  "  p.prosrc as \"%s\",\n"
			  "  pg_catalog.obj_description(p.oid, 'pg_proc') as \"%s\"",
						  _("Owner"), _("Language"),
						  _("Source code"), _("Description"));

	if (!verbose)
		appendPQExpBuffer(&buf,
						  "\nFROM pg_catalog.pg_proc p"
						  "\n     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace\n");
	else
		appendPQExpBuffer(&buf,
						  "\nFROM pg_catalog.pg_proc p"
						  "\n     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace"
		 "\n     LEFT JOIN pg_catalog.pg_language l ON l.oid = p.prolang"
						  "\n     LEFT JOIN pg_catalog.pg_user u ON u.usesysid = p.proowner\n");

	/*
	 * we skip in/out funcs by excluding functions that take or return
	 * cstring
	 */
	appendPQExpBuffer(&buf,
	   "WHERE p.prorettype <> 'pg_catalog.cstring'::pg_catalog.regtype\n"
					  "      AND p.proargtypes[0] <> 'pg_catalog.cstring'::pg_catalog.regtype\n"
					  "      AND NOT p.proisagg\n");

	processNamePattern(&buf, pattern, true, false,
					   "n.nspname", "p.proname", NULL,
					   "pg_catalog.pg_function_is_visible(p.oid)");

	appendPQExpBuffer(&buf, "ORDER BY 2, 3, 1, 4;");

	res = PSQLexec(buf.data, false);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of functions");

	printQuery(res, &myopt, pset.queryFout);

	PQclear(res);
	return true;
}



/*
 * \dT
 * describe types
 */
bool
describeTypes(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					"  pg_catalog.format_type(t.oid, NULL) AS \"%s\",\n",
					  _("Schema"), _("Name"));
	if (verbose)
		appendPQExpBuffer(&buf,
						  "  t.typname AS \"%s\",\n"
						  "  CASE WHEN t.typrelid != 0\n"
						  "      THEN CAST('tuple' AS pg_catalog.text)\n"
						  "    WHEN t.typlen < 0\n"
						  "      THEN CAST('var' AS pg_catalog.text)\n"
						  "    ELSE CAST(t.typlen AS pg_catalog.text)\n"
						  "  END AS \"%s\",\n",
						  _("Internal name"), _("Size"));
	appendPQExpBuffer(&buf,
			"  pg_catalog.obj_description(t.oid, 'pg_type') as \"%s\"\n",
					  _("Description"));

	appendPQExpBuffer(&buf, "FROM pg_catalog.pg_type t\n"
					  "     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = t.typnamespace\n");

	/*
	 * do not include array types (start with underscore); do not include
	 * complex types (typrelid!=0) unless they are standalone composite
	 * types
	 */
	appendPQExpBuffer(&buf, "WHERE (t.typrelid = 0 ");
	appendPQExpBuffer(&buf, "OR (SELECT c.relkind = 'c' FROM pg_catalog.pg_class c "
					  "WHERE c.oid = t.typrelid)) ");
	appendPQExpBuffer(&buf, "AND t.typname !~ '^_'\n");

	/* Match name pattern against either internal or external name */
	processNamePattern(&buf, pattern, true, false,
					   "n.nspname", "t.typname",
					   "pg_catalog.format_type(t.oid, NULL)",
					   "pg_catalog.pg_type_is_visible(t.oid)");

	appendPQExpBuffer(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data, false);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of data types");

	printQuery(res, &myopt, pset.queryFout);

	PQclear(res);
	return true;
}



/* \do
 */
bool
describeOperators(const char *pattern)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					  "  o.oprname AS \"%s\",\n"
					  "  CASE WHEN o.oprkind='l' THEN NULL ELSE pg_catalog.format_type(o.oprleft, NULL) END AS \"%s\",\n"
					  "  CASE WHEN o.oprkind='r' THEN NULL ELSE pg_catalog.format_type(o.oprright, NULL) END AS \"%s\",\n"
			   "  pg_catalog.format_type(o.oprresult, NULL) AS \"%s\",\n"
		 "  coalesce(pg_catalog.obj_description(o.oid, 'pg_operator'),\n"
					  "           pg_catalog.obj_description(o.oprcode, 'pg_proc')) AS \"%s\"\n"
					  "FROM pg_catalog.pg_operator o\n"
					  "     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = o.oprnamespace\n",
					  _("Schema"), _("Name"),
					  _("Left arg type"), _("Right arg type"),
					  _("Result type"), _("Description"));

	processNamePattern(&buf, pattern, false, true,
					   "n.nspname", "o.oprname", NULL,
					   "pg_catalog.pg_operator_is_visible(o.oid)");

	appendPQExpBuffer(&buf, "ORDER BY 1, 2, 3, 4;");

	res = PSQLexec(buf.data, false);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of operators");

	printQuery(res, &myopt, pset.queryFout);

	PQclear(res);
	return true;
}


/*
 * listAllDbs
 *
 * for \l, \list, and -l switch
 */
bool
listAllDbs(bool verbose)
{
	PGresult   *res;
	PQExpBufferData buf;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT d.datname as \"%s\",\n"
					  "       u.usename as \"%s\"",
					  _("Name"), _("Owner"));
	appendPQExpBuffer(&buf,
		",\n       pg_catalog.pg_encoding_to_char(d.encoding) as \"%s\"",
					  _("Encoding"));
	if (verbose)
		appendPQExpBuffer(&buf,
						  ",\n       pg_catalog.obj_description(d.oid, 'pg_database') as \"%s\"",
						  _("Description"));
	appendPQExpBuffer(&buf,
					  "\nFROM pg_catalog.pg_database d"
		  "\n  LEFT JOIN pg_catalog.pg_user u ON d.datdba = u.usesysid\n"
					  "ORDER BY 1;");

	res = PSQLexec(buf.data, false);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of databases");

	printQuery(res, &myopt, pset.queryFout);

	PQclear(res);
	return true;
}


/*
 * List Tables Grant/Revoke Permissions
 * \z (now also \dp -- perhaps more mnemonic)
 */
bool
permissionsList(const char *pattern)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	/*
	 * we ignore indexes and toast tables since they have no meaningful
	 * rights
	 */
	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					  "  c.relname as \"%s\",\n"
					  "  c.relacl as \"%s\"\n"
					  "FROM pg_catalog.pg_class c\n"
	"     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace\n"
					  "WHERE c.relkind IN ('r', 'v', 'S')\n",
					  _("Schema"), _("Table"), _("Access privileges"));

	/*
	 * Unless a schema pattern is specified, we suppress system and temp
	 * tables, since they normally aren't very interesting from a
	 * permissions point of view.  You can see 'em by explicit request
	 * though, eg with \z pg_catalog.*
	 */
	processNamePattern(&buf, pattern, true, false,
					   "n.nspname", "c.relname", NULL,
		"pg_catalog.pg_table_is_visible(c.oid) AND n.nspname !~ '^pg_'");

	appendPQExpBuffer(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data, false);
	if (!res)
	{
		termPQExpBuffer(&buf);
		return false;
	}

	myopt.nullPrint = NULL;
	printfPQExpBuffer(&buf, _("Access privileges for database \"%s\""), PQdb(pset.db));
	myopt.title = buf.data;

	printQuery(res, &myopt, pset.queryFout);

	termPQExpBuffer(&buf);
	PQclear(res);
	return true;
}



/*
 * Get object comments
 *
 * \dd [foo]
 *
 * Note: This only lists things that actually have a description. For complete
 * lists of things, there are other \d? commands.
 */
bool
objectDescription(const char *pattern)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	appendPQExpBuffer(&buf,
					  "SELECT DISTINCT tt.nspname AS \"%s\", tt.name AS \"%s\", tt.object AS \"%s\", d.description AS \"%s\"\n"
					  "FROM (\n",
				  _("Schema"), _("Name"), _("Object"), _("Description"));

	/* Aggregate descriptions */
	appendPQExpBuffer(&buf,
					  "  SELECT p.oid as oid, p.tableoid as tableoid,\n"
					  "  n.nspname as nspname,\n"
					  "  CAST(p.proname AS pg_catalog.text) as name,"
					  "  CAST('%s' AS pg_catalog.text) as object\n"
					  "  FROM pg_catalog.pg_proc p\n"
					  "       LEFT JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace\n"
					  "  WHERE p.proisagg\n",
					  _("aggregate"));
	processNamePattern(&buf, pattern, true, false,
					   "n.nspname", "p.proname", NULL,
					   "pg_catalog.pg_function_is_visible(p.oid)");

	/* Function descriptions (except in/outs for datatypes) */
	appendPQExpBuffer(&buf,
					  "UNION ALL\n"
					  "  SELECT p.oid as oid, p.tableoid as tableoid,\n"
					  "  n.nspname as nspname,\n"
					  "  CAST(p.proname AS pg_catalog.text) as name,"
					  "  CAST('%s' AS pg_catalog.text) as object\n"
					  "  FROM pg_catalog.pg_proc p\n"
					  "       LEFT JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace\n"

	 "  WHERE p.prorettype <> 'pg_catalog.cstring'::pg_catalog.regtype\n"
					  "      AND p.proargtypes[0] <> 'pg_catalog.cstring'::pg_catalog.regtype\n"
					  "      AND NOT p.proisagg\n",
					  _("function"));
	processNamePattern(&buf, pattern, true, false,
					   "n.nspname", "p.proname", NULL,
					   "pg_catalog.pg_function_is_visible(p.oid)");

	/* Operator descriptions (only if operator has its own comment) */
	appendPQExpBuffer(&buf,
					  "UNION ALL\n"
					  "  SELECT o.oid as oid, o.tableoid as tableoid,\n"
					  "  n.nspname as nspname,\n"
					  "  CAST(o.oprname AS pg_catalog.text) as name,"
					  "  CAST('%s' AS pg_catalog.text) as object\n"
					  "  FROM pg_catalog.pg_operator o\n"
					  "       LEFT JOIN pg_catalog.pg_namespace n ON n.oid = o.oprnamespace\n",
					  _("operator"));
	processNamePattern(&buf, pattern, false, false,
					   "n.nspname", "o.oprname", NULL,
					   "pg_catalog.pg_operator_is_visible(o.oid)");

	/* Type description */
	appendPQExpBuffer(&buf,
					  "UNION ALL\n"
					  "  SELECT t.oid as oid, t.tableoid as tableoid,\n"
					  "  n.nspname as nspname,\n"
					  "  pg_catalog.format_type(t.oid, NULL) as name,"
					  "  CAST('%s' AS pg_catalog.text) as object\n"
					  "  FROM pg_catalog.pg_type t\n"
					  "       LEFT JOIN pg_catalog.pg_namespace n ON n.oid = t.typnamespace\n",
					  _("data type"));
	processNamePattern(&buf, pattern, false, false,
				"n.nspname", "pg_catalog.format_type(t.oid, NULL)", NULL,
					   "pg_catalog.pg_type_is_visible(t.oid)");

	/* Relation (tables, views, indexes, sequences) descriptions */
	appendPQExpBuffer(&buf,
					  "UNION ALL\n"
					  "  SELECT c.oid as oid, c.tableoid as tableoid,\n"
					  "  n.nspname as nspname,\n"
					  "  CAST(c.relname AS pg_catalog.text) as name,\n"
					  "  CAST(\n"
					  "    CASE c.relkind WHEN 'r' THEN '%s' WHEN 'v' THEN '%s' WHEN 'i' THEN '%s' WHEN 'S' THEN '%s' END"
					  "  AS pg_catalog.text) as object\n"
					  "  FROM pg_catalog.pg_class c\n"
					  "       LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace\n"
					  "  WHERE c.relkind IN ('r', 'v', 'i', 'S')\n",
					  _("table"), _("view"), _("index"), _("sequence"));
	processNamePattern(&buf, pattern, true, false,
					   "n.nspname", "c.relname", NULL,
					   "pg_catalog.pg_table_is_visible(c.oid)");

	/* Rule description (ignore rules for views) */
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
					  _("rule"));
	/* XXX not sure what to do about visibility rule here? */
	processNamePattern(&buf, pattern, true, false,
					   "n.nspname", "r.rulename", NULL,
					   "pg_catalog.pg_table_is_visible(c.oid)");

	/* Trigger description */
	appendPQExpBuffer(&buf,
					  "UNION ALL\n"
					  "  SELECT t.oid as oid, t.tableoid as tableoid,\n"
					  "  n.nspname as nspname,\n"
					  "  CAST(t.tgname AS pg_catalog.text) as name,"
					  "  CAST('%s' AS pg_catalog.text) as object\n"
					  "  FROM pg_catalog.pg_trigger t\n"
			   "       JOIN pg_catalog.pg_class c ON c.oid = t.tgrelid\n"
					  "       LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace\n",
					  _("trigger"));
	/* XXX not sure what to do about visibility rule here? */
	processNamePattern(&buf, pattern, false, false,
					   "n.nspname", "t.tgname", NULL,
					   "pg_catalog.pg_table_is_visible(c.oid)");

	appendPQExpBuffer(&buf,
					  ") AS tt\n"
					  "  JOIN pg_catalog.pg_description d ON (tt.oid = d.objoid AND tt.tableoid = d.classoid AND d.objsubid = 0)\n");

	appendPQExpBuffer(&buf, "ORDER BY 1, 2, 3;");

	res = PSQLexec(buf.data, false);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("Object descriptions");

	printQuery(res, &myopt, pset.queryFout);

	PQclear(res);
	return true;
}



/*
 * describeTableDetails (for \d)
 *
 * This routine finds the tables to be displayed, and calls
 * describeOneTableDetails for each one.
 */
bool
describeTableDetails(const char *pattern, bool verbose)
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

	processNamePattern(&buf, pattern, false, false,
					   "n.nspname", "c.relname", NULL,
					   "pg_catalog.pg_table_is_visible(c.oid)");

	appendPQExpBuffer(&buf, "ORDER BY 2, 3;");

	res = PSQLexec(buf.data, false);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	if (PQntuples(res) == 0)
	{
		if (!QUIET())
			fprintf(stderr, _("Did not find any relation named \"%s\".\n"),
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
	int			i;
	char	   *view_def = NULL;
	const char *headers[5];
	char	  **cells = NULL;
	char	  **footers = NULL;
	char	  **ptr;
	PQExpBufferData title;
	PQExpBufferData tmpbuf;
	int			cols = 0;
	int			numrows = 0;
	struct
	{
		bool		hasindex;
		char		relkind;
		int16		checks;
		int16		triggers;
		bool		hasrules;
	}			tableinfo;
	bool		show_modifiers = false;
	bool		retval;

	retval = false;

	initPQExpBuffer(&buf);
	initPQExpBuffer(&title);
	initPQExpBuffer(&tmpbuf);

	/* Get general table info */
	printfPQExpBuffer(&buf,
	 "SELECT relhasindex, relkind, relchecks, reltriggers, relhasrules\n"
					  "FROM pg_catalog.pg_class WHERE oid = '%s'",
					  oid);
	res = PSQLexec(buf.data, false);
	if (!res)
		goto error_return;

	/* Did we get anything? */
	if (PQntuples(res) == 0)
	{
		if (!QUIET())
			fprintf(stderr, _("Did not find any relation with OID %s.\n"),
					oid);
		goto error_return;
	}

	/* FIXME: check for null pointers here? */
	tableinfo.hasindex = strcmp(PQgetvalue(res, 0, 0), "t") == 0;
	tableinfo.relkind = *(PQgetvalue(res, 0, 1));
	tableinfo.checks = atoi(PQgetvalue(res, 0, 2));
	tableinfo.triggers = atoi(PQgetvalue(res, 0, 3));
	tableinfo.hasrules = strcmp(PQgetvalue(res, 0, 4), "t") == 0;
	PQclear(res);

	headers[0] = _("Column");
	headers[1] = _("Type");
	cols = 2;

	if (tableinfo.relkind == 'r' || tableinfo.relkind == 'v')
	{
		show_modifiers = true;
		cols++;
		headers[cols - 1] = _("Modifiers");
	}

	if (verbose)
	{
		cols++;
		headers[cols - 1] = _("Description");
	}

	headers[cols] = NULL;

	/* Get column info (index requires additional checks) */
	printfPQExpBuffer(&buf, "SELECT a.attname,");
	appendPQExpBuffer(&buf, "\n  pg_catalog.format_type(a.atttypid, a.atttypmod),"
	"\n  (SELECT substring(d.adsrc for 128) FROM pg_catalog.pg_attrdef d"
					  "\n   WHERE d.adrelid = a.attrelid AND d.adnum = a.attnum AND a.atthasdef),"
					  "\n  a.attnotnull, a.attnum");
	if (verbose)
		appendPQExpBuffer(&buf, ", pg_catalog.col_description(a.attrelid, a.attnum)");
	appendPQExpBuffer(&buf, "\nFROM pg_catalog.pg_attribute a");
	if (tableinfo.relkind == 'i')
		appendPQExpBuffer(&buf, ", pg_catalog.pg_index i");
	appendPQExpBuffer(&buf, "\nWHERE a.attrelid = '%s' AND a.attnum > 0 AND NOT a.attisdropped", oid);
	if (tableinfo.relkind == 'i')
		appendPQExpBuffer(&buf, " AND a.attrelid = i.indexrelid");
	appendPQExpBuffer(&buf, "\nORDER BY a.attnum");

	res = PSQLexec(buf.data, false);
	if (!res)
		goto error_return;
	numrows = PQntuples(res);

	/* Check if table is a view */
	if (tableinfo.relkind == 'v')
	{
		PGresult   *result;

		printfPQExpBuffer(&buf, "SELECT pg_catalog.pg_get_viewdef('%s'::pg_catalog.oid, true)", oid);
		result = PSQLexec(buf.data, false);
		if (!result)
			goto error_return;

		if (PQntuples(result) > 0)
			view_def = xstrdup(PQgetvalue(result, 0, 0));

		PQclear(result);
	}

	/* Generate table cells to be printed */
	/* note: initialize all cells[] to NULL in case of error exit */
	cells = xmalloczero((numrows * cols + 1) * sizeof(*cells));

	for (i = 0; i < numrows; i++)
	{
		/* Name */
#ifdef WIN32
		cells[i * cols + 0] = mbvalidate(PQgetvalue(res, i, 0), myopt.encoding);
#else
		cells[i * cols + 0] = PQgetvalue(res, i, 0);	/* don't free this
														 * afterwards */
#endif

		/* Type */
#ifdef WIN32
		cells[i * cols + 1] = mbvalidate(PQgetvalue(res, i, 1), myopt.encoding);
#else
		cells[i * cols + 1] = PQgetvalue(res, i, 1);	/* don't free this
														 * either */
#endif

		/* Extra: not null and default */
		if (show_modifiers)
		{
			resetPQExpBuffer(&tmpbuf);
			if (strcmp(PQgetvalue(res, i, 3), "t") == 0)
				appendPQExpBufferStr(&tmpbuf, "not null");

			/* handle "default" here */
			/* (note: above we cut off the 'default' string at 128) */
			if (strlen(PQgetvalue(res, i, 2)) != 0)
			{
				if (tmpbuf.len > 0)
					appendPQExpBufferStr(&tmpbuf, " ");
				appendPQExpBuffer(&tmpbuf, "default %s",
								  PQgetvalue(res, i, 2));
			}

#ifdef WIN32
			cells[i * cols + 2] = xstrdup(mbvalidate(tmpbuf.data, myopt.encoding));
#else
			cells[i * cols + 2] = xstrdup(tmpbuf.data);
#endif
		}

		/* Description */
		if (verbose)
#ifdef WIN32
			cells[i * cols + cols - 1] = mbvalidate(PQgetvalue(res, i, 5), myopt.encoding);
#else
			cells[i * cols + cols - 1] = PQgetvalue(res, i, 5);
#endif
	}

	/* Make title */
	switch (tableinfo.relkind)
	{
		case 'r':
			printfPQExpBuffer(&title, _("Table \"%s.%s\""),
							  schemaname, relationname);
			break;
		case 'v':
			printfPQExpBuffer(&title, _("View \"%s.%s\""),
							  schemaname, relationname);
			break;
		case 'S':
			printfPQExpBuffer(&title, _("Sequence \"%s.%s\""),
							  schemaname, relationname);
			break;
		case 'i':
			printfPQExpBuffer(&title, _("Index \"%s.%s\""),
							  schemaname, relationname);
			break;
		case 's':
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
		default:
			printfPQExpBuffer(&title, _("?%c? \"%s.%s\""),
							tableinfo.relkind, schemaname, relationname);
			break;
	}

	/* Make footers */
	if (tableinfo.relkind == 'i')
	{
		/* Footer information about an index */
		PGresult   *result;

		printfPQExpBuffer(&buf,
		  "SELECT i.indisunique, i.indisprimary, a.amname, c2.relname,\n"
					  "  pg_catalog.pg_get_expr(i.indpred, i.indrelid)\n"
						  "FROM pg_catalog.pg_index i, pg_catalog.pg_class c, pg_catalog.pg_class c2, pg_catalog.pg_am a\n"
						  "WHERE i.indexrelid = c.oid AND c.oid = '%s' AND c.relam = a.oid\n"
						  "AND i.indrelid = c2.oid",
						  oid);

		result = PSQLexec(buf.data, false);
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
			char	   *indamname = PQgetvalue(result, 0, 2);
			char	   *indtable = PQgetvalue(result, 0, 3);
			char	   *indpred = PQgetvalue(result, 0, 4);

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
				appendPQExpBuffer(&tmpbuf, _(", predicate %s"), indpred);

			footers = xmalloczero(2 * sizeof(*footers));
			footers[0] = xstrdup(tmpbuf.data);
			footers[1] = NULL;
		}

		PQclear(result);
	}
	else if (view_def)
	{
		PGresult   *result = NULL;
		int			rule_count = 0;
		int			count_footers = 0;

		/* count rules other than the view rule */
		if (tableinfo.hasrules)
		{
			printfPQExpBuffer(&buf,
							  "SELECT r.rulename\n"
							  "FROM pg_catalog.pg_rewrite r\n"
				   "WHERE r.ev_class = '%s' AND r.rulename != '_RETURN'",
							  oid);
			result = PSQLexec(buf.data, false);
			if (!result)
				goto error_return;
			else
				rule_count = PQntuples(result);
		}

		/* Footer information about a view */
		footers = xmalloczero((rule_count + 2) * sizeof(*footers));
		footers[count_footers] = xmalloc(64 + strlen(view_def));
		snprintf(footers[count_footers], 64 + strlen(view_def),
				 _("View definition:\n%s"), view_def);
		count_footers++;

		/* print rules */
		for (i = 0; i < rule_count; i++)
		{
			char	   *s = _("Rules");

			if (i == 0)
				printfPQExpBuffer(&buf, "%s: %s", s, PQgetvalue(result, i, 0));
			else
				printfPQExpBuffer(&buf, "%*s  %s", (int) strlen(s), "", PQgetvalue(result, i, 0));
			if (i < rule_count - 1)
				appendPQExpBuffer(&buf, ",");

			footers[count_footers++] = xstrdup(buf.data);
		}
		PQclear(result);

		footers[count_footers] = NULL;

	}
	else if (tableinfo.relkind == 'r')
	{
		/* Footer information about a table */
		PGresult   *result1 = NULL,
				   *result2 = NULL,
				   *result3 = NULL,
				   *result4 = NULL,
				   *result5 = NULL,
				   *result6 = NULL;
		int			check_count = 0,
					index_count = 0,
					foreignkey_count = 0,
					rule_count = 0,
					trigger_count = 0,
					inherits_count = 0;
		int			count_footers = 0;

		/* count indexes */
		if (tableinfo.hasindex)
		{
			printfPQExpBuffer(&buf,
					 "SELECT c2.relname, i.indisprimary, i.indisunique, "
							  "pg_catalog.pg_get_indexdef(i.indexrelid)\n"
							  "FROM pg_catalog.pg_class c, pg_catalog.pg_class c2, pg_catalog.pg_index i\n"
							  "WHERE c.oid = '%s' AND c.oid = i.indrelid AND i.indexrelid = c2.oid\n"
							  "ORDER BY i.indisprimary DESC, i.indisunique DESC, c2.relname",
							  oid);
			result1 = PSQLexec(buf.data, false);
			if (!result1)
				goto error_return;
			else
				index_count = PQntuples(result1);
		}

		/* count table (and column) check constraints */
		if (tableinfo.checks)
		{
			printfPQExpBuffer(&buf,
							  "SELECT "
							  "pg_catalog.pg_get_constraintdef(r.oid, true), "
							  "conname\n"
							  "FROM pg_catalog.pg_constraint r\n"
							  "WHERE r.conrelid = '%s' AND r.contype = 'c'",
							  oid);
			result2 = PSQLexec(buf.data, false);
			if (!result2)
			{
				PQclear(result1);
				goto error_return;
			}
			else
				check_count = PQntuples(result2);
		}

		/* count rules */
		if (tableinfo.hasrules)
		{
			printfPQExpBuffer(&buf,
							  "SELECT r.rulename, trim(trailing ';' from pg_catalog.pg_get_ruledef(r.oid))\n"
							  "FROM pg_catalog.pg_rewrite r\n"
							  "WHERE r.ev_class = '%s'",
							  oid);
			result3 = PSQLexec(buf.data, false);
			if (!result3)
			{
				PQclear(result1);
				PQclear(result2);
				goto error_return;
			}
			else
				rule_count = PQntuples(result3);
		}

		/* count triggers (but ignore foreign-key triggers) */
		if (tableinfo.triggers)
		{
			printfPQExpBuffer(&buf,
				 "SELECT t.tgname, pg_catalog.pg_get_triggerdef(t.oid)\n"
							  "FROM pg_catalog.pg_trigger t\n"
							  "WHERE t.tgrelid = '%s' "
							  "AND (not tgisconstraint "
							  " OR NOT EXISTS"
							  "  (SELECT 1 FROM pg_catalog.pg_depend d "
							  "   JOIN pg_catalog.pg_constraint c ON (d.refclassid = c.tableoid AND d.refobjid = c.oid) "
							  "   WHERE d.classid = t.tableoid AND d.objid = t.oid AND d.deptype = 'i' AND c.contype = 'f'))",
							  oid);
			result4 = PSQLexec(buf.data, false);
			if (!result4)
			{
				PQclear(result1);
				PQclear(result2);
				PQclear(result3);
				goto error_return;
			}
			else
				trigger_count = PQntuples(result4);
		}

		/* count foreign-key constraints (there are none if no triggers) */
		if (tableinfo.triggers)
		{
			printfPQExpBuffer(&buf,
							  "SELECT conname,\n"
					 "  pg_catalog.pg_get_constraintdef(oid) as condef\n"
							  "FROM pg_catalog.pg_constraint r\n"
						   "WHERE r.conrelid = '%s' AND r.contype = 'f'",
							  oid);
			result5 = PSQLexec(buf.data, false);
			if (!result5)
			{
				PQclear(result1);
				PQclear(result2);
				PQclear(result3);
				PQclear(result4);
				goto error_return;
			}
			else
				foreignkey_count = PQntuples(result5);
		}

		/* count inherited tables */
		printfPQExpBuffer(&buf, "SELECT c.relname FROM pg_catalog.pg_class c, pg_catalog.pg_inherits i WHERE c.oid=i.inhparent AND i.inhrelid = '%s' ORDER BY inhseqno ASC", oid);

		result6 = PSQLexec(buf.data, false);
		if (!result6)
			goto error_return;
		else
			inherits_count = PQntuples(result6);

		footers = xmalloczero((index_count + check_count + rule_count + trigger_count + foreignkey_count + inherits_count + 6)
							  * sizeof(*footers));

		/* print indexes */
		if (index_count > 0)
		{
			printfPQExpBuffer(&buf, _("Indexes:"));
			footers[count_footers++] = xstrdup(buf.data);
			for (i = 0; i < index_count; i++)
			{
				const char *indexdef;
				const char *usingpos;

				/* Output index name */
				printfPQExpBuffer(&buf, _("    \"%s\""),
								  PQgetvalue(result1, i, 0));

				/* Label as primary key or unique (but not both) */
				appendPQExpBuffer(&buf,
							  strcmp(PQgetvalue(result1, i, 1), "t") == 0
								  ? _(" primary key,") :
							 (strcmp(PQgetvalue(result1, i, 2), "t") == 0
							  ? _(" unique,")
							  : ""));

				/* Everything after "USING" is echoed verbatim */
				indexdef = PQgetvalue(result1, i, 3);
				usingpos = strstr(indexdef, " USING ");
				if (usingpos)
					indexdef = usingpos + 7;

				appendPQExpBuffer(&buf, " %s", indexdef);

				footers[count_footers++] = xstrdup(buf.data);
			}
		}

		/* print check constraints */
		if (check_count > 0)
		{
			printfPQExpBuffer(&buf, _("Check constraints:"));
			footers[count_footers++] = xstrdup(buf.data);
			for (i = 0; i < check_count; i++)
			{
				printfPQExpBuffer(&buf, _("    \"%s\" %s"),
								  PQgetvalue(result2, i, 1),
								  PQgetvalue(result2, i, 0));

				footers[count_footers++] = xstrdup(buf.data);
			}
		}

		/* print foreign key constraints */
		if (foreignkey_count > 0)
		{
			printfPQExpBuffer(&buf, _("Foreign-key constraints:"));
			footers[count_footers++] = xstrdup(buf.data);
			for (i = 0; i < foreignkey_count; i++)
			{
				printfPQExpBuffer(&buf, _("    \"%s\" %s"),
								  PQgetvalue(result5, i, 0),
								  PQgetvalue(result5, i, 1));

				footers[count_footers++] = xstrdup(buf.data);
			}
		}

		/* print rules */
		if (rule_count > 0)
		{
			printfPQExpBuffer(&buf, _("Rules:"));
			footers[count_footers++] = xstrdup(buf.data);
			for (i = 0; i < rule_count; i++)
			{
				const char *ruledef;

				/* Everything after "CREATE RULE" is echoed verbatim */
				ruledef = PQgetvalue(result3, i, 1);
				ruledef += 12;

				printfPQExpBuffer(&buf, "    %s", ruledef);

				footers[count_footers++] = xstrdup(buf.data);
			}
		}

		/* print triggers */
		if (trigger_count > 0)
		{
			printfPQExpBuffer(&buf, _("Triggers:"));
			footers[count_footers++] = xstrdup(buf.data);
			for (i = 0; i < trigger_count; i++)
			{
				const char *tgdef;
				const char *usingpos;

				/* Everything after "TRIGGER" is echoed verbatim */
				tgdef = PQgetvalue(result4, i, 1);
				usingpos = strstr(tgdef, " TRIGGER ");
				if (usingpos)
					tgdef = usingpos + 9;

				printfPQExpBuffer(&buf, "    %s", tgdef);

				footers[count_footers++] = xstrdup(buf.data);
			}
		}

		/* print inherits */
		for (i = 0; i < inherits_count; i++)
		{
			char	   *s = _("Inherits");

			if (i == 0)
				printfPQExpBuffer(&buf, "%s: %s", s, PQgetvalue(result6, i, 0));
			else
				printfPQExpBuffer(&buf, "%*s  %s", (int) strlen(s), "", PQgetvalue(result6, i, 0));
			if (i < inherits_count - 1)
				appendPQExpBuffer(&buf, ",");

			footers[count_footers++] = xstrdup(buf.data);
		}

		/* end of list marker */
		footers[count_footers] = NULL;

		PQclear(result1);
		PQclear(result2);
		PQclear(result3);
		PQclear(result4);
		PQclear(result5);
		PQclear(result6);
	}

	printTable(title.data, headers,
			   (const char **) cells, (const char **) footers,
			   "llll", &myopt, pset.queryFout);

	retval = true;

error_return:

	/* clean up */
	termPQExpBuffer(&buf);
	termPQExpBuffer(&title);
	termPQExpBuffer(&tmpbuf);

	if (cells)
	{
		for (i = 0; i < numrows; i++)
		{
			if (show_modifiers)
				free(cells[i * cols + 2]);
		}
		free(cells);
	}

	if (footers)
	{
		for (ptr = footers; *ptr; ptr++)
			free(*ptr);
		free(footers);
	}

	if (view_def)
		free(view_def);

	if (res)
		PQclear(res);

	return retval;
}


/*
 * \du
 *
 * Describes users.  Any schema portion of the pattern is ignored.
 */
bool
describeUsers(const char *pattern)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT u.usename AS \"%s\",\n"
					  "  u.usesysid AS \"%s\",\n"
					  "  CASE WHEN u.usesuper AND u.usecreatedb THEN CAST('%s' AS pg_catalog.text)\n"
			"       WHEN u.usesuper THEN CAST('%s' AS pg_catalog.text)\n"
		 "       WHEN u.usecreatedb THEN CAST('%s' AS pg_catalog.text)\n"
					  "       ELSE CAST('' AS pg_catalog.text)\n"
					  "  END AS \"%s\"\n"
					  "FROM pg_catalog.pg_user u\n",
					  _("User name"), _("User ID"),
					  _("superuser, create database"),
					  _("superuser"), _("create database"),
					  _("Attributes"));

	processNamePattern(&buf, pattern, false, false,
					   NULL, "u.usename", NULL, NULL);

	appendPQExpBuffer(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data, false);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of database users");

	printQuery(res, &myopt, pset.queryFout);

	PQclear(res);
	return true;
}


/*
 * listTables()
 *
 * handler for \d, \dt, etc.
 *
 * tabtypes is an array of characters, specifying what info is desired:
 * t - tables
 * i - indexes
 * v - views
 * s - sequences
 * S - system tables (pg_catalog)
 * (any order of the above is fine)
 */
bool
listTables(const char *tabtypes, const char *pattern, bool verbose)
{
	bool		showTables = strchr(tabtypes, 't') != NULL;
	bool		showIndexes = strchr(tabtypes, 'i') != NULL;
	bool		showViews = strchr(tabtypes, 'v') != NULL;
	bool		showSeq = strchr(tabtypes, 's') != NULL;
	bool		showSystem = strchr(tabtypes, 'S') != NULL;

	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	if (!(showTables || showIndexes || showViews || showSeq))
		showTables = showViews = showSeq = true;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					  "  c.relname as \"%s\",\n"
					  "  CASE c.relkind WHEN 'r' THEN '%s' WHEN 'v' THEN '%s' WHEN 'i' THEN '%s' WHEN 'S' THEN '%s' WHEN 's' THEN '%s' END as \"%s\",\n"
					  "  u.usename as \"%s\"",
					  _("Schema"), _("Name"),
					  _("table"), _("view"), _("index"), _("sequence"),
					  _("special"), _("Type"), _("Owner"));

	if (verbose)
		appendPQExpBuffer(&buf,
		  ",\n  pg_catalog.obj_description(c.oid, 'pg_class') as \"%s\"",
						  _("Description"));

	if (showIndexes)
		appendPQExpBuffer(&buf,
						  ",\n c2.relname as \"%s\""
						  "\nFROM pg_catalog.pg_class c"
			  "\n     JOIN pg_catalog.pg_index i ON i.indexrelid = c.oid"
			  "\n     JOIN pg_catalog.pg_class c2 ON i.indrelid = c2.oid"
		"\n     LEFT JOIN pg_catalog.pg_user u ON u.usesysid = c.relowner"
						  "\n     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace\n",
						  _("Table"));
	else
		appendPQExpBuffer(&buf,
						  "\nFROM pg_catalog.pg_class c"
		"\n     LEFT JOIN pg_catalog.pg_user u ON u.usesysid = c.relowner"
						  "\n     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace\n");

	appendPQExpBuffer(&buf, "WHERE c.relkind IN (");
	if (showTables)
		appendPQExpBuffer(&buf, "'r',");
	if (showViews)
		appendPQExpBuffer(&buf, "'v',");
	if (showIndexes)
		appendPQExpBuffer(&buf, "'i',");
	if (showSeq)
		appendPQExpBuffer(&buf, "'S',");
	if (showSystem && showTables)
		appendPQExpBuffer(&buf, "'s',");
	appendPQExpBuffer(&buf, "''");		/* dummy */
	appendPQExpBuffer(&buf, ")\n");

	/*
	 * If showSystem is specified, show only system objects (those in
	 * pg_catalog).  Otherwise, suppress system objects, including those
	 * in pg_catalog and pg_toast.	(We don't want to hide temp tables
	 * though.)
	 */
	if (showSystem)
		appendPQExpBuffer(&buf, "      AND n.nspname = 'pg_catalog'\n");
	else
		appendPQExpBuffer(&buf, "      AND n.nspname NOT IN ('pg_catalog', 'pg_toast')\n");

	processNamePattern(&buf, pattern, true, false,
					   "n.nspname", "c.relname", NULL,
					   "pg_catalog.pg_table_is_visible(c.oid)");

	appendPQExpBuffer(&buf, "ORDER BY 1,2;");

	res = PSQLexec(buf.data, false);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	if (PQntuples(res) == 0 && !QUIET())
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

		printQuery(res, &myopt, pset.queryFout);
	}

	PQclear(res);
	return true;
}


/*
 * \dD
 *
 * Describes domains.
 */
bool
listDomains(const char *pattern)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					  "       t.typname as \"%s\",\n"
					  "       pg_catalog.format_type(t.typbasetype, t.typtypmod) as \"%s\",\n"
					  "       CASE WHEN t.typnotnull AND t.typdefault IS NOT NULL THEN 'not null default '||t.typdefault\n"
					  "            WHEN t.typnotnull AND t.typdefault IS NULL THEN 'not null'\n"
					  "            WHEN NOT t.typnotnull AND t.typdefault IS NOT NULL THEN 'default '||t.typdefault\n"
					  "            ELSE ''\n"
					  "       END as \"%s\"\n"
					  "FROM pg_catalog.pg_type t\n"
	"     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = t.typnamespace\n"
					  "WHERE t.typtype = 'd'\n",
					  _("Schema"),
					  _("Name"),
					  _("Type"),
					  _("Modifier"));

	processNamePattern(&buf, pattern, true, false,
					   "n.nspname", "t.typname", NULL,
					   "pg_catalog.pg_type_is_visible(t.oid)");

	appendPQExpBuffer(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data, false);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of domains");

	printQuery(res, &myopt, pset.queryFout);

	PQclear(res);
	return true;
}

/*
 * \dc
 *
 * Describes conversions.
 */
bool
listConversions(const char *pattern)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT n.nspname AS \"%s\",\n"
					  "       c.conname AS \"%s\",\n"
	"       pg_catalog.pg_encoding_to_char(c.conforencoding) AS \"%s\",\n"
	"       pg_catalog.pg_encoding_to_char(c.contoencoding) AS \"%s\",\n"
					  "       CASE WHEN c.condefault THEN '%s'\n"
					  "       ELSE '%s' END AS \"%s\"\n"
		   "FROM pg_catalog.pg_conversion c, pg_catalog.pg_namespace n\n"
					  "WHERE n.oid = c.connamespace\n",
					  _("Schema"),
					  _("Name"),
					  _("Source"),
					  _("Destination"),
					  _("yes"),
					  _("no"),
					  _("Default?"));

	processNamePattern(&buf, pattern, true, false,
					   "n.nspname", "c.conname", NULL,
					   "pg_catalog.pg_conversion_is_visible(c.oid)");

	appendPQExpBuffer(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data, false);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of conversions");

	printQuery(res, &myopt, pset.queryFout);

	PQclear(res);
	return true;
}

/*
 * \dC
 *
 * Describes casts.
 */
bool
listCasts(const char *pattern)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);
/* NEED LEFT JOIN FOR BINARY CASTS */
	printfPQExpBuffer(&buf,
		   "SELECT pg_catalog.format_type(castsource, NULL) AS \"%s\",\n"
		   "       pg_catalog.format_type(casttarget, NULL) AS \"%s\",\n"
					  "       CASE WHEN castfunc = 0 THEN '%s'\n"
					  "            ELSE p.proname\n"
					  "       END as \"%s\",\n"
					  "       CASE WHEN c.castcontext = 'e' THEN '%s'\n"
					  "            WHEN c.castcontext = 'a' THEN '%s'\n"
					  "            ELSE '%s'\n"
					  "       END as \"%s\"\n"
			 "FROM pg_catalog.pg_cast c LEFT JOIN pg_catalog.pg_proc p\n"
					  "     ON c.castfunc = p.oid\n"
					  "ORDER BY 1, 2",
					  _("Source type"),
					  _("Target type"),
					  _("(binary compatible)"),
					  _("Function"),
					  _("no"),
					  _("in assignment"),
					  _("yes"),
					  _("Implicit?"));

	res = PSQLexec(buf.data, false);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of casts");

	printQuery(res, &myopt, pset.queryFout);

	PQclear(res);
	return true;
}

/*
 * \dn
 *
 * Describes schemas (namespaces)
 */
bool
listSchemas(const char *pattern)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT n.nspname AS \"%s\",\n"
					  "       u.usename AS \"%s\"\n"
		"FROM pg_catalog.pg_namespace n LEFT JOIN pg_catalog.pg_user u\n"
					  "       ON n.nspowner=u.usesysid\n",
					  _("Name"),
					  _("Owner"));

	processNamePattern(&buf, pattern, false, false,
					   NULL, "n.nspname", NULL,
					   NULL);

	appendPQExpBuffer(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data, false);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of schemas");

	printQuery(res, &myopt, pset.queryFout);

	PQclear(res);
	return true;
}


/*
 * processNamePattern
 *
 * Scan a wildcard-pattern option and generate appropriate WHERE clauses
 * to limit the set of objects returned.  The WHERE clauses are appended
 * to buf.
 *
 * pattern: user-specified pattern option to a \d command, or NULL if none.
 * have_where: true if caller already emitted WHERE.
 * force_escape: always quote regexp special characters, even outside quotes.
 * schemavar: name of WHERE variable to match against a schema-name pattern.
 * Can be NULL if no schema.
 * namevar: name of WHERE variable to match against an object-name pattern.
 * altnamevar: NULL, or name of an alternate variable to match against name.
 * visibilityrule: clause to use if we want to restrict to visible objects
 * (for example, "pg_catalog.pg_table_is_visible(p.oid)").	Can be NULL.
 */
static void
processNamePattern(PQExpBuffer buf, const char *pattern,
				   bool have_where, bool force_escape,
				   const char *schemavar, const char *namevar,
				   const char *altnamevar, const char *visibilityrule)
{
	PQExpBufferData schemabuf;
	PQExpBufferData namebuf;
	bool		inquotes;
	const char *cp;
	int			i;

#define WHEREAND() \
	(appendPQExpBuffer(buf, have_where ? "      AND " : "WHERE "), have_where = true)

	if (pattern == NULL)
	{
		/* Default: select all visible objects */
		if (visibilityrule)
		{
			WHEREAND();
			appendPQExpBuffer(buf, "%s\n", visibilityrule);
		}
		return;
	}

	initPQExpBuffer(&schemabuf);
	initPQExpBuffer(&namebuf);

	/*
	 * Parse the pattern, converting quotes and lower-casing unquoted
	 * letters; we assume this was NOT done by scan_option.  Also, adjust
	 * shell-style wildcard characters into regexp notation.
	 */
	inquotes = false;
	cp = pattern;

	while (*cp)
	{
		if (*cp == '"')
		{
			if (inquotes && cp[1] == '"')
			{
				/* emit one quote */
				appendPQExpBufferChar(&namebuf, '"');
				cp++;
			}
			inquotes = !inquotes;
			cp++;
		}
		else if (!inquotes && isupper((unsigned char) *cp))
		{
			appendPQExpBufferChar(&namebuf,
								  tolower((unsigned char) *cp));
			cp++;
		}
		else if (!inquotes && *cp == '*')
		{
			appendPQExpBuffer(&namebuf, ".*");
			cp++;
		}
		else if (!inquotes && *cp == '?')
		{
			appendPQExpBufferChar(&namebuf, '.');
			cp++;
		}
		else if (!inquotes && *cp == '.')
		{
			/* Found schema/name separator, move current pattern to schema */
			resetPQExpBuffer(&schemabuf);
			appendPQExpBufferStr(&schemabuf, namebuf.data);
			resetPQExpBuffer(&namebuf);
			cp++;
		}
		else
		{
			/*
			 * Ordinary data character, transfer to pattern
			 *
			 * Inside double quotes, or at all times if parsing an operator
			 * name, quote regexp special characters with a backslash to
			 * avoid regexp errors.  Outside quotes, however, let them
			 * pass through as-is; this lets knowledgeable users build
			 * regexp expressions that are more powerful than shell-style
			 * patterns.
			 */
			if ((inquotes || force_escape) &&
				strchr("|*+?()[]{}.^$\\", *cp))
				appendPQExpBuffer(&namebuf, "\\\\");

			/* Ensure chars special to string literals are passed properly */
			if (*cp == '\'' || *cp == '\\')
				appendPQExpBufferChar(&namebuf, *cp);

			i = PQmblen(cp, pset.encoding);
			while (i--)
			{
				appendPQExpBufferChar(&namebuf, *cp);
				cp++;
			}
		}
	}

	/*
	 * Now decide what we need to emit.
	 */
	if (schemabuf.len > 0)
	{
		/* We have a schema pattern, so constrain the schemavar */

		appendPQExpBufferChar(&schemabuf, '$');
		/* Optimize away ".*$", and possibly the whole pattern */
		if (schemabuf.len >= 3 &&
			strcmp(schemabuf.data + (schemabuf.len - 3), ".*$") == 0)
			schemabuf.data[schemabuf.len - 3] = '\0';

		if (schemabuf.data[0] && schemavar)
		{
			WHEREAND();
			appendPQExpBuffer(buf, "%s ~ '^%s'\n",
							  schemavar, schemabuf.data);
		}
	}
	else
	{
		/* No schema pattern given, so select only visible objects */
		if (visibilityrule)
		{
			WHEREAND();
			appendPQExpBuffer(buf, "%s\n", visibilityrule);
		}
	}

	if (namebuf.len > 0)
	{
		/* We have a name pattern, so constrain the namevar(s) */

		appendPQExpBufferChar(&namebuf, '$');
		/* Optimize away ".*$", and possibly the whole pattern */
		if (namebuf.len >= 3 &&
			strcmp(namebuf.data + (namebuf.len - 3), ".*$") == 0)
			namebuf.data[namebuf.len - 3] = '\0';

		if (namebuf.data[0])
		{
			WHEREAND();
			if (altnamevar)
				appendPQExpBuffer(buf,
								  "(%s ~ '^%s'\n"
								  "        OR %s ~ '^%s')\n",
								  namevar, namebuf.data,
								  altnamevar, namebuf.data);
			else
				appendPQExpBuffer(buf,
								  "%s ~ '^%s'\n",
								  namevar, namebuf.data);
		}
	}

	termPQExpBuffer(&schemabuf);
	termPQExpBuffer(&namebuf);

#undef WHEREAND
}
