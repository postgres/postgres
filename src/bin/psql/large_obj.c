/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright 2000 by PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/large_obj.c,v 1.18 2001/10/25 05:49:54 momjian Exp $
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
 * Since all large object ops must be in a transaction, we must do some magic
 * here. You can set the variable lo_transaction to one of commit|rollback|
 * nothing to get your favourite behaviour regarding any transaction in
 * progress. Rollback is default.
 */

static char notice[80];

static void
_my_notice_handler(void *arg, const char *message)
{
	(void) arg;
	strncpy(notice, message, 79);
	notice[79] = '\0';
}


static bool
handle_transaction(void)
{
	const char *var = GetVariable(pset.vars, "LO_TRANSACTION");
	PGresult   *res;
	bool		commit;
	PQnoticeProcessor old_notice_hook;

	if (var && strcmp(var, "nothing") == 0)
		return true;

	commit = (var && strcmp(var, "commit") == 0);

	notice[0] = '\0';
	old_notice_hook = PQsetNoticeProcessor(pset.db, _my_notice_handler, NULL);

	res = PSQLexec(commit ? "COMMIT" : "ROLLBACK");
	if (!res)
		return false;

	if (notice[0])
	{
		if ((!commit && strcmp(notice, "NOTICE:  ROLLBACK: no transaction in progress\n") != 0) ||
			(commit && strcmp(notice, "NOTICE:  COMMIT: no transaction in progress\n") != 0))
			fputs(notice, stderr);
	}
	else if (!QUIET())
	{
		if (commit)
			puts(gettext("Warning: Your transaction in progress has been committed."));
		else
			puts(gettext("Warning: Your transaction in progress has been rolled back."));
	}

	PQsetNoticeProcessor(pset.db, old_notice_hook, NULL);
	return true;
}



/*
 * do_lo_export()
 *
 * Write a large object to a file
 */
bool
do_lo_export(const char *loid_arg, const char *filename_arg)
{
	PGresult   *res;
	int			status;
	bool		own_transaction = true;
	const char *var = GetVariable(pset.vars, "LO_TRANSACTION");

	if (var && strcmp(var, "nothing") == 0)
		own_transaction = false;

	if (!pset.db)
	{
		psql_error("\\lo_export: not connected to a database\n");
		return false;
	}

	if (own_transaction)
	{
		if (!handle_transaction())
			return false;

		if (!(res = PSQLexec("BEGIN")))
			return false;

		PQclear(res);
	}

	status = lo_export(pset.db, atooid(loid_arg), filename_arg);
	if (status != 1)
	{							/* of course this status is documented
								 * nowhere :( */
		fputs(PQerrorMessage(pset.db), stderr);
		if (own_transaction)
		{
			res = PQexec(pset.db, "ROLLBACK");
			PQclear(res);
		}
		return false;
	}

	if (own_transaction)
	{
		if (!(res = PSQLexec("COMMIT")))
		{
			res = PQexec(pset.db, "ROLLBACK");
			PQclear(res);
			return false;
		}

		PQclear(res);
	}

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
	bool		own_transaction = true;
	const char *var = GetVariable(pset.vars, "LO_TRANSACTION");

	if (var && strcmp(var, "nothing") == 0)
		own_transaction = false;

	if (!pset.db)
	{
		psql_error("\\lo_import: not connected to a database\n");
		return false;
	}

	if (own_transaction)
	{
		if (!handle_transaction())
			return false;

		if (!(res = PSQLexec("BEGIN")))
			return false;

		PQclear(res);
	}

	loid = lo_import(pset.db, filename_arg);
	if (loid == InvalidOid)
	{
		fputs(PQerrorMessage(pset.db), stderr);
		if (own_transaction)
		{
			res = PQexec(pset.db, "ROLLBACK");
			PQclear(res);
		}
		return false;
	}

	/* insert description if given */
	/* XXX don't try to hack pg_description if not superuser */
	/* XXX ought to replace this with some kind of COMMENT command */
	if (comment_arg && pset.issuper)
	{
		char	   *cmdbuf;
		char	   *bufptr;
		int			slen = strlen(comment_arg);

		cmdbuf = malloc(slen * 2 + 256);
		if (!cmdbuf)
		{
			if (own_transaction)
			{
				res = PQexec(pset.db, "ROLLBACK");
				PQclear(res);
			}
			return false;
		}
		sprintf(cmdbuf,
				"INSERT INTO pg_description VALUES ('%u', "
		   "(SELECT oid FROM pg_class WHERE relname = 'pg_largeobject'),"
				" 0, '", loid);
		bufptr = cmdbuf + strlen(cmdbuf);
		for (i = 0; i < slen; i++)
		{
			if (comment_arg[i] == '\'' || comment_arg[i] == '\\')
				*bufptr++ = '\\';
			*bufptr++ = comment_arg[i];
		}
		strcpy(bufptr, "')");

		if (!(res = PSQLexec(cmdbuf)))
		{
			if (own_transaction)
			{
				res = PQexec(pset.db, "ROLLBACK");
				PQclear(res);
			}
			free(cmdbuf);
			return false;
		}

		PQclear(res);
		free(cmdbuf);
	}

	if (own_transaction)
	{
		if (!(res = PSQLexec("COMMIT")))
		{
			res = PQexec(pset.db, "ROLLBACK");
			PQclear(res);
			return false;
		}

		PQclear(res);
	}


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
	bool		own_transaction = true;
	const char *var = GetVariable(pset.vars, "LO_TRANSACTION");

	if (var && strcmp(var, "nothing") == 0)
		own_transaction = false;

	if (!pset.db)
	{
		psql_error("\\lo_unlink: not connected to a database\n");
		return false;
	}

	if (own_transaction)
	{
		if (!handle_transaction())
			return false;

		if (!(res = PSQLexec("BEGIN")))
			return false;

		PQclear(res);
	}

	status = lo_unlink(pset.db, loid);
	if (status == -1)
	{
		fputs(PQerrorMessage(pset.db), stderr);
		if (own_transaction)
		{
			res = PQexec(pset.db, "ROLLBACK");
			PQclear(res);
		}
		return false;
	}

	/* remove the comment as well */
	/* XXX don't try to hack pg_description if not superuser */
	/* XXX ought to replace this with some kind of COMMENT command */
	if (pset.issuper)
	{
		sprintf(buf, "DELETE FROM pg_description WHERE objoid = '%u' "
				"AND classoid = (SELECT oid FROM pg_class WHERE relname = 'pg_largeobject')",
				loid);
		if (!(res = PSQLexec(buf)))
		{
			if (own_transaction)
			{
				res = PQexec(pset.db, "ROLLBACK");
				PQclear(res);
			}
			return false;
		}
	}

	if (own_transaction)
	{
		if (!(res = PSQLexec("COMMIT")))
		{
			res = PQexec(pset.db, "ROLLBACK");
			PQclear(res);
			return false;
		}
		PQclear(res);
	}


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
			 "SELECT loid as \"ID\", obj_description(loid, 'pg_largeobject') as \"%s\"\n"
			 "FROM (SELECT DISTINCT loid FROM pg_largeobject) x\n"
			 "ORDER BY \"ID\"",
			 gettext("Description"));

	res = PSQLexec(buf);
	if (!res)
		return false;

	myopt.topt.tuples_only = false;
	myopt.nullPrint = NULL;
	myopt.title = gettext("Large objects");

	printQuery(res, &myopt, pset.queryFout);

	PQclear(res);
	return true;
}
