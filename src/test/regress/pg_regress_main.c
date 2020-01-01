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
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/test/regress/pg_regress_main.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "pg_regress.h"

/*
 * start a psql test process for specified file (including redirection),
 * and return process ID
 */
static PID_TYPE
psql_start_test(const char *testname,
				_stringlist **resultfiles,
				_stringlist **expectfiles,
				_stringlist **tags)
{
	PID_TYPE	pid;
	char		infile[MAXPGPATH];
	char		outfile[MAXPGPATH];
	char		expectfile[MAXPGPATH];
	char		psql_cmd[MAXPGPATH * 3];
	size_t		offset = 0;
	char	   *appnameenv;

	/*
	 * Look for files in the output dir first, consistent with a vpath search.
	 * This is mainly to create more reasonable error messages if the file is
	 * not found.  It also allows local test overrides when running pg_regress
	 * outside of the source tree.
	 */
	snprintf(infile, sizeof(infile), "%s/sql/%s.sql",
			 outputdir, testname);
	if (!file_exists(infile))
		snprintf(infile, sizeof(infile), "%s/sql/%s.sql",
				 inputdir, testname);

	snprintf(outfile, sizeof(outfile), "%s/results/%s.out",
			 outputdir, testname);

	snprintf(expectfile, sizeof(expectfile), "%s/expected/%s.out",
			 outputdir, testname);
	if (!file_exists(expectfile))
		snprintf(expectfile, sizeof(expectfile), "%s/expected/%s.out",
				 inputdir, testname);

	add_stringlist_item(resultfiles, outfile);
	add_stringlist_item(expectfiles, expectfile);

	if (launcher)
	{
		offset += snprintf(psql_cmd + offset, sizeof(psql_cmd) - offset,
						   "%s ", launcher);
		if (offset >= sizeof(psql_cmd))
		{
			fprintf(stderr, _("command too long\n"));
			exit(2);
		}
	}

	/*
	 * Use HIDE_TABLEAM to hide different AMs to allow to use regression tests
	 * against different AMs without unnecessary differences.
	 */
	offset += snprintf(psql_cmd + offset, sizeof(psql_cmd) - offset,
					   "\"%s%spsql\" -X -a -q -d \"%s\" -v %s < \"%s\" > \"%s\" 2>&1",
					   bindir ? bindir : "",
					   bindir ? "/" : "",
					   dblist->str,
					   "HIDE_TABLEAM=\"on\"",
					   infile,
					   outfile);
	if (offset >= sizeof(psql_cmd))
	{
		fprintf(stderr, _("command too long\n"));
		exit(2);
	}

	appnameenv = psprintf("PGAPPNAME=pg_regress/%s", testname);
	putenv(appnameenv);

	pid = spawn_process(psql_cmd);

	if (pid == INVALID_PID)
	{
		fprintf(stderr, _("could not start process for test %s\n"),
				testname);
		exit(2);
	}

	unsetenv("PGAPPNAME");
	free(appnameenv);

	return pid;
}

static void
psql_init(int argc, char **argv)
{
	/* set default regression database name */
	add_stringlist_item(&dblist, "regression");
}

int
main(int argc, char *argv[])
{
	return regression_main(argc, argv, psql_init, psql_start_test);
}
