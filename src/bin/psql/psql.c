/*-------------------------------------------------------------------------
 *
 * psql.c--
 *	  an interactive front-end to postgreSQL
 *
 * Copyright (c) 1996, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/bin/psql/Attic/psql.c,v 1.143 1998/05/13 03:27:07 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/param.h>			/* for MAXPATHLEN */
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include "postgres.h"
#include "libpq-fe.h"
#include "pqsignal.h"
#include "stringutils.h"
#include "psqlHelp.h"
#ifndef HAVE_STRDUP
#include "strdup.h"
#endif
#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif

#ifdef HAVE_LIBREADLINE
#ifdef HAVE_READLINE_H
#include <readline.h>
#define USE_READLINE 1
#if defined(HAVE_HISTORY_H)
#include <history.h>
#define USE_HISTORY 1
#endif
#else
#if defined(HAVE_READLINE_READLINE_H)
#include <readline/readline.h>
#define USE_READLINE 1
#if defined(HAVE_READLINE_HISTORY_H)
#include <readline/history.h>
#define USE_HISTORY 1
#endif
#endif
#endif
#if defined(HAVE_HISTORY) && !defined(USE_HISTORY)
#define USE_HISTORY 1
#endif
#endif

/* This prompt string is assumed to have at least 3 characters by code in MainLoop().
 * A character two characters from the end is replaced each time by a mode character.
 */
#define PROMPT "=> "

#define PROMPT_READY	'='
#define PROMPT_CONTINUE '-'
#define PROMPT_COMMENT	'*'
#define PROMPT_QUOTE	'\''

/* Backslash command handling:
 *	0 - send currently constructed query to backend (i.e. we got a \g)
 *	1 - skip processing of this line, continue building up query
 *	2 - terminate processing of this query entirely
 *	3 - new query supplied by edit
 */
#define CMD_UNKNOWN		-1
#define CMD_SEND		0
#define CMD_SKIP_LINE	1
#define CMD_TERMINATE	2
#define CMD_NEWEDIT		3

#define MAX_QUERY_BUFFER 20000

#define COPYBUFSIZ	8192

#define DEFAULT_FIELD_SEP "|"
#define DEFAULT_EDITOR	"vi"
#define DEFAULT_SHELL  "/bin/sh"

typedef struct _psqlSettings
{
	PGconn	   *db;				/* connection to backend */
	FILE	   *queryFout;		/* where to send the query results */
	PQprintOpt	opt;			/* options to be passed to PQprint */
	char	   *prompt;			/* prompt to display */
	char	   *gfname;			/* one-shot file output argument for \g */
	bool		notty;			/* input or output is not a tty */
	bool		pipe;			/* queryFout is from a popen() */
	bool		echoQuery;		/* echo the query before sending it */
	bool		quiet;			/* run quietly, no messages, no promt */
	bool		singleStep;		/* prompt before for each query */
	bool		singleLineMode; /* query terminated by newline */
	bool		useReadline;	/* use libreadline routines */
	bool		getPassword;	/* prompt the user for a username and
								 * password */
} PsqlSettings;

#ifdef TIOCGWINSZ
struct winsize screen_size;

#else
struct winsize
{
	int			ws_row;
	int			ws_col;
}			screen_size;

#endif

/* declarations for functions in this file */
static void usage(char *progname);
static void slashUsage();
static void handleCopyOut(PGresult *res, FILE *copystream);
static void
handleCopyIn(PGresult *res, const bool mustprompt,
			 FILE *copystream);
static int
tableList(PsqlSettings *pset, bool deep_tablelist,
		  char info_type, bool system_tables);
static int	tableDesc(PsqlSettings *pset, char *table, FILE *fout);
static int	objectDescription(PsqlSettings *pset, char *object, FILE *fout);
static int	rightsList(PsqlSettings *pset);
static void prompt_for_password(char *username, char *password);
static char *
make_connect_string(char *host, char *port, char *dbname,
					char *username, char *password);

static char *gets_noreadline(char *prompt, FILE *source);
static char *gets_readline(char *prompt, FILE *source);
static char *gets_fromFile(char *prompt, FILE *source);
static int	listAllDbs(PsqlSettings *pset);
static void
SendQuery(bool *success_p, PsqlSettings *pset, const char *query,
		  const bool copy_in, const bool copy_out, FILE *copystream);
static int
HandleSlashCmds(PsqlSettings *pset,
				char *line,
				char *query);
static int	MainLoop(PsqlSettings *pset, char *query, FILE *source);
static FILE *setFout(PsqlSettings *pset, char *fname);

/*
 * usage print out usage for command line arguments
 */

static void
usage(char *progname)
{
	fprintf(stderr, "Usage: %s [options] [dbname]\n", progname);
	fprintf(stderr, "\t -a authsvc              set authentication service\n");
	fprintf(stderr, "\t -A                      turn off alignment when printing out attributes\n");
	fprintf(stderr, "\t -c query                run single query (slash commands too)\n");
	fprintf(stderr, "\t -d dbName               specify database name\n");
	fprintf(stderr, "\t -e                      echo the query sent to the backend\n");
	fprintf(stderr, "\t -f filename             use file as a source of queries\n");
	fprintf(stderr, "\t -F sep                  set the field separator (default is '|')\n");
	fprintf(stderr, "\t -h host                 set database server host\n");
	fprintf(stderr, "\t -H                      turn on html3.0 table output\n");
	fprintf(stderr, "\t -l                      list available databases\n");
	fprintf(stderr, "\t -n                      don't use readline library\n");
	fprintf(stderr, "\t -o filename             send output to filename or (|pipe)\n");
	fprintf(stderr, "\t -p port                 set port number\n");
	fprintf(stderr, "\t -q                      run quietly (no messages, no prompts)\n");
	fprintf(stderr, "\t -s                      single step mode (prompts for each query)\n");
	fprintf(stderr, "\t -S                      single line mode (i.e. query terminated by newline)\n");
	fprintf(stderr, "\t -t                      turn off printing of headings and row count\n");
	fprintf(stderr, "\t -T html                 set html3.0 table command options (cf. -H)\n");
	fprintf(stderr, "\t -u                      ask for a username and password for authentication\n");
	fprintf(stderr, "\t -x                      turn on expanded output (field names on left)\n");
	exit(1);
}

/*
 * slashUsage print out usage for the backslash commands
 */

static char *
on(bool f)
{
	return f ? "on" : "off";
}

static void
slashUsage(PsqlSettings *pset)
{
	int			usePipe = 0;
	char	   *pagerenv;
	FILE	   *fout;

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
		screen_size.ws_row <= 28 &&
		(fout = popen(pagerenv, "w")))
	{
		usePipe = 1;
		pqsignal(SIGPIPE, SIG_IGN);
	}
	else
		fout = stdout;

	/* if you add/remove a line here, change the row test above */
	fprintf(fout, " \\?           -- help\n");
	fprintf(fout, " \\a           -- toggle field-alignment (currenty %s)\n", on(pset->opt.align));
	fprintf(fout, " \\C [<captn>] -- set html3 caption (currently '%s')\n", pset->opt.caption ? pset->opt.caption : "");
	fprintf(fout, " \\connect <dbname|-> <user> -- connect to new database (currently '%s')\n", PQdb(pset->db));
	fprintf(fout, " \\copy table {from | to} <fname>\n");
	fprintf(fout, " \\d [<table>] -- list tables and indices, columns in <table>, or * for all\n");
	fprintf(fout, " \\da          -- list aggregates\n");
	fprintf(fout, " \\dd [<object>]- list comment for table, field, type, function, or operator.\n");
	fprintf(fout, " \\df          -- list functions\n");
	fprintf(fout, " \\di          -- list only indices\n");
	fprintf(fout, " \\do          -- list operators\n");
	fprintf(fout, " \\ds          -- list only sequences\n");
	fprintf(fout, " \\dS          -- list system tables and indexes\n");
	fprintf(fout, " \\dt          -- list only tables\n");
	fprintf(fout, " \\dT          -- list types\n");
	fprintf(fout, " \\e [<fname>] -- edit the current query buffer or <fname>\n");
	fprintf(fout, " \\E [<fname>] -- edit the current query buffer or <fname>, and execute\n");
	fprintf(fout, " \\f [<sep>]   -- change field separater (currently '%s')\n", pset->opt.fieldSep);
	fprintf(fout, " \\g [<fname>] [|<cmd>] -- send query to backend [and results in <fname> or pipe]\n");
	fprintf(fout, " \\h [<cmd>]   -- help on syntax of sql commands, * for all commands\n");
	fprintf(fout, " \\H           -- toggle html3 output (currently %s)\n", on(pset->opt.html3));
	fprintf(fout, " \\i <fname>   -- read and execute queries from filename\n");
	fprintf(fout, " \\l           -- list all databases\n");
	fprintf(fout, " \\m           -- toggle monitor-like table display (currently %s)\n", on(pset->opt.standard));
	fprintf(fout, " \\o [<fname>] [|<cmd>] -- send all query results to stdout, <fname>, or pipe\n");
	fprintf(fout, " \\p           -- print the current query buffer\n");
	fprintf(fout, " \\q           -- quit\n");
	fprintf(fout, " \\r           -- reset(clear) the query buffer\n");
	fprintf(fout, " \\s [<fname>] -- print history or save it in <fname>\n");
	fprintf(fout, " \\t           -- toggle table headings and row count (currently %s)\n", on(pset->opt.header));
	fprintf(fout, " \\T [<html>]  -- set html3.0 <table ...> options (currently '%s')\n", pset->opt.tableOpt ? pset->opt.tableOpt : "");
	fprintf(fout, " \\x           -- toggle expanded output (currently %s)\n", on(pset->opt.expanded));
	fprintf(fout, " \\z           -- list current grant/revoke permissions\n");
	fprintf(fout, " \\! [<cmd>]   -- shell escape or command\n");

	if (usePipe)
	{
		pclose(fout);
		pqsignal(SIGPIPE, SIG_DFL);
	}
}

static PGresult *
PSQLexec(PsqlSettings *pset, char *query)
{
	PGresult   *res;

	res = PQexec(pset->db, query);
	if (!res)
		fputs(PQerrorMessage(pset->db), stderr);
	else
	{
		if (PQresultStatus(res) == PGRES_COMMAND_OK ||
			PQresultStatus(res) == PGRES_TUPLES_OK)
			return res;
		if (!pset->quiet)
			fputs(PQerrorMessage(pset->db), stderr);
		PQclear(res);
	}
	return NULL;
}

