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
 * print out command line arguments and exit
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

/* If string begins " here, then it ought to end there to fit on an 80 column terminal> > > > > > > " */
	fprintf(stderr, "Usage: psql [options] [dbname [username]] \n");
	fprintf(stderr, "    -A              Unaligned table output mode (-P format=unaligned)\n");
	fprintf(stderr, "    -c query        Run single query (slash commands, too) and exit\n");

	/* Display default database */
	env = getenv("PGDATABASE");
	if (!env)
		env = user;
	fprintf(stderr, "    -d dbname       Specify database name to connect to (default: %s)\n", env);

	fprintf(stderr, "    -e              Echo all input in non-interactive mode\n");
	fprintf(stderr, "    -E              Display queries that internal commands generate\n");
	fprintf(stderr, "    -f filename     Execute queries from file, then exit\n");
	fprintf(stderr, "    -F sep          Set field separator (default: '" DEFAULT_FIELD_SEP "') (-P fieldsep=)\n");

	/* Display default host */
	env = getenv("PGHOST");
	fprintf(stderr, "    -h host         Specify database server host (default: ");
	if (env)
		fprintf(stderr, env);
	else
		fprintf(stderr, "domain socket");
	fprintf(stderr, ")\n");

	fprintf(stderr, "    -H              HTML table output mode (-P format=html)\n");
	fprintf(stderr, "    -l              List available databases, then exit\n");
	fprintf(stderr, "    -n              Do not use readline and history\n");
	fprintf(stderr, "    -o filename     Send query output to filename (or |pipe)\n");

	/* Display default port */
	env = getenv("PGPORT");
	fprintf(stderr, "    -p port         Specify database server port (default: %s)\n",
			env ? env : "hardwired");

	fprintf(stderr, "    -P var[=arg]    Set printing option 'var' to 'arg'. (see \\pset command)\n");
	fprintf(stderr, "    -q              Run quietly (no messages, no prompts)\n");
	fprintf(stderr, "    -s              Single step mode (confirm each query)\n");
	fprintf(stderr, "    -S              Single line mode (newline sends query)\n");
	fprintf(stderr, "    -t              Don't print headings and row count (-P tuples_only)\n");
	fprintf(stderr, "    -T text         Set HTML table tag options (e.g., width, border)\n");
	fprintf(stderr, "    -u              Prompt for username and password (same as \"-U ? -W\")\n");

	/* Display default user */
	env = getenv("PGUSER");
	if (!env)
		env = user;
	fprintf(stderr, "    -U [username]   Specifiy username, \"?\"=prompt (default user: %s)\n", env);

	fprintf(stderr, "    -x              Turn on expanded table output (-P expanded)\n");
	fprintf(stderr, "    -v name=val     Set psql variable 'name' to 'value'\n");
	fprintf(stderr, "    -V              Show version information and exit\n");
	fprintf(stderr, "    -W              Prompt for password (should happen automatically)\n");

	fprintf(stderr, "Consult the documentation for the complete details.\n");

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
slashUsage(PsqlSettings *pset)
{
	bool		usePipe = false;
	const char *pagerenv;
	FILE	   *fout;
	struct winsize screen_size;

#ifdef TIOCGWINSZ
	if (pset->notty == 0 &&
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

	if (pset->notty == 0 &&
		(pagerenv = getenv("PAGER")) &&
		(pagerenv[0] != '\0') &&
		screen_size.ws_row <= 36 &&
		(fout = popen(pagerenv, "w")))
	{
		usePipe = true;
		pqsignal(SIGPIPE, SIG_IGN);
	}
	else
		fout = stdout;

	/* if you add/remove a line here, change the row test above */
	fprintf(fout, " \\?           -- help\n");
	fprintf(fout, " \\c[onnect] [<dbname>|- [<user>|?]] -- connect to new database (currently '%s')\n", PQdb(pset->db));
	fprintf(fout, " \\copy [binary] <table> [with oids] {from|to} <fname> [with delimiters '<char>']\n");
	fprintf(fout, " \\copyright   -- show PostgreSQL copyright\n");
	fprintf(fout, " \\d           -- list tables, views, and sequences\n");
	fprintf(fout, " \\distvS      -- list only indices/sequences/tables/views/system tables\n");
	fprintf(fout, " \\da          -- list aggregates\n");
	fprintf(fout, " \\dd [<object>]- list comment for table, type, function, or operator\n");
	fprintf(fout, " \\df          -- list functions\n");
	fprintf(fout, " \\do          -- list operators\n");
	fprintf(fout, " \\dT          -- list data types\n");
	fprintf(fout, " \\e [<fname>] -- edit the current query buffer or <fname> with external editor\n");
	fprintf(fout, " \\echo <text> -- write text to stdout\n");
	fprintf(fout, " \\g [<fname>] -- send query to backend (and results in <fname> or |pipe)\n");
	fprintf(fout, " \\h [<cmd>]   -- help on syntax of sql commands, * for all commands\n");
	fprintf(fout, " \\i <fname>   -- read and execute queries from filename\n");
	fprintf(fout, " \\l           -- list all databases\n");
	fprintf(fout, " \\lo_export, \\lo_import, \\lo_list, \\lo_unlink -- large object operations\n");
	fprintf(fout, " \\o [<fname>] -- send all query results to <fname>, or |pipe\n");
	fprintf(fout, " \\p           -- print the content of the current query buffer\n");
	fprintf(fout, " \\pset        -- set table output options\n");
	fprintf(fout, " \\q           -- quit\n");
	fprintf(fout, " \\qecho <text>-- write text to query output stream (see \\o)\n");
	fprintf(fout, " \\r           -- reset (clear) the query buffer\n");
	fprintf(fout, " \\s [<fname>] -- print history or save it in <fname>\n");
	fprintf(fout, " \\set <var> [<value>] -- set/unset internal variable\n");
	fprintf(fout, " \\t           -- don't show table headers or footers (currently %s)\n", ON(pset->popt.topt.tuples_only));
	fprintf(fout, " \\x           -- toggle expanded output (currently %s)\n", ON(pset->popt.topt.expanded));
	fprintf(fout, " \\w <fname>   -- write current query buffer to a file\n");
	fprintf(fout, " \\z           -- list table access permissions\n");
	fprintf(fout, " \\! [<cmd>]   -- shell escape or command\n");

	if (usePipe)
	{
		pclose(fout);
		pqsignal(SIGPIPE, SIG_DFL);
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

		puts("Syntax: \\h <cmd> or \\help <cmd>, where <cmd> is one of the following:");

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
		puts("Or type \\h * for a complete description of all commands.");
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
		 "
		 PostgreSQL Data Base Management System

		 Copyright(c) 1996 - 9 PostgreSQL Global Development Group

		 This software is based on Postgres95, formerly known as Postgres, which
		 contains the following notice:

		 Copyright(c) 1994 - 7 Regents of the University of California

	Permission to use, copy, modify, and distribute this software and its
		 documentation for any purpose, without fee, and without a written agreement
		 is hereby granted, provided that the above copyright notice and this paragraph
		 and the following two paragraphs appear in all copies.

		 IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO ANY PARTY FOR
		 DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING LOST
		 PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN IF
		 THE UNIVERSITY OF CALIFORNIA HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH
		 DAMAGE.

		 THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
		 BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
		 PARTICULAR PURPOSE.THE SOFTWARE PROVIDED HEREUNDER IS ON AN \ "AS IS\"  BASIS,
		 AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATIONS TO PROVIDE MAINTENANCE,
		 SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.

		 (end of terms) "
	);
}
