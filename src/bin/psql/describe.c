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
describeAggregates(const char *name, PsqlSettings *pset, bool verbose, bool desc)
{
	char		buf[384 + 2 * REGEXP_CUTOFF];
	PGresult   *res;
	printQueryOpt myopt = pset->popt;

	/*
	 * There are two kinds of aggregates: ones that work on particular
	 * types ones that work on all
	 */
	strcpy(buf,
		   "SELECT a.aggname AS \"Name\", t.typname AS \"Type\"");
    if (verbose)
	strcat(buf, " ,u.usename as \"Owner\"");
	if (desc)
		strcat(buf, ",\n       obj_description(a.oid) as \"Description\"");
	strcat(buf, !verbose ?
		   ("\nFROM pg_aggregate a, pg_type t\n"
	    "WHERE a.aggbasetype = t.oid\n") :
		   ("\nFROM pg_aggregate a, pg_type t, pg_user u\n"
	    "WHERE a.aggbasetype = t.oid AND a.aggowner = u.usesysid\n")
	);

	if (name)
	{
		strcat(buf, "  AND a.aggname ~* '");
		strncat(buf, name, REGEXP_CUTOFF);
		strcat(buf, "'\n");
	}

	strcat(buf,
		   "UNION\n"
		   "SELECT a.aggname AS \"Name\", '(all types)' as \"Type\"");
    if (verbose)
	strcat(buf, " ,u.usename as \"Owner\"");
	if (desc)
		strcat(buf,
			   ",\n       obj_description(a.oid) as \"Description\"");
	strcat(buf, !verbose ?
		   ("\nFROM pg_aggregate a\n"
	    "WHERE a.aggbasetype = 0\n") :
		   ("\nFROM pg_aggregate a, pg_user u\n"
	    "WHERE a.aggbasetype = 0 AND a.aggowner = u.usesysid\n")
	);
	if (name)
	{
		strcat(buf, "  AND a.aggname ~* '");
		strncat(buf, name, REGEXP_CUTOFF);
		strcat(buf, "'\n");
	}

	strcat(buf, "ORDER BY \"Name\", \"Type\"");

	res = PSQLexec(pset, buf);
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
 * Takes an optional regexp to narrow down the function name
 */
bool
describeFunctions(const char *name, PsqlSettings *pset, bool verbose, bool desc)
{
	char		buf[384 + REGEXP_CUTOFF];
	PGresult   *res;
	printQueryOpt myopt = pset->popt;

	/*
	 * we skip in/out funcs by excluding functions that take some
	 * arguments, but have no types defined for those arguments
	 */
	strcpy(buf,
	   "SELECT t.typname as \"Result\", p.proname as \"Function\",\n"
		   "       oid8types(p.proargtypes) as \"Arguments\"");
    if (verbose)
	strcat(buf, ",\n       u.usename as \"Owner\", l.lanname as \"Language\", p.prosrc as \"Source\"");
	if (desc)
		strcat(buf, ",\n       obj_description(p.oid) as \"Description\"");

    if (!verbose)
	strcat(buf,
	       "\nFROM pg_proc p, pg_type t\n"
	       "WHERE p.prorettype = t.oid and (pronargs = 0 or oid8types(p.proargtypes) != '')\n");
    else
	strcat(buf,
	       "\nFROM pg_proc p, pg_type t, pg_language l, pg_user u\n"
	       "WHERE p.prorettype = t.oid AND p.prolang = l.oid AND p.proowner = u.usesysid\n"
	       "  AND (pronargs = 0 or oid8types(p.proargtypes) != '')\n");

	if (name)
	{
		strcat(buf, "  AND p.proname ~* '");
		strncat(buf, name, REGEXP_CUTOFF);
		strcat(buf, "'\n");
	}
	strcat(buf, "ORDER BY \"Function\", \"Result\", \"Arguments\"");

	res = PSQLexec(pset, buf);
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
 * \dT
 * describe types
 */
bool
describeTypes(const char *name, PsqlSettings *pset, bool verbose, bool desc)
{
	char		buf[256 + REGEXP_CUTOFF];
	PGresult   *res;
	printQueryOpt myopt = pset->popt;

	strcpy(buf, "SELECT t.typname AS \"Type\"");
    if (verbose)
	strcat(buf,
	       ",\n       (CASE WHEN t.typlen=-1 THEN 'var'::text ELSE t.typlen::text END) as \"Length\""
	       ",\n       u.usename as \"Owner\""
	    );
    /*
	 *	Let's always show descriptions for this.  There is room.
	 *	bjm 1999/12/31
     */
	strcat(buf, ",\n       obj_description(t.oid) as \"Description\"");
    /*
     * do not include array types (start with underscore),
     * do not include user relations (typrelid!=0)
     */
	strcat(buf, !verbose ?
	   ("\nFROM pg_type t\n"
	    "WHERE t.typrelid = 0 AND t.typname !~ '^_.*'\n") :
	   ("\nFROM pg_type t, pg_user u\n"
	    "WHERE t.typrelid = 0 AND t.typname !~ '^_.*' AND t.typowner = u.usesysid\n")
	);

	if (name)
	{
		strcat(buf, "  AND t.typname ~* '");
		strncat(buf, name, REGEXP_CUTOFF);
		strcat(buf, "' ");
	}
	strcat(buf, "ORDER BY t.typname;");

	res = PSQLexec(pset, buf);
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
 */
bool
describeOperators(const char *name, PsqlSettings *pset, bool verbose, bool desc)
{
	char		buf[1536 + 3 * REGEXP_CUTOFF];
	PGresult   *res;
	printQueryOpt myopt = pset->popt;

    /* Not used right now. Maybe later. */
    (void)verbose;

    /* FIXME: Use outer joins here when ready */

	strcpy(buf,
	   "SELECT o.oprname AS \"Op\",\n"
		   "       t1.typname AS \"Left arg\",\n"
		   "       t2.typname AS \"Right arg\",\n"
		   "       t0.typname AS \"Result\"");
	if (desc)
		strcat(buf, ",\n       obj_description(p.oid) as \"Description\"");
	strcat(buf,
	   "\nFROM   pg_proc p, pg_type t0,\n"
		   "       pg_type t1, pg_type t2,\n"
		   "       pg_operator o\n"
		   "WHERE  p.prorettype = t0.oid AND\n"
		   "       RegprocToOid(o.oprcode) = p.oid AND\n"
		   "       p.pronargs = 2 AND\n"
		   "       o.oprleft = t1.oid AND\n"
		   "       o.oprright = t2.oid\n");
	if (name)
	{
		strcat(buf, "  AND o.oprname ~ '");
		strncat(buf, name, REGEXP_CUTOFF);
		strcat(buf, "'\n");
	}

	strcat(buf, "\nUNION\n\n"
		   "SELECT o.oprname as \"Op\",\n"
		   "       ''::name AS \"Left arg\",\n"
		   "       t1.typname AS \"Right arg\",\n"
		   "       t0.typname AS \"Result\"");
	if (desc)
		strcat(buf, ",\n       obj_description(p.oid) as \"Description\"");
	strcat(buf, "\nFROM   pg_operator o, pg_proc p, pg_type t0, pg_type t1\n"
		   "WHERE  RegprocToOid(o.oprcode) = p.oid AND\n"
		   "       o.oprresult = t0.oid AND\n"
		   "       o.oprkind = 'l' AND\n"
		   "       o.oprright = t1.oid\n");
	if (name)
	{
		strcat(buf, "AND o.oprname ~ '");
		strncat(buf, name, REGEXP_CUTOFF);
		strcat(buf, "'\n");
	}

	strcat(buf, "\nUNION\n\n"
		   "SELECT o.oprname  as \"Op\",\n"
		   "       t1.typname AS \"Left arg\",\n"
		   "       ''::name AS \"Right arg\",\n"
		   "       t0.typname AS \"Result\"");
	if (desc)
		strcat(buf, ",\n       obj_description(p.oid) as \"Description\"");
	strcat(buf, "\nFROM   pg_operator o, pg_proc p, pg_type t0, pg_type t1\n"
		   "WHERE  RegprocToOid(o.oprcode) = p.oid AND\n"
		   "       o.oprresult = t0.oid AND\n"
		   "       o.oprkind = 'r' AND\n"
		   "       o.oprleft = t1.oid\n");
	if (name)
	{
		strcat(buf, "AND o.oprname ~ '");
		strncat(buf, name, REGEXP_CUTOFF);
		strcat(buf, "'\n");
	}
	strcat(buf, "\nORDER BY \"Op\", \"Left arg\", \"Right arg\", \"Result\"");

	res = PSQLexec(pset, buf);
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
listAllDbs(PsqlSettings *pset, bool desc)
{
	PGresult   *res;
	char		buf[256];
	printQueryOpt myopt = pset->popt;

	strcpy(buf,
	   "SELECT pg_database.datname as \"Database\",\n"
		   "       pg_user.usename as \"Owner\"");
#ifdef MULTIBYTE
	strcat(buf,
		   ",\n       pg_database.encoding as \"Encoding\"");
#endif
	if (desc)
		strcat(buf, ",\n       obj_description(pg_database.oid) as \"Description\"\n");
	strcat(buf, "FROM pg_database, pg_user\n"
		   "WHERE pg_database.datdba = pg_user.usesysid\n"
		   "ORDER BY \"Database\"");

	res = PSQLexec(pset, buf);
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
		strcat(descbuf, "  AND rename ~ '");
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
		strcat(descbuf, "  AND a.aggname ~* '");
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
		strcat(descbuf, "  AND p.proname ~* '");
		strncat(descbuf, object, REGEXP_CUTOFF);
		strcat(descbuf, "'\n");
	}

	/* Operator descriptions */
	strcat(descbuf, "\nUNION ALL\n\n");
	strcat(descbuf, "SELECT DISTINCT o.oprname as \"Name\", 'operator'::text as \"What\", d.description as \"Description\"\n"
		   "FROM pg_operator o, pg_description d\n"
		   /* must get comment via associated function */
		   "WHERE RegprocToOid(o.oprcode) = d.objoid\n");
	if (object)
	{
		strcat(descbuf, "  AND o.oprname ~ '");
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
		strcat(descbuf, "  AND t.typname ~* '");
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
		strcat(descbuf, "  AND c.relname ~* '");
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
		strcat(descbuf, "  AND r.rulename ~* '");
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
		strcat(descbuf, "  AND t.tgname ~* '");
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
describeTableDetails(const char *name, PsqlSettings *pset, bool desc)
{
	char		buf[512 + INDEX_MAX_KEYS * NAMEDATALEN];
	PGresult   *res = NULL;
	printTableOpt myopt = pset->popt.topt;
	int			i;
	const char *view_def = NULL;
	const char *headers[5];
	char	  **cells = NULL;
	char	   *title = NULL;
	char	  **footers = NULL;
	char	  **ptr;
	unsigned int cols;
    struct { bool hasindex; char relkind; int16 checks; int16 triggers; bool hasrules; } tableinfo;
    bool error = false;

    /* truncate table name */
    if (strlen(name) > NAMEDATALEN) {
	char *my_name = xmalloc(NAMEDATALEN+1);
	strncpy(my_name, name, NAMEDATALEN);
	my_name[NAMEDATALEN] = '\0';
	name = my_name;
    }

	/* Get general table info */
    sprintf(buf,
	    "SELECT relhasindex, relkind, relchecks, reltriggers, relhasrules\n"
	    "FROM pg_class WHERE relname='%s'",
	    name);
    res = PSQLexec(pset, buf);
    if (!res)
	return false;

	/* Did we get anything? */
	if (PQntuples(res) == 0)
	{
		if (!GetVariableBool(pset->vars, "quiet"))
			fprintf(stdout, "Did not find any relation named \"%s\".\n", name);
		PQclear(res);
		return false;
	}

    /* FIXME: check for null pointers here? */
    tableinfo.hasindex = strcmp(PQgetvalue(res,0,0),"t")==0;
    tableinfo.relkind = *(PQgetvalue(res,0,1));
    tableinfo.checks = atoi(PQgetvalue(res,0,2));
    tableinfo.triggers = atoi(PQgetvalue(res,0,3));
    tableinfo.hasrules = strcmp(PQgetvalue(res,0,4),"t")==0;
    PQclear(res);


	headers[0] = "Attribute";
	headers[1] = "Type";
	cols = 2;

    if (tableinfo.relkind == 'r' || tableinfo.relkind == 's')
    {
	cols++;
	headers[cols-1] = "Extra";
    }

	if (desc)
	{
	cols++;
		headers[cols-1] = "Description";
	}

    headers[cols] = NULL;


    /* Get column info */
	strcpy(buf, "SELECT a.attname, t.typname, a.attlen, a.atttypmod, a.attnotnull, a.atthasdef, a.attnum");
	if (desc)
		strcat(buf, ", obj_description(a.oid)");
	strcat(buf, "\nFROM pg_class c, pg_attribute a, pg_type t\n"
		   "WHERE c.relname = '");
	strncat(buf, name, NAMEDATALEN);
	strcat(buf, "'\n  AND a.attnum > 0 AND a.attrelid = c.oid AND a.atttypid = t.oid\n"
		   "ORDER BY a.attnum");

	res = PSQLexec(pset, buf);
	if (!res)
		return false;

	/* Check if table is a view */
    if (tableinfo.hasrules) {
	PGresult *result;
	sprintf(buf, "SELECT definition FROM pg_views WHERE viewname = '%s'", name);
	result = PSQLexec(pset, buf);
	if (!result)
	{
	    PQclear(res);
	    PQclear(result);
	    return false;
	}

	if (PQntuples(result) > 0)
	    view_def = xstrdup(PQgetvalue(result, 0, 0));
	PQclear(result);
    }


	/* Generate table cells to be printed */
	cells = xmalloc((PQntuples(res) * cols + 1) * sizeof(*cells));
    cells[PQntuples(res) * cols] = NULL; /* end of list */

	for (i = 0; i < PQntuples(res); i++)
	{
		int4		attypmod = atoi(PQgetvalue(res, i, 3));
		const char	   *attype = PQgetvalue(res, i, 1);

		/* Name */
		cells[i * cols + 0] = (char *)PQgetvalue(res, i, 0);	/* don't free this afterwards */

		/* Type */
	/* (convert some internal type names to "readable") */
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


		/* Extra: not null and default */
		/* (I'm cutting off the 'default' string at 128) */
	if (tableinfo.relkind == 'r' || tableinfo.relkind == 's')
	{
	    cells[i * cols + 2] = xmalloc(128 + 128);
	    cells[i * cols + 2][0] = '\0';
	    if (strcmp(PQgetvalue(res, i, 4), "t") == 0)
		strcat(cells[i * cols + 2], "not null");

	    /* handle "default" here */
	    if (strcmp(PQgetvalue(res, i, 5), "t") == 0)
	    {
		PGresult *result;

		sprintf(buf, "SELECT substring(d.adsrc for 128) FROM pg_attrdef d, pg_class c\n"
			"WHERE c.relname = '%s' AND c.oid = d.adrelid AND d.adnum = %s",
			name, PQgetvalue(res, i, 6));

		result = PSQLexec(pset, buf);
		if (!result)
		    error = true;
		else
		{
		    if (cells[i * cols + 2][0])
			strcat(cells[i * cols + 2], " ");
		    strcat(cells[i * cols + 2], "default ");
		    strcat(cells[i * cols + 2], PQgetvalue(result, 0, 0));
		    PQclear(result);
		}
	    }
	}

	if (error)
	    break;

		/* Description */
		if (desc)
			cells[i * cols + cols-1] = (char*)PQgetvalue(res, i, 7);
	}

	/* Make title */
	title = xmalloc(20 + strlen(name));
    switch (tableinfo.relkind) {
    case 'r':
	if (view_def)
	    sprintf(title, "View \"%s\"", name);
	else
	    sprintf(title, "Table \"%s\"", name);
	break;
    case 'S':
	sprintf(title, "Sequence \"%s\"", name);
	break;
    case 'i':
	sprintf(title, "Index \"%s\"", name);
	break;
    case 's':
	sprintf(title, "System table \"%s\"", name);
	break;
    default:
	sprintf(title, "?%c?", tableinfo.relkind);
    }

	/* Make footers */
    /* Information about the index */
    if (tableinfo.relkind == 'i')
    {
	PGresult * result;

	sprintf(buf, "SELECT i.indisunique, i.indisprimary, a.amname\n"
		"FROM pg_index i, pg_class c, pg_am a\n"
		"WHERE i.indexrelid = c.oid AND c.relname = '%s' AND c.relam = a.oid",
		name);

	result = PSQLexec(pset, buf);
	if (!result)
	    error = true;
	else
	{
	    footers = xmalloc(2 * sizeof(*footers));
	    footers[0] = xmalloc(NAMEDATALEN + 32);
	    sprintf(footers[0], "%s%s",
		    strcmp(PQgetvalue(result, 0, 0), "t")==0 ? "unique " : "",
		    PQgetvalue(result, 0, 2)
		);
	    if (strcmp(PQgetvalue(result, 0, 1), "t")==0)
		strcat(footers[0], " (primary key)");
	    footers[1] = NULL;
	}
    }
    /* Information about the view */
	else if (tableinfo.relkind == 'r' && view_def)
	{
		footers = xmalloc(2 * sizeof(*footers));
		footers[0] = xmalloc(20 + strlen(view_def));
		sprintf(footers[0], "View definition: %s", view_def);
		footers[1] = NULL;
	}

    /* Information about the table */
	else if (tableinfo.relkind == 'r')
	{
	PGresult *result1=NULL, *result2=NULL, *result3=NULL, *result4=NULL;
	int index_count=0, constr_count=0, rule_count=0, trigger_count=0;
	int count_footers=0;

		/* count indices */
	if (!error && tableinfo.hasindex)
	{
	    sprintf(buf, "SELECT c2.relname\n"
		    "FROM pg_class c, pg_class c2, pg_index i\n"
		    "WHERE c.relname = '%s' AND c.oid = i.indrelid AND i.indexrelid = c2.oid\n"
		    "ORDER BY c2.relname",
		    name);
	    result1 = PSQLexec(pset, buf);
	    if (!result1)
		error = true;
	    else
		index_count = PQntuples(result1);
	}

	/* count table (and column) constraints */
	if (!error && tableinfo.checks)
	{
	    sprintf(buf, "SELECT rcsrc\n"
		    "FROM pg_relcheck r, pg_class c\n"
		    "WHERE c.relname='%s' AND c.oid = r.rcrelid",
		    name);
	    result2 = PSQLexec(pset, buf);
	    if (!result2)
		error = true;
	    else
		constr_count = PQntuples(result2);
	}

	/* count rules */
	if (!error && tableinfo.hasrules)
	{
	    sprintf(buf,
		    "SELECT r.rulename\n"
		    "FROM pg_rewrite r, pg_class c\n"
		    "WHERE c.relname='%s' AND c.oid = r.ev_class",
		    name);
	    result3 = PSQLexec(pset, buf);
	    if (!result3)
		error = true;
	    else
		rule_count = PQntuples(result3);
	}

	/* count triggers */
	if (!error && tableinfo.hasrules)
	{
	    sprintf(buf,
		    "SELECT t.tgname\n"
		    "FROM pg_trigger t, pg_class c\n"
		    "WHERE c.relname='%s' AND c.oid = t.tgrelid",
		    name);
	    result4 = PSQLexec(pset, buf);
	    if (!result4)
		error = true;
	    else
		trigger_count = PQntuples(result4);
	}

	footers = xmalloc((index_count + constr_count + rule_count + trigger_count + 1) * sizeof(*footers));

	/* print indices */
	for (i = 0; i < index_count; i++)
	{
	    sprintf(buf, "%s %s",
		    index_count==1 ? "Index:" : (i==0 ? "Indices:" : "        "),
		    PQgetvalue(result1, i, 0)
		    );
	    if (i < index_count-1)
		strcat(buf, ",");

	    footers[count_footers++] = xstrdup(buf);
		}

	/* print contraints */
	for (i = 0; i < constr_count; i++)
	{
	    sprintf(buf, "%s %s",
		    constr_count==1 ? "Constraint:" : (i==0 ? "Constraints:" : "            "),
		    PQgetvalue(result2, i, 0)
		    );
	    footers[count_footers++] = xstrdup(buf);
		}

	/* print rules */
	for (i = 0; i < rule_count; i++)
	{
	    sprintf(buf, "%s %s",
		    rule_count==1 ? "Rule:" : (i==0 ? "Rules:" : "      "),
		    PQgetvalue(result3, i, 0)
		    );
	    if (i < rule_count-1)
		strcat(buf, ",");

	    footers[count_footers++] = xstrdup(buf);
		}

	/* print triggers */
	for (i = 0; i < trigger_count; i++)
	{
	    sprintf(buf, "%s %s",
		    trigger_count==1 ? "Trigger:" : (i==0 ? "Triggers:" : "         "),
		    PQgetvalue(result4, i, 0)
		    );
	    if (i < trigger_count-1)
		strcat(buf, ",");

	    footers[count_footers++] = xstrdup(buf);
		}

	/* end of list marker */
	footers[count_footers] = NULL;

	PQclear(result1);
	PQclear(result2);
	PQclear(result3);
	PQclear(result4);
	}

    if (!error) {
	myopt.tuples_only = false;
	printTable(title, headers, (const char**)cells, (const char**)footers, "llll", &myopt, pset->queryFout);
    }

	/* clean up */
	free(title);

	for (i = 0; i < PQntuples(res); i++)
	{
		free(cells[i * cols + 1]);
	if (tableinfo.relkind == 'r' || tableinfo.relkind == 's')
	    free(cells[i * cols + 2]);
	}
	free(cells);

	for (ptr = footers; footers && *ptr; ptr++)
		free(*ptr);
	free(footers);

	PQclear(res);

	return !error;
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
 * S - systems tables (~ '^pg_')
 * (any order of the above is fine)
 */
bool
listTables(const char *infotype, const char *name, PsqlSettings *pset, bool desc)
{
	bool		showTables = strchr(infotype, 't') != NULL;
	bool		showIndices = strchr(infotype, 'i') != NULL;
	bool		showViews = strchr(infotype, 'v') != NULL;
	bool		showSeq = strchr(infotype, 's') != NULL;
	bool		showSystem = strchr(infotype, 'S') != NULL;

	char		buf[1536 + 4 * REGEXP_CUTOFF];
	PGresult   *res;
	printQueryOpt myopt = pset->popt;

	buf[0] = '\0';

	/* tables */
	if (showTables)
	{
		strcat(buf, "SELECT c.relname as \"Name\", 'table'::text as \"Type\", u.usename as \"Owner\"");
		if (desc)
			strcat(buf, ", obj_description(c.oid) as \"Description\"");
		strcat(buf, "\nFROM pg_class c, pg_user u\n"
			   "WHERE c.relowner = u.usesysid AND c.relkind = 'r'\n"
			   "  AND not exists (select 1 from pg_views where viewname = c.relname)\n");
		strcat(buf, showSystem ? "  AND c.relname ~ '^pg_'\n" : "  AND c.relname !~ '^pg_'\n");
		if (name)
		{
			strcat(buf, "  AND c.relname ~ '");
			strncat(buf, name, REGEXP_CUTOFF);
			strcat(buf, "'\n");
		}
	}

	/* views */
	if (showViews)
	{
		if (buf[0])
			strcat(buf, "\nUNION\n\n");

		strcat(buf, "SELECT c.relname as \"Name\", 'view'::text as \"Type\", u.usename as \"Owner\"");
		if (desc)
			strcat(buf, ", obj_description(c.oid) as \"Description\"");
		strcat(buf, "\nFROM pg_class c, pg_user u\n"
			   "WHERE c.relowner = u.usesysid AND c.relkind = 'r'\n"
			   "  AND exists (select 1 from pg_views where viewname = c.relname)\n");
		strcat(buf, showSystem ? "  AND c.relname ~ '^pg_'\n" : "  AND c.relname !~ '^pg_'\n");
		if (name)
		{
			strcat(buf, "  AND c.relname ~ '");
			strncat(buf, name, REGEXP_CUTOFF);
			strcat(buf, "'\n");
		}
	}

	/* indices, sequences */
	if (showIndices || showSeq)
	{
		if (buf[0])
			strcat(buf, "\nUNION\n\n");

		strcat(buf,
	       "SELECT c.relname as \"Name\",\n"
			   "  (CASE WHEN relkind = 'S' THEN 'sequence'::text ELSE 'index'::text END) as \"Type\",\n"
	       "  u.usename as \"Owner\""
	    );
		if (desc)
			strcat(buf, ", obj_description(c.oid) as \"Description\"");
		strcat(buf, "\nFROM pg_class c, pg_user u\n"
			   "WHERE c.relowner = u.usesysid AND relkind in (");
		if (showIndices && showSeq)
			strcat(buf, "'i', 'S'");
		else if (showIndices)
			strcat(buf, "'i'");
		else
			strcat(buf, "'S'");
		strcat(buf, ")\n");

		/* ignore large-obj indices */
		if (showIndices)
			strcat(buf, "  AND (c.relkind != 'i' OR c.relname !~ '^xinx')\n");

		strcat(buf, showSystem ? "  AND c.relname ~ '^pg_'\n" : "  AND c.relname !~ '^pg_'\n");
		if (name)
		{
			strcat(buf, "  AND c.relname ~ '");
			strncat(buf, name, REGEXP_CUTOFF);
			strcat(buf, "'\n");
		}
	}

	/* real system catalogue tables */
	if (showSystem && showTables)
	{
		if (buf[0])
			strcat(buf, "\nUNION\n\n");

		strcat(buf, "SELECT c.relname as \"Name\", 'system'::text as \"Type\", u.usename as \"Owner\"");
		if (desc)
			strcat(buf, ", obj_description(c.oid) as \"Description\"");
		strcat(buf, "\nFROM pg_class c, pg_user u\n"
			   "WHERE c.relowner = u.usesysid AND c.relkind = 's'\n");
		if (name)
		{
			strcat(buf, "  AND c.relname ~ '");
			strncat(buf, name, REGEXP_CUTOFF);
			strcat(buf, "'\n");
		}
	}

	strcat(buf, "\nORDER BY \"Name\"");


	res = PSQLexec(pset, buf);
	if (!res)
		return false;

	if (PQntuples(res) == 0)
		fprintf(pset->queryFout, "No matching relations found.\n");

	else
	{
		myopt.topt.tuples_only = false;
		myopt.nullPrint = NULL;
		myopt.title = "List of relations";

		printQuery(res, &myopt, pset->queryFout);
	}

	PQclear(res);
	return true;
}


/* end of file */
