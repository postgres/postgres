/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2022, PostgreSQL Global Development Group
 *
 * src/bin/psql/help.c
 */
#include "postgres_fe.h"

#ifndef WIN32
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
#include "common/logging.h"
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

/* Some helper macros to make the code less verbose */
#define HELP0(str) appendPQExpBufferStr(&buf, _(str))
#define HELPN(str,...) appendPQExpBuffer(&buf, _(str), __VA_ARGS__)
#define ON(var) ((var) ? _("on") : _("off"))


/*
 * usage
 *
 * print out command line arguments
 */
void
usage(unsigned short int pager)
{
	const char *env;
	const char *user;
	char	   *errstr;
	PQExpBufferData buf;
	int			nlcount;
	FILE	   *output;

	/* Find default user, in case we need it. */
	user = getenv("PGUSER");
	if (!user)
	{
		user = get_user_name(&errstr);
		if (!user)
			pg_fatal("%s", errstr);
	}

	/*
	 * To avoid counting the output lines manually, build the output in "buf"
	 * and then count them.
	 */
	initPQExpBuffer(&buf);

	HELP0("psql is the PostgreSQL interactive terminal.\n\n");
	HELP0("Usage:\n");
	HELP0("  psql [OPTION]... [DBNAME [USERNAME]]\n\n");

	HELP0("General options:\n");
	/* Display default database */
	env = getenv("PGDATABASE");
	if (!env)
		env = user;
	HELP0("  -c, --command=COMMAND    run only single command (SQL or internal) and exit\n");
	HELPN("  -d, --dbname=DBNAME      database name to connect to (default: \"%s\")\n",
		  env);
	HELP0("  -f, --file=FILENAME      execute commands from file, then exit\n");
	HELP0("  -l, --list               list available databases, then exit\n");
	HELP0("  -v, --set=, --variable=NAME=VALUE\n"
		  "                           set psql variable NAME to VALUE\n"
		  "                           (e.g., -v ON_ERROR_STOP=1)\n");
	HELP0("  -V, --version            output version information, then exit\n");
	HELP0("  -X, --no-psqlrc          do not read startup file (~/.psqlrc)\n");
	HELP0("  -1 (\"one\"), --single-transaction\n"
		  "                           execute as a single transaction (if non-interactive)\n");
	HELP0("  -?, --help[=options]     show this help, then exit\n");
	HELP0("      --help=commands      list backslash commands, then exit\n");
	HELP0("      --help=variables     list special variables, then exit\n");

	HELP0("\nInput and output options:\n");
	HELP0("  -a, --echo-all           echo all input from script\n");
	HELP0("  -b, --echo-errors        echo failed commands\n");
	HELP0("  -e, --echo-queries       echo commands sent to server\n");
	HELP0("  -E, --echo-hidden        display queries that internal commands generate\n");
	HELP0("  -L, --log-file=FILENAME  send session log to file\n");
	HELP0("  -n, --no-readline        disable enhanced command line editing (readline)\n");
	HELP0("  -o, --output=FILENAME    send query results to file (or |pipe)\n");
	HELP0("  -q, --quiet              run quietly (no messages, only query output)\n");
	HELP0("  -s, --single-step        single-step mode (confirm each query)\n");
	HELP0("  -S, --single-line        single-line mode (end of line terminates SQL command)\n");

	HELP0("\nOutput format options:\n");
	HELP0("  -A, --no-align           unaligned table output mode\n");
	HELP0("      --csv                CSV (Comma-Separated Values) table output mode\n");
	HELPN("  -F, --field-separator=STRING\n"
		  "                           field separator for unaligned output (default: \"%s\")\n",
		  DEFAULT_FIELD_SEP);
	HELP0("  -H, --html               HTML table output mode\n");
	HELP0("  -P, --pset=VAR[=ARG]     set printing option VAR to ARG (see \\pset command)\n");
	HELP0("  -R, --record-separator=STRING\n"
		  "                           record separator for unaligned output (default: newline)\n");
	HELP0("  -t, --tuples-only        print rows only\n");
	HELP0("  -T, --table-attr=TEXT    set HTML table tag attributes (e.g., width, border)\n");
	HELP0("  -x, --expanded           turn on expanded table output\n");
	HELP0("  -z, --field-separator-zero\n"
		  "                           set field separator for unaligned output to zero byte\n");
	HELP0("  -0, --record-separator-zero\n"
		  "                           set record separator for unaligned output to zero byte\n");

	HELP0("\nConnection options:\n");
	/* Display default host */
	env = getenv("PGHOST");
	HELPN("  -h, --host=HOSTNAME      database server host or socket directory (default: \"%s\")\n",
		  env ? env : _("local socket"));
	/* Display default port */
	env = getenv("PGPORT");
	HELPN("  -p, --port=PORT          database server port (default: \"%s\")\n",
		  env ? env : DEF_PGPORT_STR);
	/* Display default user */
	HELPN("  -U, --username=USERNAME  database user name (default: \"%s\")\n",
		  user);
	HELP0("  -w, --no-password        never prompt for password\n");
	HELP0("  -W, --password           force password prompt (should happen automatically)\n");

	HELP0("\nFor more information, type \"\\?\" (for internal commands) or \"\\help\" (for SQL\n"
		  "commands) from within psql, or consult the psql section in the PostgreSQL\n"
		  "documentation.\n\n");
	HELPN("Report bugs to <%s>.\n", PACKAGE_BUGREPORT);
	HELPN("%s home page: <%s>\n", PACKAGE_NAME, PACKAGE_URL);

	/* Now we can count the lines. */
	nlcount = 0;
	for (const char *ptr = buf.data; *ptr; ptr++)
	{
		if (*ptr == '\n')
			nlcount++;
	}

	/* And dump the output, with appropriate pagination. */
	output = PageOutput(nlcount, pager ? &(pset.popt.topt) : NULL);

	fputs(buf.data, output);

	ClosePager(output);

	termPQExpBuffer(&buf);
}


