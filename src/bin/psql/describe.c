#include <config.h>
#include <c.h>
#include "describe.h"

#include <string.h>

#include <postgres.h>			/* for VARHDRSZ, int4 type */
#include <postgres_ext.h>
#include <libpq-fe.h>

#include "common.h"
#include "settings.h"
#include "print.h"
#include "variables.h"


/*----------------
 * Handlers for various slash commands displaying some sort of list
 * of things in the database.
 *
 * If you add something here, consider this:
 * - If (and only if) the variable "description" is set, the description/
 *	 comment for the object should be displayed.
 * - Try to format the query to look nice in -E output.
 *----------------
 */

/* the maximal size of regular expression we'll accept here */
/* (it is save to just change this here) */
#define REGEXP_CUTOFF 10 * NAMEDATALEN


/* \da
 * takes an optional regexp to match specific aggregates by name
 */
bool
describeAggregates(const char *name, PsqlSettings *pset)
{
	char		descbuf[384 + 2 * REGEXP_CUTOFF];		/* observe/adjust this
														 * if you change the
														 * query */
	PGresult   *res;
	bool		description = GetVariableBool(pset->vars, "description");
	printQueryOpt myopt = pset->popt;

	descbuf[0] = '\0';

	/*
	 * There are two kinds of aggregates: ones that work on particular
	 * types ones that work on all
	 */
	strcat(descbuf,
		   "SELECT a.aggname AS \"Name\", t.typname AS \"Type\"");
	if (description)
		strcat(descbuf,
			   ",\n       obj_description(a.oid) as \"Description\"");
	strcat(descbuf,
		   "\nFROM pg_aggregate a, pg_type t\n"
		   "WHERE a.aggbasetype = t.oid\n");
	if (name)
	{
		strcat(descbuf, "  AND a.aggname ~* '^");
		strncat(descbuf, name, REGEXP_CUTOFF);
		strcat(descbuf, "'\n");
	}

	strcat(descbuf,
		   "UNION\n"
		   "SELECT a.aggname AS \"Name\", '(all types)' as \"Type\"");
	if (description)
		strcat(descbuf,
			   ",\n       obj_description(a.oid) as \"Description\"");
	strcat(descbuf,
		   "\nFROM pg_aggregate a\n"
		   "WHERE a.aggbasetype = 0\n");
	if (name)
	{
		strcat(descbuf, "  AND a.aggname ~* '^");
		strncat(descbuf, name, REGEXP_CUTOFF);
		strcat(descbuf, "'\n");
	}

	strcat(descbuf, "ORDER BY \"Name\", \"Type\"");

	res = PSQLexec(pset, descbuf);
	if (!res)
		return false;

	myopt.topt.tuples_only = false;
	myopt.nullPrint = NULL;
	myopt.title = "List of aggregates";

	printQuery(res, &myopt, pset->queryFout);

	PQclear(res);
	return true;
}


/* \df
 * takes an optional regexp to narrow down the function name
 */
bool
describeFunctions(const char *name, PsqlSettings *pset)
{
	char		descbuf[384 + REGEXP_CUTOFF];
	PGresult   *res;
	printQueryOpt myopt = pset->popt;

	/*
	 * we skip in/out funcs by excluding functions that take some
	 * arguments, but have no types defined for those arguments
	 */
	descbuf[0] = '\0';

	strcat(descbuf, "SELECT t.typname as \"Result\", p.proname as \"Function\",\n"
		   "       oid8types(p.proargtypes) as \"Arguments\"");
	if (GetVariableBool(pset->vars, "description"))
		strcat(descbuf, "\n,       obj_description(p.oid) as \"Description\"");
	strcat(descbuf, "\nFROM pg_proc p, pg_type t\n"
		   "WHERE p.prorettype = t.oid and (pronargs = 0 or oid8types(p.proargtypes) != '')\n");
	if (name)
	{
		strcat(descbuf, "  AND p.proname ~* '^");
		strncat(descbuf, name, REGEXP_CUTOFF);
		strcat(descbuf, "'\n");
	}
	strcat(descbuf, "ORDER BY \"Function\", \"Result\", \"Arguments\"");

	res = PSQLexec(pset, descbuf);
	if (!res)
		return false;

	myopt.topt.tuples_only = false;
	myopt.nullPrint = NULL;
	myopt.title = "List of functions";

	printQuery(res, &myopt, pset->queryFout);

	PQclear(res);
	return true;
}



