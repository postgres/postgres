/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2003, PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/help.c,v 1.81 2003/10/02 06:39:31 petere Exp $
 */
#include "postgres_fe.h"
#include "common.h"
#include "print.h"
#include "help.h"

#include <signal.h>
#include <errno.h>

#ifndef WIN32
#ifdef HAVE_PWD_H
#include <pwd.h>				/* for getpwuid() */
#endif
#include <sys/types.h>			/* (ditto) */
#include <unistd.h>				/* for getuid() */
#else
#include <win32.h>
#endif

#include "pqsignal.h"
#include "libpq-fe.h"

#include "settings.h"
#include "common.h"
#include "sql_help.h"

#define _(x) gettext((x))

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
#ifndef WIN32
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

/* If this " is the start of the string then it ought to end there to fit in 80 columns >> " */
	printf(_("This is psql %s, the PostgreSQL interactive terminal.\n\n"),
		   PG_VERSION);
	puts(_("Usage:"));
	puts(_("  psql [OPTIONS]... [DBNAME [USERNAME]]\n"));

	puts(_("General options:"));
	/* Display default database */
	env = getenv("PGDATABASE");
	if (!env)
		env = user;
	printf(_("  -d DBNAME       specify database name to connect to (default: \"%s\")\n"), env);
	puts(_("  -c COMMAND      run only single command (SQL or internal) and exit"));
	puts(_("  -f FILENAME     execute commands from file, then exit"));
	puts(_("  -l              list available databases, then exit"));
	puts(_("  -v NAME=VALUE   set psql variable NAME to VALUE"));
	puts(_("  -X              do not read startup file (~/.psqlrc)"));
	puts(_("  --help          show this help, then exit"));
	puts(_("  --version       output version information, then exit"));

	puts(_("\nInput and output options:"));
	puts(_("  -a              echo all input from script"));
	puts(_("  -e              echo commands sent to server"));
	puts(_("  -E              display queries that internal commands generate"));
	puts(_("  -q              run quietly (no messages, only query output)"));
	puts(_("  -o FILENAME     send query results to file (or |pipe)"));
	puts(_("  -n              disable enhanced command line editing (readline)"));
	puts(_("  -s              single-step mode (confirm each query)"));
	puts(_("  -S              single-line mode (end of line terminates SQL command)"));

	puts(_("\nOutput format options:"));
	puts(_("  -A              unaligned table output mode (-P format=unaligned)"));
	puts(_("  -H              HTML table output mode (-P format=html)"));
	puts(_("  -t              print rows only (-P tuples_only)"));
	puts(_("  -T TEXT         set HTML table tag attributes (width, border) (-P tableattr=)"));
	puts(_("  -x              turn on expanded table output (-P expanded)"));
	puts(_("  -P VAR[=ARG]    set printing option VAR to ARG (see \\pset command)"));
	printf(_("  -F STRING       set field separator (default: \"%s\") (-P fieldsep=)\n"),
		   DEFAULT_FIELD_SEP);
	puts(_("  -R STRING       set record separator (default: newline) (-P recordsep=)"));

	puts(_("\nConnection options:"));
	/* Display default host */
	env = getenv("PGHOST");
	printf(_("  -h HOSTNAME     database server host or socket directory (default: \"%s\")\n"),
		   env ? env : _("local socket"));
	/* Display default port */
	env = getenv("PGPORT");
	printf(_("  -p PORT         database server port (default: \"%s\")\n"),
		   env ? env : DEF_PGPORT_STR);
	/* Display default user */
	env = getenv("PGUSER");
	if (!env)
		env = user;
	printf(_("  -U NAME         database user name (default: \"%s\")\n"), env);
	puts(_("  -W              prompt for password (should happen automatically)"));

	puts(_(
		   "\nFor more information, type \"\\?\" (for internal commands) or \"\\help\"\n"
		   "(for SQL commands) from within psql, or consult the psql section in\n"
		   "the PostgreSQL documentation.\n\n"
		   "Report bugs to <pgsql-bugs@postgresql.org>."));
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
slashUsage(unsigned short int pager)
{
	FILE	   *output;

	output = PageOutput(67, pager);

	/* if you add/remove a line here, change the row count above */

	/*
	 * if this " is the start of the string then it ought to end there to
	 * fit in 80 columns >> "
	 */
	fprintf(output, _("General\n"));
	fprintf(output, _("  \\c[onnect] [DBNAME|- [USER]]\n"
		"                 connect to new database (currently \"%s\")\n"),
			PQdb(pset.db));
	fprintf(output, _("  \\cd [DIR]      change the current working directory\n"));
	fprintf(output, _("  \\copyright     show PostgreSQL usage and distribution terms\n"));
	fprintf(output, _("  \\encoding [ENCODING]\n"
					  "                 show or set client encoding\n"));
	fprintf(output, _("  \\h [NAME]      help on syntax of SQL commands, * for all commands\n"));
	fprintf(output, _("  \\q             quit psql\n"));
	fprintf(output, _("  \\set [NAME [VALUE]]\n"
					  "                 set internal variable, or list all if no parameters\n"));
	fprintf(output, _("  \\timing        toggle timing of commands (currently %s)\n"),
			ON(pset.timing));
	fprintf(output, _("  \\unset NAME    unset (delete) internal variable\n"));
	fprintf(output, _("  \\! [COMMAND]   execute command in shell or start interactive shell\n"));
	fprintf(output, "\n");

	fprintf(output, _("Query Buffer\n"));
	fprintf(output, _("  \\e [FILE]      edit the query buffer (or file) with external editor\n"));
	fprintf(output, _("  \\g [FILE]      send query buffer to server (and results to file or |pipe)\n"));
	fprintf(output, _("  \\p             show the contents of the query buffer\n"));
	fprintf(output, _("  \\r             reset (clear) the query buffer\n"));
	fprintf(output, _("  \\s [FILE]      display history or save it to file\n"));
	fprintf(output, _("  \\w [FILE]      write query buffer to file\n"));
	fprintf(output, "\n");

	fprintf(output, _("Input/Output\n"));
	fprintf(output, _("  \\echo [STRING] write string to standard output\n"));
	fprintf(output, _("  \\i FILE        execute commands from file\n"));
	fprintf(output, _("  \\o [FILE]      send all query results to file or |pipe\n"));
	fprintf(output, _("  \\qecho [STRING]\n"
	"                 write string to query output stream (see \\o)\n"));
	fprintf(output, "\n");

	fprintf(output, _("Informational\n"));
	fprintf(output, _("  \\d [NAME]      describe table, index, sequence, or view\n"));
	fprintf(output, _("  \\d{t|i|s|v|S} [PATTERN] (add \"+\" for more detail)\n"
					  "                 list tables/indexes/sequences/views/system tables\n"));
	fprintf(output, _("  \\da [PATTERN]  list aggregate functions\n"));
	fprintf(output, _("  \\dc [PATTERN]  list conversions\n"));
	fprintf(output, _("  \\dC            list casts\n"));
	fprintf(output, _("  \\dd [PATTERN]  show comment for object\n"));
	fprintf(output, _("  \\dD [PATTERN]  list domains\n"));
	fprintf(output, _("  \\df [PATTERN]  list functions (add \"+\" for more detail)\n"));
	fprintf(output, _("  \\dn [PATTERN]  list schemas\n"));
	fprintf(output, _("  \\do [NAME]     list operators\n"));
	fprintf(output, _("  \\dl            list large objects, same as \\lo_list\n"));
	fprintf(output, _("  \\dp [PATTERN]  list table access privileges\n"));
	fprintf(output, _("  \\dT [PATTERN]  list data types (add \"+\" for more detail)\n"));
	fprintf(output, _("  \\du [PATTERN]  list users\n"));
	fprintf(output, _("  \\l             list all databases (add \"+\" for more detail)\n"));
	fprintf(output, _("  \\z [PATTERN]   list table access privileges (same as \\dp)\n"));
	fprintf(output, "\n");

	fprintf(output, _("Formatting\n"));
	fprintf(output, _("  \\a             toggle between unaligned and aligned output mode\n"));
	fprintf(output, _("  \\C [STRING]    set table title, or unset if none\n"));
	fprintf(output, _("  \\f [STRING]    show or set field separator for unaligned query output\n"));
	fprintf(output, _("  \\H             toggle HTML output mode (currently %s)\n"),
			ON(pset.popt.topt.format == PRINT_HTML));
	fprintf(output, _("  \\pset NAME [VALUE]\n"
					  "                 set table output option\n"
					  "                 (NAME := {format|border|expanded|fieldsep|footer|null|\n"
			  "                 recordsep|tuples_only|title|tableattr|pager})\n"));
	fprintf(output, _("  \\t             show only rows (currently %s)\n"),
			ON(pset.popt.topt.tuples_only));
	fprintf(output, _("  \\T [STRING]    set HTML <table> tag attributes, or unset if none\n"));
	fprintf(output, _("  \\x             toggle expanded output (currently %s)\n"),
			ON(pset.popt.topt.expanded));
	fprintf(output, "\n");

	fprintf(output, _("Copy, Large Object\n"));
	fprintf(output, _("  \\copy ...      perform SQL COPY with data stream to the client host\n"));
	fprintf(output, _("  \\lo_export\n"
					  "  \\lo_import\n"
					  "  \\lo_list\n"
					  "  \\lo_unlink     large object operations\n"));

	if (output != stdout)
	{
		pclose(output);
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
helpSQL(const char *topic, unsigned short int pager)
{
#define VALUE_OR_NULL(a) ((a) ? (a) : "")

	if (!topic || strlen(topic) == 0)
	{
		int			i;
		int			items_per_column = (QL_HELP_COUNT + 2) / 3;
		FILE	   *output;

		output = PageOutput(items_per_column + 1, pager);

		fputs(_("Available help:\n"), output);

		for (i = 0; i < items_per_column; i++)
		{
			fprintf(output, "  %-26s%-26s",
					VALUE_OR_NULL(QL_HELP[i].cmd),
					VALUE_OR_NULL(QL_HELP[i + items_per_column].cmd));
			if (i + 2 * items_per_column < QL_HELP_COUNT)
				fprintf(output, "%-26s",
				   VALUE_OR_NULL(QL_HELP[i + 2 * items_per_column].cmd));
			fputc('\n', output);
		}
		/* Only close if we used the pager */
		if (output != stdout)
		{
			pclose(output);
#ifndef WIN32
			pqsignal(SIGPIPE, SIG_DFL);
#endif
		}
	}
	else
	{
		int			i;
		bool		help_found = false;
		FILE	   *output;
		size_t		len;
		int			nl_count = 0;
		char		*ch;
		
		/* don't care about trailing spaces */
		len = strlen(topic);
		while (topic[len - 1] == ' ')
			len--;

		/* Count newlines for pager */
		for (i = 0; QL_HELP[i].cmd; i++)
		{
			if (strncasecmp(topic, QL_HELP[i].cmd, len) == 0 ||
				strcmp(topic, "*") == 0)
			{
				nl_count += 5;
				for (ch = QL_HELP[i].syntax; *ch != '\0'; ch++)
					if (*ch == '\n')
						nl_count++;
				/* If we have an exact match, exit.  Fixes \h SELECT */
				if (strcasecmp(topic, QL_HELP[i].cmd) == 0)
					break;
			}
		}

		output = PageOutput(nl_count, pager);

		for (i = 0; QL_HELP[i].cmd; i++)
		{
			if (strncasecmp(topic, QL_HELP[i].cmd, len) == 0 ||
				strcmp(topic, "*") == 0)
			{
				help_found = true;
				fprintf(output, _("Command:     %s\n"
								  "Description: %s\n"
								  "Syntax:\n%s\n\n"),
						QL_HELP[i].cmd,
						gettext(QL_HELP[i].help),
						gettext(QL_HELP[i].syntax));
				/* If we have an exact match, exit.  Fixes \h SELECT */
				if (strcasecmp(topic, QL_HELP[i].cmd) == 0)
					break;
			}
		}

		if (!help_found)
			fprintf(output, _("No help available for \"%-.*s\".\nTry \\h with no arguments to see available help.\n"), (int) len, topic);

		/* Only close if we used the pager */
		if (output != stdout)
		{
			pclose(output);
#ifndef WIN32
			pqsignal(SIGPIPE, SIG_DFL);
#endif
		}
	}
}



void
print_copyright(void)
{
	puts(
		 "PostgreSQL Data Base Management System\n\n"
		 "Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group\n\n"
		 "This software is based on Postgres95, formerly known as Postgres, which\n"
		 "contains the following notice:\n\n"
		 "Portions Copyright(c) 1994, Regents of the University of California\n\n"
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
		 "SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS."
		);
}
