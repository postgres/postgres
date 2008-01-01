/*-------------------------------------------------------------------------
 *
 * pg_regress_main --- regression test for the main backend
 *
 * This is a C implementation of the previous shell script for running
 * the regression tests, and should be mostly compatible with it.
 * Initial author of C translation: Magnus Hagander
 *
 * This code is released under the terms of the PostgreSQL License.
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/test/regress/pg_regress_main.c,v 1.3 2008/01/01 19:46:00 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "pg_regress.h"

/*
 * start a psql test process for specified file (including redirection),
 * and return process ID
 */
static PID_TYPE
psql_start_test(const char *testname,
				_stringlist ** resultfiles,
				_stringlist ** expectfiles,
				_stringlist ** tags)
{
	PID_TYPE	pid;
	char		infile[MAXPGPATH];
	char		outfile[MAXPGPATH];
	char		expectfile[MAXPGPATH];
	char		psql_cmd[MAXPGPATH * 3];

	snprintf(infile, sizeof(infile), "%s/sql/%s.sql",
			 inputdir, testname);
	snprintf(outfile, sizeof(outfile), "%s/results/%s.out",
			 outputdir, testname);
	snprintf(expectfile, sizeof(expectfile), "%s/expected/%s.out",
			 inputdir, testname);

	add_stringlist_item(resultfiles, outfile);
	add_stringlist_item(expectfiles, expectfile);

	snprintf(psql_cmd, sizeof(psql_cmd),
			 SYSTEMQUOTE "\"%s%spsql\" -X -a -q -d \"%s\" < \"%s\" > \"%s\" 2>&1" SYSTEMQUOTE,
			 psqldir ? psqldir : "",
			 psqldir ? "/" : "",
			 dblist->str,
			 infile,
			 outfile);

	pid = spawn_process(psql_cmd);

	if (pid == INVALID_PID)
	{
		fprintf(stderr, _("could not start process for test %s\n"),
				testname);
		exit_nicely(2);
	}

	return pid;
}

static void
psql_init(void)
{
	/* set default regression database name */
	add_stringlist_item(&dblist, "regression");
}

int
main(int argc, char *argv[])
{
	return regression_main(argc, argv, psql_init, psql_start_test);
}