/*
 * describeTypes
 *
 * for \dT
 */
bool
describeTypes(const char *name, PsqlSettings *pset)
{
	char		descbuf[256 + REGEXP_CUTOFF];
	PGresult   *res;
	printQueryOpt myopt = pset->popt;

	descbuf[0] = '\0';
	strcat(descbuf, "SELECT typname AS \"Type\"");
	if (GetVariableBool(pset->vars, "description"))
		strcat(descbuf, ", obj_description(oid) as \"Description\"");
	strcat(descbuf, "\nFROM pg_type\n"
		   "WHERE typrelid = 0 AND typname !~ '^_.*'\n");

	if (name)
	{
		strcat(descbuf, "  AND typname ~* '^");
		strncat(descbuf, name, REGEXP_CUTOFF);
		strcat(descbuf, "' ");
	}
	strcat(descbuf, "ORDER BY typname;");

	res = PSQLexec(pset, descbuf);
	if (!res)
		return false;

	myopt.topt.tuples_only = false;
	myopt.nullPrint = NULL;
	myopt.title = "List of types";

	printQuery(res, &myopt, pset->queryFout);

	PQclear(res);
	return true;
}



/* \do
 * NOTE: The (optional) argument here is _not_ a regexp since with all the
 * funny chars floating around that would probably confuse people. It's an
 * exact match string.
 */
bool
describeOperators(const char *name, PsqlSettings *pset)
{
	char		descbuf[1536 + 3 * 32]; /* 32 is max length for operator
										 * name */
	PGresult   *res;
	bool		description = GetVariableBool(pset->vars, "description");
	printQueryOpt myopt = pset->popt;

	descbuf[0] = '\0';

	strcat(descbuf, "SELECT o.oprname AS \"Op\",\n"
		   "       t1.typname AS \"Left arg\",\n"
		   "       t2.typname AS \"Right arg\",\n"
		   "       t0.typname AS \"Result\"");
	if (description)
		strcat(descbuf, ",\n       obj_description(p.oid) as \"Description\"");
	strcat(descbuf, "\nFROM   pg_proc p, pg_type t0,\n"
		   "       pg_type t1, pg_type t2,\n"
		   "       pg_operator o\n"
		   "WHERE  p.prorettype = t0.oid AND\n"
		   "       RegprocToOid(o.oprcode) = p.oid AND\n"
		   "       p.pronargs = 2 AND\n"
		   "       o.oprleft = t1.oid AND\n"
		   "       o.oprright = t2.oid\n");
	if (name)
	{
		strcat(descbuf, "  AND o.oprname = '");
		strncat(descbuf, name, 32);
		strcat(descbuf, "'\n");
	}

	strcat(descbuf, "\nUNION\n\n"
		   "SELECT o.oprname as \"Op\",\n"
		   "       ''::name AS \"Left arg\",\n"
		   "       t1.typname AS \"Right arg\",\n"
		   "       t0.typname AS \"Result\"");
	if (description)
		strcat(descbuf, ",\n       obj_description(p.oid) as \"Description\"");
	strcat(descbuf, "\nFROM   pg_operator o, pg_proc p, pg_type t0, pg_type t1\n"
		   "WHERE  RegprocToOid(o.oprcode) = p.oid AND\n"
		   "       o.oprresult = t0.oid AND\n"
		   "       o.oprkind = 'l' AND\n"
		   "       o.oprright = t1.oid\n");
	if (name)
	{
		strcat(descbuf, "AND o.oprname = '");
		strncat(descbuf, name, 32);
		strcat(descbuf, "'\n");
	}

	strcat(descbuf, "\nUNION\n\n"
		   "SELECT o.oprname  as \"Op\",\n"
		   "       t1.typname AS \"Left arg\",\n"
		   "       ''::name AS \"Right arg\",\n"
		   "       t0.typname AS \"Result\"");
	if (description)
		strcat(descbuf, ",\n       obj_description(p.oid) as \"Description\"");
	strcat(descbuf, "\nFROM   pg_operator o, pg_proc p, pg_type t0, pg_type t1\n"
		   "WHERE  RegprocToOid(o.oprcode) = p.oid AND\n"
		   "       o.oprresult = t0.oid AND\n"
		   "       o.oprkind = 'r' AND\n"
		   "       o.oprleft = t1.oid\n");
	if (name)
	{
		strcat(descbuf, "AND o.oprname = '");
		strncat(descbuf, name, 32);
		strcat(descbuf, "'\n");
	}
	strcat(descbuf, "\nORDER BY \"Op\", \"Left arg\", \"Right arg\", \"Result\"");

	res = PSQLexec(pset, descbuf);
	if (!res)
		return false;

	myopt.topt.tuples_only = false;
	myopt.nullPrint = NULL;
	myopt.title = "List of operators";

	printQuery(res, &myopt, pset->queryFout);

	PQclear(res);
	return true;
}


