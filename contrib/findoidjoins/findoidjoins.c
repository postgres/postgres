/*
 * findoidjoins.c, requires src/interfaces/libpgeasy
 *
 */

#include <stdio.h>
#include <string.h>
#include "libpq-fe.h"

#include "halt.h"
#include "libpgeasy.h"

PGresult   *attres,
		   *relres;

int
main(int argc, char **argv)
{
	char		query[4000];
	char		relname[256];
	char		relname2[256];
	char		attname[256];
	char		typname[256];
	int			count;

	if (argc != 2)
		halt("Usage:  %s database\n", argv[0]);

	connectdb(argv[1], NULL, NULL, NULL, NULL);
	on_error_continue();
	on_error_stop();

	doquery("BEGIN WORK");
	doquery("\
		DECLARE c_attributes BINARY CURSOR FOR \
		SELECT typname, relname, a.attname \
		FROM pg_class c, pg_attribute a, pg_type t \
		WHERE a.attnum > 0 AND \
			  relkind = 'r' AND \
			  relhasrules = 'f' AND \
			  (typname = 'oid' OR \
			   typname = 'regproc') AND \
			  a.attrelid = c.oid AND \
			  a.atttypid = t.oid \
		ORDER BY 2, a.attnum ; \
		");
	doquery("FETCH ALL IN c_attributes");
	attres = get_result();

	doquery("\
		DECLARE c_relations BINARY CURSOR FOR \
		SELECT relname \
		FROM pg_class c \
		WHERE relkind = 'r' AND \
			  relhasrules = 'f' \
		ORDER BY 1; \
		");
	doquery("FETCH ALL IN c_relations");
	relres = get_result();

	set_result(attres);
	while (fetch(typname, relname, attname) != END_OF_TUPLES)
	{
		set_result(relres);
		reset_fetch();
		while (fetch(relname2) != END_OF_TUPLES)
		{
			unset_result(relres);
			if (strcmp(typname, "oid") == 0)
				sprintf(query, "\
					DECLARE c_matches BINARY CURSOR FOR \
					SELECT	count(*) \
						FROM %s t1, %s t2 \
					WHERE t1.%s = t2.oid ", relname, relname2, attname);
			else
				sprintf(query, "\
					DECLARE c_matches BINARY CURSOR FOR \
					SELECT	count(*) \
								FROM %s t1, %s t2 \
								WHERE RegprocToOid(t1.%s) = t2.oid ", relname, relname2, attname);

			doquery(query);
			doquery("FETCH ALL IN c_matches");
			fetch(&count);
			if (count != 0)
				printf("Join %s.%s => %s.oid\n", relname, attname, relname2);
			doquery("CLOSE c_matches");
			set_result(relres);
		}
		set_result(attres);
	}

	set_result(relres);
	doquery("CLOSE c_relations");
	PQclear(relres);

	set_result(attres);
	doquery("CLOSE c_attributes");
	PQclear(attres);
	unset_result(attres);

	doquery("COMMIT WORK");

	disconnectdb();
	return 0;
}
