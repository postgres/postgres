/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright 2000 by PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/describe.c,v 1.54 2002/05/13 17:45:30 tgl Exp $
 */
#include "postgres_fe.h"
#include "describe.h"

#include "libpq-fe.h"
#include "pqexpbuffer.h"

#include "common.h"
#include "settings.h"
#include "print.h"
#include "variables.h"

#define _(x) gettext((x))


/*----------------
 * Handlers for various slash commands displaying some sort of list
 * of things in the database.
 *
 * If you add something here, try to format the query to look nice in -E output.
 *----------------
 */


/* \da
 * takes an optional regexp to match specific aggregates by name
 */
bool
describeAggregates(const char *name)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	/*
	 * There are two kinds of aggregates: ones that work on particular
	 * types and ones that work on all (denoted by input type = 0)
	 */
	printfPQExpBuffer(&buf,
			 "SELECT p.proname AS \"%s\",\n"
			 "  CASE p.proargtypes[0]\n"
			 "    WHEN 0 THEN CAST('%s' AS text)\n"
			 "    ELSE format_type(p.proargtypes[0], NULL)\n"
			 "  END AS \"%s\",\n"
			 "  obj_description(p.oid, 'pg_proc') as \"%s\"\n"
			 "FROM pg_proc p\n"
			 "WHERE p.proisagg\n",
			 _("Name"), _("(all types)"),
			 _("Data type"), _("Description"));

	if (name)
		appendPQExpBuffer(&buf, "  AND p.proname ~ '^%s'\n", name);

	appendPQExpBuffer(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
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
 * Takes an optional regexp to narrow down the function name
 */
bool
describeFunctions(const char *name, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	/*
	 * we skip in/out funcs by excluding functions that take some
	 * arguments, but have no types defined for those arguments
	 */
	printfPQExpBuffer(&buf,
			 "SELECT format_type(p.prorettype, NULL) as \"%s\",\n"
			 "  p.proname as \"%s\",\n"
			 "  oidvectortypes(p.proargtypes) as \"%s\"",
			 _("Result data type"), _("Name"),
			 _("Argument data types"));

	if (verbose)
		appendPQExpBuffer(&buf,
				 ",\n  u.usename as \"%s\",\n"
				 "  l.lanname as \"%s\",\n"
				 "  p.prosrc as \"%s\",\n"
				 "  obj_description(p.oid, 'pg_proc') as \"%s\"",
				 _("Owner"), _("Language"),
				 _("Source code"), _("Description"));

	if (!verbose)
		appendPQExpBuffer(&buf,
			   "\nFROM pg_proc p\n"
			   "WHERE p.prorettype <> 0 AND (pronargs = 0 OR oidvectortypes(p.proargtypes) <> '') AND NOT p.proisagg\n");
	else
		appendPQExpBuffer(&buf,
			   "\nFROM pg_proc p,  pg_language l, pg_user u\n"
			   "WHERE p.prolang = l.oid AND p.proowner = u.usesysid\n"
			   "  AND p.prorettype <> 0 AND (pronargs = 0 OR oidvectortypes(p.proargtypes) <> '') AND NOT p.proisagg\n");

	if (name)
		appendPQExpBuffer(&buf, "  AND p.proname ~ '^%s'\n", name);
	appendPQExpBuffer(&buf, "ORDER BY 2, 1, 3;");

	res = PSQLexec(buf.data);
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
describeTypes(const char *name, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
			 "SELECT format_type(t.oid, NULL) AS \"%s\",\n",
			 _("Name"));
	if (verbose)
		appendPQExpBuffer(&buf,
				 "  t.typname AS \"%s\",\n"
				 "  CASE WHEN t.typlen = -1\n"
				 "    THEN CAST('var' AS text)\n"
				 "    ELSE CAST(t.typlen AS text)\n"
				 "  END AS \"%s\",\n",
				 _("Internal name"), _("Size"));
	appendPQExpBuffer(&buf,
			 "  obj_description(t.oid, 'pg_type') as \"%s\"\n",
			 _("Description"));

	/*
	 * do not include array types (start with underscore), do not include
	 * user relations (typrelid!=0)
	 */
	appendPQExpBuffer(&buf, "FROM pg_type t\nWHERE t.typrelid = 0 AND t.typname !~ '^_.*'\n");

	if (name)
		/* accept either internal or external type name */
		appendPQExpBuffer(&buf, "  AND (format_type(t.oid, NULL) ~ '^%s' OR t.typname ~ '^%s')\n", name, name);

	appendPQExpBuffer(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
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
describeOperators(const char *name)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
			 "SELECT o.oprname AS \"%s\",\n"
			 "  CASE WHEN o.oprkind='l' THEN NULL ELSE format_type(o.oprleft, NULL) END AS \"%s\",\n"
			 "  CASE WHEN o.oprkind='r' THEN NULL ELSE format_type(o.oprright, NULL) END AS \"%s\",\n"
			 "  format_type(o.oprresult, NULL) AS \"%s\",\n"
			 "  coalesce(obj_description(o.oid, 'pg_operator'),"
			 "           obj_description(o.oprcode, 'pg_proc')) AS \"%s\"\n"
			 "FROM pg_operator o\n",
			 _("Name"), _("Left arg type"), _("Right arg type"),
			 _("Result type"), _("Description"));
	if (name)
		appendPQExpBuffer(&buf, "WHERE o.oprname = '%s'\n", name);

	appendPQExpBuffer(&buf, "ORDER BY 1, 2, 3, 4;");

	res = PSQLexec(buf.data);
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
listAllDbs(bool desc)
{
	PGresult   *res;
	PQExpBufferData buf;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
			 "SELECT d.datname as \"%s\",\n"
			 "       u.usename as \"%s\"",
			 _("Name"), _("Owner"));
#ifdef MULTIBYTE
	appendPQExpBuffer(&buf,
			 ",\n       pg_encoding_to_char(d.encoding) as \"%s\"",
			 _("Encoding"));
#endif
	if (desc)
		appendPQExpBuffer(&buf,
			 ",\n       obj_description(d.oid, 'pg_database') as \"%s\"",
				 _("Description"));
	appendPQExpBuffer(&buf,
	"\nFROM pg_database d LEFT JOIN pg_user u ON d.datdba = u.usesysid\n"
		   "ORDER BY 1;");

	res = PSQLexec(buf.data);
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
permissionsList(const char *name)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	/* Currently, we ignore indexes since they have no meaningful rights */
	printfPQExpBuffer(&buf,
			 "SELECT relname as \"%s\",\n"
			 "       relacl as \"%s\"\n"
			 "FROM   pg_class\n"
			 "WHERE  relkind in ('r', 'v', 'S') AND\n"
			 "       relname NOT LIKE 'pg$_%%' ESCAPE '$'\n",
			 _("Table"), _("Access privileges"));
	if (name)
		appendPQExpBuffer(&buf, "  AND relname ~ '^%s'\n", name);
	appendPQExpBuffer(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
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
objectDescription(const char *object)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
			 "SELECT DISTINCT tt.name AS \"%s\", tt.object AS \"%s\", d.description AS \"%s\"\n"
			 "FROM (\n"

	/* Aggregate descriptions */
			 "  SELECT p.oid as oid, p.tableoid as tableoid,\n"
	  "  CAST(p.proname AS text) as name, CAST('%s' AS text) as object\n"
			 "  FROM pg_proc p\n"
		"  WHERE p.proisagg\n"

	/* Function descriptions (except in/outs for datatypes) */
			 "UNION ALL\n"
			 "  SELECT p.oid as oid, p.tableoid as tableoid,\n"
	  "  CAST(p.proname AS text) as name, CAST('%s' AS text) as object\n"
			 "  FROM pg_proc p\n"
		"  WHERE (p.pronargs = 0 or oidvectortypes(p.proargtypes) <> '') AND NOT p.proisagg\n"

	/* Operator descriptions (only if operator has its own comment) */
			 "UNION ALL\n"
			 "  SELECT o.oid as oid, o.tableoid as tableoid,\n"
	  "  CAST(o.oprname AS text) as name, CAST('%s' AS text) as object\n"
			 "  FROM pg_operator o\n"

	/* Type description */
			 "UNION ALL\n"
			 "  SELECT t.oid as oid, t.tableoid as tableoid,\n"
	 "  format_type(t.oid, NULL) as name, CAST('%s' AS text) as object\n"
			 "  FROM pg_type t\n"

	/* Relation (tables, views, indexes, sequences) descriptions */
			 "UNION ALL\n"
			 "  SELECT c.oid as oid, c.tableoid as tableoid,\n"
			 "  CAST(c.relname AS text) as name,\n"
			 "  CAST(\n"
			 "    CASE c.relkind WHEN 'r' THEN '%s' WHEN 'v' THEN '%s' WHEN 'i' THEN '%s' WHEN 'S' THEN '%s' END"
			 "  AS text) as object\n"
			 "  FROM pg_class c\n"

	/* Rule description (ignore rules for views) */
			 "UNION ALL\n"
			 "  SELECT r.oid as oid, r.tableoid as tableoid,\n"
			 "  CAST(r.rulename AS text) as name, CAST('%s' AS text) as object\n"
			 "  FROM pg_rewrite r\n"
			 "  WHERE r.rulename != '_RETURN'\n"

	/* Trigger description */
			 "UNION ALL\n"
			 "  SELECT t.oid as oid, t.tableoid as tableoid,\n"
	   "  CAST(t.tgname AS text) as name, CAST('%s' AS text) as object\n"
			 "  FROM pg_trigger t\n"

			 ") AS tt,\n"
			 "pg_description d\n"
			 "WHERE tt.oid = d.objoid and tt.tableoid = d.classoid and d.objsubid = 0\n",

			 _("Name"), _("Object"), _("Description"),
			 _("aggregate"), _("function"), _("operator"),
			 _("data type"), _("table"), _("view"),
			 _("index"), _("sequence"), _("rule"),
			 _("trigger")
		);

	if (object)
		appendPQExpBuffer(&buf, "  AND tt.name ~ '^%s'\n", object);
	appendPQExpBuffer(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
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
 * Unfortunately, the information presented here is so complicated that it cannot
 * be done in a single query. So we have to assemble the printed table by hand
 * and pass it to the underlying printTable() function.
 *
 */

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


bool
describeTableDetails(const char *name, bool desc)
{
	PQExpBufferData buf;
	PGresult   *res = NULL;
	printTableOpt myopt = pset.popt.topt;
	int			i;
	const char *view_def = NULL;
	const char *headers[5];
	char	  **cells = NULL;
	char	  **footers = NULL;
	char	  **ptr;
	PQExpBufferData title;
	unsigned int cols = 0;
	struct
	{
		bool		hasindex;
		char		relkind;
		int16		checks;
		int16		triggers;
		bool		hasrules;
	}			tableinfo;
	bool		retval;

	retval = false;

	initPQExpBuffer(&buf);
	initPQExpBuffer(&title);

	/* Get general table info */
	printfPQExpBuffer(&buf,
					  "SELECT relhasindex, relkind, relchecks, reltriggers, relhasrules\n"
					  "FROM pg_class WHERE relname='%s'",
					  name);
	res = PSQLexec(buf.data);
	if (!res)
		goto error_return;

	/* Did we get anything? */
	if (PQntuples(res) == 0)
	{
		if (!QUIET())
			fprintf(stderr, _("Did not find any relation named \"%s\".\n"), name);
		PQclear(res);
		res = NULL;
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
		cols++;
		headers[cols - 1] = _("Modifiers");
	}

	if (desc)
	{
		cols++;
		headers[cols - 1] = _("Description");
	}

	headers[cols] = NULL;


	/* Get column info (index requires additional checks) */
	if (tableinfo.relkind == 'i')
		printfPQExpBuffer(&buf, "SELECT\n  CASE i.indproc WHEN ('-'::regproc) THEN a.attname\n  ELSE SUBSTR(pg_get_indexdef(attrelid),\n  POSITION('(' in pg_get_indexdef(attrelid)))\n  END, ");
	else
		printfPQExpBuffer(&buf, "SELECT a.attname, ");
	appendPQExpBuffer(&buf, "format_type(a.atttypid, a.atttypmod), a.attnotnull, a.atthasdef, a.attnum");
	if (desc)
		appendPQExpBuffer(&buf, ", col_description(a.attrelid, a.attnum)");
	appendPQExpBuffer(&buf, "\nFROM pg_class c, pg_attribute a");
	if (tableinfo.relkind == 'i')
		appendPQExpBuffer(&buf, ", pg_index i");
	appendPQExpBuffer(&buf, "\nWHERE c.relname = '%s'\n  AND a.attnum > 0 AND a.attrelid = c.oid", name);
	if (tableinfo.relkind == 'i')
		appendPQExpBuffer(&buf, " AND a.attrelid = i.indexrelid");
	appendPQExpBuffer(&buf, "\nORDER BY a.attnum");

	res = PSQLexec(buf.data);
	if (!res)
		goto error_return;

	/* Check if table is a view */
	if (tableinfo.hasrules)
	{
		PGresult   *result;

		printfPQExpBuffer(&buf, "SELECT definition FROM pg_views WHERE viewname = '%s'", name);
		result = PSQLexec(buf.data);
		if (!result)
		{
			PQclear(res);
			PQclear(result);
			goto error_return;
		}

		if (PQntuples(result) > 0)
			view_def = xstrdup(PQgetvalue(result, 0, 0));
		PQclear(result);
	}


	/* Generate table cells to be printed */
	cells = xmalloc((PQntuples(res) * cols + 1) * sizeof(*cells));
	cells[PQntuples(res) * cols] = NULL;		/* end of list */

	for (i = 0; i < PQntuples(res); i++)
	{
		/* Name */
		cells[i * cols + 0] = PQgetvalue(res, i, 0);	/* don't free this
														 * afterwards */
		/* Type */
		cells[i * cols + 1] = PQgetvalue(res, i, 1);	/* don't free this
														 * either */

		/* Extra: not null and default */
		/* (I'm cutting off the 'default' string at 128) */
		if (tableinfo.relkind == 'r' || tableinfo.relkind == 'v')
		{
			cells[i * cols + 2] = xmalloc(128 + 128);
			cells[i * cols + 2][0] = '\0';
			if (strcmp(PQgetvalue(res, i, 2), "t") == 0)
				strcat(cells[i * cols + 2], "not null");

			/* handle "default" here */
			if (strcmp(PQgetvalue(res, i, 3), "t") == 0)
			{
				PGresult   *result;

				printfPQExpBuffer(&buf,
								  "SELECT substring(d.adsrc for 128) FROM pg_attrdef d, pg_class c\n"
								  "WHERE c.relname = '%s' AND c.oid = d.adrelid AND d.adnum = %s",
								  name, PQgetvalue(res, i, 4));

				result = PSQLexec(buf.data);

				if (cells[i * cols + 2][0])
					strcat(cells[i * cols + 2], " ");
				strcat(cells[i * cols + 2], "default ");
				strcat(cells[i * cols + 2], result ? PQgetvalue(result, 0, 0) : "?");

				PQclear(result);
			}
		}

		/* Description */
		if (desc)
			cells[i * cols + cols - 1] = PQgetvalue(res, i, 5);
	}

	/* Make title */
	switch (tableinfo.relkind)
	{
		case 'r':
			printfPQExpBuffer(&title, _("Table \"%s\""), name);
			break;
		case 'v':
			printfPQExpBuffer(&title, _("View \"%s\""), name);
			break;
		case 'S':
			printfPQExpBuffer(&title, _("Sequence \"%s\""), name);
			break;
		case 'i':
			printfPQExpBuffer(&title, _("Index \"%s\""), name);
			break;
		case 's':
			printfPQExpBuffer(&title, _("Special relation \"%s\""), name);
			break;
		case 't':
			printfPQExpBuffer(&title, _("TOAST table \"%s\""), name);
			break;
		default:
			printfPQExpBuffer(&title, _("?%c? \"%s\""), tableinfo.relkind, name);
			break;
	}

	/* Make footers */
	if (tableinfo.relkind == 'i')
	{
		/* Footer information about an index */
		PGresult   *result;
		printfPQExpBuffer(&buf,
						  "SELECT i.indisunique, i.indisprimary, a.amname, c2.relname,\n"
						  "pg_get_expr(i.indpred,i.indrelid)\n"
						  "FROM pg_index i, pg_class c, pg_class c2, pg_am a\n"
						  "WHERE i.indexrelid = c.oid AND c.relname = '%s' AND c.relam = a.oid\n"
						  "AND i.indrelid = c2.oid",
						  name);

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
			PQExpBufferData tmpbuf;
			char	   *indisunique = PQgetvalue(result, 0, 0);
			char	   *indisprimary = PQgetvalue(result, 0, 1);
			char	   *indamname = PQgetvalue(result, 0, 2);
			char	   *indtable = PQgetvalue(result, 0, 3);
			char	   *indpred = PQgetvalue(result, 0, 4);

			initPQExpBuffer(&tmpbuf);

			if (strcmp(indisprimary, "t") == 0)
				printfPQExpBuffer(&tmpbuf, _("primary key, "));
			else if (strcmp(indisunique, "t") == 0)
				printfPQExpBuffer(&tmpbuf, _("unique, "));
			else
				resetPQExpBuffer(&tmpbuf);
			appendPQExpBuffer(&tmpbuf, "%s, ", indamname);

			appendPQExpBuffer(&tmpbuf, _("for table \"%s\""), indtable);
			if (strlen(indpred))
				appendPQExpBuffer(&tmpbuf, ", predicate %s", indpred);

			footers = xmalloc(2 * sizeof(*footers));
			footers[0] = xstrdup(tmpbuf.data);
			footers[1] = NULL;
			termPQExpBuffer(&tmpbuf);
		}

		PQclear(result);
	}
	else if (view_def)
	{
		PGresult   *result = NULL;
		int			rule_count = 0;
		int			count_footers = 0;

		/* count rules */
		if (tableinfo.hasrules)
		{
			printfPQExpBuffer(&buf,
					"SELECT r.rulename\n"
					"FROM pg_rewrite r, pg_class c\n"
					"WHERE c.relname = '%s' AND c.oid = r.ev_class\n"
					"AND r.rulename != '_RETURN'",
					name);
			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else
				rule_count = PQntuples(result);
		}

		/* Footer information about a view */
		footers = xmalloc((rule_count + 2) * sizeof(*footers));
		footers[count_footers] = xmalloc(64 + strlen(view_def));
		snprintf(footers[count_footers], 64 + strlen(view_def),
				 _("View definition: %s"), view_def);
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
				   *result4 = NULL;
		int			index_count = 0,
					constr_count = 0,
					rule_count = 0,
					trigger_count = 0;
		int			count_footers = 0;

		/* count indexes */
		if (tableinfo.hasindex)
		{
			printfPQExpBuffer(&buf,
					"SELECT c2.relname, i.indisprimary, i.indisunique,\n"
					"SUBSTR(pg_get_indexdef(i.indexrelid),\n"
					"POSITION('USING ' IN pg_get_indexdef(i.indexrelid))+5)\n"
					"FROM pg_class c, pg_class c2, pg_index i\n"
					"WHERE c.relname = '%s' AND c.oid = i.indrelid AND i.indexrelid = c2.oid\n"
					"ORDER BY i.indisprimary DESC, i.indisunique DESC, c2.relname",
					name);
			result1 = PSQLexec(buf.data);
			if (!result1)
				goto error_return;
			else
				index_count = PQntuples(result1);
		}

		/* count table (and column) constraints */
		if (tableinfo.checks)
		{
			printfPQExpBuffer(&buf,
					"SELECT rcsrc, rcname\n"
					"FROM pg_relcheck r, pg_class c\n"
					"WHERE c.relname='%s' AND c.oid = r.rcrelid",
					name);
			result2 = PSQLexec(buf.data);
			if (!result2)
				goto error_return;
			else
				constr_count = PQntuples(result2);
		}

		/* count rules */
		if (tableinfo.hasrules)
		{
			printfPQExpBuffer(&buf,
					"SELECT r.rulename\n"
					"FROM pg_rewrite r, pg_class c\n"
					"WHERE c.relname='%s' AND c.oid = r.ev_class",
					name);
			result3 = PSQLexec(buf.data);
			if (!result3)
				goto error_return;
			else
				rule_count = PQntuples(result3);
		}

		/* count triggers */
		if (tableinfo.triggers)
		{
			printfPQExpBuffer(&buf,
					"SELECT t.tgname\n"
					"FROM pg_trigger t, pg_class c\n"
					"WHERE c.relname='%s' AND c.oid = t.tgrelid",
					name);
			result4 = PSQLexec(buf.data);
			if (!result4)
				goto error_return;
			else
				trigger_count = PQntuples(result4);
		}

		footers = xmalloc((index_count + constr_count + rule_count + trigger_count + 1)
						  * sizeof(*footers));

		/* print indexes */
		for (i = 0; i < index_count; i++)
		{
			char	   *s = _("Indexes");
	
			if (i == 0)
				printfPQExpBuffer(&buf, "%s: %s", s, PQgetvalue(result1, i, 0));
			else
				printfPQExpBuffer(&buf, "%*s  %s", (int) strlen(s), "", PQgetvalue(result1, i, 0));

			/* Label as primary key or unique (but not both) */
			appendPQExpBuffer(&buf,
							  strcmp(PQgetvalue(result1,i,1),"t") == 0
							  ? _(" primary key") :
							  (strcmp(PQgetvalue(result1,i,2),"t") == 0
							   ? _(" unique")
							   : ""));

			/* Everything after "USING" is echoed verbatim */
			appendPQExpBuffer(&buf, "%s", PQgetvalue(result1,i,3));

			if (i < index_count - 1)
				appendPQExpBuffer(&buf, ",");

			footers[count_footers++] = xstrdup(buf.data);
		}


		/* print constraints */
		for (i = 0; i < constr_count; i++)
		{
			char	   *s = _("Check constraints");

			if (i == 0)
				printfPQExpBuffer(&buf, _("%s: \"%s\" %s"),
								  s,
								  PQgetvalue(result2, i, 1),
								  PQgetvalue(result2, i, 0));
			else
				printfPQExpBuffer(&buf, _("%*s  \"%s\" %s"),
								  (int) strlen(s), "",
								  PQgetvalue(result2, i, 1),
								  PQgetvalue(result2, i, 0));
			footers[count_footers++] = xstrdup(buf.data);
		}

		/* print rules */
		for (i = 0; i < rule_count; i++)
		{
			char	   *s = _("Rules");

			if (i == 0)
				printfPQExpBuffer(&buf, "%s: %s", s, PQgetvalue(result3, i, 0));
			else
				printfPQExpBuffer(&buf, "%*s  %s", (int) strlen(s), "", PQgetvalue(result3, i, 0));
			if (i < rule_count - 1)
				appendPQExpBuffer(&buf, ",");

			footers[count_footers++] = xstrdup(buf.data);
		}

		/* print triggers */
		for (i = 0; i < trigger_count; i++)
		{
			char	   *s = _("Triggers");

			if (i == 0)
				printfPQExpBuffer(&buf, "%s: %s", s, PQgetvalue(result4, i, 0));
			else
				printfPQExpBuffer(&buf, "%*s  %s", (int) strlen(s), "", PQgetvalue(result4, i, 0));
			if (i < trigger_count - 1)
				appendPQExpBuffer(&buf, ",");

			footers[count_footers++] = xstrdup(buf.data);
		}

		/* end of list marker */
		footers[count_footers] = NULL;

		PQclear(result1);
		PQclear(result2);
		PQclear(result3);
		PQclear(result4);
	}

	printTable(title.data, headers,
			   (const char **) cells, (const char **) footers,
			   "llll", &myopt, pset.queryFout);

	retval = true;

error_return:

	/* clean up */
	termPQExpBuffer(&buf);
	termPQExpBuffer(&title);

	if (cells)
	{
		for (i = 0; i < PQntuples(res); i++)
			if (tableinfo.relkind == 'r' || tableinfo.relkind == 'v')
				free(cells[i * cols + 2]);
		free(cells);
	}

	if (footers)
	{
		for (ptr = footers; *ptr; ptr++)
			free(*ptr);
		free(footers);
	}

	if (res)
		PQclear(res);

	return retval;
}


/*
 * \du [user]
 *
 * Describes users, possibly based on a simplistic prefix search on the
 * argument.
 */
bool
describeUsers(const char *name)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
			 "SELECT u.usename AS \"%s\",\n"
			 "  u.usesysid AS \"%s\",\n"
			 "  CASE WHEN u.usesuper AND u.usecreatedb THEN CAST('%s' AS text)\n"
			 "       WHEN u.usesuper THEN CAST('%s' AS text)\n"
			 "       WHEN u.usecreatedb THEN CAST('%s' AS text)\n"
			 "       ELSE CAST('' AS text)\n"
			 "  END AS \"%s\"\n"
			 "FROM pg_user u\n",
			 _("User name"), _("User ID"),
			 _("superuser, create database"),
			 _("superuser"), _("create database"),
			 _("Attributes"));
	if (name)
		appendPQExpBuffer(&buf, "WHERE u.usename ~ '^%s'\n", name);
	appendPQExpBuffer(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
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
 * The infotype is an array of characters, specifying what info is desired:
 * t - tables
 * i - indexes
 * v - views
 * s - sequences
 * S - systems tables (~ '^pg_')
 * (any order of the above is fine)
 *
 * Note: For some reason it always happens to people that their tables have owners
 * that are no longer in pg_user; consequently they wouldn't show up here. The code
 * tries to fix this the painful way, hopefully outer joins will be done sometime.
 */
bool
listTables(const char *infotype, const char *name, bool desc)
{
	bool		showTables = strchr(infotype, 't') != NULL;
	bool		showIndexes = strchr(infotype, 'i') != NULL;
	bool		showViews = strchr(infotype, 'v') != NULL;
	bool		showSeq = strchr(infotype, 's') != NULL;
	bool		showSystem = strchr(infotype, 'S') != NULL;

	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	if (showSystem && !(showSeq || showIndexes || showViews || showTables))
		showTables = showViews = showSeq = true;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
			 "SELECT c.relname as \"%s\",\n"
			 "  CASE c.relkind WHEN 'r' THEN '%s' WHEN 'v' THEN '%s' WHEN 'i' THEN '%s' WHEN 'S' THEN '%s' WHEN 's' THEN '%s' END as \"%s\",\n"
			 "  u.usename as \"%s\"",
			 _("Name"), _("table"), _("view"), _("index"), _("sequence"),
			 _("special"), _("Type"), _("Owner"));

	if (desc)
		appendPQExpBuffer(&buf,
				 ",\n  obj_description(c.oid, 'pg_class') as \"%s\"",
				 _("Description"));
    if (showIndexes)
		appendPQExpBuffer(&buf,
						  ",\n c2.relname as \"%s\""
						  "\nFROM pg_class c, pg_class c2, pg_index i, pg_user u\n"
						  "WHERE c.relowner = u.usesysid\n"
						  "AND i.indrelid = c2.oid AND i.indexrelid = c.oid\n",
						  _("Table"));
	else
		appendPQExpBuffer(&buf,
						  "\nFROM pg_class c, pg_user u\n"
						  "WHERE c.relowner = u.usesysid\n");

	appendPQExpBuffer(&buf, "AND c.relkind IN (");
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
	appendPQExpBuffer(&buf, "''");			/* dummy */
	appendPQExpBuffer(&buf, ")\n");

	if (showSystem)
		appendPQExpBuffer(&buf, "  AND c.relname ~ '^pg_'\n");
	else
		appendPQExpBuffer(&buf, "  AND c.relname !~ '^pg_'\n");

	if (name)
		appendPQExpBuffer(&buf, "  AND c.relname ~ '^%s'\n", name);

	appendPQExpBuffer(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	if (PQntuples(res) == 0 && !QUIET())
	{
		if (name)
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
 * \dD [domain]
 *
 * Describes domains, possibly based on a simplistic prefix search on the
 * argument.
 */
bool
listDomains(const char *name)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
		 "SELECT t.typname as \"%s\",\n"
		 "       format_type(t.typbasetype, t.typtypmod) as \"%s\",\n"
		 "       CASE WHEN t.typnotnull AND t.typdefault IS NOT NULL THEN 'not null default '||t.typdefault\n"
		 "            WHEN t.typnotnull AND t.typdefault IS NULL THEN 'not null'\n"
		 "            WHEN NOT t.typnotnull AND t.typdefault IS NOT NULL THEN 'default '||t.typdefault\n"
		 "            ELSE ''\n"
		 "       END as \"%s\"\n"
		 "FROM pg_type t\n"
		 "WHERE t.typtype = 'd'\n",
		 _("Name"),
		 _("Type"),
		 _("Modifier"));
	if (name)
		appendPQExpBuffer(&buf, "AND t.typname ~ '^%s'\n", name);
	appendPQExpBuffer(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of domains");

	printQuery(res, &myopt, pset.queryFout);

	PQclear(res);
	return true;
}
