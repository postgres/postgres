/*
 * pgmultiresult.c
 *
 */

#include <stdio.h>
#include "libpq-fe.h"
#include "../halt.h"
#include "libpgeasy.h"

int
main(int argc, char **argv)
{
	char		query[4000];
	char		val[4000];
	char		optstr[256];
	PGresult 	*res1, *res2;
	int			res1_done = 0, res2_done = 0;

	if (argc != 2)
		halt("Usage:  %s database\n", argv[0]);

	snprintf(optstr, 256, "dbname=%s", argv[1]);
	connectdb(optstr);

	doquery("\
		SELECT lanname \
		FROM pg_language \
		ORDER BY lanname \
		");
	res1 = get_result();

	doquery("\
		SELECT amname \
		FROM pg_am \
		ORDER BY amname \
		");
	res2 = get_result();

	while (!res1_done && !res2_done)
	{

		set_result(res1);

		if (!res1_done)
		{
			if (fetch(val) != END_OF_TUPLES)
				puts(val);
			else	res1_done = 1;
		}

		res1 = get_result();

		set_result(res2);

		if (!res2_done)
		{
			if (fetch(val) != END_OF_TUPLES)
				puts(val);
			else	res2_done = 1;
		}

		res2 = get_result();
	}

	disconnectdb();
	return 0;
}
