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
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/ecpg/test/pg_regress_ecpg.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "pg_regress.h"

#define LINEBUFSIZE 300

static void
ecpg_filter(const char *sourcefile, const char *outfile)
{
	/*
	 * Create a filtered copy of sourcefile, replacing #line x
	 * "./../bla/foo.h" with #line x "foo.h"
	 */
	FILE	   *s,
			   *t;
	char		linebuf[LINEBUFSIZE];

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

	while (fgets(linebuf, LINEBUFSIZE, s))
	{
		/* check for "#line " in the beginning */
		if (strstr(linebuf, "#line ") == linebuf)
		{
			char	   *p = strchr(linebuf, '"');
			char	   *n;
			int			plen = 1;

			while (*p && (*(p + plen) == '.' || strchr(p + plen, '/') != NULL))
			{
				plen++;
			}
			/* plen is one more than the number of . and / characters */
			if (plen > 1)
			{
				n = (char *) malloc(plen);
				StrNCpy(n, p + 1, plen);
				replace_string(linebuf, n, "");
			}
		}
		fputs(linebuf, t);
	}
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
	char	   *outfile_stdout,
				expectfile_stdout[MAXPGPATH];
	char	   *outfile_stderr,
				expectfile_stderr[MAXPGPATH];
	char	   *outfile_source,
				expectfile_source[MAXPGPATH];
	char		cmd[MAXPGPATH * 3];
	char	   *testname_dash;
	char	   *appnameenv;

	snprintf(inprg, sizeof(inprg), "%s/%s", inputdir, testname);

	testname_dash = strdup(testname);
	replace_string(testname_dash, "/", "-");
	snprintf(expectfile_stdout, sizeof(expectfile_stdout),
			 "%s/expected/%s.stdout",
			 outputdir, testname_dash);
	snprintf(expectfile_stderr, sizeof(expectfile_stderr),
			 "%s/expected/%s.stderr",
			 outputdir, testname_dash);
	snprintf(expectfile_source, sizeof(expectfile_source),
			 "%s/expected/%s.c",
			 outputdir, testname_dash);

	/*
	 * We can use replace_string() here because the replacement string does
	 * not occupy more space than the replaced one.
	 */
	outfile_stdout = strdup(expectfile_stdout);
	replace_string(outfile_stdout, "/expected/", "/results/");
	outfile_stderr = strdup(expectfile_stderr);
	replace_string(outfile_stderr, "/expected/", "/results/");
	outfile_source = strdup(expectfile_source);
	replace_string(outfile_source, "/expected/", "/results/");

	add_stringlist_item(resultfiles, outfile_stdout);
	add_stringlist_item(expectfiles, expectfile_stdout);
	add_stringlist_item(tags, "stdout");

	add_stringlist_item(resultfiles, outfile_stderr);
	add_stringlist_item(expectfiles, expectfile_stderr);
	add_stringlist_item(tags, "stderr");

	add_stringlist_item(resultfiles, outfile_source);
	add_stringlist_item(expectfiles, expectfile_source);
	add_stringlist_item(tags, "source");

	snprintf(insource, sizeof(insource), "%s.c", testname);
	ecpg_filter(insource, outfile_source);

	snprintf(inprg, sizeof(inprg), "%s/%s", inputdir, testname);

	snprintf(cmd, sizeof(cmd),
			 "\"%s\" >\"%s\" 2>\"%s\"",
			 inprg,
			 outfile_stdout,
			 outfile_stderr);

	appnameenv = psprintf("PGAPPNAME=ecpg/%s", testname_dash);
	putenv(appnameenv);

	pid = spawn_process(cmd);

	if (pid == INVALID_PID)
	{
		fprintf(stderr, _("could not start process for test %s\n"),
				testname);
		exit(2);
	}

	unsetenv("PGAPPNAME");
	free(appnameenv);

	free(testname_dash);
	free(outfile_stdout);
	free(outfile_stderr);
	free(outfile_source);

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
