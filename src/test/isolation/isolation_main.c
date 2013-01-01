/*-------------------------------------------------------------------------
 *
 * isolation_main --- pg_regress test launcher for isolation tests
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/test/isolation/isolation_main.c
 *
 *-------------------------------------------------------------------------
 */

#include "pg_regress.h"

/*
 * start an isolation tester process for specified file (including
 * redirection), and return process ID
 */
static PID_TYPE
isolation_start_test(const char *testname,
					 _stringlist ** resultfiles,
					 _stringlist ** expectfiles,
					 _stringlist ** tags)
{
	PID_TYPE	pid;
	char		infile[MAXPGPATH];
	char		outfile[MAXPGPATH];
	char		expectfile[MAXPGPATH];
	char		psql_cmd[MAXPGPATH * 3];
	size_t		offset = 0;

	/*
	 * Look for files in the output dir first, consistent with a vpath search.
	 * This is mainly to create more reasonable error messages if the file is
	 * not found.  It also allows local test overrides when running pg_regress
	 * outside of the source tree.
	 */
	snprintf(infile, sizeof(infile), "%s/specs/%s.spec",
			 outputdir, testname);
	if (!file_exists(infile))
		snprintf(infile, sizeof(infile), "%s/specs/%s.spec",
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
		offset += snprintf(psql_cmd + offset, sizeof(psql_cmd) - offset,
						   "%s ", launcher);

	snprintf(psql_cmd + offset, sizeof(psql_cmd) - offset,
			 SYSTEMQUOTE "\"./isolationtester\" \"dbname=%s\" < \"%s\" > \"%s\" 2>&1" SYSTEMQUOTE,
			 dblist->str,
			 infile,
			 outfile);

	pid = spawn_process(psql_cmd);

	if (pid == INVALID_PID)
	{
		fprintf(stderr, _("could not start process for test %s\n"),
				testname);
		exit(2);
	}

	return pid;
}

static void
isolation_init(void)
{
	/* set default regression database name */
	add_stringlist_item(&dblist, "isolationtest");
}

int
main(int argc, char *argv[])
{
	return regression_main(argc, argv, isolation_init, isolation_start_test);
}
