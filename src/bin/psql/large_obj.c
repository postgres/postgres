#include <c.h>
#include "large_obj.h"

#include <stdio.h>
#include <string.h>

#include <libpq-fe.h>
#include <postgres.h>

#include "settings.h"
#include "variables.h"
#include "common.h"
#include "print.h"


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
		if ((!commit && strcmp(notice, "NOTICE:  UserAbortTransactionBlock and not in in-progress state\n") != 0) ||
			(commit && strcmp(notice, "NOTICE:  EndTransactionBlock and not inprogress/abort state\n") != 0))
			fputs(notice, stderr);
	}
	else if (!QUIET())
	{
		if (commit)
			puts("Warning: Your transaction in progress has been committed.");
		else
			puts("Warning: Your transaction in progress has been rolled back.");
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
        if (!pset.cur_cmd_interactive)
            fprintf(stderr, "%s: ", pset.progname);
		fputs("\\lo_export: not connected to a database\n", stderr);
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

	status = lo_export(pset.db, atol(loid_arg), (char *) filename_arg);
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
	char		buf[1024];
	unsigned int i;
	bool		own_transaction = true;
	const char *var = GetVariable(pset.vars, "LO_TRANSACTION");

	if (var && strcmp(var, "nothing") == 0)
		own_transaction = false;

	if (!pset.db)
	{
        if (!pset.cur_cmd_interactive)
            fprintf(stderr, "%s: ", pset.progname);
		fputs("\\lo_import: not connected to a database\n", stderr);
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

	loid = lo_import(pset.db, (char *) filename_arg);
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
	if (comment_arg)
	{
		sprintf(buf, "INSERT INTO pg_description VALUES (%d, '", loid);
		for (i = 0; i < strlen(comment_arg); i++)
			if (comment_arg[i] == '\'')
				strcat(buf, "\\'");
			else
				strncat(buf, &comment_arg[i], 1);
		strcat(buf, "')");

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


	fprintf(pset.queryFout, "lo_import %d\n", loid);
    sprintf(buf, "%u", (unsigned int)loid);
    SetVariable(pset.vars, "LASTOID", buf);

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
	Oid			loid = (Oid) atol(loid_arg);
	char		buf[256];
	bool		own_transaction = true;
	const char *var = GetVariable(pset.vars, "LO_TRANSACTION");

	if (var && strcmp(var, "nothing") == 0)
		own_transaction = false;

	if (!pset.db)
	{
        if (!pset.cur_cmd_interactive)
            fprintf(stderr, "%s: ", pset.progname);
		fputs("\\lo_unlink: not connected to a database\n", stderr);
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
	sprintf(buf, "DELETE FROM pg_description WHERE objoid = %d", loid);
	if (!(res = PSQLexec(buf)))
	{
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


	fprintf(pset.queryFout, "lo_unlink %d\n", loid);

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

	strcpy(buf,
           "SELECT usename as \"Owner\", substring(relname from 5) as \"ID\",\n"
           "  obj_description(pg_class.oid) as \"Description\"\n"
           "FROM pg_class, pg_user\n"
		   "WHERE usesysid = relowner AND relkind = 'l'\n"
           "UNION\n"
           "SELECT NULL as \"Owner\", substring(relname from 5) as \"ID\",\n"
           "  obj_description(pg_class.oid) as \"Description\"\n"
           "FROM pg_class\n"
		   "WHERE not exists (select 1 from pg_user where usesysid = relowner) AND relkind = 'l'\n"
		   "ORDER BY \"ID\"");

	res = PSQLexec(buf);
	if (!res)
		return false;

	myopt.topt.tuples_only = false;
	myopt.nullPrint = NULL;
	myopt.title = "Large objects";

	printQuery(res, &myopt, pset.queryFout);

	PQclear(res);
	return true;
}
