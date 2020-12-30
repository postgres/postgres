/*-------------------------------------------------------------------------
 *
 * pg_regress_ecpg --- regression test driver for ecpg
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
 * src/interfaces/ecpg/test/pg_regress_ecpg.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "pg_regress.h"
#include "common/string.h"
#include "lib/stringinfo.h"


static void
ecpg_filter(const char *sourcefile, const char *outfile)
{
	/*
	 * Create a filtered copy of sourcefile, replacing #line x
	 * "./../bla/foo.h" with #line x "foo.h"
	 */
	FILE	   *s,
			   *t;
	StringInfoData linebuf;

	s = fopen(sourcefile, "r");
	if (!s)
	{
		fprintf(stderr, "Could not open file %s for reading\n", sourcefile);
		exit(2);
	}
	t = fopen(outfile, "w");
	if (!t)
	{
		fprintf(stderr, "Could not open file %s for writing\n", outfile);
		exit(2);
	}

	initStringInfo(&linebuf);

	while (pg_get_line_buf(s, &linebuf))
	{
		/* check for "#line " in the beginning */
		if (strstr(linebuf.data, "#line ") == linebuf.data)
		{
			char	   *p = strchr(linebuf.data, '"');
			int			plen = 1;

			while (*p && (*(p + plen) == '.' || strchr(p + plen, '/') != NULL))
			{
				plen++;
			}
			/* plen is one more than the number of . and / characters */
			if (plen > 1)
			{
				memmove(p + 1, p + plen, strlen(p + plen) + 1);
				/* we don't bother to fix up linebuf.len */
			}
		}
		fputs(linebuf.data, t);
	}

	pfree(linebuf.data);
	fclose(s);
	fclose(t);
}

/*
 * start an ecpg test process for specified file (including redirection),
 * and return process ID
 */

static PID_TYPE
ecpg_start_test(const char *testname,
				_stringlist **resultfiles,
				_stringlist **expectfiles,
				_stringlist **tags)
{
	PID_TYPE	pid;
	char		inprg[MAXPGPATH];
	char		insource[MAXPGPATH];
	StringInfoData testname_dash;
	char		outfile_stdout[MAXPGPATH],
				expectfile_stdout[MAXPGPATH];
	char		outfile_stderr[MAXPGPATH],
				expectfile_stderr[MAXPGPATH];
	char		outfile_source[MAXPGPATH],
				expectfile_source[MAXPGPATH];
	char		cmd[MAXPGPATH * 3];
	char	   *appnameenv;

	snprintf(inprg, sizeof(inprg), "%s/%s", inputdir, testname);
	snprintf(insource, sizeof(insource), "%s.c", testname);

	initStringInfo(&testname_dash);
	appendStringInfoString(&testname_dash, testname);
	replace_string(&testname_dash, "/", "-");

	snprintf(expectfile_stdout, sizeof(expectfile_stdout),
			 "%s/expected/%s.stdout",
			 outputdir, testname_dash.data);
	snprintf(expectfile_stderr, sizeof(expectfile_stderr),
			 "%s/expected/%s.stderr",
			 outputdir, testname_dash.data);
	snprintf(expectfile_source, sizeof(expectfile_source),
			 "%s/expected/%s.c",
			 outputdir, testname_dash.data);

	snprintf(outfile_stdout, sizeof(outfile_stdout),
			 "%s/results/%s.stdout",
			 outputdir, testname_dash.data);
	snprintf(outfile_stderr, sizeof(outfile_stderr),
			 "%s/results/%s.stderr",
			 outputdir, testname_dash.data);
	snprintf(outfile_source, sizeof(outfile_source),
			 "%s/results/%s.c",
			 outputdir, testname_dash.data);

	add_stringlist_item(resultfiles, outfile_stdout);
	add_stringlist_item(expectfiles, expectfile_stdout);
	add_stringlist_item(tags, "stdout");

	add_stringlist_item(resultfiles, outfile_stderr);
	add_stringlist_item(expectfiles, expectfile_stderr);
	add_stringlist_item(tags, "stderr");

	add_stringlist_item(resultfiles, outfile_source);
	add_stringlist_item(expectfiles, expectfile_source);
	add_stringlist_item(tags, "source");

	ecpg_filter(insource, outfile_source);

	snprintf(cmd, sizeof(cmd),
			 "\"%s\" >\"%s\" 2>\"%s\"",
			 inprg,
			 outfile_stdout,
			 outfile_stderr);

	appnameenv = psprintf("ecpg/%s", testname_dash.data);
	setenv("PGAPPNAME", appnameenv, 1);
	free(appnameenv);

	pid = spawn_process(cmd);

	if (pid == INVALID_PID)
	{
		fprintf(stderr, _("could not start process for test %s\n"),
				testname);
		exit(2);
	}

	unsetenv("PGAPPNAME");

	free(testname_dash.data);

	return pid;
}

static void
ecpg_init(int argc, char *argv[])
{
	/* nothing to do here at the moment */
}

int
main(int argc, char *argv[])
{
	return regression_main(argc, argv, ecpg_init, ecpg_start_test);
}
