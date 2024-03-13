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
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/ecpg/test/pg_regress_ecpg.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "common/string.h"
#include "lib/stringinfo.h"
#include "pg_regress.h"


/*
 * Create a filtered copy of sourcefile, removing any path
 * appearing in #line directives; for example, replace
 * #line x "./../bla/foo.h" with #line x "foo.h".
 * This is needed because the path part can vary depending
 * on compiler, platform, build options, etc.
 */
static void
ecpg_filter_source(const char *sourcefile, const char *outfile)
{
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
 * Remove the details of connection failure error messages
 * in a test result file, since the target host/pathname and/or port
 * can vary.  Rewrite the result file in-place.
 *
 * At some point it might be interesting to unify this with
 * ecpg_filter_source, but building a general pattern matcher
 * is no fun, nor does it seem desirable to introduce a
 * dependency on an external one.
 */
static void
ecpg_filter_stderr(const char *resultfile, const char *tmpfile)
{
	FILE	   *s,
			   *t;
	StringInfoData linebuf;

	s = fopen(resultfile, "r");
	if (!s)
	{
		fprintf(stderr, "Could not open file %s for reading\n", resultfile);
		exit(2);
	}
	t = fopen(tmpfile, "w");
	if (!t)
	{
		fprintf(stderr, "Could not open file %s for writing\n", tmpfile);
		exit(2);
	}

	initStringInfo(&linebuf);

	while (pg_get_line_buf(s, &linebuf))
	{
		char	   *p1 = strstr(linebuf.data, "connection to server ");

		if (p1)
		{
			char	   *p2 = strstr(p1, "failed: ");

			if (p2)
			{
				memmove(p1 + 21, p2, strlen(p2) + 1);
				/* we don't bother to fix up linebuf.len */
			}
		}
		fputs(linebuf.data, t);
	}

	pfree(linebuf.data);
	fclose(s);
	fclose(t);
	if (rename(tmpfile, resultfile) != 0)
	{
		fprintf(stderr, "Could not overwrite file %s with %s\n",
				resultfile, tmpfile);
		exit(2);
	}
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
	snprintf(insource, sizeof(insource), "%s/%s.c", inputdir, testname);

	/* make a version of the test name that has dashes in place of slashes */
	initStringInfo(&testname_dash);
	appendStringInfoString(&testname_dash, testname);
	for (char *c = testname_dash.data; *c != '\0'; c++)
	{
		if (*c == '/')
			*c = '-';
	}

	snprintf(expectfile_stdout, sizeof(expectfile_stdout),
			 "%s/expected/%s.stdout",
			 expecteddir, testname_dash.data);
	snprintf(expectfile_stderr, sizeof(expectfile_stderr),
			 "%s/expected/%s.stderr",
			 expecteddir, testname_dash.data);
	snprintf(expectfile_source, sizeof(expectfile_source),
			 "%s/expected/%s.c",
			 expecteddir, testname_dash.data);

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

	ecpg_filter_source(insource, outfile_source);

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
ecpg_postprocess_result(const char *filename)
{
	int			nlen = strlen(filename);

	/* Only stderr files require filtering, at the moment */
	if (nlen > 7 && strcmp(filename + nlen - 7, ".stderr") == 0)
	{
		char	   *tmpfile = psprintf("%s.tmp", filename);

		ecpg_filter_stderr(filename, tmpfile);
		pfree(tmpfile);
	}
}

static void
ecpg_init(int argc, char *argv[])
{
	/* nothing to do here at the moment */
}

int
main(int argc, char *argv[])
{
	return regression_main(argc, argv,
						   ecpg_init,
						   ecpg_start_test,
						   ecpg_postprocess_result);
}
