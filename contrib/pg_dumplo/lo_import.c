/* -------------------------------------------------------------------------
 * pg_dumplo
 *
 * $Header: /cvsroot/pgsql/contrib/pg_dumplo/Attic/lo_import.c,v 1.9 2002/09/05 21:01:16 tgl Exp $
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
pglo_import(LODumpMaster * pgLO)
{
	LOlist		loa;
	Oid			new_oid;
	char		tab[MAX_TABLE_NAME],
				attr[MAX_ATTR_NAME],
				path[BUFSIZ],
				lo_path[BUFSIZ],
				Qbuff[QUERY_BUFSIZ];

	while (fgets(Qbuff, QUERY_BUFSIZ, pgLO->index))
	{

		if (*Qbuff == '#')
			continue;

		if (!pgLO->remove && !pgLO->quiet)
			printf(Qbuff);

		sscanf(Qbuff, "%u\t%s\t%s\t%s\n", &loa.lo_oid, tab, attr, path);
		loa.lo_table = tab;
		loa.lo_attr = attr;

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
				 "UPDATE \"%s\" SET \"%s\"=%u WHERE \"%s\"=%u",
			loa.lo_table, loa.lo_attr, new_oid, loa.lo_attr, loa.lo_oid);

		/* fprintf(stderr, Qbuff); */

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
