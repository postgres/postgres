/* -------------------------------------------------------------------------
 * pg_dumplo
 *
 * $Header: /cvsroot/pgsql/contrib/pg_dumplo/Attic/lo_export.c,v 1.11 2002/09/05 21:01:16 tgl Exp $
 *
 *					Karel Zak 1999-2000
 * -------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "libpq-fe.h"
#include "libpq/libpq-fs.h"

#include "pg_dumplo.h"

extern int	errno;


void
load_lolist(LODumpMaster * pgLO)
{
	LOlist	   *ll;
	int			i;
	int			n;

	/*
	 * Now find any candidate tables who have columns of type oid.
	 *
	 * NOTE: System tables including pg_largeobject will be ignored.
	 * Otherwise we'd end up dumping all LOs, referenced or not.
	 *
	 * NOTE: the system oid column is ignored, as it has attnum < 1. This
	 * shouldn't matter for correctness, but it saves time.
	 */
	pgLO->res = PQexec(pgLO->conn,
					   "SELECT c.relname, a.attname "
					   "FROM pg_class c, pg_attribute a, pg_type t "
					   "WHERE a.attnum > 0 "
					   "    AND a.attrelid = c.oid "
					   "    AND a.atttypid = t.oid "
					   "    AND t.typname = 'oid' "
					   "    AND c.relkind = 'r' "
					   "    AND c.relname NOT LIKE 'pg_%'");

	if (PQresultStatus(pgLO->res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, "%s: Failed to get LO OIDs:\n%s", progname,
				PQerrorMessage(pgLO->conn));
		exit(RE_ERROR);
	}

	if ((n = PQntuples(pgLO->res)) == 0)
	{
		fprintf(stderr, "%s: No OID columns in the database.\n", progname);
		exit(RE_ERROR);
	}

	pgLO->lolist = (LOlist *) malloc((n + 1) * sizeof(LOlist));

	if (!pgLO->lolist)
	{
		fprintf(stderr, "%s: can't allocate memory\n", progname);
		exit(RE_ERROR);
	}

	for (i = 0, ll = pgLO->lolist; i < n; i++, ll++)
	{
		ll->lo_table = strdup(PQgetvalue(pgLO->res, i, 0));
		ll->lo_attr = strdup(PQgetvalue(pgLO->res, i, 1));
	}
	ll->lo_table = ll->lo_attr = (char *) NULL;

	PQclear(pgLO->res);
}

void
pglo_export(LODumpMaster * pgLO)
{
	LOlist	   *ll;
	int			tuples;
	char		path[BUFSIZ],
				Qbuff[QUERY_BUFSIZ];

	if (pgLO->action != ACTION_SHOW)
	{
		time_t		t;

		time(&t);
		fprintf(pgLO->index, "#\n# This is the PostgreSQL large object dump index\n#\n");
		fprintf(pgLO->index, "#\tDate:     %s", ctime(&t));
		fprintf(pgLO->index, "#\tHost:     %s\n", pgLO->host);
		fprintf(pgLO->index, "#\tDatabase: %s\n", pgLO->db);
		fprintf(pgLO->index, "#\tUser:     %s\n", pgLO->user);
		fprintf(pgLO->index, "#\n# oid\ttable\tattribut\tinfile\n#\n");
	}

	pgLO->counter = 0;

	for (ll = pgLO->lolist; ll->lo_table != NULL; ll++)
	{
		/*
		 * Query: find the LOs referenced by this column
		 */
		snprintf(Qbuff, QUERY_BUFSIZ,
				 "SELECT DISTINCT l.loid FROM \"%s\" x, pg_largeobject l WHERE x.\"%s\" = l.loid",
				 ll->lo_table, ll->lo_attr);

		/* puts(Qbuff); */

		pgLO->res = PQexec(pgLO->conn, Qbuff);

		if (PQresultStatus(pgLO->res) != PGRES_TUPLES_OK)
		{
			fprintf(stderr, "%s: Failed to get LO OIDs:\n%s", progname,
					PQerrorMessage(pgLO->conn));
		}
		else if ((tuples = PQntuples(pgLO->res)) == 0)
		{
			if (!pgLO->quiet && pgLO->action == ACTION_EXPORT_ATTR)
				printf("%s: no large objects in \"%s\".\"%s\"\n",
					   progname, ll->lo_table, ll->lo_attr);
		}
		else
		{

			int			t;
			char	   *val;

			/*
			 * Create DIR/FILE
			 */
			if (pgLO->action != ACTION_SHOW)
			{

				snprintf(path, BUFSIZ, "%s/%s/%s", pgLO->space, pgLO->db,
						 ll->lo_table);

				if (mkdir(path, DIR_UMASK) == -1)
				{
					if (errno != EEXIST)
					{
						perror(path);
						exit(RE_ERROR);
					}
				}

				snprintf(path, BUFSIZ, "%s/%s/%s/%s", pgLO->space, pgLO->db,
						 ll->lo_table, ll->lo_attr);

				if (mkdir(path, DIR_UMASK) == -1)
				{
					if (errno != EEXIST)
					{
						perror(path);
						exit(RE_ERROR);
					}
				}

				if (!pgLO->quiet)
					printf("dump %s.%s (%d large obj)\n",
						   ll->lo_table, ll->lo_attr, tuples);
			}

			pgLO->counter += tuples;

			for (t = 0; t < tuples; t++)
			{
				Oid			lo;

				val = PQgetvalue(pgLO->res, t, 0);

				lo = atooid(val);

				if (pgLO->action == ACTION_SHOW)
				{
					printf("%s.%s: %u\n", ll->lo_table, ll->lo_attr, lo);
					continue;
				}

				snprintf(path, BUFSIZ, "%s/%s/%s/%s/%s", pgLO->space,
						 pgLO->db, ll->lo_table, ll->lo_attr, val);

				if (lo_export(pgLO->conn, lo, path) < 0)
					fprintf(stderr, "%s: lo_export failed:\n%s", progname,
							PQerrorMessage(pgLO->conn));

				else
					fprintf(pgLO->index, "%s\t%s\t%s\t%s/%s/%s/%s\n", val,
							ll->lo_table, ll->lo_attr, pgLO->db, ll->lo_table, ll->lo_attr, val);
			}
		}

		PQclear(pgLO->res);
	}
}
