/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2015, PostgreSQL Global Development Group
 *
 * src/bin/psql/large_obj.c
 */
#include "postgres_fe.h"
#include "large_obj.h"


#include "settings.h"
#include "common.h"

static void print_lo_result(const char *fmt,...) pg_attribute_printf(1, 2);

static void
print_lo_result(const char *fmt,...)
{
	va_list		ap;

	if (!pset.quiet)
	{
		if (pset.popt.topt.format == PRINT_HTML)
			fputs("<p>", pset.queryFout);

		va_start(ap, fmt);
		vfprintf(pset.queryFout, fmt, ap);
		va_end(ap);

		if (pset.popt.topt.format == PRINT_HTML)
			fputs("</p>\n", pset.queryFout);
		else
			fputs("\n", pset.queryFout);
	}

	if (pset.logfile)
	{
		va_start(ap, fmt);
		vfprintf(pset.logfile, fmt, ap);
		va_end(ap);
		fputs("\n", pset.logfile);
	}
}


/*
 * Prepare to do a large-object operation.  We *must* be inside a transaction
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
			if (!(res = PSQLexec("BEGIN")))
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

	if (own_transaction && pset.autocommit)
	{
		/* close out our own xact */
		if (!(res = PSQLexec("COMMIT")))
		{
			res = PSQLexec("ROLLBACK");
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

	if (own_transaction && pset.autocommit)
	{
		/* close out our own xact */
		res = PSQLexec("ROLLBACK");
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

	SetCancelConn();
	status = lo_export(pset.db, atooid(loid_arg), filename_arg);
	ResetCancelConn();

	/* of course this status is documented nowhere :( */
	if (status != 1)
	{
		psql_error("%s", PQerrorMessage(pset.db));
		return fail_lo_xact("\\lo_export", own_transaction);
	}

	if (!finish_lo_xact("\\lo_export", own_transaction))
		return false;

	print_lo_result("lo_export");

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
	bool		own_transaction;

	if (!start_lo_xact("\\lo_import", &own_transaction))
		return false;

	SetCancelConn();
	loid = lo_import(pset.db, filename_arg);
	ResetCancelConn();

	if (loid == InvalidOid)
	{
		psql_error("%s", PQerrorMessage(pset.db));
		return fail_lo_xact("\\lo_import", own_transaction);
	}

	/* insert description if given */
	if (comment_arg)
	{
		char	   *cmdbuf;
		char	   *bufptr;
		size_t		slen = strlen(comment_arg);

		cmdbuf = malloc(slen * 2 + 256);
		if (!cmdbuf)
			return fail_lo_xact("\\lo_import", own_transaction);
		sprintf(cmdbuf, "COMMENT ON LARGE OBJECT %u IS '", loid);
		bufptr = cmdbuf + strlen(cmdbuf);
		bufptr += PQescapeStringConn(pset.db, bufptr, comment_arg, slen, NULL);
		strcpy(bufptr, "'");

		if (!(res = PSQLexec(cmdbuf)))
		{
			free(cmdbuf);
			return fail_lo_xact("\\lo_import", own_transaction);
		}

		PQclear(res);
		free(cmdbuf);
	}

	if (!finish_lo_xact("\\lo_import", own_transaction))
		return false;

	print_lo_result("lo_import %u", loid);

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
	int			status;
	Oid			loid = atooid(loid_arg);
	bool		own_transaction;

	if (!start_lo_xact("\\lo_unlink", &own_transaction))
		return false;

	SetCancelConn();
	status = lo_unlink(pset.db, loid);
	ResetCancelConn();

	if (status == -1)
	{
		psql_error("%s", PQerrorMessage(pset.db));
		return fail_lo_xact("\\lo_unlink", own_transaction);
	}

	if (!finish_lo_xact("\\lo_unlink", own_transaction))
		return false;

	print_lo_result("lo_unlink %u", loid);

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

	if (pset.sversion >= 90000)
	{
		snprintf(buf, sizeof(buf),
				 "SELECT oid as \"%s\",\n"
				 "  pg_catalog.pg_get_userbyid(lomowner) as \"%s\",\n"
			"  pg_catalog.obj_description(oid, 'pg_largeobject') as \"%s\"\n"
				 "  FROM pg_catalog.pg_largeobject_metadata "
				 "  ORDER BY oid",
				 gettext_noop("ID"),
				 gettext_noop("Owner"),
				 gettext_noop("Description"));
	}
	else
	{
		snprintf(buf, sizeof(buf),
				 "SELECT loid as \"%s\",\n"
		   "  pg_catalog.obj_description(loid, 'pg_largeobject') as \"%s\"\n"
			 "FROM (SELECT DISTINCT loid FROM pg_catalog.pg_largeobject) x\n"
				 "ORDER BY 1",
				 gettext_noop("ID"),
				 gettext_noop("Description"));
	}

	res = PSQLexec(buf);
	if (!res)
		return false;

	myopt.topt.tuples_only = false;
	myopt.nullPrint = NULL;
	myopt.title = _("Large objects");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}