/*
 * Code to support command cancellation.
 * If interactive, we enable a SIGINT signal catcher that sends
 * a cancel request to the backend.
 * Note that sending the cancel directly from the signal handler
 * is safe only because the cancel is sent as an OOB message.
 * If it were inline data, then we'd risk inserting it into the
 * middle of a normal data message by doing this.
 * (It's probably not too cool to write on stderr, for that matter...
 *  but for debugging purposes we'll risk that.)
 */

static PGconn * cancelConn = NULL; /* connection to try cancel on */

static void
handle_sigint (SIGNAL_ARGS)
{
	if (cancelConn == NULL)
		exit(1);				/* accept signal if no connection */
	/* Try to send cancel request */
	if (PQrequestCancel(cancelConn))
	{
		fprintf(stderr, "\nCANCEL request sent\n");
	}
	else
	{
		fprintf(stderr, "\nCannot send cancel request:\n%s\n",
				PQerrorMessage(cancelConn));
	}
}


/*
 * listAllDbs
 *
 * list all the databases in the system returns 0 if all went well
 *
 *
 */

static int
listAllDbs(PsqlSettings *pset)
{
	PGresult   *results;
	char	   *query = "select * from pg_database;";

	if (!(results = PSQLexec(pset, query)))
		return 1;
	else
	{
		PQprint(pset->queryFout,
				results,
				&pset->opt);
		PQclear(results);
		return 0;
	}
}

/*
 * List The Database Tables returns 0 if all went well
 *
 */
int
tableList(PsqlSettings *pset, bool deep_tablelist, char info_type,
		  bool system_tables)
{
	char		listbuf[512];
	int			nColumns;
	int			i;
	char	   *rk;
	char	   *rr;
	PGresult   *res;
	int			usePipe = 0;
	char	   *pagerenv;
	FILE	   *fout;

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

	listbuf[0] = '\0';
	strcat(listbuf, "SELECT usename, relname, relkind, relhasrules ");
	strcat(listbuf, "FROM pg_class, pg_user ");
	switch (info_type)
	{
		case 't':
			strcat(listbuf, "WHERE ( relkind = 'r') ");
			break;
		case 'i':
			strcat(listbuf, "WHERE ( relkind = 'i') ");
			break;
		case 'S':
			strcat(listbuf, "WHERE ( relkind = 'S') ");
			break;
		case 'b':
		default:
			strcat(listbuf, "WHERE ( relkind = 'r' OR relkind = 'i' OR relkind = 'S') ");
			break;
	}
	if (!system_tables)
		strcat(listbuf, "  and relname !~ '^pg_'");
	else
		strcat(listbuf, "  and relname ~ '^pg_'");
	strcat(listbuf, "  and relname !~ '^xin[vx][0-9]+'");

	/*
	 * the usesysid = relowner won't work on stock 1.0 dbs, need to add in
	 * the int4oideq function
	 */
	strcat(listbuf, " and usesysid = relowner");
	strcat(listbuf, " ORDER BY relname ");
	if (!(res = PSQLexec(pset, listbuf)))
		return -1;
	/* first, print out the attribute names */
	nColumns = PQntuples(res);
	if (nColumns > 0)
	{
		if (pset->notty == 0 &&
			(pagerenv = getenv("PAGER")) &&
			pagerenv[0] != '\0' &&
			(deep_tablelist ||
			 screen_size.ws_row <= nColumns + 7) &&
			(fout = popen(pagerenv, "w")))
		{
			usePipe = 1;
			pqsignal(SIGPIPE, SIG_IGN);
		}
		else
			fout = stdout;

		if (deep_tablelist)
		{
			/* describe everything here */
			char	  **table;

			table = (char **) malloc(nColumns * sizeof(char *));
			if (table == NULL)
				perror("malloc");

			/* load table table */
			for (i = 0; i < nColumns; i++)
			{
				table[i] = (char *) malloc(PQgetlength(res, i, 1) * sizeof(char) + 1);
				if (table[i] == NULL)
					perror("malloc");
				strcpy(table[i], PQgetvalue(res, i, 1));
			}

			PQclear(res);
			for (i = 0; i < nColumns; i++)
				tableDesc(pset, table[i], fout);
			free(table);
		}
		else
		{
			/* Display the information */

			fprintf(fout, "\nDatabase    = %s\n", PQdb(pset->db));
			fprintf(fout, " +------------------+----------------------------------+----------+\n");
			fprintf(fout, " |  Owner           |             Relation             |   Type   |\n");
			fprintf(fout, " +------------------+----------------------------------+----------+\n");

			/* next, print out the instances */
			for (i = 0; i < PQntuples(res); i++)
			{
				fprintf(fout, " | %-16.16s", PQgetvalue(res, i, 0));
				fprintf(fout, " | %-32.32s | ", PQgetvalue(res, i, 1));
				rk = PQgetvalue(res, i, 2);
				rr = PQgetvalue(res, i, 3);
				if (strcmp(rk, "r") == 0)
					fprintf(fout, "%-8.8s |", (rr[0] == 't') ? "view?" : "table");
				else if (strcmp(rk, "i") == 0)
					fprintf(fout, "%-8.8s |", "index");
				else
					fprintf(fout, "%-8.8s |", "sequence");
				fprintf(fout, "\n");
			}
			fprintf(fout, " +------------------+----------------------------------+----------+\n");
			PQclear(res);
		}
		if (usePipe)
		{
			pclose(fout);
			pqsignal(SIGPIPE, SIG_DFL);
		}
		return 0;

	}
	else
	{
		PQclear(res);
		switch (info_type)
		{
			case 't':
				fprintf(stderr, "Couldn't find any tables!\n");
				break;
			case 'i':
				fprintf(stderr, "Couldn't find any indices!\n");
				break;
			case 'S':
				fprintf(stderr, "Couldn't find any sequences!\n");
				break;
			case 'b':
			default:
				fprintf(stderr, "Couldn't find any tables, sequences or indices!\n");
				break;
		}
		return -1;
	}
}

/*
 * List Tables Grant/Revoke Permissions returns 0 if all went well
 *
 */
int
rightsList(PsqlSettings *pset)
{
	char		listbuf[512];
	int			nColumns;
	int			i;
	int			usePipe = 0;
	char	   *pagerenv;
	FILE	   *fout;
	PGresult   *res;

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

	listbuf[0] = '\0';
	strcat(listbuf, "SELECT relname, relacl ");
	strcat(listbuf, "FROM pg_class, pg_user ");
	strcat(listbuf, "WHERE ( relkind = 'r' OR relkind = 'i' OR relkind = 'S') ");
	strcat(listbuf, "  and relname !~ '^pg_'");
	strcat(listbuf, "  and relname !~ '^xin[vx][0-9]+'");
	strcat(listbuf, "  and usesysid = relowner");
	strcat(listbuf, "  ORDER BY relname ");
	if (!(res = PSQLexec(pset, listbuf)))
		return -1;
	/* first, print out the attribute names */
	nColumns = PQntuples(res);
	if (nColumns > 0)
	{
		if (pset->notty == 0 &&
			(pagerenv = getenv("PAGER")) &&
			pagerenv[0] != '\0' &&
			screen_size.ws_row <= nColumns + 7 &&
			(fout = popen(pagerenv, "w")))
		{
			usePipe = 1;
			pqsignal(SIGPIPE, SIG_IGN);
		}
		else
			fout = stdout;

		/* Display the information */

		fprintf(fout, "\nDatabase    = %s\n", PQdb(pset->db));
		fprintf(fout, " +------------------+----------------------------------------------------+\n");
		fprintf(fout, " |  Relation        |             Grant/Revoke Permissions               |\n");
		fprintf(fout, " +------------------+----------------------------------------------------+\n");

		/* next, print out the instances */
		for (i = 0; i < PQntuples(res); i++)
		{
			fprintf(fout, " | %-16.16s", PQgetvalue(res, i, 0));
			fprintf(fout, " | %-50.50s | ", PQgetvalue(res, i, 1));
			fprintf(fout, "\n");
		}
		fprintf(fout, " +------------------+----------------------------------------------------+\n");
		PQclear(res);
		if (usePipe)
		{
			pclose(fout);
			pqsignal(SIGPIPE, SIG_DFL);
		}
		return 0;
	}
	else
	{
		fprintf(stderr, "Couldn't find any tables!\n");
		return -1;
	}
}

/*
 * Describe a table
 *
 * Describe the columns in a database table. returns 0 if all went well
 *
 *
 */
