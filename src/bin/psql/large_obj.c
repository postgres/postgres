/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2003, PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/large_obj.c,v 1.29.4.1 2006/05/21 19:57:07 momjian Exp $
 */
#include "postgres_fe.h"
#include "large_obj.h"

#include "libpq-fe.h"

#include "settings.h"
#include "variables.h"
#include "common.h"
#include "print.h"


#define atooid(x)  ((Oid) strtoul((x), NULL, 10))


/*
 * Prepare to do a large-object operation.	We *must* be inside a transaction
 * block for all these operations, so start one if needed.
 *
 * Returns TRUE if okay, FALSE if failed.  *own_transaction is set to indicate
 * if we started our own transaction or not.
 */
static bool
start_lo_xact(const char *operation, bool *own_transaction)
{
	PGTransactionStatusType tstatus;
	PGresult   *res;

	*own_transaction = false;

	if (!pset.db)
	{
		psql_error("%s: not connected to a database\n", operation);
		return false;
	}

	tstatus = PQtransactionStatus(pset.db);

	switch (tstatus)
	{
		case PQTRANS_IDLE:
			/* need to start our own xact */
			if (!(res = PSQLexec("BEGIN", false)))
				return false;
			PQclear(res);
			*own_transaction = true;
			break;
		case PQTRANS_INTRANS:
			/* use the existing xact */
			break;
		case PQTRANS_INERROR:
			psql_error("%s: current transaction is aborted\n", operation);
			return false;
		default:
			psql_error("%s: unknown transaction status\n", operation);
			return false;
	}

	return true;
}

/*
 * Clean up after a successful LO operation
 */
static bool
finish_lo_xact(const char *operation, bool own_transaction)
{
	PGresult   *res;

	if (own_transaction &&
		GetVariableBool(pset.vars, "AUTOCOMMIT"))
	{
		/* close out our own xact */
		if (!(res = PSQLexec("COMMIT", false)))
		{
			res = PSQLexec("ROLLBACK", false);
			PQclear(res);
			return false;
		}
		PQclear(res);
	}

	return true;
}

/*
 * Clean up after a failed LO operation
 */
static bool
fail_lo_xact(const char *operation, bool own_transaction)
{
	PGresult   *res;

	if (own_transaction &&
		GetVariableBool(pset.vars, "AUTOCOMMIT"))
	{
		/* close out our own xact */
		res = PSQLexec("ROLLBACK", false);
		PQclear(res);
	}

	return false;				/* always */
}


/*
 * do_lo_export()
 *
 * Write a large object to a file
 */
bool
do_lo_export(const char *loid_arg, const char *filename_arg)
{
	int			status;
	bool		own_transaction;

	if (!start_lo_xact("\\lo_export", &own_transaction))
		return false;

	status = lo_export(pset.db, atooid(loid_arg), filename_arg);
	if (status != 1)
	{							/* of course this status is documented
								 * nowhere :( */
		fputs(PQerrorMessage(pset.db), stderr);
		return fail_lo_xact("\\lo_export", own_transaction);
	}

	if (!finish_lo_xact("\\lo_export", own_transaction))
		return false;

	fprintf(pset.queryFout, "lo_export\n");

	return true;
}


/*
 * do_lo_import()
 *
 * Copy large object from file to database
 */
bool
do_lo_import(const char *filename_arg, const char *comment_arg)
{
	PGresult   *res;
	Oid			loid;
	char		oidbuf[32];
	unsigned int i;
	bool		own_transaction;

	if (!start_lo_xact("\\lo_import", &own_transaction))
		return false;

	loid = lo_import(pset.db, filename_arg);
	if (loid == InvalidOid)
	{
		fputs(PQerrorMessage(pset.db), stderr);
		return fail_lo_xact("\\lo_import", own_transaction);
	}

	/* insert description if given */
	/* XXX don't try to hack pg_description if not superuser */
	/* XXX ought to replace this with some kind of COMMENT command */
	if (comment_arg && is_superuser())
	{
		char	   *cmdbuf;
		char	   *bufptr;
		size_t		slen = strlen(comment_arg);

		cmdbuf = malloc(slen * 2 + 256);
		if (!cmdbuf)
			return fail_lo_xact("\\lo_import", own_transaction);
		sprintf(cmdbuf,
				"INSERT INTO pg_catalog.pg_description VALUES ('%u', "
				"'pg_catalog.pg_largeobject'::regclass, "
				"0, '",
				loid);
		bufptr = cmdbuf + strlen(cmdbuf);
		for (i = 0; i < slen; i++)
		{
			if (comment_arg[i] == '\'' || comment_arg[i] == '\\')
				*bufptr++ = comment_arg[i];
			*bufptr++ = comment_arg[i];
		}
		strcpy(bufptr, "')");

		if (!(res = PSQLexec(cmdbuf, false)))
		{
			free(cmdbuf);
			return fail_lo_xact("\\lo_import", own_transaction);
		}

		PQclear(res);
		free(cmdbuf);
	}

	if (!finish_lo_xact("\\lo_import", own_transaction))
		return false;

	fprintf(pset.queryFout, "lo_import %u\n", loid);
	sprintf(oidbuf, "%u", loid);
	SetVariable(pset.vars, "LASTOID", oidbuf);

	return true;
}


/*
 * do_lo_unlink()
 *
 * removes a large object out of the database
 */
bool
do_lo_unlink(const char *loid_arg)
{
	PGresult   *res;
	int			status;
	Oid			loid = atooid(loid_arg);
	char		buf[256];
	bool		own_transaction;

	if (!start_lo_xact("\\lo_unlink", &own_transaction))
		return false;

	status = lo_unlink(pset.db, loid);
	if (status == -1)
	{
		fputs(PQerrorMessage(pset.db), stderr);
		return fail_lo_xact("\\lo_unlink", own_transaction);
	}

	/* remove the comment as well */
	/* XXX don't try to hack pg_description if not superuser */
	/* XXX ought to replace this with some kind of COMMENT command */
	if (is_superuser())
	{
		snprintf(buf, sizeof(buf),
			 "DELETE FROM pg_catalog.pg_description WHERE objoid = '%u' "
				 "AND classoid = 'pg_catalog.pg_largeobject'::regclass",
				 loid);
		if (!(res = PSQLexec(buf, false)))
			return fail_lo_xact("\\lo_unlink", own_transaction);
		PQclear(res);
	}

	if (!finish_lo_xact("\\lo_unlink", own_transaction))
		return false;

	fprintf(pset.queryFout, "lo_unlink %u\n", loid);

	return true;
}



/*
 * do_lo_list()
 *
 * Show all large objects in database with comments
 */
bool
do_lo_list(void)
{
	PGresult   *res;
	char		buf[1024];
	printQueryOpt myopt = pset.popt;

	snprintf(buf, sizeof(buf),
			 "SELECT loid as \"ID\", pg_catalog.obj_description(loid, 'pg_largeobject') as \"%s\"\n"
		 "FROM (SELECT DISTINCT loid FROM pg_catalog.pg_largeobject) x\n"
			 "ORDER BY \"ID\"",
			 gettext("Description"));

	res = PSQLexec(buf, false);
	if (!res)
		return false;

	myopt.topt.tuples_only = false;
	myopt.nullPrint = NULL;
	myopt.title = gettext("Large objects");

	printQuery(res, &myopt, pset.queryFout);

	PQclear(res);
	return true;
}
