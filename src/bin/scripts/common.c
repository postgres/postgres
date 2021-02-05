/*-------------------------------------------------------------------------
 *
 *	common.c
 *		Common support routines for bin/scripts/
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/scripts/common.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <signal.h>
#include <unistd.h>

#include "common.h"
#include "common/connect.h"
#include "common/logging.h"
#include "common/string.h"
#include "fe_utils/cancel.h"
#include "fe_utils/query_utils.h"
#include "fe_utils/string_utils.h"

/*
 * Split TABLE[(COLUMNS)] into TABLE and [(COLUMNS)] portions.  When you
 * finish using them, pg_free(*table).  *columns is a pointer into "spec",
 * possibly to its NUL terminator.
 */
void
splitTableColumnsSpec(const char *spec, int encoding,
					  char **table, const char **columns)
{
	bool		inquotes = false;
	const char *cp = spec;

	/*
	 * Find the first '(' not identifier-quoted.  Based on
	 * dequote_downcase_identifier().
	 */
	while (*cp && (*cp != '(' || inquotes))
	{
		if (*cp == '"')
		{
			if (inquotes && cp[1] == '"')
				cp++;			/* pair does not affect quoting */
			else
				inquotes = !inquotes;
			cp++;
		}
		else
			cp += PQmblen(cp, encoding);
	}
	*table = pnstrdup(spec, cp - spec);
	*columns = cp;
}

/*
 * Break apart TABLE[(COLUMNS)] of "spec".  With the reset_val of search_path
 * in effect, have regclassin() interpret the TABLE portion.  Append to "buf"
 * the qualified name of TABLE, followed by any (COLUMNS).  Exit on failure.
 * We use this to interpret --table=foo under the search path psql would get,
 * in advance of "ANALYZE public.foo" under the always-secure search path.
 */
void
appendQualifiedRelation(PQExpBuffer buf, const char *spec,
						PGconn *conn, bool echo)
{
	char	   *table;
	const char *columns;
	PQExpBufferData sql;
	PGresult   *res;
	int			ntups;

	splitTableColumnsSpec(spec, PQclientEncoding(conn), &table, &columns);

	/*
	 * Query must remain ABSOLUTELY devoid of unqualified names.  This would
	 * be unnecessary given a regclassin() variant taking a search_path
	 * argument.
	 */
	initPQExpBuffer(&sql);
	appendPQExpBufferStr(&sql,
						 "SELECT c.relname, ns.nspname\n"
						 " FROM pg_catalog.pg_class c,"
						 " pg_catalog.pg_namespace ns\n"
						 " WHERE c.relnamespace OPERATOR(pg_catalog.=) ns.oid\n"
						 "  AND c.oid OPERATOR(pg_catalog.=) ");
	appendStringLiteralConn(&sql, table, conn);
	appendPQExpBufferStr(&sql, "::pg_catalog.regclass;");

	executeCommand(conn, "RESET search_path;", echo);

	/*
	 * One row is a typical result, as is a nonexistent relation ERROR.
	 * regclassin() unconditionally accepts all-digits input as an OID; if no
	 * relation has that OID; this query returns no rows.  Catalog corruption
	 * might elicit other row counts.
	 */
	res = executeQuery(conn, sql.data, echo);
	ntups = PQntuples(res);
	if (ntups != 1)
	{
		pg_log_error(ngettext("query returned %d row instead of one: %s",
							  "query returned %d rows instead of one: %s",
							  ntups),
					 ntups, sql.data);
		PQfinish(conn);
		exit(1);
	}
	appendPQExpBufferStr(buf,
						 fmtQualifiedId(PQgetvalue(res, 0, 1),
										PQgetvalue(res, 0, 0)));
	appendPQExpBufferStr(buf, columns);
	PQclear(res);
	termPQExpBuffer(&sql);
	pg_free(table);

	PQclear(executeQuery(conn, ALWAYS_SECURE_SEARCH_PATH_SQL, echo));
}


/*
 * Check yes/no answer in a localized way.  1=yes, 0=no, -1=neither.
 */

/* translator: abbreviation for "yes" */
#define PG_YESLETTER gettext_noop("y")
/* translator: abbreviation for "no" */
#define PG_NOLETTER gettext_noop("n")

bool
yesno_prompt(const char *question)
{
	char		prompt[256];

	/*------
	   translator: This is a question followed by the translated options for
	   "yes" and "no". */
	snprintf(prompt, sizeof(prompt), _("%s (%s/%s) "),
			 _(question), _(PG_YESLETTER), _(PG_NOLETTER));

	for (;;)
	{
		char	   *resp;

		resp = simple_prompt(prompt, true);

		if (strcmp(resp, _(PG_YESLETTER)) == 0)
		{
			free(resp);
			return true;
		}
		if (strcmp(resp, _(PG_NOLETTER)) == 0)
		{
			free(resp);
			return false;
		}
		free(resp);

		printf(_("Please answer \"%s\" or \"%s\".\n"),
			   _(PG_YESLETTER), _(PG_NOLETTER));
	}
}
