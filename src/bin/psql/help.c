/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2013, PostgreSQL Global Development Group
 *
 * src/bin/psql/help.c
 */
#include "postgres_fe.h"

#ifndef WIN32
#ifdef HAVE_PWD_H
#include <pwd.h>				/* for getpwuid() */
#endif
#include <sys/types.h>			/* (ditto) */
#include <unistd.h>				/* for geteuid() */
#else
#include <win32.h>
#endif

#ifndef WIN32
#include <sys/ioctl.h>			/* for ioctl() */
#endif

#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif

#include "common.h"
#include "help.h"
#include "input.h"
#include "settings.h"
#include "sql_help.h"


/*
 * PLEASE:
 * If you change something in this file, also make the same changes
 * in the DocBook documentation, file ref/psql-ref.sgml. If you don't
 * know how to do it, please find someone who can help you.
 */


/*
 * usage
 *
 * print out command line arguments
 */
#define ON(var) (var ? _("on") : _("off"))

void
usage(void)
{
	const char *env;
	const char *user;

#ifndef WIN32
	struct passwd *pw = NULL;
#endif

	/* Find default user, in case we need it. */
	user = getenv("PGUSER");
	if (!user)
	{
#if !defined(WIN32) && !defined(__OS2__)
		pw = getpwuid(geteuid());
		if (pw)
			user = pw->pw_name;
		else
		{
			psql_error("could not get current user name: %s\n", strerror(errno));
			exit(EXIT_FAILURE);
		}
#else							/* WIN32 */
		char		buf[128];
		DWORD		bufsize = sizeof(buf) - 1;

		if (GetUserName(buf, &bufsize))
			user = buf;
#endif   /* WIN32 */
	}

	printf(_("psql is the PostgreSQL interactive terminal.\n\n"));
	printf(_("Usage:\n"));
	printf(_("  psql [OPTION]... [DBNAME [USERNAME]]\n\n"));

	printf(_("General options:\n"));
	/* Display default database */
	env = getenv("PGDATABASE");
	if (!env)
		env = user;
	printf(_("  -c, --command=COMMAND    run only single command (SQL or internal) and exit\n"));
	printf(_("  -d, --dbname=DBNAME      database name to connect to (default: \"%s\")\n"), env);
	printf(_("  -f, --file=FILENAME      execute commands from file, then exit\n"));
	printf(_("  -l, --list               list available databases, then exit\n"));
	printf(_("  -v, --set=, --variable=NAME=VALUE\n"
			 "                           set psql variable NAME to VALUE\n"));
	printf(_("  -V, --version            output version information, then exit\n"));
	printf(_("  -X, --no-psqlrc          do not read startup file (~/.psqlrc)\n"));
	printf(_("  -1 (\"one\"), --single-transaction\n"
			 "                           execute as a single transaction (if non-interactive)\n"));
	printf(_("  -?, --help               show this help, then exit\n"));

	printf(_("\nInput and output options:\n"));
	printf(_("  -a, --echo-all           echo all input from script\n"));
	printf(_("  -e, --echo-queries       echo commands sent to server\n"));
	printf(_("  -E, --echo-hidden        display queries that internal commands generate\n"));
	printf(_("  -L, --log-file=FILENAME  send session log to file\n"));
	printf(_("  -n, --no-readline        disable enhanced command line editing (readline)\n"));
	printf(_("  -o, --output=FILENAME    send query results to file (or |pipe)\n"));
	printf(_("  -q, --quiet              run quietly (no messages, only query output)\n"));
	printf(_("  -s, --single-step        single-step mode (confirm each query)\n"));
	printf(_("  -S, --single-line        single-line mode (end of line terminates SQL command)\n"));

	printf(_("\nOutput format options:\n"));
	printf(_("  -A, --no-align           unaligned table output mode\n"));
	printf(_("  -F, --field-separator=STRING\n"
	   "                           set field separator (default: \"%s\")\n"),
		   DEFAULT_FIELD_SEP);
	printf(_("  -H, --html               HTML table output mode\n"));
	printf(_("  -P, --pset=VAR[=ARG]     set printing option VAR to ARG (see \\pset command)\n"));
	printf(_("  -R, --record-separator=STRING\n"
	"                           set record separator (default: newline)\n"));
	printf(_("  -t, --tuples-only        print rows only\n"));
	printf(_("  -T, --table-attr=TEXT    set HTML table tag attributes (e.g., width, border)\n"));
	printf(_("  -x, --expanded           turn on expanded table output\n"));
	printf(_("  -z, --field-separator-zero\n"
		   "                           set field separator to zero byte\n"));
	printf(_("  -0, --record-separator-zero\n"
		  "                           set record separator to zero byte\n"));

	printf(_("\nConnection options:\n"));
	/* Display default host */
	env = getenv("PGHOST");
	printf(_("  -h, --host=HOSTNAME      database server host or socket directory (default: \"%s\")\n"),
		   env ? env : _("local socket"));
	/* Display default port */
	env = getenv("PGPORT");
	printf(_("  -p, --port=PORT          database server port (default: \"%s\")\n"),
		   env ? env : DEF_PGPORT_STR);
	/* Display default user */
	env = getenv("PGUSER");
	if (!env)
		env = user;
	printf(_("  -U, --username=USERNAME  database user name (default: \"%s\")\n"), env);
	printf(_("  -w, --no-password        never prompt for password\n"));
	printf(_("  -W, --password           force password prompt (should happen automatically)\n"));

	printf(_("\nFor more information, type \"\\?\" (for internal commands) or \"\\help\" (for SQL\n"
			 "commands) from within psql, or consult the psql section in the PostgreSQL\n"
			 "documentation.\n\n"));
	printf(_("Report bugs to <pgsql-bugs@postgresql.org>.\n"));
}