int
tableDesc(PsqlSettings *pset, char *table, FILE *fout)
{
	char		descbuf[512];
	int			nColumns;
	char	   *rtype;
	char	   *rnotnull;
	char	   *rhasdef;
	int			i;
	int			attlen,
				atttypmod;
	PGresult   *res,
			   *res2;
	int			usePipe = 0;
	char	   *pagerenv;

#ifdef TIOCGWINSZ
	if (fout == NULL &&
		pset->notty == 0 &&
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

	/* Build the query */

	/*
	 * if the table name is surrounded by double-quotes, then don't
	 * convert case
	 */
	if (*table == '"')
	{
		table++;
		if (*(table + strlen(table) - 1) == '"')
			*(table + strlen(table) - 1) = '\0';
	}
	else
	{
		for (i = 0; table[i]; i++)
			if (isupper(table[i]))
				table[i] = tolower(table[i]);
	}

	descbuf[0] = '\0';
	strcat(descbuf, "SELECT a.attnum, a.attname, t.typname, a.attlen, ");
	strcat(descbuf, "a.atttypmod, a.attnotnull, a.atthasdef ");
	strcat(descbuf, "FROM pg_class c, pg_attribute a, pg_type t ");
	strcat(descbuf, "WHERE c.relname = '");
	strcat(descbuf, table);
	strcat(descbuf, "'");
	strcat(descbuf, "    and a.attnum > 0 ");
	strcat(descbuf, "    and a.attrelid = c.oid ");
	strcat(descbuf, "    and a.atttypid = t.oid ");
	strcat(descbuf, "  ORDER BY attnum ");
	if (!(res = PSQLexec(pset, descbuf)))
		return -1;
	/* first, print out the attribute names */
	nColumns = PQntuples(res);
	if (nColumns > 0)
	{
		if (fout == NULL)
		{
			if (pset->notty == 0 &&
				(pagerenv = getenv("PAGER")) &&
				pagerenv[0] != '\0' &&
				screen_size.ws_row <= nColumns + 7 &&
				(fout = popen(pagerenv, "w")))
			{
				usePipe = 1;
				pqsignal(SIGPIPE, SIG_IGN);
			}
			else
				fout = stdout;
		}

		/*
		 * Display the information
		 */

		fprintf(fout, "\nTable    = %s\n", table);
		fprintf(fout, "+----------------------------------+----------------------------------+-------+\n");
		fprintf(fout, "|              Field               |              Type                | Length|\n");
		fprintf(fout, "+----------------------------------+----------------------------------+-------+\n");

		/* next, print out the instances */
		for (i = 0; i < PQntuples(res); i++)
		{
			char		type_str[33];

			fprintf(fout, "| %-32.32s | ", PQgetvalue(res, i, 1));
			rtype = PQgetvalue(res, i, 2);
			attlen = atoi(PQgetvalue(res, i, 3));
			atttypmod = atoi(PQgetvalue(res, i, 4));
			rnotnull = PQgetvalue(res, i, 5);
			rhasdef = PQgetvalue(res, i, 6);

			strcpy(type_str, rtype);
			if (strcmp(rtype, "bpchar") == 0)
				strcpy(type_str, "char()");
			else if (strcmp(rtype, "varchar") == 0)
				strcpy(type_str, "varchar()");
			else if (rtype[0] == '_')
			{
				strcpy(type_str, rtype + 1);
				strncat(type_str, "[]", 32 - strlen(type_str));
				type_str[32] = '\0';
			}

			if (rnotnull[0] == 't')
			{
				strncat(type_str, " not null", 32 - strlen(type_str));
				type_str[32] = '\0';
			}
			if (rhasdef[0] == 't')
			{
				descbuf[0] = '\0';
				strcat(descbuf, "SELECT d.adsrc ");
				strcat(descbuf, "FROM pg_attrdef d, pg_class c ");
				strcat(descbuf, "WHERE c.relname = '");
				strcat(descbuf, table);
				strcat(descbuf, "'");
				strcat(descbuf, "    and c.oid = d.adrelid ");
				strcat(descbuf, "    and d.adnum = ");
				strcat(descbuf, PQgetvalue(res, i, 0));
				if (!(res2 = PSQLexec(pset, descbuf)))
					return -1;
				strcat(type_str, " default ");
				strncat(type_str, PQgetvalue(res2, 0, 0), 32 - strlen(type_str));
				type_str[32] = '\0';
			}
			fprintf(fout, "%-32.32s |", type_str);

			if (strcmp(rtype, "text") == 0)
				fprintf(fout, "%6s |", "var");
			else if (strcmp(rtype, "bpchar") == 0 ||
					 strcmp(rtype, "varchar") == 0)
				fprintf(fout, "%6i |", atttypmod != -1 ? atttypmod - VARHDRSZ : 0);
			else
			{
				if (attlen > 0)
					fprintf(fout, "%6i |", attlen);
				else
					fprintf(fout, "%6s |", "var");
			}
			fprintf(fout, "\n");
		}
		fprintf(fout, "+----------------------------------+----------------------------------+-------+\n");
		PQclear(res);
		if (usePipe)
		{
			pclose(fout);
			pqsignal(SIGPIPE, SIG_DFL);
		}
		return 0;
	}
	else
	{
		fprintf(stderr, "Couldn't find table %s!\n", table);
		return -1;
	}
}

/*
 * Get object comments
 *
 * Describe the columns in a database table. returns 0 if all went well
 *
 *
 */
int
objectDescription(PsqlSettings *pset, char *object, FILE *fout)
{
	char		descbuf[512];
	int			nDescriptions;
	int			i;
	PGresult   *res;
	int			usePipe = 0;
	char	   *pagerenv;

#ifdef TIOCGWINSZ
	if (fout == NULL &&
		pset->notty == 0 &&
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

	/* Build the query */

	while (isspace(*object))
		object++;

	/*
	 * if the object name is surrounded by double-quotes, then don't
	 * convert case
	 */
	if (*object == '"')
	{
		object++;
		if (*(object + strlen(object) - 1) == '"')
			*(object + strlen(object) - 1) = '\0';
	}
	else
	{
		for (i = 0; object[i]; i++)
			if (isupper(object[i]))
				object[i] = tolower(object[i]);
	}

	descbuf[0] = '\0';
	if (strchr(object, '.') != NULL)
	{
		char		table[NAMEDATALEN],
					column[NAMEDATALEN];

		StrNCpy(table, object,
				((strchr(object, '.') - object + 1) < NAMEDATALEN) ?
				(strchr(object, '.') - object + 1) : NAMEDATALEN);
		StrNCpy(column, strchr(object, '.') + 1, NAMEDATALEN);
		strcat(descbuf, "SELECT DISTINCT description ");
		strcat(descbuf, "FROM pg_class, pg_attribute, pg_description ");
		strcat(descbuf, "WHERE pg_class.relname = '");
		strcat(descbuf, table);
		strcat(descbuf, "' and ");
		strcat(descbuf, "pg_class.oid = pg_attribute.attrelid and ");
		strcat(descbuf, "pg_attribute.attname = '");
		strcat(descbuf, column);
		strcat(descbuf, "' and ");
		strcat(descbuf, " pg_attribute.oid = pg_description.objoid ");
		if (!(res = PSQLexec(pset, descbuf)))
			return -1;
	}
	else
	{
		strcat(descbuf, "SELECT DISTINCT description ");
		strcat(descbuf, "FROM pg_class, pg_description ");
		strcat(descbuf, "WHERE pg_class.relname = '");
		strcat(descbuf, object);
		strcat(descbuf, "'");
		strcat(descbuf, " and pg_class.oid = pg_description.objoid ");
		if (!(res = PSQLexec(pset, descbuf)))
			return -1;
		else if (PQntuples(res) <= 0)
		{
			PQclear(res);
			descbuf[0] = '\0';
			strcat(descbuf, "SELECT DISTINCT description ");
			strcat(descbuf, "FROM pg_type, pg_description ");
			strcat(descbuf, "WHERE pg_type.typname = '");
			strcat(descbuf, object);
			strcat(descbuf, "' and ");
			strcat(descbuf, " pg_type.oid = pg_description.objoid ");
			if (!(res = PSQLexec(pset, descbuf)))
				return -1;
			else if (PQntuples(res) <= 0)
			{
				PQclear(res);
				descbuf[0] = '\0';
				strcat(descbuf, "SELECT DISTINCT description ");
				strcat(descbuf, "FROM pg_proc, pg_description ");
				strcat(descbuf, "WHERE pg_proc.proname = '");
				strcat(descbuf, object);
				strcat(descbuf, "'");
				strcat(descbuf, " and pg_proc.oid = pg_description.objoid ");
				if (!(res = PSQLexec(pset, descbuf)))
					return -1;
				else if (PQntuples(res) <= 0)
				{
					PQclear(res);
					descbuf[0] = '\0';
					strcat(descbuf, "SELECT DISTINCT description ");
					strcat(descbuf, "FROM pg_operator, pg_description ");
					strcat(descbuf, "WHERE pg_operator.oprname = '");
					strcat(descbuf, object);
					strcat(descbuf, "'");
					/* operator descriptions are attached to the proc */
					strcat(descbuf, " and RegprocToOid(pg_operator.oprcode) = pg_description.objoid ");
					if (!(res = PSQLexec(pset, descbuf)))
						return -1;
					else if (PQntuples(res) <= 0)
					{
						PQclear(res);
						descbuf[0] = '\0';
						strcat(descbuf, "SELECT DISTINCT description ");
						strcat(descbuf, "FROM pg_aggregate, pg_description ");
						strcat(descbuf, "WHERE pg_aggregate.aggname = '");
						strcat(descbuf, object);
						strcat(descbuf, "'");
						strcat(descbuf, " and pg_aggregate.oid = pg_description.objoid ");
						if (!(res = PSQLexec(pset, descbuf)))
							return -1;
					}
				}
			}
		}
	}
	nDescriptions = PQntuples(res);
	if (nDescriptions > 0)
	{
		if (fout == NULL)
		{
			if (pset->notty == 0 &&
				(pagerenv = getenv("PAGER")) &&
				pagerenv[0] != '\0' &&
				screen_size.ws_row <= nDescriptions + 1 &&
				(fout = popen(pagerenv, "w")))
			{
				usePipe = 1;
				pqsignal(SIGPIPE, SIG_IGN);
			}
			else
				fout = stdout;
		}

		/*
		 * * Display the information
		 */

		fprintf(fout, "\nObject    = %s\n", object);

		/* next, print out the instances */
		for (i = 0; i < PQntuples(res); i++)
			fprintf(fout, "%s\n", PQgetvalue(res, i, 0));

		PQclear(res);
		if (usePipe)
		{
			pclose(fout);
			pqsignal(SIGPIPE, SIG_DFL);
		}
		return 0;

	}
	else
	{
		fprintf(stderr, "Couldn't find comments for object %s!\n", object);
		return -1;
	}
}

typedef char *(*READ_ROUTINE) (char *prompt, FILE *source);

/*
 * gets_noreadline	prompt source gets a line of input without calling
 * readline, the source is ignored
 */
static char *
gets_noreadline(char *prompt, FILE *source)
{
	fputs(prompt, stdout);
	fflush(stdout);
	return gets_fromFile(prompt, stdin);
}

/*
 * gets_readline  prompt source the routine to get input from GNU readline(),
 * the source is ignored the prompt argument is used as the prompting string
 */
static char *
gets_readline(char *prompt, FILE *source)
{
	char	   *s;

#ifdef USE_READLINE
	s = readline(prompt);
#else
	char		buf[500];

	printf("%s", prompt);
	s = fgets(buf, 500, stdin);
#endif
	fputc('\r', stdout);
	fflush(stdout);
	return s;
}

/*
 * gets_fromFile  prompt source
 *
 * the routine to read from a file, the prompt argument is ignored the source
 * argument is a FILE *
 */
static char *
gets_fromFile(char *prompt, FILE *source)
{
	char	   *line;
	int			len;

	line = malloc(MAX_QUERY_BUFFER + 1);

	/* read up to MAX_QUERY_BUFFER characters */
	if (fgets(line, MAX_QUERY_BUFFER, source) == NULL)
	{
		free(line);
		return NULL;
	}

	line[MAX_QUERY_BUFFER - 1] = '\0';
	len = strlen(line);
	if (len == MAX_QUERY_BUFFER)
	{
		fprintf(stderr, "line read exceeds maximum length.  Truncating at %d\n",
				MAX_QUERY_BUFFER);
	}
	return line;
}

/*
 * SendQuery: send the query string to the backend return *success_p = 1 if
 * the query executed successfully returns *success_p = 0 otherwise
 */
static void
SendQuery(bool *success_p, PsqlSettings *pset, const char *query,
		  const bool copy_in, const bool copy_out, FILE *copystream)
{

	PGresult   *results;
	PGnotify   *notify;

	if (pset->singleStep)
		fprintf(stdout, "\n**************************************"
				"*****************************************\n");

	if (pset->echoQuery || pset->singleStep)
	{
		fprintf(stderr, "QUERY: %s\n", query);
		fflush(stderr);
	}
	if (pset->singleStep)
	{
		fprintf(stdout, "\n**************************************"
				"*****************************************\n");
		fflush(stdout);
		printf("\npress return to continue ..\n");
		gets_fromFile("", stdin);
	}
	results = PQexec(pset->db, query);
	if (results == NULL)
	{
		fprintf(stderr, "%s", PQerrorMessage(pset->db));
		*success_p = false;
	}
	else
	{
		switch (PQresultStatus(results))
		{
			case PGRES_TUPLES_OK:
				if (pset->gfname)
				{
					PsqlSettings settings_copy = *pset;
					FILE	   *fp;

					settings_copy.queryFout = stdout;
					fp = setFout(&settings_copy, pset->gfname);
					if (!fp || fp == stdout)
					{
						*success_p = false;
						break;
					}
					else
						*success_p = true;
					PQprint(fp,
							results,
							&pset->opt);
					if (settings_copy.pipe)
						pclose(fp);
					else
						fclose(fp);
					free(pset->gfname);
					pset->gfname = NULL;
					break;
				}
				else
				{
					*success_p = true;
					PQprint(pset->queryFout,
							results,
							&(pset->opt));
					fflush(pset->queryFout);
				}
				break;
			case PGRES_EMPTY_QUERY:
				*success_p = true;
				break;
			case PGRES_COMMAND_OK:
				*success_p = true;
				if (!pset->quiet)
					printf("%s\n", PQcmdStatus(results));
				break;
			case PGRES_COPY_OUT:
				*success_p = true;
				if (copy_out)
					handleCopyOut(results, copystream);
				else
				{
					if (!pset->quiet)
						printf("Copy command returns...\n");

					handleCopyOut(results, stdout);
				}
				break;
			case PGRES_COPY_IN:
				*success_p = true;
				if (copy_in)
					handleCopyIn(results, false, copystream);
				else
					handleCopyIn(results, !pset->quiet && !pset->notty, stdin);
				break;
			case PGRES_NONFATAL_ERROR:
			case PGRES_FATAL_ERROR:
			case PGRES_BAD_RESPONSE:
				*success_p = false;
				fprintf(stderr, "%s", PQerrorMessage(pset->db));
				break;
		}

		if (PQstatus(pset->db) == CONNECTION_BAD)
		{
			fprintf(stderr,
					"We have lost the connection to the backend, so "
					"further processing is impossible.  "
					"Terminating.\n");
			exit(2);			/* we are out'ta here */
		}
		/* check for asynchronous returns */
		while ((notify = PQnotifies(pset->db)) != NULL)
		{
			fprintf(stderr,
				 "ASYNC NOTIFY of '%s' from backend pid '%d' received\n",
					notify->relname, notify->be_pid);
			free(notify);
		}
		if (results)
			PQclear(results);
	}
}



static void
editFile(char *fname)
{
	char	   *editorName;
	char	   *sys;

	editorName = getenv("EDITOR");
	if (!editorName)
		editorName = DEFAULT_EDITOR;
	sys = malloc(strlen(editorName) + strlen(fname) + 32 + 1);
	if (!sys)
	{
		perror("malloc");
		exit(1);
	}
	sprintf(sys, "exec '%s' '%s'", editorName, fname);
	system(sys);
	free(sys);
}

static bool
toggle(PsqlSettings *pset, bool *sw, char *msg)
{
	*sw = !*sw;
	if (!pset->quiet)
		printf("turned %s %s\n", on(*sw), msg);
	return *sw;
}



static void
unescape(char *dest, const char *source)
{
	/*-----------------------------------------------------------------------------
	  Return as the string <dest> the value of string <source> with escape
	  sequences turned into the bytes they represent.
	-----------------------------------------------------------------------------*/
	char	   *p;
	bool		esc;			/* Last character we saw was the escape
								 * character (/) */

	esc = false;				/* Haven't seen escape character yet */
	for (p = (char *) source; *p; p++)
	{
		char		c;			/* Our output character */

		if (esc)
		{
			switch (*p)
			{
				case 'n':
					c = '\n';
					break;
				case 'r':
					c = '\r';
					break;
				case 't':
					c = '\t';
					break;
				case 'f':
					c = '\f';
					break;
				case '\\':
					c = '\\';
					break;
				default:
					c = *p;
			}
			esc = false;
		}
		else if (*p == '\\')
		{
			esc = true;
			c = ' ';			/* meaningless, but compiler doesn't know
								 * that */
		}
		else
		{
			c = *p;
			esc = false;
		}
		if (!esc)
			*dest++ = c;
	}
	*dest = '\0';				/* Terminating null character */
}



static void
parse_slash_copy(const char *args, char *table, const int table_len,
				 char *file, const int file_len,
				 bool *from_p, bool *error_p)
{

	char		work_args[200];

	/*
	 * A copy of the \copy command arguments, except that we modify it as
	 * we parse to suit our parsing needs.
	 */
	char	   *table_tok,
			   *fromto_tok;

	strncpy(work_args, args, sizeof(work_args));
	work_args[sizeof(work_args) - 1] = '\0';

	*error_p = false;			/* initial assumption */

	table_tok = strtok(work_args, " ");
	if (table_tok == NULL)
	{
		fprintf(stderr, "\\copy needs arguments.\n");
		*error_p = true;
	}
	else
	{
		strncpy(table, table_tok, table_len);
		file[table_len - 1] = '\0';

		fromto_tok = strtok(NULL, "  ");
		if (fromto_tok == NULL)
		{
			fprintf(stderr, "'FROM' or 'TO' must follow table name.\n");
			*error_p = true;
		}
		else
		{
			if (strcasecmp(fromto_tok, "from") == 0)
				*from_p = true;
			else if (strcasecmp(fromto_tok, "to") == 0)
				*from_p = false;
			else
			{
				fprintf(stderr,
						"Unrecognized token found where "
						"'FROM' or 'TO' expected: '%s'.\n",
						fromto_tok);
				*error_p = true;
			}
			if (!*error_p)
			{
				char	   *file_tok;

				file_tok = strtok(NULL, " ");
				if (file_tok == NULL)
				{
					fprintf(stderr, "A file pathname must follow '%s'.\n",
							fromto_tok);
					*error_p = true;
				}
				else
				{
					strncpy(file, file_tok, file_len);
					file[file_len - 1] = '\0';
					if (strtok(NULL, " ") != NULL)
					{
						fprintf(stderr,
						  "You have extra tokens after the filename.\n");
						*error_p = true;
					}
				}
			}
		}
	}
}



static void
do_copy(const char *args, PsqlSettings *pset)
{
	/*---------------------------------------------------------------------------
	  Execute a \copy command (frontend copy).	We have to open a file, then
	  submit a COPY query to the backend and either feed it data from the
	  file or route its response into the file.

	  We do a text copy with default (tab) column delimiters.  Some day, we
	  should do all the things a backend copy can do.

	----------------------------------------------------------------------------*/
	char		query[200];

	/* The COPY command we send to the back end */
	bool		from;

	/* The direction of the copy is from a file to a table. */
	char		file[MAXPATHLEN + 1];

	/* The pathname of the file from/to which we copy */
	char		table[NAMEDATALEN];

	/* The name of the table from/to which we copy */
	bool		syntax_error;

	/* The \c command has invalid syntax */
	FILE	   *copystream;

	parse_slash_copy(args, table, sizeof(table), file, sizeof(file),
					 &from, &syntax_error);

	if (!syntax_error)
	{
		strcpy(query, "COPY ");
		strcat(query, table);

		if (from)
			strcat(query, " FROM stdin");
		else
			strcat(query, " TO stdout");

		if (from)
		{
			copystream = fopen(file, "r");
		}
		else
		{
			copystream = fopen(file, "w");
		}
		if (copystream == NULL)
			fprintf(stderr,
				"Unable to open file %s which to copy, errno = %s (%d).",
					from ? "from" : "to", strerror(errno), errno);
		else
		{
			bool		success;/* The query succeeded at the backend */

			SendQuery(&success, pset, query, from, !from, copystream);
			fclose(copystream);
			if (!pset->quiet)
			{
				if (success)
					printf("Successfully copied.\n");
				else
					printf("Copy failed.\n");
			}
		}
	}
}


static void
do_connect(const char *new_dbname,
		   const char *new_user,
		   PsqlSettings *pset)
{
	if (!new_dbname)
		fprintf(stderr, "\\connect must be followed by a database name\n");
	else if (new_user != NULL && pset->getPassword)
		fprintf(stderr, "You can't specify a username when using passwords.\n");
	else
	{
		PGconn	   *olddb = pset->db;
		static char *userenv = NULL;
		char	   *old_userenv = NULL;
		const char *dbparam;

		if (new_user != NULL)
		{

			/*
			 * PQsetdb() does not allow us to specify the user, so we have
			 * to do it via PGUSER
			 */
			if (userenv != NULL)
				old_userenv = userenv;
			userenv = malloc(strlen("PGUSER=") + strlen(new_user) + 1);
			sprintf(userenv, "PGUSER=%s", new_user);
			/* putenv() may continue to use memory as part of environment */
			putenv(userenv);
			/* can delete old memory if we malloc'ed it */
			if (old_userenv != NULL)
				free(old_userenv);
		}

		if (strcmp(new_dbname, "-") != 0)
			dbparam = new_dbname;
		else
			dbparam = PQdb(olddb);

		pset->db = PQsetdb(PQhost(olddb), PQport(olddb),
						   NULL, NULL, dbparam);
		if (!pset->quiet)
		{
			if (!new_user)
				printf("connecting to new database: %s\n", dbparam);
			else if (dbparam != new_dbname)
				printf("connecting as new user: %s\n", new_user);
			else
				printf("connecting to new database: %s as user: %s\n",
					   dbparam, new_user);
		}

		if (PQstatus(pset->db) == CONNECTION_BAD)
		{
			fprintf(stderr, "%s\n", PQerrorMessage(pset->db));
			fprintf(stderr, "Could not connect to new database. exiting\n");
			exit(2);
		}
		else
		{
			cancelConn = pset->db; /* redirect sigint's loving attentions */
			PQfinish(olddb);
			free(pset->prompt);
			pset->prompt = malloc(strlen(PQdb(pset->db)) + 10);
			sprintf(pset->prompt, "%s%s", PQdb(pset->db), PROMPT);
		}
	}
}


static void
do_edit(const char *filename_arg, char *query, int *status_p)
{

	int			fd;
	char		tmp[64];
	char	   *fname;
	int			cc;
	const int	ql = strlen(query);
	bool		error;

	if (filename_arg)
	{
		fname = (char *) filename_arg;
		error = false;
	}
	else
	{
		sprintf(tmp, "/tmp/psql.%ld.%ld", (long) geteuid(), (long) getpid());
		fname = tmp;
		unlink(tmp);
		if (ql > 0)
		{
			if ((fd = open(tmp, O_EXCL | O_CREAT | O_WRONLY, 0600)) == -1)
			{
				perror(tmp);
				error = true;
			}
			else
			{
				if (query[ql - 1] != '\n')
					strcat(query, "\n");
				if (write(fd, query, ql) != ql)
				{
					perror(tmp);
					close(fd);
					unlink(tmp);
					error = true;
				}
				else
					error = false;
				close(fd);
			}
		}
		else
			error = false;
	}

	if (error)
		*status_p = CMD_SKIP_LINE;
	else
	{
		editFile(fname);
		if ((fd = open(fname, O_RDONLY)) == -1)
		{
			perror(fname);
			if (!filename_arg)
				unlink(fname);
			*status_p = CMD_SKIP_LINE;
		}
		else
		{
			if ((cc = read(fd, query, MAX_QUERY_BUFFER)) == -1)
			{
				perror(fname);
				close(fd);
				if (!filename_arg)
					unlink(fname);
				*status_p = CMD_SKIP_LINE;
			}
			else
			{
				query[cc] = '\0';
				close(fd);
				if (!filename_arg)
					unlink(fname);
				rightTrim(query);
				*status_p = CMD_NEWEDIT;
			}
		}
	}
}



static void
do_help(PsqlSettings *pset, const char *topic)
{

	if (!topic)
	{
		char		left_center_right;	/* Which column we're displaying */
		int			i;			/* Index into QL_HELP[] */

		printf("type \\h <cmd> where <cmd> is one of the following:\n");

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
		printf("type \\h * for a complete description of all commands\n");
	}
	else
	{
		int			i;			/* Index into QL_HELP[] */
		bool		help_found; /* We found the help he asked for */

		int			usePipe = 0;
		char	   *pagerenv;
		FILE	   *fout;

		if (strcmp(topic, "*") == 0 &&
			(pset->notty == 0) &&
			(pagerenv = getenv("PAGER")) &&
			(pagerenv[0] != '\0') &&
			(fout = popen(pagerenv, "w")))
		{
			usePipe = 1;
			pqsignal(SIGPIPE, SIG_IGN);
		}
		else
			fout = stdout;

		help_found = false;		/* Haven't found it yet */
		for (i = 0; QL_HELP[i].cmd; i++)
		{
			if (strcmp(QL_HELP[i].cmd, topic) == 0 ||
				strcmp(topic, "*") == 0)
			{
				help_found = true;
				fprintf(fout, "Command: %s\n", QL_HELP[i].cmd);
				fprintf(fout, "Description: %s\n", QL_HELP[i].help);
				fprintf(fout, "Syntax:\n");
				fprintf(fout, "%s\n", QL_HELP[i].syntax);
				fprintf(fout, "\n");
			}
		}

		if (usePipe)
		{
			pclose(fout);
			pqsignal(SIGPIPE, SIG_DFL);
		}

		if (!help_found)
			fprintf(stderr, "command not found, "
					"try \\h with no arguments to see available help\n");
	}
}



static void
do_shell(const char *command)
{

	if (!command)
	{
		char	   *sys;
		char	   *shellName;

		shellName = getenv("SHELL");
		if (shellName == NULL)
			shellName = DEFAULT_SHELL;
		sys = malloc(strlen(shellName) + 16);
		if (!sys)
		{
			perror("malloc");
			exit(1);
		}
		sprintf(sys, "exec %s", shellName);
		system(sys);
		free(sys);
	}
	else
		system(command);
}



/*
 * HandleSlashCmds:
 *
 * Handles all the different commands that start with \ db_ptr is a pointer to
 * the TgDb* structure line is the current input line prompt_ptr is a pointer
 * to the prompt string, a pointer is used because the prompt can be used
 * with a connection to a new database.
 * Returns a status:
 *	0 - send currently constructed query to backend (i.e. we got a \g)
 *	1 - skip processing of this line, continue building up query
 *	2 - terminate processing of this query entirely
 *	3 - new query supplied by edit
 */
static int
HandleSlashCmds(PsqlSettings *pset,
				char *line,
				char *query)
{
	int			status = CMD_SKIP_LINE;
	char	   *optarg;
	bool		success;

	/*
	 * Pointer inside the <cmd> string to the argument of the slash
	 * command, assuming it is a one-character slash command.  If it's not
	 * a one-character command, this is meaningless.
	 */
	char	   *optarg2;

	/*
	 * Pointer inside the <cmd> string to the argument of the slash
	 * command assuming it's not a one-character command.  If it's a
	 * one-character command, this is meaningless.
	 */
	char	   *cmd;

	/*
	 * String: value of the slash command, less the slash and with escape
	 * sequences decoded.
	 */
	int			blank_loc;

	/* Offset within <cmd> of first blank */

	cmd = malloc(strlen(line)); /* unescaping better not make string grow. */

	unescape(cmd, line + 1);	/* sets cmd string */

	if (strlen(cmd) >= 1 && cmd[strlen(cmd) - 1] == ';')		/* strip trailing ; */
		cmd[strlen(cmd) - 1] = '\0';

	/*
	 * Originally, there were just single character commands.  Now, we
	 * define some longer, friendly commands, but we have to keep the old
	 * single character commands too.  \c used to be what \connect is now.
	 * Complicating matters is the fact that with the single-character
	 * commands, you can start the argument right after the single
	 * character, so "\copy" would mean "connect to database named 'opy'".
	 */

	if (strlen(cmd) > 1)
		optarg = cmd + 1 + strspn(cmd + 1, " \t");
	else
		optarg = NULL;

	blank_loc = strcspn(cmd, " \t");
	if (blank_loc == 0 || !cmd[blank_loc])
		optarg2 = NULL;
	else
		optarg2 = cmd + blank_loc + strspn(cmd + blank_loc, " \t");

	switch (cmd[0])
	{
		case 'a':				/* toggles to align fields on output */
			toggle(pset, &pset->opt.align, "field alignment");
			break;
		case 'C':				/* define new caption */
			if (pset->opt.caption)
			{
				free(pset->opt.caption);
				pset->opt.caption = NULL;
			}
			if (optarg && !(pset->opt.caption = strdup(optarg)))
			{
				perror("malloc");
				exit(CMD_TERMINATE);
			}
			break;
		case 'c':
			{
				if (strncmp(cmd, "copy ", strlen("copy ")) == 0 ||
				    strncmp(cmd, "copy	", strlen("copy	")) == 0)
					do_copy(optarg2, pset);
				else if (strcmp(cmd, "copy") == 0)
				{
					fprintf(stderr, "See \\? for help\n");
					break;
				}
				else if (strncmp(cmd, "connect ", strlen("connect ")) == 0 ||
				  strcmp(cmd, "connect") == 0 /* issue error message */ )
				{
					char	   *optarg3 = NULL;
					int			blank_loc2;

					if (optarg2)
					{
						blank_loc2 = strcspn(optarg2, " \t");
						if (blank_loc2 == 0 || *(optarg2 + blank_loc2) == '\0')
							optarg3 = NULL;
						else
						{
							optarg3 = optarg2 + blank_loc2 +
								strspn(optarg2 + blank_loc2, " \t");
							*(optarg2 + blank_loc2) = '\0';
						}
					}
					do_connect(optarg2, optarg3, pset);
				}
				else
				{
					char	   *optarg3 = NULL;
					int			blank_loc2;

					if (optarg)
					{
						blank_loc2 = strcspn(optarg, " \t");
						if (blank_loc2 == 0 || *(optarg + blank_loc2) == '\0')
							optarg3 = NULL;
						else
						{
							optarg3 = optarg + blank_loc2 +
								strspn(optarg + blank_loc2, " \t");
							*(optarg + blank_loc2) = '\0';
						}
					}
					do_connect(optarg, optarg3, pset);
				}
			}
			break;
		case 'd':				/* \d describe database information */
			if (strncmp(cmd, "da", 2) == 0)
			{
				/* aggregates */
				SendQuery(&success, pset, "\
					SELECT	a.aggname AS aggname, \
							t.typname AS typname, \
							obj_description(a.oid) as description \
					FROM	pg_aggregate a, pg_type t \
					WHERE	a.aggbasetype = t.oid \
					ORDER BY aggname, typname;",
						  false, false, 0);
				SendQuery(&success, pset, "\
					SELECT	a.aggname AS aggname, \
							'all types' as all_types, \
							obj_description(a.oid) as description \
					FROM	pg_aggregate a \
					WHERE	a.aggbasetype = 0 \
					ORDER BY aggname;",
						  false, false, 0);
			}
			else if (strncmp(cmd, "dd", 2) == 0)
				/* descriptions */
				objectDescription(pset, optarg + 1, NULL);
			else if (strncmp(cmd, "df", 2) == 0)
			{
				/* functions/procedures */

				/*
				 * we skip in/out funcs by excluding functions that take
				 * some arguments, but have no types defined for those
				 * arguments
				 */
				SendQuery(&success, pset, "\
					SELECT	t.typname as return_type, \
							p.proname as function, \
							substr(oid8types(p.proargtypes),1,20) as arguments, \
							substr(obj_description(p.oid),1,28) as description \
					FROM 	pg_proc p, pg_type t \
					WHERE 	p.prorettype = t.oid and \
							(pronargs = 0 or oid8types(p.proargtypes) != '') and \
							t.typname != 'bool' \
					ORDER BY return_type, function;",
						  false, false, 0);
				SendQuery(&success, pset, "\
					SELECT	t.typname as rtns, \
							p.proname as function, \
							oid8types(p.proargtypes) as arguments, \
							substr(obj_description(p.oid),1,34) as description \
					FROM pg_proc p, pg_type t \
					WHERE p.prorettype = t.oid and \
							(pronargs = 0 or oid8types(p.proargtypes) != '') and \
							t.typname = 'bool' \
					ORDER BY rtns, function;",
						  false, false, 0);
			}
			else if (strncmp(cmd, "di", 2) == 0)
				/* only indices */
				tableList(pset, false, 'i', false);
			else if (strncmp(cmd, "do", 2) == 0)
			{
				/* operators */
				SendQuery(&success, pset, "\
					SELECT	o.oprname AS op, \
							t0.typname AS result, \
							t1.typname AS left_type, \
							t2.typname AS right_type, \
							substr(obj_description(p.oid),1,42) as description \
					FROM	pg_proc p, pg_type t0, \
							pg_type t1, pg_type t2, \
							pg_operator o \
					WHERE	p.prorettype = t0.oid AND \
							RegprocToOid(o.oprcode) = p.oid AND \
							p.pronargs = 2 AND \
							o.oprleft = t1.oid AND \
							o.oprright = t2.oid \
					ORDER BY op, result, left_type, right_type;",
						  false, false, 0);
				SendQuery(&success, pset, "\
					SELECT	o.oprname AS left_unary, \
							t0.typname AS return_type, \
							t1.typname AS operand, \
							obj_description(p.oid) as description \
					FROM	pg_operator o, pg_proc p, pg_type t0, pg_type t1 \
					WHERE	RegprocToOid(o.oprcode) = p.oid AND \
							o.oprresult = t0.oid AND \
							o.oprkind = 'l' AND \
							o.oprright = t1.oid \
					ORDER BY left_unary, return_type, operand;",
						  false, false, 0);
				SendQuery(&success, pset, "\
					SELECT	o.oprname AS right_unary, \
							t0.typname AS return_type, \
							t1.typname AS operand, \
							obj_description(p.oid) as description \
					FROM	pg_operator o, pg_proc p, pg_type t0, pg_type t1 \
					WHERE	RegprocToOid(o.oprcode) = p.oid AND \
							o.oprresult = t0.oid AND \
							o.oprkind = 'r' AND \
							o.oprleft = t1.oid \
					ORDER BY right_unary, return_type, operand;",
						  false, false, 0);
			}
			else if (strncmp(cmd, "ds", 2) == 0)
				/* only sequences */
				tableList(pset, false, 'S', false);
			else if (strncmp(cmd, "dS", 2) == 0)
				/* system tables */
				tableList(pset, false, 'b', true);
			else if (strncmp(cmd, "dt", 2) == 0)
				/* only tables */
				tableList(pset, false, 't', false);
			else if (strncmp(cmd, "dT", 2) == 0)
				/* types */
				SendQuery(&success, pset, "\
					SELECT	typname AS type, \
							obj_description(oid) as description \
					FROM	pg_type \
					WHERE	typrelid = 0 AND \
							typname !~ '^_.*' \
					ORDER BY type;",
						  false, false, 0);
			else if (!optarg)
				/* show tables, sequences and indices */
				tableList(pset, false, 'b', false);
			else if (strcmp(optarg, "*") == 0)
			{					/* show everything */
				if (tableList(pset, false, 'b', false) == 0)
					tableList(pset, true, 'b', false);
			}
			else if (strncmp(cmd, "d ", 2) == 0)
				/* describe the specified table */
				tableDesc(pset, optarg, NULL);
			else
				slashUsage(pset);

			break;
		case 'e':				/* edit */
			{
				do_edit(optarg, query, &status);
				break;
			}
		case 'E':
			{
				FILE	   *fd;
				static char *lastfile;
				struct stat st,
							st2;

				if (optarg)
				{
					if (lastfile)
						free(lastfile);
					lastfile = malloc(strlen(optarg + 1));
					if (!lastfile)
					{
						perror("malloc");
						exit(CMD_TERMINATE);
					}
					strcpy(lastfile, optarg);
				}
				else if (!lastfile)
				{
					fprintf(stderr, "\\r must be followed by a file name initially\n");
					break;
				}
				stat(lastfile, &st);
				editFile(lastfile);
				if ((stat(lastfile, &st2) == -1) || ((fd = fopen(lastfile, "r")) == NULL))
				{
					perror(lastfile);
					break;
				}
				if (st2.st_mtime == st.st_mtime)
				{
					if (!pset->quiet)
						fprintf(stderr, "warning: %s not modified. query not executed\n", lastfile);
					fclose(fd);
					break;
				}
				MainLoop(pset, query, fd);
				fclose(fd);
				break;
			}
		case 'f':
			{
				char	   *fs = DEFAULT_FIELD_SEP;

				if (optarg)
					fs = optarg;
				if (optarg && !*optarg && strlen(cmd) > 2)
					fs = cmd + 2;
				if (pset->opt.fieldSep)
					free(pset->opt.fieldSep);
				if (!(pset->opt.fieldSep = strdup(fs)))
				{
					perror("malloc");
					exit(CMD_TERMINATE);
				}
				if (!pset->quiet)
					printf("field separator changed to '%s'\n", pset->opt.fieldSep);
				break;
			}
		case 'g':				/* \g means send query */
			if (!optarg)
				pset->gfname = NULL;
			else if (!(pset->gfname = strdup(optarg)))
			{
				perror("malloc");
				exit(CMD_TERMINATE);
			}
			status = CMD_SEND;
			break;
		case 'h':				/* help */
			{
				do_help(pset, optarg);
				break;
			}
		case 'i':				/* \i is include file */
			{
				FILE	   *fd;

				if (!optarg)
				{
					fprintf(stderr, "\\i must be followed by a file name\n");
					break;
				}
				if ((fd = fopen(optarg, "r")) == NULL)
				{
					fprintf(stderr, "file named %s could not be opened\n", optarg);
					break;
				}
				MainLoop(pset, query, fd);
				fclose(fd);
				break;
			}
		case 'l':				/* \l is list database */
			listAllDbs(pset);
			break;
		case 'H':
			if (toggle(pset, &pset->opt.html3, "HTML3.0 tabular output"))
				pset->opt.standard = 0;
			break;
		case 'o':
			setFout(pset, optarg);
			break;
		case 'p':
			if (query)
			{
				fputs(query, stdout);
				fputc('\n', stdout);
			}
			break;
		case 'q':				/* \q is quit */
			status = CMD_TERMINATE;
			break;
		case 'r':				/* reset(clear) the buffer */
			query[0] = '\0';
			if (!pset->quiet)
				printf("buffer reset(cleared)\n");
			break;
		case 's':				/* \s is save history to a file */
			if (!optarg)
				optarg = "/dev/tty";
#ifdef USE_HISTORY
			if (write_history(optarg) != 0)
				fprintf(stderr, "cannot write history to %s\n", optarg);
#endif
			break;
		case 'm':				/* monitor like type-setting */
			if (toggle(pset, &pset->opt.standard, "standard SQL separaters and padding"))
			{
				pset->opt.html3 = pset->opt.expanded = 0;
				pset->opt.align = pset->opt.header = 1;
				if (pset->opt.fieldSep)
					free(pset->opt.fieldSep);
				pset->opt.fieldSep = strdup("|");
				if (!pset->quiet)
					printf("field separator changed to '%s'\n", pset->opt.fieldSep);
			}
			else
			{
				if (pset->opt.fieldSep)
					free(pset->opt.fieldSep);
				pset->opt.fieldSep = strdup(DEFAULT_FIELD_SEP);
				if (!pset->quiet)
					printf("field separator changed to '%s'\n", pset->opt.fieldSep);
			}
			break;
		case 'z':				/* list table rights (grant/revoke) */
			rightsList(pset);
			break;
		case 't':				/* toggle headers */
			toggle(pset, &pset->opt.header, "output headings and row count");
			break;
		case 'T':				/* define html <table ...> option */
			if (pset->opt.tableOpt)
				free(pset->opt.tableOpt);
			if (!optarg)
				pset->opt.tableOpt = NULL;
			else if (!(pset->opt.tableOpt = strdup(optarg)))
			{
				perror("malloc");
				exit(CMD_TERMINATE);
			}
			break;
		case 'x':
			toggle(pset, &pset->opt.expanded, "expanded table representation");
			break;
		case '!':
			do_shell(optarg);
			break;
		default:
		case '?':				/* \? is help */
			slashUsage(pset);
			break;
	}
	free(cmd);
	return status;
}	/* HandleSlashCmds() */

/* MainLoop()
 * Main processing loop for reading lines of input
 *	and sending them to the backend.
 *
 * This loop is re-entrant. May be called by \i command
 *	which reads input from a file.
 * db_ptr must be initialized and set.
 */

static int
MainLoop(PsqlSettings *pset, char *query, FILE *source)
{
	char	   *line;			/* line of input */
	char	   *xcomment;		/* start of extended comment */
	int			len;			/* length of the line */
	bool		query_alloced = false;
	int			successResult = 1;
	int			slashCmdStatus = CMD_SEND;

	/*
	 * slashCmdStatus can be: CMD_UNKNOWN	  - send currently constructed
	 * query to backend (i.e. we got a \g) CMD_SEND		   - send
	 * currently constructed query to backend (i.e. we got a \g)
	 * CMD_SKIP_LINE   - skip processing of this line, continue building
	 * up query CMD_TERMINATE	- terminate processing of this query
	 * entirely CMD_NEWEDIT		- new query supplied by edit
	 */

	bool		querySent = false;
	bool		interactive;
	READ_ROUTINE GetNextLine;
	bool		eof = 0;

	/* We've reached the end of our command input. */
	bool		success;
	bool		in_quote;
	bool		was_bslash;		/* backslash */
	int			paren_level;
	char	   *query_start;

	if (query_alloced == false)
	{
		if ((query = malloc(MAX_QUERY_BUFFER)) == NULL)
		{

			perror("Memory Allocation Failed");

		}
		else
		{
			query_alloced = true;
		}
	}

	interactive = ((source == stdin) && !pset->notty);
	if (interactive)
	{
		if (pset->prompt)
			free(pset->prompt);
		pset->prompt =
			malloc(strlen(PQdb(pset->db)) + strlen(PROMPT) + 1);
		if (pset->quiet)
			pset->prompt[0] = '\0';
		else
			sprintf(pset->prompt, "%s%s", PQdb(pset->db), PROMPT);
		if (pset->useReadline)
		{
#ifdef USE_HISTORY
			using_history();
#endif
			GetNextLine = gets_readline;
		}
		else
			GetNextLine = gets_noreadline;
	}
	else
		GetNextLine = gets_fromFile;

	query[0] = '\0';
	xcomment = NULL;
	in_quote = false;
	paren_level = 0;
	slashCmdStatus = CMD_UNKNOWN;		/* set default */

	/* main loop to get queries and execute them */
	while (!eof)
	{

		/*
		 * just returned from editing the line? then just copy to the
		 * input buffer
		 */
		if (slashCmdStatus == CMD_NEWEDIT)
		{
			paren_level = 0;
			line = strdup(query);
			query[0] = '\0';

			/*
			 * otherwise, get another line and set interactive prompt if
			 * necessary
			 */
		}
		else
		{
			if (interactive && !pset->quiet)
			{
				if (in_quote)
					pset->prompt[strlen(pset->prompt) - 3] = PROMPT_QUOTE;
				else if (xcomment != NULL)
					pset->prompt[strlen(pset->prompt) - 3] = PROMPT_COMMENT;
				else if (query[0] != '\0' && !querySent)
					pset->prompt[strlen(pset->prompt) - 3] = PROMPT_CONTINUE;
				else
					pset->prompt[strlen(pset->prompt) - 3] = PROMPT_READY;
			}
			line = GetNextLine(pset->prompt, source);
#ifdef USE_HISTORY
			if (interactive && pset->useReadline && line != NULL)
				add_history(line);		/* save non-empty lines in history */
#endif
		}

		/*
		 * query - pointer to current command query_start - placeholder
		 * for next command
		 */

		if (line == NULL || (!interactive && *line == '\0'))
		{						/* No more input.  Time to quit, or \i
								 * done */
			if (!pset->quiet)
				printf("EOF\n");/* Goes on prompt line */
			eof = true;
			continue;
		}

		/* not currently inside an extended comment? */
		if (xcomment == NULL)
		{
			query_start = line;

			/* otherwise, continue the extended comment... */
		}
		else
		{
			query_start = line;
			xcomment = line;
		}

		/* remove whitespaces on the right, incl. \n's */
		line = rightTrim(line);

		/* echo back if input is from file */
		if (!interactive && !pset->singleStep && !pset->quiet)
			fprintf(stderr, "%s\n", line);

		slashCmdStatus = CMD_UNKNOWN;
		/* nothing on line after trimming? then ignore */
		if (line[0] == '\0')
		{
			free(line);
			continue;
		}

		len = strlen(line);

		if (pset->singleLineMode)
		{
			SendQuery(&success, pset, line, false, false, 0);
			successResult &= success;
			querySent = true;
		}
		else
		{
			int			i;

			was_bslash = false;

			for (i = 0; i < len; i++)
			{
				if (line[i] == '\\' && !in_quote)
				{
					char		hold_char = line[i];

					line[i] = '\0';
					if (query_start[0] != '\0')
					{
						if (query[0] != '\0')
						{
							strcat(query, "\n");
							strcat(query, query_start);
						}
						else
						{
							strcpy(query, query_start);
						}
					}
					line[i] = hold_char;
					query_start = line + i;
					break;		/* handle command */

					/* start an extended comment? */
				}

				if (querySent && !isspace(line[i]))
				{
					query[0] = '\0';
					querySent = false;
				}

				if (was_bslash)
					was_bslash = false;
				else if (i > 0 && line[i - 1] == '\\')
					was_bslash = true;

				/* inside a quote? */
				if (in_quote && (line[i] != '\'' || was_bslash))
				{
					 /* do nothing */ ;
				}
				else if (xcomment != NULL)		/* inside an extended
												 * comment? */
				{
					if (line[i] == '*' && line[i + 1] == '/')
					{
						xcomment = NULL;
						i++;
					}
				}
				/* possible backslash command? */
				else if (line[i] == '/' && line[i + 1] == '*')
				{
					xcomment = line + i;
					i++;

				}
				/* single-line comment? truncate line */
				else if ((line[i] == '-' && line[i + 1] == '-') ||
						 (line[i] == '/' && line[i + 1] == '/'))
				{
					/* print comment at top of query */
					if (pset->singleStep)
						fprintf(stdout, "%s\n", line + i);
					line[i] = '\0';		/* remove comment */
					break;
				}
				else if (line[i] == '\'')
				{
					in_quote ^= 1;
				}
				/* semi-colon? then send query now */
				else if (!paren_level && line[i] == ';')
				{
					char		hold_char = line[i + 1];

					line[i + 1] = '\0';
					if (query_start[0] != '\0')
					{
						if (query[0] != '\0')
						{
							strcat(query, "\n");
							strcat(query, query_start);
						}
						else
							strcpy(query, query_start);
					}
					SendQuery(&success, pset, query, false, false, 0);
					successResult &= success;
					line[i + 1] = hold_char;
					query_start = line + i + 1;
					/* sometimes, people do ';\g', don't execute twice */
					if (*query_start && /* keeps us from going off the end */
						*query_start == '\\' &&
						*(query_start + 1) == 'g')
						query_start += 2;
					querySent = true;
				}
				else if (line[i] == '(')
				{
					paren_level++;

				}
				else if (paren_level && line[i] == ')')
				{
					paren_level--;
				}
			}
		}

		/* nothing on line after trimming? then ignore */
		if (line[0] == '\0')
		{
			free(line);
			continue;
		}

		if (!in_quote && query_start[0] == '\\')
		{
			slashCmdStatus = HandleSlashCmds(pset,
											 query_start,
											 query);
			if (slashCmdStatus == CMD_SKIP_LINE)
			{
				if (query[0] == '\0')
					paren_level = 0;
				free(line);
				continue;
			}
			if (slashCmdStatus == CMD_TERMINATE)
			{
				free(line);
				break;
			}
			free(line);
		}
		else if (strlen(query) + strlen(query_start) > MAX_QUERY_BUFFER)
		{
			fprintf(stderr, "query buffer max length of %d exceeded\n",
					MAX_QUERY_BUFFER);
			fprintf(stderr, "query line ignored\n");
			free(line);
		}
		else
		{
			if (query_start[0] != '\0')
			{
				querySent = false;
				if (query[0] != '\0')
				{
					strcat(query, "\n");
					strcat(query, query_start);
				}
				else
					strcpy(query, query_start);
			}
			free(line);
		}

		/* had a backslash-g? force the query to be sent */
		if (slashCmdStatus == CMD_SEND)
		{
			SendQuery(&success, pset, query, false, false, 0);
			successResult &= success;
			xcomment = NULL;
			in_quote = false;
			paren_level = 0;
			querySent = true;
		}
	}							/* while */
	if (query_alloced)
		free(query);

	return successResult;
}	/* MainLoop() */

int
main(int argc, char **argv)
{
	extern char *optarg;
	extern int	optind;

	char	   *dbname = NULL;
	char	   *host = NULL;
	char	   *port = NULL;
	char	   *qfilename = NULL;
	char		errbuf[ERROR_MSG_LENGTH];

	PsqlSettings settings;

	char	   *singleQuery = NULL;

	bool		listDatabases = 0;
	int			successResult = 1;
	bool		singleSlashCmd = 0;
	int			c;

	char	   *home = NULL;	/* Used to store $HOME */

	MemSet(&settings, 0, sizeof settings);
	settings.opt.align = 1;
	settings.opt.header = 1;
	settings.queryFout = stdout;
	settings.opt.fieldSep = strdup(DEFAULT_FIELD_SEP);
	settings.opt.pager = 1;
	if (!isatty(0) || !isatty(1))
	{
		/* Noninteractive defaults */
		settings.notty = 1;
	}
	else
	{
		/* Interactive defaults */
		pqsignal(SIGINT, handle_sigint);	/* control-C => cancel */
#ifdef USE_READLINE
		settings.useReadline = 1;
#endif
	}
#ifdef PSQL_ALWAYS_GET_PASSWORDS
	settings.getPassword = 1;
#else
	settings.getPassword = 0;
#endif

	while ((c = getopt(argc, argv, "Aa:c:d:ef:F:lh:Hnso:p:qStT:ux")) != EOF)
	{
		switch (c)
		{
			case 'A':
				settings.opt.align = 0;
				break;
			case 'a':
				fe_setauthsvc(optarg, errbuf);
				break;
			case 'c':
				singleQuery = strdup(optarg);
				if (singleQuery[0] == '\\')
				{
					singleSlashCmd = 1;
				}
				break;
			case 'd':
				dbname = optarg;
				break;
			case 'e':
				settings.echoQuery = 1;
				break;
			case 'f':
				qfilename = optarg;
				break;
			case 'F':
				settings.opt.fieldSep = strdup(optarg);
				break;
			case 'l':
				listDatabases = 1;
				break;
			case 'h':
				host = optarg;
				break;
			case 'H':
				settings.opt.html3 = 1;
				break;
			case 'n':
				settings.useReadline = 0;
				break;
			case 'o':
				setFout(&settings, optarg);
				break;
			case 'p':
				port = optarg;
				break;
			case 'q':
				settings.quiet = 1;
				break;
			case 's':
				settings.singleStep = 1;
				break;
			case 'S':
				settings.singleLineMode = 1;
				break;
			case 't':
				settings.opt.header = 0;
				break;
			case 'T':
				settings.opt.tableOpt = strdup(optarg);
				break;
			case 'u':
				settings.getPassword = 1;
				break;
			case 'x':
				settings.opt.expanded = 1;
				break;
			default:
				usage(argv[0]);
				break;
		}
	}
	/* if we still have an argument, use it as the database name */
	if (argc - optind == 1)
		dbname = argv[optind];

	if (listDatabases)
		dbname = "template1";

	if (settings.getPassword)
	{
		char		username[9];
		char		password[9];
		char	   *connect_string;

		prompt_for_password(username, password);

		/* now use PQconnectdb so we can pass these options */
		connect_string = make_connect_string(host, port, dbname, username, password);
		settings.db = PQconnectdb(connect_string);
		free(connect_string);
	}
	else
	{
		settings.db = PQsetdb(host, port, NULL, NULL, dbname);
	}

	dbname = PQdb(settings.db);

	if (PQstatus(settings.db) == CONNECTION_BAD)
	{
		fprintf(stderr, "Connection to database '%s' failed.\n", dbname);
		fprintf(stderr, "%s", PQerrorMessage(settings.db));
		PQfinish(settings.db);
		exit(1);
	}

	cancelConn = settings.db;	/* enable SIGINT to send cancel */

	if (listDatabases)
	{
		exit(listAllDbs(&settings));
	}
	if (!settings.quiet && !settings.notty && !singleQuery && !qfilename)
	{
		printf("Welcome to the POSTGRESQL interactive sql monitor:\n");
		printf("  Please read the file COPYRIGHT for copyright terms "
			   "of POSTGRESQL\n\n");
		printf("   type \\? for help on slash commands\n");
		printf("   type \\q to quit\n");
		printf("   type \\g or terminate with semicolon to execute query\n");
		printf(" You are currently connected to the database: %s\n\n", dbname);
	}

	/*
	 * 20.06.97 ACRM See if we've got a /etc/psqlrc or .psqlrc file
	 */
	if (!access("/etc/psqlrc", R_OK))
		HandleSlashCmds(&settings, "\\i /etc/psqlrc", "");
	if ((home = getenv("HOME")) != NULL)
	{
		char	   *psqlrc = NULL,
				   *line = NULL;

		if ((psqlrc = (char *) malloc(strlen(home) + 10)) != NULL)
		{
			sprintf(psqlrc, "%s/.psqlrc", home);
			if (!access(psqlrc, R_OK))
			{
				if ((line = (char *) malloc(strlen(psqlrc) + 5)) != NULL)
				{
					sprintf(line, "\\i %s", psqlrc);
					HandleSlashCmds(&settings, line, "");
					free(line);
				}
			}
			free(psqlrc);
		}
	}
	/* End of check for psqlrc files */

	if (qfilename || singleSlashCmd)
	{

		/*
		 * read in a file full of queries instead of reading in queries
		 * interactively
		 */
		char	   *line;

		if (singleSlashCmd)
		{
			/* Not really a query, but "Do what I mean, not what I say." */
			line = singleQuery;
		}
		else
		{
			line = malloc(strlen(qfilename) + 5);
			sprintf(line, "\\i %s", qfilename);
		}
		HandleSlashCmds(&settings, line, "");
		free(line);
	}
	else
	{
		if (singleQuery)
		{
			bool		success;/* The query succeeded at the backend */

			SendQuery(&success, &settings, singleQuery, false, false, 0);
			successResult = success;
		}
		else
			successResult = MainLoop(&settings, NULL, stdin);
	}

	PQfinish(settings.db);
	free(settings.opt.fieldSep);
	if (settings.prompt)
		free(settings.prompt);

	return !successResult;
}

#define COPYBUFSIZ	8192

static void
handleCopyOut(PGresult *res, FILE *copystream)
{
	bool		copydone;
	char		copybuf[COPYBUFSIZ];
	int			ret;

	copydone = false;			/* Can't be done; haven't started. */

	while (!copydone)
	{
		ret = PQgetline(res->conn, copybuf, COPYBUFSIZ);

		if (copybuf[0] == '\\' &&
			copybuf[1] == '.' &&
			copybuf[2] == '\0')
		{
			copydone = true;	/* don't print this... */
		}
		else
		{
			fputs(copybuf, copystream);
			switch (ret)
			{
				case EOF:
					copydone = true;
					/* FALLTHROUGH */
				case 0:
					fputc('\n', copystream);
					break;
				case 1:
					break;
			}
		}
	}
	fflush(copystream);
	PQendcopy(res->conn);
}



static void
handleCopyIn(PGresult *res, const bool mustprompt, FILE *copystream)
{
	bool		copydone = false;
	bool		firstload;
	bool		linedone;
	char		copybuf[COPYBUFSIZ];
	char	   *s;
	int			buflen;
	int			c;

	if (mustprompt)
	{
		fputs("Enter info followed by a newline\n", stdout);
		fputs("End with a backslash and a "
			  "period on a line by itself.\n", stdout);
	}
	while (!copydone)
	{							/* for each input line ... */
		if (mustprompt)
		{
			fputs(">> ", stdout);
			fflush(stdout);
		}
		firstload = true;
		linedone = false;
		while (!linedone)
		{						/* for each buffer ... */
			s = copybuf;
			buflen = COPYBUFSIZ;
			for (; buflen > 1 &&
				 !(linedone = (c = getc(copystream)) == '\n' || c == EOF);
				 --buflen)
			{
				*s++ = c;
			}
			if (c == EOF)
			{
				PQputline(res->conn, "\\.");
				copydone = true;
				break;
			}
			*s = '\0';
			PQputline(res->conn, copybuf);
			if (firstload)
			{
				if (!strcmp(copybuf, "\\."))
				{
					copydone = true;
				}
				firstload = false;
			}
		}
		PQputline(res->conn, "\n");
	}
	PQendcopy(res->conn);
}



/*
 * try to open fname and return a FILE *, if it fails, use stdout, instead
 */

static FILE *
setFout(PsqlSettings *pset, char *fname)
{
	if (pset->queryFout && pset->queryFout != stdout)
	{
		if (pset->pipe)
			pclose(pset->queryFout);
		else
			fclose(pset->queryFout);
	}
	if (!fname)
	{
		pset->queryFout = stdout;
		pqsignal(SIGPIPE, SIG_DFL);
	}
	else
	{
		if (*fname == '|')
		{
			pqsignal(SIGPIPE, SIG_IGN);
			pset->queryFout = popen(fname + 1, "w");
			pset->pipe = 1;
		}
		else
		{
			pset->queryFout = fopen(fname, "w");
			pqsignal(SIGPIPE, SIG_DFL);
			pset->pipe = 0;
		}
		if (!pset->queryFout)
		{
			perror(fname);
			pset->queryFout = stdout;
		}
	}
	return pset->queryFout;
}

static void
prompt_for_password(char *username, char *password)
{
	int			length;

#ifdef HAVE_TERMIOS_H
	struct termios t_orig,
				t;

#endif

	printf("Username: ");
	fgets(username, 9, stdin);
	length = strlen(username);
	/* skip rest of the line */
	if (length > 0 && username[length - 1] != '\n')
	{
		static char buf[512];

		do
		{
			fgets(buf, 512, stdin);
		} while (buf[strlen(buf) - 1] != '\n');
	}
	if (length > 0 && username[length - 1] == '\n')
		username[length - 1] = '\0';

	printf("Password: ");
#ifdef HAVE_TERMIOS_H
	tcgetattr(0, &t);
	t_orig = t;
	t.c_lflag &= ~ECHO;
	tcsetattr(0, TCSADRAIN, &t);
#endif
	fgets(password, 9, stdin);
#ifdef HAVE_TERMIOS_H
	tcsetattr(0, TCSADRAIN, &t_orig);
#endif

	length = strlen(password);
	/* skip rest of the line */
	if (length > 0 && password[length - 1] != '\n')
	{
		static char buf[512];

		do
		{
			fgets(buf, 512, stdin);
		} while (buf[strlen(buf) - 1] != '\n');
	}
	if (length > 0 && password[length - 1] == '\n')
		password[length - 1] = '\0';

	printf("\n\n");
}

static char *
make_connect_string(char *host, char *port, char *dbname,
					char *username, char *password)
{
	int			connect_string_len = 0;
	char	   *connect_string;

	if (host)
		connect_string_len += 6 + strlen(host); /* 6 == "host=" + " " */
	if (username)
		connect_string_len += 6 + strlen(username);		/* 6 == "user=" + " " */
	if (password)
		connect_string_len += 10 + strlen(password);	/* 10 == "password=" + "
														 * " */
	if (port)
		connect_string_len += 6 + strlen(port); /* 6 == "port=" + " " */
	if (dbname)
		connect_string_len += 8 + strlen(dbname);		/* 8 == "dbname=" + " " */
	connect_string_len += 18;	/* "authtype=password" + null */

	connect_string = (char *) malloc(connect_string_len);
	if (!connect_string)
	{
		return 0;
	}
	connect_string[0] = '\0';
	if (host)
	{
		strcat(connect_string, "host=");
		strcat(connect_string, host);
		strcat(connect_string, " ");
	}
	if (username)
	{
		strcat(connect_string, "user=");
		strcat(connect_string, username);
		strcat(connect_string, " ");
	}
	if (password)
	{
		strcat(connect_string, "password=");
		strcat(connect_string, password);
		strcat(connect_string, " ");
	}
	if (port)
	{
		strcat(connect_string, "port=");
		strcat(connect_string, port);
		strcat(connect_string, " ");
	}
	if (dbname)
	{
		strcat(connect_string, "dbname=");
		strcat(connect_string, dbname);
		strcat(connect_string, " ");
	}
	strcat(connect_string, "authtype=password");

	return connect_string;
}