/*
 * slashUsage
 *
 * print out help for the backslash commands
 */
void
slashUsage(unsigned short int pager)
{
	PQExpBufferData buf;
	int			nlcount;
	FILE	   *output;
	char	   *currdb;

	currdb = PQdb(pset.db);

	/*
	 * To avoid counting the output lines manually, build the output in "buf"
	 * and then count them.
	 */
	initPQExpBuffer(&buf);

	HELP0("General\n");
	HELP0("  \\copyright             show PostgreSQL usage and distribution terms\n");
	HELP0("  \\crosstabview [COLUMNS] execute query and display result in crosstab\n");
	HELP0("  \\errverbose            show most recent error message at maximum verbosity\n");
	HELP0("  \\g [(OPTIONS)] [FILE]  execute query (and send result to file or |pipe);\n"
		  "                         \\g with no arguments is equivalent to a semicolon\n");
	HELP0("  \\gdesc                 describe result of query, without executing it\n");
	HELP0("  \\gexec                 execute query, then execute each value in its result\n");
	HELP0("  \\gset [PREFIX]         execute query and store result in psql variables\n");
	HELP0("  \\gx [(OPTIONS)] [FILE] as \\g, but forces expanded output mode\n");
	HELP0("  \\q                     quit psql\n");
	HELP0("  \\watch [SEC]           execute query every SEC seconds\n");
	HELP0("\n");

	HELP0("Help\n");

	HELP0("  \\? [commands]          show help on backslash commands\n");
	HELP0("  \\? options             show help on psql command-line options\n");
	HELP0("  \\? variables           show help on special variables\n");
	HELP0("  \\h [NAME]              help on syntax of SQL commands, * for all commands\n");
	HELP0("\n");

	HELP0("Query Buffer\n");
	HELP0("  \\e [FILE] [LINE]       edit the query buffer (or file) with external editor\n");
	HELP0("  \\ef [FUNCNAME [LINE]]  edit function definition with external editor\n");
	HELP0("  \\ev [VIEWNAME [LINE]]  edit view definition with external editor\n");
	HELP0("  \\p                     show the contents of the query buffer\n");
	HELP0("  \\r                     reset (clear) the query buffer\n");
#ifdef USE_READLINE
	HELP0("  \\s [FILE]              display history or save it to file\n");
#endif
	HELP0("  \\w FILE                write query buffer to file\n");
	HELP0("\n");

	HELP0("Input/Output\n");
	HELP0("  \\copy ...              perform SQL COPY with data stream to the client host\n");
	HELP0("  \\echo [-n] [STRING]    write string to standard output (-n for no newline)\n");
	HELP0("  \\i FILE                execute commands from file\n");
	HELP0("  \\ir FILE               as \\i, but relative to location of current script\n");
	HELP0("  \\o [FILE]              send all query results to file or |pipe\n");
	HELP0("  \\qecho [-n] [STRING]   write string to \\o output stream (-n for no newline)\n");
	HELP0("  \\warn [-n] [STRING]    write string to standard error (-n for no newline)\n");
	HELP0("\n");

	HELP0("Conditional\n");
	HELP0("  \\if EXPR               begin conditional block\n");
	HELP0("  \\elif EXPR             alternative within current conditional block\n");
	HELP0("  \\else                  final alternative within current conditional block\n");
	HELP0("  \\endif                 end conditional block\n");
	HELP0("\n");

	HELP0("Informational\n");
	HELP0("  (options: S = show system objects, + = additional detail)\n");
	HELP0("  \\d[S+]                 list tables, views, and sequences\n");
	HELP0("  \\d[S+]  NAME           describe table, view, sequence, or index\n");
	HELP0("  \\da[S]  [PATTERN]      list aggregates\n");
	HELP0("  \\dA[+]  [PATTERN]      list access methods\n");
	HELP0("  \\dAc[+] [AMPTRN [TYPEPTRN]]  list operator classes\n");
	HELP0("  \\dAf[+] [AMPTRN [TYPEPTRN]]  list operator families\n");
	HELP0("  \\dAo[+] [AMPTRN [OPFPTRN]]   list operators of operator families\n");
	HELP0("  \\dAp[+] [AMPTRN [OPFPTRN]]   list support functions of operator families\n");
	HELP0("  \\db[+]  [PATTERN]      list tablespaces\n");
	HELP0("  \\dc[S+] [PATTERN]      list conversions\n");
	HELP0("  \\dconfig[+] [PATTERN]  list configuration parameters\n");
	HELP0("  \\dC[+]  [PATTERN]      list casts\n");
	HELP0("  \\dd[S]  [PATTERN]      show object descriptions not displayed elsewhere\n");
	HELP0("  \\dD[S+] [PATTERN]      list domains\n");
	HELP0("  \\ddp    [PATTERN]      list default privileges\n");
	HELP0("  \\dE[S+] [PATTERN]      list foreign tables\n");
	HELP0("  \\des[+] [PATTERN]      list foreign servers\n");
	HELP0("  \\det[+] [PATTERN]      list foreign tables\n");
	HELP0("  \\deu[+] [PATTERN]      list user mappings\n");
	HELP0("  \\dew[+] [PATTERN]      list foreign-data wrappers\n");
	HELP0("  \\df[anptw][S+] [FUNCPTRN [TYPEPTRN ...]]\n"
		  "                         list [only agg/normal/procedure/trigger/window] functions\n");
	HELP0("  \\dF[+]  [PATTERN]      list text search configurations\n");
	HELP0("  \\dFd[+] [PATTERN]      list text search dictionaries\n");
	HELP0("  \\dFp[+] [PATTERN]      list text search parsers\n");
	HELP0("  \\dFt[+] [PATTERN]      list text search templates\n");
	HELP0("  \\dg[S+] [PATTERN]      list roles\n");
	HELP0("  \\di[S+] [PATTERN]      list indexes\n");
	HELP0("  \\dl[+]                 list large objects, same as \\lo_list\n");
	HELP0("  \\dL[S+] [PATTERN]      list procedural languages\n");
	HELP0("  \\dm[S+] [PATTERN]      list materialized views\n");
	HELP0("  \\dn[S+] [PATTERN]      list schemas\n");
	HELP0("  \\do[S+] [OPPTRN [TYPEPTRN [TYPEPTRN]]]\n"
		  "                         list operators\n");
	HELP0("  \\dO[S+] [PATTERN]      list collations\n");
	HELP0("  \\dp     [PATTERN]      list table, view, and sequence access privileges\n");
	HELP0("  \\dP[itn+] [PATTERN]    list [only index/table] partitioned relations [n=nested]\n");
	HELP0("  \\drds [ROLEPTRN [DBPTRN]] list per-database role settings\n");
	HELP0("  \\dRp[+] [PATTERN]      list replication publications\n");
	HELP0("  \\dRs[+] [PATTERN]      list replication subscriptions\n");
	HELP0("  \\ds[S+] [PATTERN]      list sequences\n");
	HELP0("  \\dt[S+] [PATTERN]      list tables\n");
	HELP0("  \\dT[S+] [PATTERN]      list data types\n");
	HELP0("  \\du[S+] [PATTERN]      list roles\n");
	HELP0("  \\dv[S+] [PATTERN]      list views\n");
	HELP0("  \\dx[+]  [PATTERN]      list extensions\n");
	HELP0("  \\dX     [PATTERN]      list extended statistics\n");
	HELP0("  \\dy[+]  [PATTERN]      list event triggers\n");
	HELP0("  \\l[+]   [PATTERN]      list databases\n");
	HELP0("  \\sf[+]  FUNCNAME       show a function's definition\n");
	HELP0("  \\sv[+]  VIEWNAME       show a view's definition\n");
	HELP0("  \\z      [PATTERN]      same as \\dp\n");
	HELP0("\n");

	HELP0("Large Objects\n");
	HELP0("  \\lo_export LOBOID FILE write large object to file\n");
	HELP0("  \\lo_import FILE [COMMENT]\n"
		  "                         read large object from file\n");
	HELP0("  \\lo_list[+]            list large objects\n");
	HELP0("  \\lo_unlink LOBOID      delete a large object\n");
	HELP0("\n");

	HELP0("Formatting\n");
	HELP0("  \\a                     toggle between unaligned and aligned output mode\n");
	HELP0("  \\C [STRING]            set table title, or unset if none\n");
	HELP0("  \\f [STRING]            show or set field separator for unaligned query output\n");
	HELPN("  \\H                     toggle HTML output mode (currently %s)\n",
		  ON(pset.popt.topt.format == PRINT_HTML));
	HELP0("  \\pset [NAME [VALUE]]   set table output option\n"
		  "                         (border|columns|csv_fieldsep|expanded|fieldsep|\n"
		  "                         fieldsep_zero|footer|format|linestyle|null|\n"
		  "                         numericlocale|pager|pager_min_lines|recordsep|\n"
		  "                         recordsep_zero|tableattr|title|tuples_only|\n"
		  "                         unicode_border_linestyle|unicode_column_linestyle|\n"
		  "                         unicode_header_linestyle)\n");
	HELPN("  \\t [on|off]            show only rows (currently %s)\n",
		  ON(pset.popt.topt.tuples_only));
	HELP0("  \\T [STRING]            set HTML <table> tag attributes, or unset if none\n");
	HELPN("  \\x [on|off|auto]       toggle expanded output (currently %s)\n",
		  pset.popt.topt.expanded == 2 ? _("auto") : ON(pset.popt.topt.expanded));
	HELP0("\n");

	HELP0("Connection\n");
	if (currdb)
		HELPN("  \\c[onnect] {[DBNAME|- USER|- HOST|- PORT|-] | conninfo}\n"
			  "                         connect to new database (currently \"%s\")\n",
			  currdb);
	else
		HELP0("  \\c[onnect] {[DBNAME|- USER|- HOST|- PORT|-] | conninfo}\n"
			  "                         connect to new database (currently no connection)\n");
	HELP0("  \\conninfo              display information about current connection\n");
	HELP0("  \\encoding [ENCODING]   show or set client encoding\n");
	HELP0("  \\password [USERNAME]   securely change the password for a user\n");
	HELP0("\n");

	HELP0("Operating System\n");
	HELP0("  \\cd [DIR]              change the current working directory\n");
	HELP0("  \\getenv PSQLVAR ENVVAR fetch environment variable\n");
	HELP0("  \\setenv NAME [VALUE]   set or unset environment variable\n");
	HELPN("  \\timing [on|off]       toggle timing of commands (currently %s)\n",
		  ON(pset.timing));
	HELP0("  \\! [COMMAND]           execute command in shell or start interactive shell\n");
	HELP0("\n");

	HELP0("Variables\n");
	HELP0("  \\prompt [TEXT] NAME    prompt user to set internal variable\n");
	HELP0("  \\set [NAME [VALUE]]    set internal variable, or list all if no parameters\n");
	HELP0("  \\unset NAME            unset (delete) internal variable\n");

	/* Now we can count the lines. */
	nlcount = 0;
	for (const char *ptr = buf.data; *ptr; ptr++)
	{
		if (*ptr == '\n')
			nlcount++;
	}

	/* And dump the output, with appropriate pagination. */
	output = PageOutput(nlcount, pager ? &(pset.popt.topt) : NULL);

	fputs(buf.data, output);

	ClosePager(output);

	termPQExpBuffer(&buf);
}