/*
 * listAllDbs
 *
 * for \l, \list, and -l switch
 */
bool
listAllDbs(PsqlSettings *pset)
{
	PGresult   *res;
	char		descbuf[256];
	printQueryOpt myopt = pset->popt;

	descbuf[0] = '\0';
	strcat(descbuf, "SELECT pg_database.datname as \"Database\",\n"
		   "       pg_user.usename as \"Owner\""
#ifdef MULTIBYTE
		   ",\n       pg_database.encoding as \"Encoding\""
#endif
		);
	if (GetVariableBool(pset->vars, "description"))
		strcat(descbuf, ",\n       obj_description(pg_database.oid) as \"Description\"\n");
	strcat(descbuf, "FROM pg_database, pg_user\n"
		   "WHERE pg_database.datdba = pg_user.usesysid\n"
		   "ORDER BY \"Database\"");

	res = PSQLexec(pset, descbuf);
	if (!res)
		return false;

	myopt.topt.tuples_only = false;
	myopt.nullPrint = NULL;
	myopt.title = "List of databases";

	printQuery(res, &myopt, pset->queryFout);

	PQclear(res);
	return true;
}


/* List Tables Grant/Revoke Permissions
 * \z (now also \dp -- perhaps more mnemonic)
 *
 */
bool
permissionsList(const char *name, PsqlSettings *pset)
{
	char		descbuf[256 + REGEXP_CUTOFF];
	PGresult   *res;
	printQueryOpt myopt = pset->popt;

	descbuf[0] = '\0';
	/* Currently, we ignore indexes since they have no meaningful rights */
	strcat(descbuf, "SELECT relname as \"Relation\",\n"
		   "       relacl as \"Access permissions\"\n"
		   "FROM   pg_class\n"
		   "WHERE  ( relkind = 'r' OR relkind = 'S') AND\n"
		   "       relname !~ '^pg_'\n");
	if (name)
	{
		strcat(descbuf, "  AND rename ~ '^");
		strncat(descbuf, name, REGEXP_CUTOFF);
		strcat(descbuf, "'\n");
	}
	strcat(descbuf, "ORDER BY relname");

	res = PSQLexec(pset, descbuf);
	if (!res)
		return false;

	if (PQntuples(res) == 0)
		fputs("Couldn't find any tables.\n", pset->queryFout);
	else
	{
		myopt.topt.tuples_only = false;
		myopt.nullPrint = NULL;
		sprintf(descbuf, "Access permissions for database \"%s\"", PQdb(pset->db));
		myopt.title = descbuf;

		printQuery(res, &myopt, pset->queryFout);
	}

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
objectDescription(const char *object, PsqlSettings *pset)
{
	char		descbuf[2048 + 7 * REGEXP_CUTOFF];
	PGresult   *res;
	printQueryOpt myopt = pset->popt;

	descbuf[0] = '\0';

	/* Aggregate descriptions */
	strcat(descbuf, "SELECT DISTINCT a.aggname as \"Name\", 'aggregate'::text as \"What\", d.description as \"Description\"\n"
		   "FROM pg_aggregate a, pg_description d\n"
		   "WHERE a.oid = d.objoid\n");
	if (object)
	{
		strcat(descbuf, "  AND a.aggname ~* '^");
		strncat(descbuf, object, REGEXP_CUTOFF);
		strcat(descbuf, "'\n");
	}

	/* Function descriptions (except in/outs for datatypes) */
	strcat(descbuf, "\nUNION ALL\n\n");
	strcat(descbuf, "SELECT DISTINCT p.proname as \"Name\", 'function'::text as \"What\", d.description as \"Description\"\n"
		   "FROM pg_proc p, pg_description d\n"
		   "WHERE p.oid = d.objoid AND (p.pronargs = 0 or oid8types(p.proargtypes) != '')\n");
	if (object)
	{
		strcat(descbuf, "  AND p.proname ~* '^");
		strncat(descbuf, object, REGEXP_CUTOFF);
		strcat(descbuf, "'\n");
	}

	/* Operator descriptions */
	strcat(descbuf, "\nUNION ALL\n\n");
	strcat(descbuf, "SELECT DISTINCT o.oprname as \"Name\", 'operator'::text as \"What\", d.description as \"Description\"\n"
		   "FROM pg_operator o, pg_description d\n"
		   // must get comment via associated function
		   "WHERE RegprocToOid(o.oprcode) = d.objoid\n");
	if (object)
	{
		strcat(descbuf, "  AND o.oprname = '");
		strncat(descbuf, object, REGEXP_CUTOFF);
		strcat(descbuf, "'\n");
	}

	/* Type description */
	strcat(descbuf, "\nUNION ALL\n\n");
	strcat(descbuf, "SELECT DISTINCT t.typname as \"Name\", 'type'::text as \"What\", d.description as \"Description\"\n"
		   "FROM pg_type t, pg_description d\n"
		   "WHERE t.oid = d.objoid\n");
	if (object)
	{
		strcat(descbuf, "  AND t.typname ~* '^");
		strncat(descbuf, object, REGEXP_CUTOFF);
		strcat(descbuf, "'\n");
	}

	/* Relation (tables, views, indices, sequences) descriptions */
	strcat(descbuf, "\nUNION ALL\n\n");
	strcat(descbuf, "SELECT DISTINCT c.relname as \"Name\", 'relation'::text||'('||c.relkind||')' as \"What\", d.description as \"Description\"\n"
		   "FROM pg_class c, pg_description d\n"
		   "WHERE c.oid = d.objoid\n");
	if (object)
	{
		strcat(descbuf, "  AND c.relname ~* '^");
		strncat(descbuf, object, REGEXP_CUTOFF);
		strcat(descbuf, "'\n");
	}

	/* Rule description (ignore rules for views) */
	strcat(descbuf, "\nUNION ALL\n\n");
	strcat(descbuf, "SELECT DISTINCT r.rulename as \"Name\", 'rule'::text as \"What\", d.description as \"Description\"\n"
		   "FROM pg_rewrite r, pg_description d\n"
		   "WHERE r.oid = d.objoid AND r.rulename !~ '^_RET'\n");
	if (object)
	{
		strcat(descbuf, "  AND r.rulename ~* '^");
		strncat(descbuf, object, REGEXP_CUTOFF);
		strcat(descbuf, "'\n");
	}

	/* Trigger description */
	strcat(descbuf, "\nUNION ALL\n\n");
	strcat(descbuf, "SELECT DISTINCT t.tgname as \"Name\", 'trigger'::text as \"What\", d.description as \"Description\"\n"
		   "FROM pg_trigger t, pg_description d\n"
		   "WHERE t.oid = d.objoid\n");
	if (object)
	{
		strcat(descbuf, "  AND t.tgname ~* '^");
		strncat(descbuf, object, REGEXP_CUTOFF);
		strcat(descbuf, "'\n");
	}

	strcat(descbuf, "\nORDER BY \"Name\"");


	res = PSQLexec(pset, descbuf);
	if (!res)
		return false;

	myopt.topt.tuples_only = false;
	myopt.nullPrint = NULL;
	myopt.title = "Object descriptions";

	printQuery(res, &myopt, pset->queryFout);

	PQclear(res);
	return true;
}



/*
 * describeTableDetails (for \d)
 *
 * Unfortunately, the information presented here is so complicated that it
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
		perror("malloc");
		exit(EXIT_FAILURE);
	}
	return tmp;
}


bool
describeTableDetails(const char *name, PsqlSettings *pset)
{
	char		descbuf[512 + NAMEDATALEN];
	PGresult   *res = NULL,
			   *res2 = NULL,
			   *res3 = NULL;
	printTableOpt myopt = pset->popt.topt;
	bool		description = GetVariableBool(pset->vars, "description");
	int			i;
	const char	   *view_def = NULL;
	const char *headers[5];
	char	  **cells = NULL;
	char	   *title = NULL;
	char	  **footers = NULL;
	char	  **ptr;
	unsigned int cols;

	cols = 3 + (description ? 1 : 0);

	headers[0] = "Attribute";
	headers[1] = "Type";
	headers[2] = "Info";
	if (description)
	{
		headers[3] = "Description";
		headers[4] = NULL;
	}
	else
		headers[3] = NULL;

	/* Get general table info */
	strcpy(descbuf, "SELECT a.attname, t.typname, a.attlen, a.atttypmod, a.attnotnull, a.atthasdef, a.attnum");
	if (description)
		strcat(descbuf, ", obj_description(a.oid)");
	strcat(descbuf, "\nFROM pg_class c, pg_attribute a, pg_type t\n"
		   "WHERE c.relname = '");
	strncat(descbuf, name, NAMEDATALEN);
	strcat(descbuf, "'\n  AND a.attnum > 0 AND a.attrelid = c.oid AND a.atttypid = t.oid\n"
		   "ORDER BY a.attnum");

	res = PSQLexec(pset, descbuf);
	if (!res)
		return false;

	/* Did we get anything? */
	if (PQntuples(res) == 0)
	{
		if (!GetVariableBool(pset->vars, "quiet"))
			fprintf(stdout, "Did not find any class named \"%s\".\n", name);
		PQclear(res);
		return false;
	}

	/* Check if table is a view */
	strcpy(descbuf, "SELECT definition FROM pg_views WHERE viewname = '");
	strncat(descbuf, name, NAMEDATALEN);
	strcat(descbuf, "'");
	res2 = PSQLexec(pset, descbuf);
	if (!res2)
		return false;

	if (PQntuples(res2) > 0)
		view_def = PQgetvalue(res2, 0, 0);



	/* Generate table cells to be printed */
	cells = calloc(PQntuples(res) * cols + 1, sizeof(*cells));
	if (!cells)
	{
		perror("calloc");
		exit(EXIT_FAILURE);
	}

	for (i = 0; i < PQntuples(res); i++)
	{
		int4		attypmod = atoi(PQgetvalue(res, i, 3));
		const char	   *attype = PQgetvalue(res, i, 1);

		/* Name */
		cells[i * cols + 0] = (char*)PQgetvalue(res, i, 0);	/* don't free this afterwards */

		/* Type */
		cells[i * cols + 1] = xmalloc(NAMEDATALEN + 16);
		if (strcmp(attype, "bpchar") == 0)
			sprintf(cells[i * cols + 1], "char(%d)", attypmod != -1 ? attypmod - VARHDRSZ : 0);
		else if (strcmp(attype, "varchar") == 0)
			sprintf(cells[i * cols + 1], "varchar(%d)", attypmod != -1 ? attypmod - VARHDRSZ : 0);
		else if (strcmp(attype, "numeric") == 0)
			sprintf(cells[i * cols + 1], "numeric(%d,%d)", ((attypmod - VARHDRSZ) >> 16) & 0xffff,
					(attypmod - VARHDRSZ) & 0xffff);
		else if (attype[0] == '_')
			sprintf(cells[i * cols + 1], "%s[]", attype + 1);
		else
			strcpy(cells[i * cols + 1], attype);

		/* Info */
		cells[i * cols + 2] = xmalloc(128 + 128);		/* I'm cutting off the
									 * 'default' string at 128 */
		cells[i * cols + 2][0] = '\0';
		if (strcmp(PQgetvalue(res, i, 4), "t") == 0)
			strcat(cells[i * cols + 2], "not null");
		if (strcmp(PQgetvalue(res, i, 5), "t") == 0)
		{
			/* handle "default" here */
			strcpy(descbuf, "SELECT substring(d.adsrc for 128) FROM pg_attrdef d, pg_class c\n"
				   "WHERE c.relname = '");
			strncat(descbuf, name, NAMEDATALEN);
			strcat(descbuf, "' AND c.oid = d.adrelid AND d.adnum = ");
			strcat(descbuf, PQgetvalue(res, i, 6));

			res3 = PSQLexec(pset, descbuf);
			if (!res)
				return false;
			if (cells[i * cols + 2][0])
				strcat(cells[i * cols + 2], " ");
			strcat(cells[i * cols + 2], "default ");
			strcat(cells[i * cols + 2], PQgetvalue(res3, 0, 0));
		}

		/* Description */
		if (description)
			cells[i * cols + 3] = (char*)PQgetvalue(res, i, 7);
	}

	/* Make title */
	title = xmalloc(10 + strlen(name));
	if (view_def)
		sprintf(title, "View \"%s\"", name);
	else
		sprintf(title, "Table \"%s\"", name);

	/* Make footers */
	if (view_def)
	{
		footers = xmalloc(2 * sizeof(*footers));
		footers[0] = xmalloc(20 + strlen(view_def));
		sprintf(footers[0], "View definition: %s", view_def);
		footers[1] = NULL;
	}
	else
	{
		/* display indices */
		strcpy(descbuf, "SELECT c2.relname\n"
			   "FROM pg_class c, pg_class c2, pg_index i\n"
			   "WHERE c.relname = '");
		strncat(descbuf, name, NAMEDATALEN);
		strcat(descbuf, "' AND c.oid = i.indrelid AND i.indexrelid = c2.oid\n"
			   "ORDER BY c2.relname");
		res3 = PSQLexec(pset, descbuf);
		if (!res3)
			return false;

		if (PQntuples(res3) > 0)
		{
			footers = xmalloc((PQntuples(res3) + 1) * sizeof(*footers));

			for (i = 0; i < PQntuples(res3); i++)
			{
				footers[i] = xmalloc(10 + NAMEDATALEN);
				if (PQntuples(res3) == 1)
					sprintf(footers[i], "Index: %s", PQgetvalue(res3, i, 0));
				else if (i == 0)
					sprintf(footers[i], "Indices: %s", PQgetvalue(res3, i, 0));
				else
					sprintf(footers[i], "         %s", PQgetvalue(res3, i, 0));
			}

			footers[i] = NULL;
		}
	}


	myopt.tuples_only = false;
	printTable(title, headers, (const char**)cells, (const char**)footers, "llll", &myopt, pset->queryFout);

	/* clean up */
	free(title);

	for (i = 0; i < PQntuples(res); i++)
	{
		free(cells[i * cols + 1]);
		free(cells[i * cols + 2]);
	}
	free(cells);

	for (ptr = footers; footers && *ptr; ptr++)
		free(*ptr);
	free(footers);

	PQclear(res);
	PQclear(res2);
	PQclear(res3);

	return true;
}



