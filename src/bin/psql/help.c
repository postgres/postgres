#include <config.h>
#include <c.h>
#include "help.h"

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

#ifndef WIN32
#include <sys/ioctl.h>			/* for ioctl() */
#ifdef HAVE_PWD_H
#include <pwd.h>				/* for getpwuid() */
#endif
#include <sys/types.h>			/* (ditto) */
#include <unistd.h>				/* for getuid() */
#else
#define strcasecmp(x,y) stricmp(x,y)
#define popen(x,y) _popen(x,y)
#define pclose(x) _pclose(x)
#endif

#include <pqsignal.h>
#include <libpq-fe.h>

#include "settings.h"
#include "common.h"
#include "sql_help.h"


/*
 * usage
 *
 * print out command line arguments
 */
#define ON(var) (var ? "on" : "off")

void
usage(void)
{
	const char *env;
	const char *user;

#ifndef WIN32
	struct passwd *pw = NULL;

#endif

	/* Find default user, in case we need it. */
	user = getenv("USER");
	if (!user)
	{
#ifndef WIN32
		pw = getpwuid(getuid());
		if (pw)
			user = pw->pw_name;
		else
		{
			perror("getpwuid()");
			exit(EXIT_FAILURE);
		}
#else
		user = "?";
#endif
	}

/* If this " is the start of the string then it ought to end there to fit in 80 columns >> " */
    puts(  "This is psql, the PostgreSQL interactive terminal.");
	puts(  "\nUsage:");
    puts(  "  psql [options] [dbname [username]]");
    puts(  "\nOptions:");
	puts(  "  -A              Unaligned table output mode (-P format=unaligned");
	puts(  "  -c query        Run only single query (or slash command) and exit");

	/* Display default database */
	env = getenv("PGDATABASE");
	if (!env)
		env = user;
	printf("  -d dbname       Specify database name to connect to (default: %s)\n", env);

	puts(  "  -e              Echo all input in non-interactive mode");
	puts(  "  -E              Display queries that internal commands generate");
	puts(  "  -f filename     Execute queries from file, then exit");
	puts(  "  -F sep          Set field separator (default: \"" DEFAULT_FIELD_SEP "\") (-P fieldsep=)");

	/* Display default host */
	env = getenv("PGHOST");
	printf("  -h host         Specify database server host (default: ");
	if (env)
		fputs(env, stdout);
	else
		fputs("domain socket", stdout);
	puts(")");

	puts(  "  -H              HTML table output mode (-P format=html)");
	puts(  "  -l              List available databases, then exit");
	puts(  "  -n              Do not use readline or history");
	puts(  "  -o filename     Send query output to filename (or |pipe)");

	/* Display default port */
	env = getenv("PGPORT");
	printf("  -p port         Specify database server port (default: %s)\n",
           env ? env : "hardwired");

	puts(  "  -P var[=arg]    Set printing option 'var' to 'arg' (see \\pset command)");
	puts(  "  -q              Run quietly (no messages, only query output)");
	puts(  "  -s              Single step mode (confirm each query)");
	puts(  "  -S              Single line mode (newline terminates query)");
	puts(  "  -t              Don't print headings and row count (-P tuples_only)");
	puts(  "  -T text         Set HTML table tag options (width, border) (-P tableattr=)");

	/* Display default user */
	env = getenv("PGUSER");
	if (!env)
		env = user;
	printf("  -U [username]   Specifiy username, \"?\"=prompt (default user: %s)\n", env);

	puts(  "  -x              Turn on expanded table output (-P expanded)");
	puts(  "  -v name=val     Set psql variable 'name' to 'value'");
	puts(  "  -V              Show version information and exit");
	puts(  "  -W              Prompt for password (should happen automatically)");

    puts(  "\nFor more information, type \"\\?\" (for internal commands) or \"\\help\"");
    puts(  "(for SQL commands) from within psql, or consult the psql section in the");
    puts(  "PostgreSQL manual, which accompanies the distribution and is also available at");
    puts(  "<http://www.postgresql.org>.");
    puts(  "Report bugs to <bugs@postgresql.org>.");

#ifndef WIN32
	if (pw)
		free(pw);
#endif
}



/*
 * slashUsage
 *
 * print out help for the backslash commands
 */

#ifndef TIOCGWINSZ
struct winsize
{
	int			ws_row;
	int			ws_col;
};

#endif

