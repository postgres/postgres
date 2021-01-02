/*
 * uri-regress.c
 *		A test program for libpq URI format
 *
 * This is a helper for libpq conninfo regression testing.  It takes a single
 * conninfo string as a parameter, parses it using PQconninfoParse, and then
 * prints out the values from the parsed PQconninfoOption struct that differ
 * from the defaults (obtained from PQconndefaults).
 *
 * Portions Copyright (c) 2012-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/interfaces/libpq/test/uri-regress.c
 */

#include "postgres_fe.h"

#include "libpq-fe.h"

int
main(int argc, char *argv[])
{
	PQconninfoOption *opts;
	PQconninfoOption *defs;
	PQconninfoOption *opt;
	PQconninfoOption *def;
	char	   *errmsg = NULL;
	bool		local = true;

	if (argc != 2)
		return 1;

	opts = PQconninfoParse(argv[1], &errmsg);
	if (opts == NULL)
	{
		fprintf(stderr, "uri-regress: %s", errmsg);
		return 1;
	}

	defs = PQconndefaults();
	if (defs == NULL)
	{
		fprintf(stderr, "uri-regress: cannot fetch default options\n");
		return 1;
	}

	/*
	 * Loop on the options, and print the value of each if not the default.
	 *
	 * XXX this coding assumes that PQconninfoOption structs always have the
	 * keywords in the same order.
	 */
	for (opt = opts, def = defs; opt->keyword; ++opt, ++def)
	{
		if (opt->val != NULL)
		{
			if (def->val == NULL || strcmp(opt->val, def->val) != 0)
				printf("%s='%s' ", opt->keyword, opt->val);

			/*
			 * Try to detect if this is a Unix-domain socket or inet.  This is
			 * a bit grotty but it's the same thing that libpq itself does.
			 *
			 * Note that we directly test for '/' instead of using
			 * is_absolute_path, as that would be considerably more messy.
			 * This would fail on Windows, but that platform doesn't have
			 * Unix-domain sockets anyway.
			 */
			if (*opt->val &&
				(strcmp(opt->keyword, "hostaddr") == 0 ||
				 (strcmp(opt->keyword, "host") == 0 && *opt->val != '/')))
			{
				local = false;
			}
		}
	}

	if (local)
		printf("(local)\n");
	else
		printf("(inet)\n");

	return 0;
}