/*
 * listTables()
 *
 * handler for \d, \dt, etc.
 *
 * The infotype is an array of characters, specifying what info is desired:
 * t - tables
 * i - indices
 * v - views
 * s - sequences
 * S - systems tables (~'^pg_')
 * (any order of the above is fine)
 */
bool
listTables(const char *infotype, const char *name, PsqlSettings *pset)
{
	bool		showTables = strchr(infotype, 't') != NULL;
	bool		showIndices = strchr(infotype, 'i') != NULL;
	bool		showViews = strchr(infotype, 'v') != NULL;
	bool		showSeq = strchr(infotype, 's') != NULL;
	bool		showSystem = strchr(infotype, 'S') != NULL;

	bool		description = GetVariableBool(pset->vars, "description");

	char		descbuf[1536 + 4 * REGEXP_CUTOFF];
	PGresult   *res;
	printQueryOpt myopt = pset->popt;

	descbuf[0] = '\0';

	/* tables */
	if (showTables)
	{
		strcat(descbuf, "SELECT u.usename as \"Owner\", c.relname as \"Name\", 'table'::text as \"Type\"");
		if (description)
			strcat(descbuf, ", obj_description(c.oid) as \"Description\"");
		strcat(descbuf, "\nFROM pg_class c, pg_user u\n"
			   "WHERE c.relowner = u.usesysid AND c.relkind = 'r'\n"
			   "  AND not exists (select 1 from pg_views where viewname = c.relname)\n");
		strcat(descbuf, showSystem ? "  AND c.relname ~ '^pg_'\n" : "  AND c.relname !~ '^pg_'\n");
		if (name)
		{
			strcat(descbuf, "  AND c.relname ~ '^");
			strncat(descbuf, name, REGEXP_CUTOFF);
			strcat(descbuf, "'\n");
		}
	}

	/* views */
	if (showViews)
	{
		if (descbuf[0])
			strcat(descbuf, "\nUNION\n\n");

		strcat(descbuf, "SELECT u.usename as \"Owner\", c.relname as \"Name\", 'view'::text as \"Type\"");
		if (description)
			strcat(descbuf, ", obj_description(c.oid) as \"Description\"");
		strcat(descbuf, "\nFROM pg_class c, pg_user u\n"
			   "WHERE c.relowner = u.usesysid AND c.relkind = 'r'\n"
			   "  AND exists (select 1 from pg_views where viewname = c.relname)\n");
		strcat(descbuf, showSystem ? "  AND c.relname ~ '^pg_'\n" : "  AND c.relname !~ '^pg_'\n");
		if (name)
		{
			strcat(descbuf, "  AND c.relname ~ '^");
			strncat(descbuf, name, REGEXP_CUTOFF);
			strcat(descbuf, "'\n");
		}
	}

	/* indices, sequences */
	if (showIndices || showSeq)
	{
		if (descbuf[0])
			strcat(descbuf, "\nUNION\n\n");

		strcat(descbuf, "SELECT u.usename as \"Owner\", c.relname as \"Name\",\n"
			   "  (CASE WHEN relkind = 'S' THEN 'sequence'::text ELSE 'index'::text END) as \"Type\"");
		if (description)
			strcat(descbuf, ", obj_description(c.oid) as \"Description\"");
		strcat(descbuf, "\nFROM pg_class c, pg_user u\n"
			   "WHERE c.relowner = u.usesysid AND relkind in (");
		if (showIndices && showSeq)
			strcat(descbuf, "'i', 'S'");
		else if (showIndices)
			strcat(descbuf, "'i'");
		else
			strcat(descbuf, "'S'");
		strcat(descbuf, ")\n");

		/* ignore large-obj indices */
		if (showIndices)
			strcat(descbuf, "  AND (c.relkind != 'i' OR c.relname !~ '^xinx')\n");

		strcat(descbuf, showSystem ? "  AND c.relname ~ '^pg_'\n" : "  AND c.relname !~ '^pg_'\n");
		if (name)
		{
			strcat(descbuf, "  AND c.relname ~ '^");
			strncat(descbuf, name, REGEXP_CUTOFF);
			strcat(descbuf, "'\n");
		}
	}

	/* real system catalogue tables */
	if (showSystem && showTables)
	{
		if (descbuf[0])
			strcat(descbuf, "\nUNION\n\n");

		strcat(descbuf, "SELECT u.usename as \"Owner\", c.relname as \"Name\", 'system'::text as \"Type\"");
		if (description)
			strcat(descbuf, ", obj_description(c.oid) as \"Description\"");
		strcat(descbuf, "\nFROM pg_class c, pg_user u\n"
			   "WHERE c.relowner = u.usesysid AND c.relkind = 's'\n");
		if (name)
		{
			strcat(descbuf, "  AND c.relname ~ '^");
			strncat(descbuf, name, REGEXP_CUTOFF);
			strcat(descbuf, "'\n");
		}
	}

	strcat(descbuf, "\nORDER BY \"Name\"");


	res = PSQLexec(pset, descbuf);
	if (!res)
		return false;

	if (PQntuples(res) == 0)
		fprintf(pset->queryFout, "No matching classes found.\n");

	else
	{
		myopt.topt.tuples_only = false;
		myopt.nullPrint = NULL;
		myopt.title = "List of classes";

		printQuery(res, &myopt, pset->queryFout);
	}

	PQclear(res);
	return true;
}


/* the end */