void
slashUsage(void)
{
	bool		usePipe = false;
	const char *pagerenv;
	FILE	   *fout;
	struct winsize screen_size;

#ifdef TIOCGWINSZ
	if (pset.notty == 0 &&
		(ioctl(fileno(stdout), TIOCGWINSZ, &screen_size) == -1 ||
		 screen_size.ws_col == 0 ||
		 screen_size.ws_row == 0))
	{
#endif
		screen_size.ws_row = 24;
		screen_size.ws_col = 80;
#ifdef TIOCGWINSZ
	}
#endif

	if (pset.notty == 0 &&
		(pagerenv = getenv("PAGER")) &&
		(pagerenv[0] != '\0') &&
		screen_size.ws_row <= 36 &&
		(fout = popen(pagerenv, "w")))
	{
		usePipe = true;
#ifndef WIN32
		pqsignal(SIGPIPE, SIG_IGN);
#endif
	}
	else
		fout = stdout;

	/* if you add/remove a line here, change the row test above */
	fprintf(fout, " \\?             help\n");
	fprintf(fout, " \\c[onnect] [dbname|- [user|?]]\n"
                  "                 connect to new database (currently '%s')\n", PQdb(pset.db));
	fprintf(fout, " \\copy ...      perform SQL COPY with data stream to the client machine");
	fprintf(fout, " \\copyright     show PostgreSQL usage and distribution terms\n");
	fprintf(fout, " \\d <table>     describe table (or view, index, sequence)\n");
	fprintf(fout, " \\d{i|s|t|v|S}  list only indices/sequences/tables/views/system tables\n");
	fprintf(fout, " \\da            list aggregates\n");
	fprintf(fout, " \\dd [object]   list comment for table, type, function, or operator\n");
	fprintf(fout, " \\df            list functions\n");
	fprintf(fout, " \\do            list operators\n");
	fprintf(fout, " \\dT            list data types\n");
	fprintf(fout, " \\e [fname]     edit the current query buffer or <fname> with external editor\n");
	fprintf(fout, " \\echo <text>   write text to stdout\n");
	fprintf(fout, " \\g [fname]     send query to backend (and results in <fname> or |pipe)\n");
	fprintf(fout, " \\h [cmd]       help on syntax of sql commands, * for all commands\n");
	fprintf(fout, " \\i <fname>     read and execute queries from filename\n");
	fprintf(fout, " \\l             list all databases\n");
	fprintf(fout, " \\lo_export, \\lo_import, \\lo_list, \\lo_unlink\n"
                  "                 large object operations\n");
	fprintf(fout, " \\o [fname]     send all query results to <fname>, or |pipe\n");
	fprintf(fout, " \\p             show the content of the current query buffer\n");
	fprintf(fout, " \\pset [opt]    set table output options\n");
	fprintf(fout, " \\q             quit psql\n");
	fprintf(fout, " \\qecho <text>  write text to query output stream (see \\o)\n");
	fprintf(fout, " \\r             reset (clear) the query buffer\n");
	fprintf(fout, " \\s [fname]     print history or save it in <fname>\n");
	fprintf(fout, " \\set <var> <value>  set internal variable\n");
	fprintf(fout, " \\t             don't show table headers or footers (currently %s)\n", ON(pset.popt.topt.tuples_only));
	fprintf(fout, " \\unset <var>   unset (delete) internal variable\n");
	fprintf(fout, " \\x             toggle expanded output (currently %s)\n", ON(pset.popt.topt.expanded));
	fprintf(fout, " \\w <fname>     write current query buffer to a file\n");
	fprintf(fout, " \\z             list table access permissions\n");
	fprintf(fout, " \\! [cmd]       shell escape or command\n");

	if (usePipe)
	{
		pclose(fout);
#ifndef WIN32
		pqsignal(SIGPIPE, SIG_DFL);
#endif
	}
}



/*
 * helpSQL -- help with SQL commands
 *
 */
void
helpSQL(const char *topic)
{
	if (!topic || strlen(topic) == 0)
	{
		char		left_center_right;	/* Which column we're displaying */
		int			i;			/* Index into QL_HELP[] */

		puts("Available help:");

		left_center_right = 'L';/* Start with left column */
		i = 0;
		while (QL_HELP[i].cmd != NULL)
		{
			switch (left_center_right)
			{
				case 'L':
					printf("    %-25s", QL_HELP[i].cmd);
					left_center_right = 'C';
					break;
				case 'C':
					printf("%-25s", QL_HELP[i].cmd);
					left_center_right = 'R';
					break;
				case 'R':
					printf("%-25s\n", QL_HELP[i].cmd);
					left_center_right = 'L';
					break;
			}
			i++;
		}
		if (left_center_right != 'L')
			puts("\n");
	}

	else
	{
		int			i;
		bool		help_found = false;

		for (i = 0; QL_HELP[i].cmd; i++)
		{
			if (strcasecmp(QL_HELP[i].cmd, topic) == 0 ||
				strcmp(topic, "*") == 0)
			{
				help_found = true;
				printf("Command: %s\nDescription: %s\nSyntax:\n%s\n\n",
					 QL_HELP[i].cmd, QL_HELP[i].help, QL_HELP[i].syntax);
			}
		}

		if (!help_found)
			printf("No help available for '%s'.\nTry \\h with no arguments to see available help.\n", topic);
	}
}




void
print_copyright(void)
{
	puts(
		 "PostgreSQL Data Base Management System\n\n"
		 "Copyright(c) 1996 - 9 PostgreSQL Global Development Group\n\n"
		 "This software is based on Postgres95, formerly known as Postgres, which\n"
		 "contains the following notice:\n\n"
	  "Copyright(c) 1994 - 7 Regents of the University of California\n\n"
		 "Permission to use, copy, modify, and distribute this software and its\n"
		 "documentation for any purpose, without fee, and without a written agreement\n"
		 "is hereby granted, provided that the above copyright notice and this paragraph\n"
		 "and the following two paragraphs appear in all copies.\n\n"
		 "IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO ANY PARTY FOR\n"
		 "DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING LOST\n"
		 "PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN IF\n"
		 "THE UNIVERSITY OF CALIFORNIA HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH\n"
		 "DAMAGE.\n\n"
		 "THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,\n"
		 "BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A\n"
		 "PARTICULAR PURPOSE.THE SOFTWARE PROVIDED HEREUNDER IS ON AN \"AS IS\" BASIS,\n"
		 "AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATIONS TO PROVIDE MAINTENANCE,\n"
		 "SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.");
}