/*
 * helpVariables
 *
 * show list of available variables (options) from command line
 */
void
helpVariables(unsigned short int pager)
{
	PQExpBufferData buf;
	int			nlcount;
	FILE	   *output;

	/*
	 * To avoid counting the output lines manually, build the output in "buf"
	 * and then count them.
	 */
	initPQExpBuffer(&buf);

	HELP0("List of specially treated variables\n\n");

	HELP0("psql variables:\n");
	HELP0("Usage:\n");
	HELP0("  psql --set=NAME=VALUE\n  or \\set NAME VALUE inside psql\n\n");

	HELP0("  AUTOCOMMIT\n"
		  "    if set, successful SQL commands are automatically committed\n");
	HELP0("  COMP_KEYWORD_CASE\n"
		  "    determines the case used to complete SQL key words\n"
		  "    [lower, upper, preserve-lower, preserve-upper]\n");
	HELP0("  DBNAME\n"
		  "    the currently connected database name\n");
	HELP0("  ECHO\n"
		  "    controls what input is written to standard output\n"
		  "    [all, errors, none, queries]\n");
	HELP0("  ECHO_HIDDEN\n"
		  "    if set, display internal queries executed by backslash commands;\n"
		  "    if set to \"noexec\", just show them without execution\n");
	HELP0("  ENCODING\n"
		  "    current client character set encoding\n");
	HELP0("  ERROR\n"
		  "    true if last query failed, else false\n");
	HELP0("  FETCH_COUNT\n"
		  "    the number of result rows to fetch and display at a time (0 = unlimited)\n");
	HELP0("  HIDE_TABLEAM\n"
		  "    if set, table access methods are not displayed\n");
	HELP0("  HIDE_TOAST_COMPRESSION\n"
		  "    if set, compression methods are not displayed\n");
	HELP0("  HISTCONTROL\n"
		  "    controls command history [ignorespace, ignoredups, ignoreboth]\n");
	HELP0("  HISTFILE\n"
		  "    file name used to store the command history\n");
	HELP0("  HISTSIZE\n"
		  "    maximum number of commands to store in the command history\n");
	HELP0("  HOST\n"
		  "    the currently connected database server host\n");
	HELP0("  IGNOREEOF\n"
		  "    number of EOFs needed to terminate an interactive session\n");
	HELP0("  LASTOID\n"
		  "    value of the last affected OID\n");
	HELP0("  LAST_ERROR_MESSAGE\n"
		  "  LAST_ERROR_SQLSTATE\n"
		  "    message and SQLSTATE of last error, or empty string and \"00000\" if none\n");
	HELP0("  ON_ERROR_ROLLBACK\n"
		  "    if set, an error doesn't stop a transaction (uses implicit savepoints)\n");
	HELP0("  ON_ERROR_STOP\n"
		  "    stop batch execution after error\n");
	HELP0("  PORT\n"
		  "    server port of the current connection\n");
	HELP0("  PROMPT1\n"
		  "    specifies the standard psql prompt\n");
	HELP0("  PROMPT2\n"
		  "    specifies the prompt used when a statement continues from a previous line\n");
	HELP0("  PROMPT3\n"
		  "    specifies the prompt used during COPY ... FROM STDIN\n");
	HELP0("  QUIET\n"
		  "    run quietly (same as -q option)\n");
	HELP0("  ROW_COUNT\n"
		  "    number of rows returned or affected by last query, or 0\n");
	HELP0("  SERVER_VERSION_NAME\n"
		  "  SERVER_VERSION_NUM\n"
		  "    server's version (in short string or numeric format)\n");
	HELP0("  SHOW_ALL_RESULTS\n"
		  "    show all results of a combined query (\\;) instead of only the last\n");
	HELP0("  SHOW_CONTEXT\n"
		  "    controls display of message context fields [never, errors, always]\n");
	HELP0("  SINGLELINE\n"
		  "    if set, end of line terminates SQL commands (same as -S option)\n");
	HELP0("  SINGLESTEP\n"
		  "    single-step mode (same as -s option)\n");
	HELP0("  SQLSTATE\n"
		  "    SQLSTATE of last query, or \"00000\" if no error\n");
	HELP0("  USER\n"
		  "    the currently connected database user\n");
	HELP0("  VERBOSITY\n"
		  "    controls verbosity of error reports [default, verbose, terse, sqlstate]\n");
	HELP0("  VERSION\n"
		  "  VERSION_NAME\n"
		  "  VERSION_NUM\n"
		  "    psql's version (in verbose string, short string, or numeric format)\n");

	HELP0("\nDisplay settings:\n");
	HELP0("Usage:\n");
	HELP0("  psql --pset=NAME[=VALUE]\n  or \\pset NAME [VALUE] inside psql\n\n");

	HELP0("  border\n"
		  "    border style (number)\n");
	HELP0("  columns\n"
		  "    target width for the wrapped format\n");
	HELP0("  expanded (or x)\n"
		  "    expanded output [on, off, auto]\n");
	HELPN("  fieldsep\n"
		  "    field separator for unaligned output (default \"%s\")\n",
		  DEFAULT_FIELD_SEP);
	HELP0("  fieldsep_zero\n"
		  "    set field separator for unaligned output to a zero byte\n");
	HELP0("  footer\n"
		  "    enable or disable display of the table footer [on, off]\n");
	HELP0("  format\n"
		  "    set output format [unaligned, aligned, wrapped, html, asciidoc, ...]\n");
	HELP0("  linestyle\n"
		  "    set the border line drawing style [ascii, old-ascii, unicode]\n");
	HELP0("  null\n"
		  "    set the string to be printed in place of a null value\n");
	HELP0("  numericlocale\n"
		  "    enable display of a locale-specific character to separate groups of digits\n");
	HELP0("  pager\n"
		  "    control when an external pager is used [yes, no, always]\n");
	HELP0("  recordsep\n"
		  "    record (line) separator for unaligned output\n");
	HELP0("  recordsep_zero\n"
		  "    set record separator for unaligned output to a zero byte\n");
	HELP0("  tableattr (or T)\n"
		  "    specify attributes for table tag in html format, or proportional\n"
		  "    column widths for left-aligned data types in latex-longtable format\n");
	HELP0("  title\n"
		  "    set the table title for subsequently printed tables\n");
	HELP0("  tuples_only\n"
		  "    if set, only actual table data is shown\n");
	HELP0("  unicode_border_linestyle\n"
		  "  unicode_column_linestyle\n"
		  "  unicode_header_linestyle\n"
		  "    set the style of Unicode line drawing [single, double]\n");

	HELP0("\nEnvironment variables:\n");
	HELP0("Usage:\n");

#ifndef WIN32
	HELP0("  NAME=VALUE [NAME=VALUE] psql ...\n  or \\setenv NAME [VALUE] inside psql\n\n");
#else
	HELP0("  set NAME=VALUE\n  psql ...\n  or \\setenv NAME [VALUE] inside psql\n\n");
#endif

	HELP0("  COLUMNS\n"
		  "    number of columns for wrapped format\n");
	HELP0("  PGAPPNAME\n"
		  "    same as the application_name connection parameter\n");
	HELP0("  PGDATABASE\n"
		  "    same as the dbname connection parameter\n");
	HELP0("  PGHOST\n"
		  "    same as the host connection parameter\n");
	HELP0("  PGPASSFILE\n"
		  "    password file name\n");
	HELP0("  PGPASSWORD\n"
		  "    connection password (not recommended)\n");
	HELP0("  PGPORT\n"
		  "    same as the port connection parameter\n");
	HELP0("  PGUSER\n"
		  "    same as the user connection parameter\n");
	HELP0("  PSQL_EDITOR, EDITOR, VISUAL\n"
		  "    editor used by the \\e, \\ef, and \\ev commands\n");
	HELP0("  PSQL_EDITOR_LINENUMBER_ARG\n"
		  "    how to specify a line number when invoking the editor\n");
	HELP0("  PSQL_HISTORY\n"
		  "    alternative location for the command history file\n");
	HELP0("  PSQL_PAGER, PAGER\n"
		  "    name of external pager program\n");
#ifndef WIN32
	HELP0("  PSQL_WATCH_PAGER\n"
		  "    name of external pager program used for \\watch\n");
#endif
	HELP0("  PSQLRC\n"
		  "    alternative location for the user's .psqlrc file\n");
	HELP0("  SHELL\n"
		  "    shell used by the \\! command\n");
	HELP0("  TMPDIR\n"
		  "    directory for temporary files\n");

	/* Now we can count the lines. */
	nlcount = 0;
	for (const char *ptr = buf.data; *ptr; ptr++)
	{
		if (*ptr == '\n')
			nlcount++;
	}

	/* And dump the output, with appropriate pagination. */
	output = PageOutput(nlcount, pager ? &(pset.popt.topt) : NULL);

	fputs(buf.data, output);

	ClosePager(output);

	termPQExpBuffer(&buf);
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

		/* Find screen width to determine how many columns will fit */
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
					pass;
		FILE	   *output = NULL;
		size_t		len,
					wordlen,
					j;
		int			nl_count;

		/*
		 * len is the amount of the input to compare to the help topic names.
		 * We first try exact match, then first + second words, then first
		 * word only.
		 */
		len = strlen(topic);

		for (pass = 1; pass <= 3; pass++)
		{
			if (pass > 1)		/* Nothing on first pass - try the opening
								 * word(s) */
			{
				wordlen = j = 1;
				while (j < len && topic[j++] != ' ')
					wordlen++;
				if (pass == 2 && j < len)
				{
					wordlen++;
					while (j < len && topic[j++] != ' ')
						wordlen++;
				}
				if (wordlen >= len)
				{
					/* Failed to shorten input, so try next pass if any */
					continue;
				}
				len = wordlen;
			}

			/*
			 * Count newlines for pager.  This logic must agree with what the
			 * following loop will do!
			 */
			nl_count = 0;
			for (i = 0; QL_HELP[i].cmd; i++)
			{
				if (pg_strncasecmp(topic, QL_HELP[i].cmd, len) == 0 ||
					strcmp(topic, "*") == 0)
				{
					/* magic constant here must match format below! */
					nl_count += 7 + QL_HELP[i].nl_count;

					/* If we have an exact match, exit.  Fixes \h SELECT */
					if (pg_strcasecmp(topic, QL_HELP[i].cmd) == 0)
						break;
				}
			}
			/* If no matches, don't open the output yet */
			if (nl_count == 0)
				continue;

			if (!output)
				output = PageOutput(nl_count, pager ? &(pset.popt.topt) : NULL);

			for (i = 0; QL_HELP[i].cmd; i++)
			{
				if (pg_strncasecmp(topic, QL_HELP[i].cmd, len) == 0 ||
					strcmp(topic, "*") == 0)
				{
					PQExpBufferData buffer;
					char	   *url;

					initPQExpBuffer(&buffer);
					QL_HELP[i].syntaxfunc(&buffer);
					url = psprintf("https://www.postgresql.org/docs/%s/%s.html",
								   strstr(PG_VERSION, "devel") ? "devel" : PG_MAJORVERSION,
								   QL_HELP[i].docbook_id);
					/* # of newlines in format must match constant above! */
					fprintf(output, _("Command:     %s\n"
									  "Description: %s\n"
									  "Syntax:\n%s\n\n"
									  "URL: %s\n\n"),
							QL_HELP[i].cmd,
							_(QL_HELP[i].help),
							buffer.data,
							url);
					free(url);
					termPQExpBuffer(&buffer);

					/* If we have an exact match, exit.  Fixes \h SELECT */
					if (pg_strcasecmp(topic, QL_HELP[i].cmd) == 0)
						break;
				}
			}
			break;
		}

		/* If we never found anything, report that */
		if (!output)
		{
			output = PageOutput(2, pager ? &(pset.popt.topt) : NULL);
			fprintf(output, _("No help available for \"%s\".\n"
							  "Try \\h with no arguments to see available help.\n"),
					topic);
		}

		ClosePager(output);
	}
}



void
print_copyright(void)
{
	puts("PostgreSQL Database Management System\n"
		 "(formerly known as Postgres, then as Postgres95)\n\n"
		 "Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group\n\n"
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
		 "PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.\n");
}