/*
 * slashUsage
 *
 * print out help for the backslash commands
 */
void
slashUsage(unsigned short int pager)
{
	FILE	   *output;
	char	   *currdb;

	currdb = PQdb(pset.db);

	output = PageOutput(103, pager);

	/* if you add/remove a line here, change the row count above */

	fprintf(output, _("General\n"));
	fprintf(output, _("  \\copyright             show PostgreSQL usage and distribution terms\n"));
	fprintf(output, _("  \\g [FILE] or ;         execute query (and send results to file or |pipe)\n"));
	fprintf(output, _("  \\gset [PREFIX]         execute query and store results in psql variables\n"));
	fprintf(output, _("  \\h [NAME]              help on syntax of SQL commands, * for all commands\n"));
	fprintf(output, _("  \\q                     quit psql\n"));
	fprintf(output, _("  \\watch [SEC]           execute query every SEC seconds\n"));
	fprintf(output, "\n");

	fprintf(output, _("Query Buffer\n"));
	fprintf(output, _("  \\e [FILE] [LINE]       edit the query buffer (or file) with external editor\n"));
	fprintf(output, _("  \\ef [FUNCNAME [LINE]]  edit function definition with external editor\n"));
	fprintf(output, _("  \\p                     show the contents of the query buffer\n"));
	fprintf(output, _("  \\r                     reset (clear) the query buffer\n"));
#ifdef USE_READLINE
	fprintf(output, _("  \\s [FILE]              display history or save it to file\n"));
#endif
	fprintf(output, _("  \\w FILE                write query buffer to file\n"));
	fprintf(output, "\n");

	fprintf(output, _("Input/Output\n"));
	fprintf(output, _("  \\copy ...              perform SQL COPY with data stream to the client host\n"));
	fprintf(output, _("  \\echo [STRING]         write string to standard output\n"));
	fprintf(output, _("  \\i FILE                execute commands from file\n"));
	fprintf(output, _("  \\ir FILE               as \\i, but relative to location of current script\n"));
	fprintf(output, _("  \\o [FILE]              send all query results to file or |pipe\n"));
	fprintf(output, _("  \\qecho [STRING]        write string to query output stream (see \\o)\n"));
	fprintf(output, "\n");

	fprintf(output, _("Informational\n"));
	fprintf(output, _("  (options: S = show system objects, + = additional detail)\n"));
	fprintf(output, _("  \\d[S+]                 list tables, views, and sequences\n"));
	fprintf(output, _("  \\d[S+]  NAME           describe table, view, sequence, or index\n"));
	fprintf(output, _("  \\da[S]  [PATTERN]      list aggregates\n"));
	fprintf(output, _("  \\db[+]  [PATTERN]      list tablespaces\n"));
	fprintf(output, _("  \\dc[S+] [PATTERN]      list conversions\n"));
	fprintf(output, _("  \\dC[+]  [PATTERN]      list casts\n"));
	fprintf(output, _("  \\dd[S]  [PATTERN]      show object descriptions not displayed elsewhere\n"));
	fprintf(output, _("  \\ddp    [PATTERN]      list default privileges\n"));
	fprintf(output, _("  \\dD[S+] [PATTERN]      list domains\n"));
	fprintf(output, _("  \\det[+] [PATTERN]      list foreign tables\n"));
	fprintf(output, _("  \\des[+] [PATTERN]      list foreign servers\n"));
	fprintf(output, _("  \\deu[+] [PATTERN]      list user mappings\n"));
	fprintf(output, _("  \\dew[+] [PATTERN]      list foreign-data wrappers\n"));
	fprintf(output, _("  \\df[antw][S+] [PATRN]  list [only agg/normal/trigger/window] functions\n"));
	fprintf(output, _("  \\dF[+]  [PATTERN]      list text search configurations\n"));
	fprintf(output, _("  \\dFd[+] [PATTERN]      list text search dictionaries\n"));
	fprintf(output, _("  \\dFp[+] [PATTERN]      list text search parsers\n"));
	fprintf(output, _("  \\dFt[+] [PATTERN]      list text search templates\n"));
	fprintf(output, _("  \\dg[+]  [PATTERN]      list roles\n"));
	fprintf(output, _("  \\di[S+] [PATTERN]      list indexes\n"));
	fprintf(output, _("  \\dl                    list large objects, same as \\lo_list\n"));
	fprintf(output, _("  \\dL[S+] [PATTERN]      list procedural languages\n"));
	fprintf(output, _("  \\dm[S+] [PATTERN]      list materialized views\n"));
	fprintf(output, _("  \\dn[S+] [PATTERN]      list schemas\n"));
	fprintf(output, _("  \\do[S]  [PATTERN]      list operators\n"));
	fprintf(output, _("  \\dO[S+] [PATTERN]      list collations\n"));
	fprintf(output, _("  \\dp     [PATTERN]      list table, view, and sequence access privileges\n"));
	fprintf(output, _("  \\drds [PATRN1 [PATRN2]] list per-database role settings\n"));
	fprintf(output, _("  \\ds[S+] [PATTERN]      list sequences\n"));
	fprintf(output, _("  \\dt[S+] [PATTERN]      list tables\n"));
	fprintf(output, _("  \\dT[S+] [PATTERN]      list data types\n"));
	fprintf(output, _("  \\du[+]  [PATTERN]      list roles\n"));
	fprintf(output, _("  \\dv[S+] [PATTERN]      list views\n"));
	fprintf(output, _("  \\dE[S+] [PATTERN]      list foreign tables\n"));
	fprintf(output, _("  \\dx[+]  [PATTERN]      list extensions\n"));
	fprintf(output, _("  \\dy     [PATTERN]      list event triggers\n"));
	fprintf(output, _("  \\l[+]   [PATTERN]      list databases\n"));
	fprintf(output, _("  \\sf[+] FUNCNAME        show a function's definition\n"));
	fprintf(output, _("  \\z      [PATTERN]      same as \\dp\n"));
	fprintf(output, "\n");

	fprintf(output, _("Formatting\n"));
	fprintf(output, _("  \\a                     toggle between unaligned and aligned output mode\n"));
	fprintf(output, _("  \\C [STRING]            set table title, or unset if none\n"));
	fprintf(output, _("  \\f [STRING]            show or set field separator for unaligned query output\n"));
	fprintf(output, _("  \\H                     toggle HTML output mode (currently %s)\n"),
			ON(pset.popt.topt.format == PRINT_HTML));
	fprintf(output, _("  \\pset NAME [VALUE]     set table output option\n"
					  "                         (NAME := {format|border|expanded|fieldsep|fieldsep_zero|footer|null|\n"
					  "                         numericlocale|recordsep|recordsep_zero|tuples_only|title|tableattr|pager})\n"));
	fprintf(output, _("  \\t [on|off]            show only rows (currently %s)\n"),
			ON(pset.popt.topt.tuples_only));
	fprintf(output, _("  \\T [STRING]            set HTML <table> tag attributes, or unset if none\n"));
	fprintf(output, _("  \\x [on|off|auto]       toggle expanded output (currently %s)\n"),
		pset.popt.topt.expanded == 2 ? "auto" : ON(pset.popt.topt.expanded));
	fprintf(output, "\n");

	fprintf(output, _("Connection\n"));
	if (currdb)
		fprintf(output, _("  \\c[onnect] [DBNAME|- USER|- HOST|- PORT|-]\n"
						  "                         connect to new database (currently \"%s\")\n"),
				currdb);
	else
		fprintf(output, _("  \\c[onnect] [DBNAME|- USER|- HOST|- PORT|-]\n"
						  "                         connect to new database (currently no connection)\n"));
	fprintf(output, _("  \\encoding [ENCODING]   show or set client encoding\n"));
	fprintf(output, _("  \\password [USERNAME]   securely change the password for a user\n"));
	fprintf(output, _("  \\conninfo              display information about current connection\n"));
	fprintf(output, "\n");

	fprintf(output, _("Operating System\n"));
	fprintf(output, _("  \\cd [DIR]              change the current working directory\n"));
	fprintf(output, _("  \\setenv NAME [VALUE]   set or unset environment variable\n"));
	fprintf(output, _("  \\timing [on|off]       toggle timing of commands (currently %s)\n"),
			ON(pset.timing));
	fprintf(output, _("  \\! [COMMAND]           execute command in shell or start interactive shell\n"));
	fprintf(output, "\n");

	fprintf(output, _("Variables\n"));
	fprintf(output, _("  \\prompt [TEXT] NAME    prompt user to set internal variable\n"));
	fprintf(output, _("  \\set [NAME [VALUE]]    set internal variable, or list all if no parameters\n"));
	fprintf(output, _("  \\unset NAME            unset (delete) internal variable\n"));
	fprintf(output, "\n");

	fprintf(output, _("Large Objects\n"));
	fprintf(output, _("  \\lo_export LOBOID FILE\n"
					  "  \\lo_import FILE [COMMENT]\n"
					  "  \\lo_list\n"
					  "  \\lo_unlink LOBOID      large object operations\n"));

	ClosePager(output);
}



