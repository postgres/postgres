/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2016, PostgreSQL Global Development Group
 *
 * src/bin/psql/help.c
 */
#include "postgres_fe.h"

#ifndef WIN32
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
#include "common/username.h"
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
usage(unsigned short int pager)
{
	const char *env;
	const char *user;
	char	   *errstr;
	FILE	   *output;

	/* Find default user, in case we need it. */
	user = getenv("PGUSER");
	if (!user)
	{
		user = get_user_name(&errstr);
		if (!user)
		{
			psql_error("%s\n", errstr);
			exit(EXIT_FAILURE);
		}
	}

	/*
	 * Keep this line count in sync with the number of lines printed below!
	 * Use "psql --help=options | wc" to count correctly.
	 */
	output = PageOutput(60, pager ? &(pset.popt.topt) : NULL);

	fprintf(output, _("psql is the PostgreSQL interactive terminal.\n\n"));
	fprintf(output, _("Usage:\n"));
	fprintf(output, _("  psql [OPTION]... [DBNAME [USERNAME]]\n\n"));

	fprintf(output, _("General options:\n"));
	/* Display default database */
	env = getenv("PGDATABASE");
	if (!env)
		env = user;
	fprintf(output, _("  -c, --command=COMMAND    run only single command (SQL or internal) and exit\n"));
	fprintf(output, _("  -d, --dbname=DBNAME      database name to connect to (default: \"%s\")\n"), env);
	fprintf(output, _("  -f, --file=FILENAME      execute commands from file, then exit\n"));
	fprintf(output, _("  -l, --list               list available databases, then exit\n"));
	fprintf(output, _("  -v, --set=, --variable=NAME=VALUE\n"
					  "                           set psql variable NAME to VALUE\n"
					  "                           (e.g., -v ON_ERROR_STOP=1)\n"));
	fprintf(output, _("  -V, --version            output version information, then exit\n"));
	fprintf(output, _("  -X, --no-psqlrc          do not read startup file (~/.psqlrc)\n"));
	fprintf(output, _("  -1 (\"one\"), --single-transaction\n"
					  "                           execute as a single transaction (if non-interactive)\n"));
	fprintf(output, _("  -?, --help[=options]     show this help, then exit\n"));
	fprintf(output, _("      --help=commands      list backslash commands, then exit\n"));
	fprintf(output, _("      --help=variables     list special variables, then exit\n"));

	fprintf(output, _("\nInput and output options:\n"));
	fprintf(output, _("  -a, --echo-all           echo all input from script\n"));
	fprintf(output, _("  -b, --echo-errors        echo failed commands\n"));
	fprintf(output, _("  -e, --echo-queries       echo commands sent to server\n"));
	fprintf(output, _("  -E, --echo-hidden        display queries that internal commands generate\n"));
	fprintf(output, _("  -L, --log-file=FILENAME  send session log to file\n"));
	fprintf(output, _("  -n, --no-readline        disable enhanced command line editing (readline)\n"));
	fprintf(output, _("  -o, --output=FILENAME    send query results to file (or |pipe)\n"));
	fprintf(output, _("  -q, --quiet              run quietly (no messages, only query output)\n"));
	fprintf(output, _("  -s, --single-step        single-step mode (confirm each query)\n"));
	fprintf(output, _("  -S, --single-line        single-line mode (end of line terminates SQL command)\n"));

	fprintf(output, _("\nOutput format options:\n"));
	fprintf(output, _("  -A, --no-align           unaligned table output mode\n"));
	fprintf(output, _("  -F, --field-separator=STRING\n"
					  "                           field separator for unaligned output (default: \"%s\")\n"),
			DEFAULT_FIELD_SEP);
	fprintf(output, _("  -H, --html               HTML table output mode\n"));
	fprintf(output, _("  -P, --pset=VAR[=ARG]     set printing option VAR to ARG (see \\pset command)\n"));
	fprintf(output, _("  -R, --record-separator=STRING\n"
					  "                           record separator for unaligned output (default: newline)\n"));
	fprintf(output, _("  -t, --tuples-only        print rows only\n"));
	fprintf(output, _("  -T, --table-attr=TEXT    set HTML table tag attributes (e.g., width, border)\n"));
	fprintf(output, _("  -x, --expanded           turn on expanded table output\n"));
	fprintf(output, _("  -z, --field-separator-zero\n"
					  "                           set field separator for unaligned output to zero byte\n"));
	fprintf(output, _("  -0, --record-separator-zero\n"
					  "                           set record separator for unaligned output to zero byte\n"));

	fprintf(output, _("\nConnection options:\n"));
	/* Display default host */
	env = getenv("PGHOST");
	fprintf(output, _("  -h, --host=HOSTNAME      database server host or socket directory (default: \"%s\")\n"),
			env ? env : _("local socket"));
	/* Display default port */
	env = getenv("PGPORT");
	fprintf(output, _("  -p, --port=PORT          database server port (default: \"%s\")\n"),
			env ? env : DEF_PGPORT_STR);
	/* Display default user */
	env = getenv("PGUSER");
	if (!env)
		env = user;
	fprintf(output, _("  -U, --username=USERNAME  database user name (default: \"%s\")\n"), env);
	fprintf(output, _("  -w, --no-password        never prompt for password\n"));
	fprintf(output, _("  -W, --password           force password prompt (should happen automatically)\n"));

	fprintf(output, _("\nFor more information, type \"\\?\" (for internal commands) or \"\\help\" (for SQL\n"
					  "commands) from within psql, or consult the psql section in the PostgreSQL\n"
					  "documentation.\n\n"));
	fprintf(output, _("Report bugs to <pgsql-bugs@postgresql.org>.\n"));

	ClosePager(output);
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

	/*
	 * Keep this line count in sync with the number of lines printed below!
	 * Use "psql --help=commands | wc" to count correctly.  It's okay to count
	 * the USE_READLINE line even in builds without that.
	 */
	output = PageOutput(109, pager ? &(pset.popt.topt) : NULL);

	fprintf(output, _("General\n"));
	fprintf(output, _("  \\copyright             show PostgreSQL usage and distribution terms\n"));
	fprintf(output, _("  \\g [FILE] or ;         execute query (and send results to file or |pipe)\n"));
	fprintf(output, _("  \\gset [PREFIX]         execute query and store results in psql variables\n"));
	fprintf(output, _("  \\q                     quit psql\n"));
	fprintf(output, _("  \\watch [SEC]           execute query every SEC seconds\n"));
	fprintf(output, "\n");

	fprintf(output, _("Help\n"));

	fprintf(output, _("  \\? [commands]          show help on backslash commands\n"));
	fprintf(output, _("  \\? options             show help on psql command-line options\n"));
	fprintf(output, _("  \\? variables           show help on special variables\n"));
	fprintf(output, _("  \\h [NAME]              help on syntax of SQL commands, * for all commands\n"));
	fprintf(output, "\n");

	fprintf(output, _("Query Buffer\n"));
	fprintf(output, _("  \\e [FILE] [LINE]       edit the query buffer (or file) with external editor\n"));
	fprintf(output, _("  \\ef [FUNCNAME [LINE]]  edit function definition with external editor\n"));
	fprintf(output, _("  \\ev [VIEWNAME [LINE]]  edit view definition with external editor\n"));
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
	fprintf(output, _("  \\sf[+]  FUNCNAME       show a function's definition\n"));
	fprintf(output, _("  \\sv[+]  VIEWNAME       show a view's definition\n"));
	fprintf(output, _("  \\z      [PATTERN]      same as \\dp\n"));
	fprintf(output, "\n");

	fprintf(output, _("Formatting\n"));
	fprintf(output, _("  \\a                     toggle between unaligned and aligned output mode\n"));
	fprintf(output, _("  \\C [STRING]            set table title, or unset if none\n"));
	fprintf(output, _("  \\f [STRING]            show or set field separator for unaligned query output\n"));
	fprintf(output, _("  \\H                     toggle HTML output mode (currently %s)\n"),
			ON(pset.popt.topt.format == PRINT_HTML));
	fprintf(output, _("  \\pset [NAME [VALUE]]   set table output option\n"
					  "                         (NAME := {format|border|expanded|fieldsep|fieldsep_zero|footer|null|\n"
					  "                         numericlocale|recordsep|recordsep_zero|tuples_only|title|tableattr|pager|\n"
					  "                         unicode_border_linestyle|unicode_column_linestyle|unicode_header_linestyle})\n"));
	fprintf(output, _("  \\t [on|off]            show only rows (currently %s)\n"),
			ON(pset.popt.topt.tuples_only));
	fprintf(output, _("  \\T [STRING]            set HTML <table> tag attributes, or unset if none\n"));
	fprintf(output, _("  \\x [on|off|auto]       toggle expanded output (currently %s)\n"),
		pset.popt.topt.expanded == 2 ? "auto" : ON(pset.popt.topt.expanded));
	fprintf(output, "\n");

	fprintf(output, _("Connection\n"));
	if (currdb)
		fprintf(output, _("  \\c[onnect] {[DBNAME|- USER|- HOST|- PORT|-] | conninfo}\n"
						  "                         connect to new database (currently \"%s\")\n"),
				currdb);
	else
		fprintf(output, _("  \\c[onnect] {[DBNAME|- USER|- HOST|- PORT|-] | conninfo}\n"
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
 * helpVariables
 *
 * show list of available variables (options) from command line
 */
void
helpVariables(unsigned short int pager)
{
	FILE	   *output;

	/*
	 * Keep this line count in sync with the number of lines printed below!
	 * Use "psql --help=variables | wc" to count correctly; but notice that
	 * Windows builds currently print one more line than non-Windows builds.
	 * Using the larger number is fine.
	 */
	output = PageOutput(87, pager ? &(pset.popt.topt) : NULL);

	fprintf(output, _("List of specially treated variables\n\n"));

	fprintf(output, _("psql variables:\n"));
	fprintf(output, _("Usage:\n"));
	fprintf(output, _("  psql --set=NAME=VALUE\n  or \\set NAME VALUE inside psql\n\n"));

	fprintf(output, _("  AUTOCOMMIT         if set, successful SQL commands are automatically committed\n"));
	fprintf(output, _("  COMP_KEYWORD_CASE  determines the case used to complete SQL key words\n"
	"                     [lower, upper, preserve-lower, preserve-upper]\n"));
	fprintf(output, _("  DBNAME             the currently connected database name\n"));
	fprintf(output, _("  ECHO               controls what input is written to standard output\n"
					  "                     [all, errors, none, queries]\n"));
	fprintf(output, _("  ECHO_HIDDEN        if set, display internal queries executed by backslash commands;\n"
					  "                     if set to \"noexec\", just show without execution\n"));
	fprintf(output, _("  ENCODING           current client character set encoding\n"));
	fprintf(output, _("  FETCH_COUNT        the number of result rows to fetch and display at a time\n"
					  "                     (default: 0=unlimited)\n"));
	fprintf(output, _("  HISTCONTROL        controls command history [ignorespace, ignoredups, ignoreboth]\n"));
	fprintf(output, _("  HISTFILE           file name used to store the command history\n"));
	fprintf(output, _("  HISTSIZE           the number of commands to store in the command history\n"));
	fprintf(output, _("  HOST               the currently connected database server host\n"));
	fprintf(output, _("  IGNOREEOF          if unset, sending an EOF to interactive session terminates application\n"));
	fprintf(output, _("  LASTOID            value of the last affected OID\n"));
	fprintf(output, _("  ON_ERROR_ROLLBACK  if set, an error doesn't stop a transaction (uses implicit savepoints)\n"));
	fprintf(output, _("  ON_ERROR_STOP      stop batch execution after error\n"));
	fprintf(output, _("  PORT               server port of the current connection\n"));
	fprintf(output, _("  PROMPT1            specifies the standard psql prompt\n"));
	fprintf(output, _("  PROMPT2            specifies the prompt used when a statement continues from a previous line\n"));
	fprintf(output, _("  PROMPT3            specifies the prompt used during COPY ... FROM STDIN\n"));
	fprintf(output, _("  QUIET              run quietly (same as -q option)\n"));
	fprintf(output, _("  SHOW_CONTEXT       controls display of message context fields [never, errors, always]\n"));
	fprintf(output, _("  SINGLELINE         end of line terminates SQL command mode (same as -S option)\n"));
	fprintf(output, _("  SINGLESTEP         single-step mode (same as -s option)\n"));
	fprintf(output, _("  USER               the currently connected database user\n"));
	fprintf(output, _("  VERBOSITY          controls verbosity of error reports [default, verbose, terse]\n"));

	fprintf(output, _("\nDisplay settings:\n"));
	fprintf(output, _("Usage:\n"));
	fprintf(output, _("  psql --pset=NAME[=VALUE]\n  or \\pset NAME [VALUE] inside psql\n\n"));

	fprintf(output, _("  border             border style (number)\n"));
	fprintf(output, _("  columns            target width for the wrapped format\n"));
	fprintf(output, _("  expanded (or x)    expanded output [on, off, auto]\n"));
	fprintf(output, _("  fieldsep           field separator for unaligned output (default \"%s\")\n"), DEFAULT_FIELD_SEP);
	fprintf(output, _("  fieldsep_zero      set field separator for unaligned output to zero byte\n"));
	fprintf(output, _("  format             set output format [unaligned, aligned, wrapped, html, asciidoc, ...]\n"));
	fprintf(output, _("  footer             enable or disable display of the table footer [on, off]\n"));
	fprintf(output, _("  linestyle          set the border line drawing style [ascii, old-ascii, unicode]\n"));
	fprintf(output, _("  null               set the string to be printed in place of a null value\n"));
	fprintf(output, _("  numericlocale      enable or disable display of a locale-specific character to separate\n"
					  "                     groups of digits [on, off]\n"));
	fprintf(output, _("  pager              control when an external pager is used [yes, no, always]\n"));
	fprintf(output, _("  recordsep          record (line) separator for unaligned output\n"));
	fprintf(output, _("  recordsep_zero     set record separator for unaligned output to zero byte\n"));
	fprintf(output, _("  tableattr (or T)   specify attributes for table tag in html format or proportional\n"
					  "                     column widths for left-aligned data types in latex-longtable format\n"));
	fprintf(output, _("  title              set the table title for any subsequently printed tables\n"));
	fprintf(output, _("  tuples_only        if set, only actual table data is shown\n"));
	fprintf(output, _("  unicode_border_linestyle\n"
					  "  unicode_column_linestyle\n"
					  "  unicode_header_linestyle\n"
					  "                     set the style of Unicode line drawing [single, double]\n"));

	fprintf(output, _("\nEnvironment variables:\n"));
	fprintf(output, _("Usage:\n"));

#ifndef WIN32
	fprintf(output, _("  NAME=VALUE [NAME=VALUE] psql ...\n  or \\setenv NAME [VALUE] inside psql\n\n"));
#else
	fprintf(output, _("  set NAME=VALUE\n  psql ...\n  or \\setenv NAME [VALUE] inside psql\n\n"));
#endif

	fprintf(output, _("  COLUMNS            number of columns for wrapped format\n"));
	fprintf(output, _("  PAGER              name of external pager program\n"));
	fprintf(output, _("  PGAPPNAME          same as the application_name connection parameter\n"));
	fprintf(output, _("  PGDATABASE         same as the dbname connection parameter\n"));
	fprintf(output, _("  PGHOST             same as the host connection parameter\n"));
	fprintf(output, _("  PGPORT             same as the port connection parameter\n"));
	fprintf(output, _("  PGUSER             same as the user connection parameter\n"));
	fprintf(output, _("  PGPASSWORD         connection password (not recommended)\n"));
	fprintf(output, _("  PGPASSFILE         password file name\n"));
	fprintf(output, _("  PSQL_EDITOR, EDITOR, VISUAL\n"
					  "                     editor used by the \\e, \\ef, and \\ev commands\n"));
	fprintf(output, _("  PSQL_EDITOR_LINENUMBER_ARG\n"
					  "                     how to specify a line number when invoking the editor\n"));
	fprintf(output, _("  PSQL_HISTORY       alternative location for the command history file\n"));
	fprintf(output, _("  PSQLRC             alternative location for the user's .psqlrc file\n"));
	fprintf(output, _("  SHELL              shell used by the \\! command\n"));
	fprintf(output, _("  TMPDIR             directory for temporary files\n"));

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

		output = PageOutput(nrows + 1, pager ? &(pset.popt.topt) : NULL);

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
						output = PageOutput(nl_count, pager ? &(pset.popt.topt) : NULL);
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
				output = PageOutput(nl_count, pager ? &(pset.popt.topt) : NULL);

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
		 "Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group\n\n"
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
