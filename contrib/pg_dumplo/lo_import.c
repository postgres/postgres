/* -------------------------------------------------------------------------
 * pg_dumplo
 *
 * $PostgreSQL: pgsql/contrib/pg_dumplo/lo_import.c,v 1.11 2004/11/28 23:49:49 tgl Exp $
 *
 * Karel Zak 1999-2004
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
pglo_import(LODumpMaster * pgLO)
{
	LOlist		loa;
	Oid			new_oid;
	int		ret, line=0;
	char		tab[MAX_TABLE_NAME],
				attr[MAX_ATTR_NAME],
				sch[MAX_SCHEMA_NAME],
				path[BUFSIZ],
				lo_path[BUFSIZ],
				Qbuff[QUERY_BUFSIZ];

	while (fgets(Qbuff, QUERY_BUFSIZ, pgLO->index))
	{
		line++;
		
		if (*Qbuff == '#')
			continue;

		if (!pgLO->remove && !pgLO->quiet)
			printf(Qbuff);

		if ((ret=sscanf(Qbuff, "%u\t%s\t%s\t%s\t%s\n", &loa.lo_oid, tab, attr, path, sch)) < 5)
		{
			/* backward compatible mode */
			ret = sscanf(Qbuff, "%u\t%s\t%s\t%s\n", &loa.lo_oid, tab, attr, path);
			strcpy(sch, "public");
		}
		if (ret < 4)
		{
			fprintf(stderr, "%s: index file reading failed at line %d\n", progname, line);
			PQexec(pgLO->conn, "ROLLBACK");
			fprintf(stderr, "\n%s: ROLLBACK\n", progname);
			exit(RE_ERROR);
		}
		
		loa.lo_schema = sch;
		loa.lo_table = tab;
		loa.lo_attr = attr;

		if (path && *path=='/')
			/* absolute path */
			snprintf(lo_path, BUFSIZ, "%s", path);
		else
			snprintf(lo_path, BUFSIZ, "%s/%s", pgLO->space, path);

		/*
		 * Import LO
		 */
		if ((new_oid = lo_import(pgLO->conn, lo_path)) == 0)
		{

			fprintf(stderr, "%s: %s\n", progname, PQerrorMessage(pgLO->conn));

			PQexec(pgLO->conn, "ROLLBACK");
			fprintf(stderr, "\n%s: ROLLBACK\n", progname);
			exit(RE_ERROR);
		}

		if (pgLO->remove)
		{
			notice(pgLO, FALSE);
			if (lo_unlink(pgLO->conn, loa.lo_oid) < 0)
				fprintf(stderr, "%s: can't remove LO %u:\n%s",
						progname, loa.lo_oid, PQerrorMessage(pgLO->conn));

			else if (!pgLO->quiet)
				printf("remove old %u and create new %u\n",
					   loa.lo_oid, new_oid);
			notice(pgLO, TRUE);
		}

		pgLO->counter++;

		/*
		 * UPDATE oid in tab
		 */
		snprintf(Qbuff, QUERY_BUFSIZ,
				 "UPDATE \"%s\".\"%s\" SET \"%s\"=%u WHERE \"%s\"=%u",
			loa.lo_schema, loa.lo_table, loa.lo_attr, new_oid, loa.lo_attr, loa.lo_oid);

		/*fprintf(stderr, Qbuff);*/

		pgLO->res = PQexec(pgLO->conn, Qbuff);

		if (PQresultStatus(pgLO->res) != PGRES_COMMAND_OK)
		{
			fprintf(stderr, "%s: %s\n", progname, PQerrorMessage(pgLO->conn));
			PQclear(pgLO->res);
			PQexec(pgLO->conn, "ROLLBACK");
			fprintf(stderr, "\n%s: ROLLBACK\n", progname);
			exit(RE_ERROR);
		}
		PQclear(pgLO->res);
	}
}