/*
 * helpSQL -- help with SQL commands
 *
 * Note: we assume caller removed any trailing spaces in "topic".
 */
void
helpSQL(const char *topic, unsigned short int pager)
{
#define VALUE_OR_NULL(a) ((a) ? (a) : "")

	if (!topic || strlen(topic) == 0)
	{
		/* Print all the available command names */
		int			screen_width;
		int			ncolumns;
		int			nrows;
		FILE	   *output;
		int			i;
		int			j;

#ifdef TIOCGWINSZ
		struct winsize screen_size;

		if (ioctl(fileno(stdout), TIOCGWINSZ, &screen_size) == -1)
			screen_width = 80;	/* ioctl failed, assume 80 */
		else
			screen_width = screen_size.ws_col;
#else
		screen_width = 80;		/* default assumption */
#endif

		ncolumns = (screen_width - 3) / (QL_MAX_CMD_LEN + 1);
		ncolumns = Max(ncolumns, 1);
		nrows = (QL_HELP_COUNT + (ncolumns - 1)) / ncolumns;

		output = PageOutput(nrows + 1, pager);

		fputs(_("Available help:\n"), output);

		for (i = 0; i < nrows; i++)
		{
			fprintf(output, "  ");
			for (j = 0; j < ncolumns - 1; j++)
				fprintf(output, "%-*s",
						QL_MAX_CMD_LEN + 1,
						VALUE_OR_NULL(QL_HELP[i + j * nrows].cmd));
			if (i + j * nrows < QL_HELP_COUNT)
				fprintf(output, "%s",
						VALUE_OR_NULL(QL_HELP[i + j * nrows].cmd));
			fputc('\n', output);
		}

		ClosePager(output);
	}
	else
	{
		int			i,
					j,
					x = 0;
		bool		help_found = false;
		FILE	   *output = NULL;
		size_t		len,
					wordlen;
		int			nl_count = 0;

		/*
		 * We first try exact match, then first + second words, then first
		 * word only.
		 */
		len = strlen(topic);

		for (x = 1; x <= 3; x++)
		{
			if (x > 1)			/* Nothing on first pass - try the opening
								 * word(s) */
			{
				wordlen = j = 1;
				while (topic[j] != ' ' && j++ < len)
					wordlen++;
				if (x == 2)
				{
					j++;
					while (topic[j] != ' ' && j++ <= len)
						wordlen++;
				}
				if (wordlen >= len)		/* Don't try again if the same word */
				{
					if (!output)
						output = PageOutput(nl_count, pager);
					break;
				}
				len = wordlen;
			}

			/* Count newlines for pager */
			for (i = 0; QL_HELP[i].cmd; i++)
			{
				if (pg_strncasecmp(topic, QL_HELP[i].cmd, len) == 0 ||
					strcmp(topic, "*") == 0)
				{
					nl_count += 5 + QL_HELP[i].nl_count;

					/* If we have an exact match, exit.  Fixes \h SELECT */
					if (pg_strcasecmp(topic, QL_HELP[i].cmd) == 0)
						break;
				}
			}

			if (!output)
				output = PageOutput(nl_count, pager);

			for (i = 0; QL_HELP[i].cmd; i++)
			{
				if (pg_strncasecmp(topic, QL_HELP[i].cmd, len) == 0 ||
					strcmp(topic, "*") == 0)
				{
					PQExpBufferData buffer;

					initPQExpBuffer(&buffer);
					QL_HELP[i].syntaxfunc(&buffer);
					help_found = true;
					fprintf(output, _("Command:     %s\n"
									  "Description: %s\n"
									  "Syntax:\n%s\n\n"),
							QL_HELP[i].cmd,
							_(QL_HELP[i].help),
							buffer.data);
					/* If we have an exact match, exit.  Fixes \h SELECT */
					if (pg_strcasecmp(topic, QL_HELP[i].cmd) == 0)
						break;
				}
			}
			if (help_found)		/* Don't keep trying if we got a match */
				break;
		}

		if (!help_found)
			fprintf(output, _("No help available for \"%s\".\nTry \\h with no arguments to see available help.\n"), topic);

		ClosePager(output);
	}
}



void
print_copyright(void)
{
	puts(
		 "PostgreSQL Database Management System\n"
		 "(formerly known as Postgres, then as Postgres95)\n\n"
		 "Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group\n\n"
		 "Portions Copyright (c) 1994, The Regents of the University of California\n\n"
	"Permission to use, copy, modify, and distribute this software and its\n"
		 "documentation for any purpose, without fee, and without a written agreement\n"
	 "is hereby granted, provided that the above copyright notice and this\n"
	   "paragraph and the following two paragraphs appear in all copies.\n\n"
		 "IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO ANY PARTY FOR\n"
		 "DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING\n"
		 "LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS\n"
		 "DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA HAS BEEN ADVISED OF THE\n"
		 "POSSIBILITY OF SUCH DAMAGE.\n\n"
	  "THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY WARRANTIES,\n"
		 "INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY\n"
		 "AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS\n"
		 "ON AN \"AS IS\" BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATIONS TO\n"
	"PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.\n"
		);
}
