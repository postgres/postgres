/*-------------------------------------------------------------------------
 *
 * psql.c--
 *    an interactive front-end to postgreSQL
 *
 * Copyright (c) 1996, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/bin/psql/Attic/psql.c,v 1.47 1997/01/10 20:52:32 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/param.h>	/* for MAXPATHLEN */
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include "postgres.h"
#include "libpq-fe.h"
#include "pqsignal.h"
#include "stringutils.h"
#include "psqlHelp.h"
#ifdef NEED_STRDUP
#include "strdup.h"
#endif

#ifdef NOREADLINE
#include "rlstubs.h"
#else
/* from the GNU readline library */
#ifdef OLD_READLINE
#include "readline.h"
#include "history.h"
#else
#include <readline/readline.h>
#include <readline/history.h>
#endif
#endif

#define PROMPT "=> "

#define MAX_QUERY_BUFFER 20000

#define COPYBUFSIZ  8192

#define DEFAULT_FIELD_SEP "|"
#define DEFAULT_EDITOR  "vi"
#define DEFAULT_SHELL  "/bin/sh"

typedef struct _psqlSettings {
    PGconn         *db;		/* connection to backend */
    FILE           *queryFout;	/* where to send the query results */
    PQprintOpt      opt;	/* options to be passed to PQprint */
    char           *prompt;	/* prompt to display */
    char           *gfname;	/* one-shot file output argument for \g */
    bool            notty;	/* input or output is not a tty */
    bool            pipe;	/* queryFout is from a popen() */
    bool            echoQuery;	/* echo the query before sending it */
    bool            quiet;	/* run quietly, no messages, no promt */
    bool            singleStep;	/* prompt before for each query */
    bool            singleLineMode;	/* query terminated by newline */
    bool            useReadline;/* use libreadline routines */
}               PsqlSettings;

/* declarations for functions in this file */
static void     usage(char *progname);
static void     slashUsage();
static void     handleCopyOut(PGresult * res, bool quiet, FILE * copystream);
static void
handleCopyIn(PGresult * res, const bool mustprompt,
	     FILE * copystream);
static int      tableList(PsqlSettings * ps, bool deep_tablelist);
static int      tableDesc(PsqlSettings * ps, char *table);

char           *gets_noreadline(char *prompt, FILE * source);
char           *gets_readline(char *prompt, FILE * source);
char           *gets_fromFile(char *prompt, FILE * source);
int             listAllDbs(PsqlSettings * settings);
void
SendQuery(bool * success_p, PsqlSettings * settings, const char *query,
	  const bool copy_in, const bool copy_out, FILE * copystream);
int
HandleSlashCmds(PsqlSettings * settings,
		char *line,
		char *query);
int             MainLoop(PsqlSettings * settings, FILE * source);
/* probably should move this into libpq */
void
PQprint(FILE * fp,
	PGresult * res,
	PQprintOpt * po
);

FILE           *setFout(PsqlSettings * ps, char *fname);

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
    fprintf(stderr, "\t -F sep                  set the field separator (default is " ")\n");
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
    fprintf(stderr, "\t -x                      turn on expanded output (field names on left)\n");
    exit(1);
}

/*
 * slashUsage print out usage for the backslash commands
 */

static char    *
on(bool f)
{
    return f ? "on" : "off";
}

static void
slashUsage(PsqlSettings * ps)
{
    fprintf(stderr, " \\?           -- help\n");
    fprintf(stderr, " \\a           -- toggle field-alignment (currenty %s)\n", on(ps->opt.align));
    fprintf(stderr, " \\C [<captn>] -- set html3 caption (currently '%s')\n", ps->opt.caption ? ps->opt.caption : "");
    fprintf(stderr, " \\connect <dbname>  -- connect to new database (currently '%s')\n", PQdb(ps->db));
    fprintf(stderr, " \\copy {<table> to <file> | <file> from <table>}\n");
    fprintf(stderr, " \\d [<table>] -- list tables in database or columns in <table>, * for all\n");
    fprintf(stderr, " \\e [<fname>] -- edit the current query buffer or <fname>, \\E execute too\n");
    fprintf(stderr, " \\f [<sep>]   -- change field separater (currently '%s')\n", ps->opt.fieldSep);
    fprintf(stderr, " \\g [<fname>] [|<cmd>] -- send query to backend [and results in <fname> or pipe]\n");
    fprintf(stderr, " \\h [<cmd>]   -- help on syntax of sql commands, * for all commands\n");
    fprintf(stderr, " \\H           -- toggle html3 output (currently %s)\n", on(ps->opt.html3));
    fprintf(stderr, " \\i <fname>   -- read and execute queries from filename\n");
    fprintf(stderr, " \\l           -- list all databases\n");
    fprintf(stderr, " \\m           -- toggle monitor-like table display (currently %s)\n", on(ps->opt.standard));
    fprintf(stderr, " \\o [<fname>] [|<cmd>] -- send all query results to stdout, <fname>, or pipe\n");
    fprintf(stderr, " \\p           -- print the current query buffer\n");
    fprintf(stderr, " \\q           -- quit\n");
    fprintf(stderr, " \\r           -- reset(clear) the query buffer\n");
    fprintf(stderr, " \\s [<fname>] -- print history or save it in <fname>\n");
    fprintf(stderr, " \\t           -- toggle table headings and row count (currently %s)\n", on(ps->opt.header));
    fprintf(stderr, " \\T [<html>]  -- set html3.0 <table ...> options (currently '%s')\n", ps->opt.tableOpt ? ps->opt.tableOpt : "");
    fprintf(stderr, " \\x           -- toggle expanded output (currently %s)\n", on(ps->opt.expanded));
    fprintf(stderr, " \\! [<cmd>]   -- shell escape or command\n");
}

static PGresult *
PSQLexec(PsqlSettings * ps, char *query)
{
    PGresult       *res = PQexec(ps->db, query);
    if (!res)
	fputs(PQerrorMessage(ps->db), stderr);
    else {
	if (PQresultStatus(res) == PGRES_COMMAND_OK ||
	    PQresultStatus(res) == PGRES_TUPLES_OK)
	    return res;
	if (!ps->quiet)
	    fputs(PQerrorMessage(ps->db), stderr);
	PQclear(res);
    }
    return NULL;
}
/*
 * listAllDbs
 * 
 * list all the databases in the system returns 0 if all went well
 * 
 * 
 */

int
listAllDbs(PsqlSettings * ps)
{
    PGresult       *results;
    char           *query = "select * from pg_database;";

    if (!(results = PSQLexec(ps, query)))
	return 1;
    else {
	PQprint(ps->queryFout,
		results,
		&ps->opt);
	PQclear(results);
	return 0;
    }
}

/*
 * List The Database Tables returns 0 if all went well
 * 
 */
int
tableList(PsqlSettings * ps, bool deep_tablelist)
{
    char            listbuf[256];
    int             nColumns;
    int             i;
    char           *rk;
    char           *rr;

    PGresult       *res;

    listbuf[0] = '\0';
    strcat(listbuf, "SELECT usename, relname, relkind, relhasrules");
    strcat(listbuf, "  FROM pg_class, pg_user ");
    strcat(listbuf, "WHERE ( relkind = 'r' OR relkind = 'i') ");
    strcat(listbuf, "  and relname !~ '^pg_'");
    strcat(listbuf, "  and relname !~ '^Inv[0-9]+'");
    /*
     * the usesysid = relowner won't work on stock 1.0 dbs, need to add in
     * the int4oideq function
     */
    strcat(listbuf, "  and usesysid = relowner");
    strcat(listbuf, "  ORDER BY relname ");
    if (!(res = PSQLexec(ps, listbuf)))
	return -1;

    /* first, print out the attribute names */
    nColumns = PQntuples(res);
    if (nColumns > 0) {
	if (deep_tablelist) {
	    /* describe everything here */
	    char          **table;
	    table = (char **) malloc(nColumns * sizeof(char *));
	    if (table == NULL)
		perror("malloc");

	    /* load table table */
	    for (i = 0; i < nColumns; i++) {
		table[i] = (char *) malloc(PQgetlength(res, i, 1) * sizeof(char) + 1);
		if (table[i] == NULL)
		    perror("malloc");
		strcpy(table[i], PQgetvalue(res, i, 1));
	    }

	    PQclear(res);
	    for (i = 0; i < nColumns; i++) {
		tableDesc(ps, table[i]);
	    }
	    free(table);
	} else {
	    /* Display the information */

	    printf("\nDatabase    = %s\n", PQdb(ps->db));
	    printf(" +------------------+----------------------------------+----------+\n");
	    printf(" |  Owner           |             Relation             |   Type   |\n");
	    printf(" +------------------+----------------------------------+----------+\n");

	    /* next, print out the instances */
	    for (i = 0; i < PQntuples(res); i++) {
		printf(" | %-16.16s", PQgetvalue(res, i, 0));
		printf(" | %-32.32s | ", PQgetvalue(res, i, 1));
		rk = PQgetvalue(res, i, 2);
		rr = PQgetvalue(res, i, 3);
		if (strcmp(rk, "r") == 0)
		    printf("%-8.8s |", (rr[0] == 't') ? "view?" : "table");
		else
		    printf("%-8.8s |", "index");
		printf("\n");
	    }
	    printf(" +------------------+----------------------------------+----------+\n");
	    PQclear(res);
	}
	return (0);

    } else {
	fprintf(stderr, "Couldn't find any tables!\n");
	return (-1);
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
tableDesc(PsqlSettings * ps, char *table)
{
    char            descbuf[256];
    int             nColumns;
    char           *rtype;
    int             i;
    int             rsize;

    PGresult       *res;

    /* Build the query */

    descbuf[0] = '\0';
    strcat(descbuf, "SELECT a.attnum, a.attname, t.typname, a.attlen");
    strcat(descbuf, "  FROM pg_class c, pg_attribute a, pg_type t ");
    strcat(descbuf, "    WHERE c.relname = '");
    strcat(descbuf, table);
    strcat(descbuf, "'");
    strcat(descbuf, "    and a.attnum > 0 ");
    strcat(descbuf, "    and a.attrelid = c.oid ");
    strcat(descbuf, "    and a.atttypid = t.oid ");
    strcat(descbuf, "  ORDER BY attnum ");
    if (!(res = PSQLexec(ps, descbuf)))
	return -1;
    /* first, print out the attribute names */
    nColumns = PQntuples(res);
    if (nColumns > 0) {
	/*
	 * * Display the information
	 */

	printf("\nTable    = %s\n", table);
	printf("+----------------------------------+----------------------------------+-------+\n");
	printf("|              Field               |              Type                | Length|\n");
	printf("+----------------------------------+----------------------------------+-------+\n");

	/* next, print out the instances */
	for (i = 0; i < PQntuples(res); i++) {
	    printf("| %-32.32s | ", PQgetvalue(res, i, 1));
	    rtype = PQgetvalue(res, i, 2);
	    rsize = atoi(PQgetvalue(res, i, 3));
	    if (strcmp(rtype, "text") == 0) {
		printf("%-32.32s |", rtype);
		printf("%6s |", "var");
	    } else if (strcmp(rtype, "bpchar") == 0) {
		printf("%-32.32s |", "(bp)char");
		printf("%6i |", rsize > 0 ? rsize - 4 : 0);
	    } else if (strcmp(rtype, "varchar") == 0) {
		printf("%-32.32s |", rtype);
		printf("%6i |", rsize > 0 ? rsize - 4 : 0);
	    } else {
		/* array types start with an underscore */
		if (rtype[0] != '_')
		    printf("%-32.32s |", rtype);
		else {
		    char           *newname;
		    newname = malloc(strlen(rtype) + 2);
		    strcpy(newname, rtype + 1);
		    strcat(newname, "[]");
		    printf("%-32.32s |", newname);
		    free(newname);
		}
		if (rsize > 0)
		    printf("%6i |", rsize);
		else
		    printf("%6s |", "var");
	    }
	    printf("\n");
	}
	printf("+----------------------------------+----------------------------------+-------+\n");

	PQclear(res);
	return (0);

    } else {
	fprintf(stderr, "Couldn't find table %s!\n", table);
	return (-1);
    }
}

typedef char   *(*READ_ROUTINE) (char *prompt, FILE * source);

/*
 * gets_noreadline  prompt source gets a line of input without calling
 * readline, the source is ignored
 */
char           *
gets_noreadline(char *prompt, FILE * source)
{
    fputs(prompt, stdout);
    fflush(stdout);
    return (gets_fromFile(prompt, stdin));
}

/*
 * gets_readline  prompt source the routine to get input from GNU readline(),
 * the source is ignored the prompt argument is used as the prompting string
 */
char           *
gets_readline(char *prompt, FILE * source)
{
    char *s = readline(prompt);
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
char           *
gets_fromFile(char *prompt, FILE * source)
{
    char           *line;
    int             len;

    line = malloc(MAX_QUERY_BUFFER + 1);

    /* read up to MAX_QUERY_BUFFER characters */
    if (fgets(line, MAX_QUERY_BUFFER, source) == NULL)
	return NULL;

    line[MAX_QUERY_BUFFER - 1] = '\0';
    len = strlen(line);
    if (len == MAX_QUERY_BUFFER) {
	fprintf(stderr, "line read exceeds maximum length.  Truncating at %d\n",
		MAX_QUERY_BUFFER);
    }
    return line;
}

/*
 * SendQuery: send the query string to the backend return *success_p = 1 if
 * the query executed successfully returns *success_p = 0 otherwise
 */
void
SendQuery(bool * success_p, PsqlSettings * settings, const char *query,
	  const bool copy_in, const bool copy_out, FILE * copystream)
{

    PGresult       *results;
    PGnotify       *notify;

    if (settings->singleStep)
	fprintf(stdout, "\n**************************************"
		"*****************************************\n");

    if (settings->echoQuery || settings->singleStep) {
	fprintf(stderr, "QUERY: %s\n", query);
	fflush(stderr);
    }
    if (settings->singleStep) {
	fprintf(stdout, "\n**************************************"
		"*****************************************\n");
	fflush(stdout);
	printf("\npress return to continue ..\n");
	gets_fromFile("", stdin);
    }
    results = PQexec(settings->db, query);
    if (results == NULL) {
	fprintf(stderr, "%s", PQerrorMessage(settings->db));
	*success_p = false;
    } else {
	switch (PQresultStatus(results)) {
	case PGRES_TUPLES_OK:
	    if (settings->gfname) {
		PsqlSettings    ps = *settings;
		FILE           *fp;
		ps.queryFout = stdout;
		fp = setFout(&ps, settings->gfname);
		if (!fp || fp == stdout) {
		    *success_p = false;
		    break;
		} else
		    *success_p = true;
		PQprint(fp,
			results,
			&(settings->opt));
		if (ps.pipe)
		    pclose(fp);
		else
		    fclose(fp);
		free(settings->gfname);
		settings->gfname = NULL;
		break;
	    } else {
		*success_p = true;
		PQprint(settings->queryFout,
			results,
			&(settings->opt));
		fflush(settings->queryFout);
	    }
	    PQclear(results);
	    break;
	case PGRES_EMPTY_QUERY:
	    *success_p = true;
	    break;
	case PGRES_COMMAND_OK:
	    *success_p = true;
	    if (!settings->quiet)
		fprintf(stdout, "%s\n", PQcmdStatus(results));
	    break;
	case PGRES_COPY_OUT:
	    *success_p = true;
	    if (copy_out) {
		handleCopyOut(results, settings->quiet, copystream);
	    } else {
		if (!settings->quiet)
		    fprintf(stdout, "Copy command returns...\n");

		handleCopyOut(results, settings->quiet, stdout);
	    }
	    break;
	case PGRES_COPY_IN:
	    *success_p = true;
	    if (copy_in)
		handleCopyIn(results, false, copystream);
	    else
		handleCopyIn(results, !settings->quiet, stdin);
	    break;
	case PGRES_NONFATAL_ERROR:
	case PGRES_FATAL_ERROR:
	case PGRES_BAD_RESPONSE:
	    *success_p = false;
	    fprintf(stderr, "%s", PQerrorMessage(settings->db));
	    break;
	}

	if (PQstatus(settings->db) == CONNECTION_BAD) {
	    fprintf(stderr,
		    "We have lost the connection to the backend, so "
		    "further processing is impossible.  "
		    "Terminating.\n");
	    exit(2);		/* we are out'ta here */
	}
	/* check for asynchronous returns */
	notify = PQnotifies(settings->db);
	if (notify) {
	    fprintf(stderr,
		    "ASYNC NOTIFY of '%s' from backend pid '%d' received\n",
		    notify->relname, notify->be_pid);
	    free(notify);
	}
    }
}



static void
editFile(char *fname)
{
    char           *editorName;
    char           *sys;
    editorName = getenv("EDITOR");
    if (!editorName)
	editorName = DEFAULT_EDITOR;
    sys = malloc(strlen(editorName) + strlen(fname) + 32 + 1);
    if (!sys) {
	perror("malloc");
	exit(1);
    }
    sprintf(sys, "exec '%s' '%s'", editorName, fname);
    system(sys);
    free(sys);
}

static          bool
toggle(PsqlSettings * settings, bool * sw, char *msg)
{
    *sw = !*sw;
    if (!settings->quiet)
	fprintf(stderr, "turned %s %s\n", on(*sw), msg);
    return *sw;
}



static void
unescape(char *dest, const char *source)
{
    /*-----------------------------------------------------------------------------
      Return as the string <dest> the value of string <source> with escape
      sequences turned into the bytes they represent.
    -----------------------------------------------------------------------------*/
    char           *p;
    bool            esc;	/* Last character we saw was the escape
				 * character (/) */

    esc = false;		/* Haven't seen escape character yet */
    for (p = (char *) source; *p; p++) {
	char            c;	/* Our output character */

	if (esc) {
	    switch (*p) {
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
	} else if (*p == '\\') {
	    esc = true;
	    c = ' ';		/* meaningless, but compiler doesn't know
				 * that */
	} else {
	    c = *p;
	    esc = false;
	}
	if (!esc)
	    *dest++ = c;
    }
    *dest = '\0';		/* Terminating null character */
}



static void
parse_slash_copy(const char *args, char *table, const int table_len,
		 char *file, const int file_len,
		 bool * from_p, bool * error_p)
{

    char            work_args[200];
    /*
     * A copy of the \copy command arguments, except that we modify it as we
     * parse to suit our parsing needs.
     */
    char           *table_tok, *fromto_tok;

    strncpy(work_args, args, sizeof(work_args));
    work_args[sizeof(work_args) - 1] = '\0';

    *error_p = false;		/* initial assumption */

    table_tok = strtok(work_args, " ");
    if (table_tok == NULL) {
	fprintf(stderr, "\\copy needs arguments.\n");
	*error_p = true;
    } else {
	strncpy(table, table_tok, table_len);
	file[table_len - 1] = '\0';

	fromto_tok = strtok(NULL, "  ");
	if (fromto_tok == NULL) {
	    fprintf(stderr, "'FROM' or 'TO' must follow table name.\n");
	    *error_p = true;
	} else {
	    if (strcasecmp(fromto_tok, "from") == 0)
		*from_p = true;
	    else if (strcasecmp(fromto_tok, "to") == 0)
		*from_p = false;
	    else {
		fprintf(stderr,
			"Unrecognized token found where "
			"'FROM' or 'TO' expected: '%s'.\n",
			fromto_tok);
		*error_p = true;
	    }
	    if (!*error_p) {
		char           *file_tok;

		file_tok = strtok(NULL, " ");
		if (file_tok == NULL) {
		    fprintf(stderr, "A file pathname must follow '%s'.\n",
			    fromto_tok);
		    *error_p = true;
		} else {
		    strncpy(file, file_tok, file_len);
		    file[file_len - 1] = '\0';
		    if (strtok(NULL, " ") != NULL) {
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
do_copy(const char *args, PsqlSettings * settings)
{
    /*---------------------------------------------------------------------------
      Execute a \copy command (frontend copy).  We have to open a file, then
      submit a COPY query to the backend and either feed it data from the
      file or route its response into the file.

      We do a text copy with default (tab) column delimiters.  Some day, we
      should do all the things a backend copy can do.

    ----------------------------------------------------------------------------*/
    char            query[200];
    /* The COPY command we send to the back end */
    bool            from;
    /* The direction of the copy is from a file to a table. */
    char            file[MAXPATHLEN + 1];
    /* The pathname of the file from/to which we copy */
    char            table[NAMEDATALEN + 1];
    /* The name of the table from/to which we copy */
    bool            syntax_error;
    /* The \c command has invalid syntax */
    FILE           *copystream;

    parse_slash_copy(args, table, sizeof(table), file, sizeof(file),
		     &from, &syntax_error);

    if (!syntax_error) {
	strcpy(query, "COPY ");
	strcat(query, table);

	if (from)
	    strcat(query, " FROM stdin");
	else
	    strcat(query, " TO stdout");

	if (from) {
	    copystream = fopen(file, "r");
	} else {
	    copystream = fopen(file, "w");
	}
	if (copystream == NULL)
	    fprintf(stderr,
		    "Unable to open file %s which to copy, errno = %s (%d).",
		    from ? "from" : "to", strerror(errno), errno);
	else {
	    bool            success;	/* The query succeeded at the backend */

	    SendQuery(&success, settings, query, from, !from, copystream);
	    fclose(copystream);
	    if (!settings->quiet) {
		if (success)
		    fprintf(stdout, "Successfully copied.\n");
		else
		    fprintf(stdout, "Copy failed.\n");
	    }
	}
    }
}


static void
do_connect(const char *new_dbname, PsqlSettings * settings)
{

    char           *dbname = PQdb(settings->db);
    if (!new_dbname)
	fprintf(stderr, "\\connect must be followed by a database name\n");
    else {
	PGconn         *olddb = settings->db;

	printf("closing connection to database: %s\n", dbname);
	settings->db = PQsetdb(PQhost(olddb), PQport(olddb),
			       NULL, NULL, new_dbname);
	printf("connecting to new database: %s\n", new_dbname);
	if (PQstatus(settings->db) == CONNECTION_BAD) {
	    fprintf(stderr, "%s\n", PQerrorMessage(settings->db));
	    printf("reconnecting to %s\n", dbname);
	    settings->db = PQsetdb(PQhost(olddb), PQport(olddb),
				   NULL, NULL, dbname);
	    if (PQstatus(settings->db) == CONNECTION_BAD) {
		fprintf(stderr,
			"could not reconnect to %s. exiting\n", dbname);
		exit(2);
	    }
	} else {
	    PQfinish(olddb);
	    free(settings->prompt);
	    settings->prompt = malloc(strlen(PQdb(settings->db)) + 10);
	    sprintf(settings->prompt, "%s%s", PQdb(settings->db), PROMPT);
	}
    }
}


static void
do_edit(const char *filename_arg, char *query, int *status_p)
{

    int             fd;
    char            tmp[64];
    char           *fname;
    int             cc;
    const int       ql = strlen(query);
    bool            error;

    if (filename_arg) {
	fname = (char *) filename_arg;
	error = false;
    } else {
	sprintf(tmp, "/tmp/psql.%ld.%ld", (long) geteuid(), (long) getpid());
	fname = tmp;
	unlink(tmp);
	if (ql > 0) {
	    if ((fd = open(tmp, O_EXCL | O_CREAT | O_WRONLY, 0600)) == -1) {
		perror(tmp);
		error = true;
	    } else {
		if (query[ql - 1] != '\n')
		    strcat(query, "\n");
		if (write(fd, query, ql) != ql) {
		    perror(tmp);
		    close(fd);
		    unlink(tmp);
		    error = true;
		} else
		    error = false;
		close(fd);
	    }
	} else
	    error = false;
    }

    if (error)
	*status_p = 1;
    else {
	editFile(fname);
	if ((fd = open(fname, O_RDONLY)) == -1) {
	    perror(fname);
	    if (!filename_arg)
		unlink(fname);
	    *status_p = 1;
	} else {
	    if ((cc = read(fd, query, MAX_QUERY_BUFFER)) == -1) {
		perror(fname);
		close(fd);
		if (!filename_arg)
		    unlink(fname);
		*status_p = 1;
	    } else {
		query[cc] = '\0';
		close(fd);
		if (!filename_arg)
		    unlink(fname);
		rightTrim(query);
		*status_p = 3;
	    }
	}
    }
}



static void
do_help(const char *topic)
{

    if (!topic) {
	char            left_center_right;	/* Which column we're
						 * displaying */
	int             i;	/* Index into QL_HELP[] */

	printf("type \\h <cmd> where <cmd> is one of the following:\n");

	left_center_right = 'L';/* Start with left column */
	i = 0;
	while (QL_HELP[i].cmd != NULL) {
	    switch (left_center_right) {
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
	    };
	    i++;
	}
	if (left_center_right != 'L')
	    puts("\n");
	printf("type \\h * for a complete description of all commands\n");
    } else {
	int             i;	/* Index into QL_HELP[] */
	bool            help_found;	/* We found the help he asked for */

	help_found = false;	/* Haven't found it yet */
	for (i = 0; QL_HELP[i].cmd; i++) {
	    if (strcmp(QL_HELP[i].cmd, topic) == 0 ||
		strcmp(topic, "*") == 0) {
		help_found = true;
		printf("Command: %s\n", QL_HELP[i].cmd);
		printf("Description: %s\n", QL_HELP[i].help);
		printf("Syntax:\n");
		printf("%s\n", QL_HELP[i].syntax);
		printf("\n");
	    }
	}
	if (!help_found)
	    printf("command not found, "
		   "try \\h with no arguments to see available help\n");
    }
}



static void
do_shell(const char *command)
{

    if (!command) {
	char           *sys;
	char           *shellName;

	shellName = getenv("SHELL");
	if (shellName == NULL)
	    shellName = DEFAULT_SHELL;
	sys = malloc(strlen(shellName) + 16);
	if (!sys) {
	    perror("malloc");
	    exit(1);
	}
	sprintf(sys, "exec %s", shellName);
	system(sys);
	free(sys);
    } else
	system(command);
}



/*
 * HandleSlashCmds:
 * 
 * Handles all the different commands that start with \ db_ptr is a pointer to
 * the TgDb* structure line is the current input line prompt_ptr is a pointer
 * to the prompt string, a pointer is used because the prompt can be used
 * with a connection to a new database returns a status: 0 - send currently
 * constructed query to backend (i.e. we got a \g) 1 - skip processing of
 * this line, continue building up query 2 - terminate processing of this
 * query entirely, 3 - new query supplied by edit
 */
int
HandleSlashCmds(PsqlSettings * settings,
		char *line,
		char *query)
{
    int             status = 1;
    char           *optarg;
    /*
     * Pointer inside the <cmd> string to the argument of the slash command,
     * assuming it is a one-character slash command.  If it's not a
     * one-character command, this is meaningless.
     */
    char           *optarg2;
    /*
     * Pointer inside the <cmd> string to the argument of the slash command
     * assuming it's not a one-character command.  If it's a one-character
     * command, this is meaningless.
     */
    char           *cmd;
    /*
     * String: value of the slash command, less the slash and with escape
     * sequences decoded.
     */
    int             blank_loc;
    /* Offset within <cmd> of first blank */

    cmd = malloc(strlen(line));	/* unescaping better not make string grow. */

    unescape(cmd, line + 1);	/* sets cmd string */

    /*
     * Originally, there were just single character commands.  Now, we define
     * some longer, friendly commands, but we have to keep the old single
     * character commands too.  \c used to be what \connect is now.
     * Complicating matters is the fact that with the single-character
     * commands, you can start the argument right after the single character,
     * so "\copy" would mean "connect to database named 'opy'".
     */

    if (strlen(cmd) > 1)
	optarg = cmd + 1 + strspn(cmd + 1, " \t");
    else
	optarg = NULL;

    blank_loc = strcspn(cmd, " \t");
    if (blank_loc == 0)
	optarg2 = NULL;
    else
	optarg2 = cmd + blank_loc + strspn(cmd + blank_loc, " \t");


    switch (cmd[0]) {
    case 'a':			/* toggles to align fields on output */
	toggle(settings, &settings->opt.align, "field alignment");
	break;
    case 'C':			/* define new caption */
	if (settings->opt.caption)
	    free(settings->opt.caption);
	if (!optarg)
	{
	    if (settings->opt.caption)
	    	free(settings->opt.caption);
	    settings->opt.caption = NULL;
	}
	else if (!(settings->opt.caption = strdup(optarg))) {
	    perror("malloc");
	    exit(1);
	}
	break;
    case 'c':{
	    if (strncmp(cmd, "copy ", strlen("copy ")) == 0)
		do_copy(optarg2, settings);
	    else if (strncmp(cmd, "connect ", strlen("connect ")) == 0)
		do_connect(optarg2, settings);
	    else
		do_connect(optarg, settings);
	}
	break;
    case 'd':			/* \d describe tables or columns in a table */
	if (!optarg) {
	    tableList(settings, 0);
	    break;
	}
	if (strcmp(optarg, "*") == 0) {
	    tableList(settings, 0);
	    tableList(settings, 1);
	} else {
	    tableDesc(settings, optarg);
	}
	break;
    case 'e':			/* edit */
	{
	    do_edit(optarg, query, &status);
	    break;
	}
    case 'E':
	{
	    FILE           *fd;
	    static char    *lastfile;
	    struct stat     st, st2;
	    if (optarg) {
		if (lastfile)
		    free(lastfile);
		lastfile = malloc(strlen(optarg + 1));
		if (!lastfile) {
		    perror("malloc");
		    exit(1);
		}
		strcpy(lastfile, optarg);
	    } else if (!lastfile) {
		fprintf(stderr, "\\r must be followed by a file name initially\n");
		break;
	    }
	    stat(lastfile, &st);
	    editFile(lastfile);
	    if ((stat(lastfile, &st2) == -1) || ((fd = fopen(lastfile, "r")) == NULL)) {
		perror(lastfile);
		break;
	    }
	    if (st2.st_mtime == st.st_mtime) {
		if (!settings->quiet)
		    fprintf(stderr, "warning: %s not modified. query not executed\n", lastfile);
		fclose(fd);
		break;
	    }
	    MainLoop(settings, fd);
	    fclose(fd);
	    break;
	}
    case 'f':
	{
	    char           *fs = DEFAULT_FIELD_SEP;
	    if (optarg)
		fs = optarg;
	    if (settings->opt.fieldSep)
	    	free(settings->opt.fieldSep);
	    if (!(settings->opt.fieldSep = strdup(fs))) {
		perror("malloc");
		exit(1);
	    }
	    if (!settings->quiet)
		fprintf(stderr, "field separater changed to '%s'\n", settings->opt.fieldSep);
	    break;
	}
    case 'g':			/* \g means send query */
	if (!optarg)
	    settings->gfname = NULL;
	else if (!(settings->gfname = strdup(optarg))) {
	    perror("malloc");
	    exit(1);
	}
	status = 0;
	break;
    case 'h':			/* help */
	{
	    do_help(optarg);
	    break;
	}
    case 'i':			/* \i is include file */
	{
	    FILE           *fd;

	    if (!optarg) {
		fprintf(stderr, "\\i must be followed by a file name\n");
		break;
	    }
	    if ((fd = fopen(optarg, "r")) == NULL) {
		fprintf(stderr, "file named %s could not be opened\n", optarg);
		break;
	    }
	    MainLoop(settings, fd);
	    fclose(fd);
	    break;
	}
    case 'l':			/* \l is list database */
	listAllDbs(settings);
	break;
    case 'H':
	if (toggle(settings, &settings->opt.html3, "HTML3.0 tabular output"))
	    settings->opt.standard = 0;
	break;
    case 'o':
	setFout(settings, optarg);
	break;
    case 'p':
	if (query) {
	    fputs(query, stdout);
	    fputc('\n', stdout);
	}
	break;
    case 'q':			/* \q is quit */
	status = 2;
	break;
    case 'r':			/* reset(clear) the buffer */
	query[0] = '\0';
	if (!settings->quiet)
	    fprintf(stderr, "buffer reset(cleared)\n");
	break;
    case 's':			/* \s is save history to a file */
	if (!optarg)
	    optarg = "/dev/tty";
	if (write_history(optarg) != 0)
	    fprintf(stderr, "cannot write history to %s\n", optarg);
	break;
    case 'm':			/* monitor like type-setting */
	if (toggle(settings, &settings->opt.standard, "standard SQL separaters and padding")) {
	    settings->opt.html3 = settings->opt.expanded = 0;
	    settings->opt.align = settings->opt.header = 1;
	    if (settings->opt.fieldSep)
		free(settings->opt.fieldSep);
	    settings->opt.fieldSep = strdup("|");
	    if (!settings->quiet)
		fprintf(stderr, "field separater changed to '%s'\n", settings->opt.fieldSep);
	} else {
	    if (settings->opt.fieldSep)
	    	free(settings->opt.fieldSep);
	    settings->opt.fieldSep = strdup(DEFAULT_FIELD_SEP);
	    if (!settings->quiet)
		fprintf(stderr, "field separater changed to '%s'\n", settings->opt.fieldSep);
	}
	break;
    case 't':			/* toggle headers */
	toggle(settings, &settings->opt.header, "output headings and row count");
	break;
    case 'T':			/* define html <table ...> option */
	if (settings->opt.tableOpt)
	    free(settings->opt.tableOpt);
	if (!optarg)
	    settings->opt.tableOpt = NULL;
	else if (!(settings->opt.tableOpt = strdup(optarg))) {
	    perror("malloc");
	    exit(1);
	}
	break;
    case 'x':
	toggle(settings, &settings->opt.expanded, "expanded table representation");
	break;
    case '!':
	do_shell(optarg);
	break;
    default:
    case '?':			/* \? is help */
	slashUsage(settings);
	break;
    }
    free(cmd);
    return status;
}

/*
 * MainLoop: main processing loop for reading lines of input and sending them
 * to the backend
 * 
 * this loop is re-entrant.  May be called by \i command which reads input from
 * a file
 * 
 * db_ptr must be initialized and set
 */

int
MainLoop(PsqlSettings * settings, FILE * source)
{
    char           *line;	/* line of input */
    int             len;	/* length of the line */
    char            query[MAX_QUERY_BUFFER];	/* multi-line query storage */
    int             successResult = 1;
    int             slashCmdStatus = 0;
    /*
     * slashCmdStatus can be: 0 - send currently constructed query to backend
     * (i.e. we got a \g) 1 - skip processing of this line, continue building
     * up query 2 - terminate processing of this query entirely 3 - new query
     * supplied by edit
     */

    bool            querySent = false;
    bool            interactive;
    READ_ROUTINE    GetNextLine;
    bool            eof = 0;
    /* We've reached the end of our command input. */
    bool            success;
    bool            in_quote;
    bool            was_bslash;	/* backslash */
    bool            was_dash;
    int             paren_level;
    char           *query_start;

    interactive = ((source == stdin) && !settings->notty);
    if (interactive) {
	if (settings->prompt)
	    free(settings->prompt);
	settings->prompt =
	    malloc(strlen(PQdb(settings->db)) + strlen(PROMPT) + 1);
	if (settings->quiet)
	    settings->prompt[0] = '\0';
	else
	    sprintf(settings->prompt, "%s%s", PQdb(settings->db), PROMPT);
	if (settings->useReadline) {
	    using_history();
	    GetNextLine = gets_readline;
	} else
	    GetNextLine = gets_noreadline;
    } else
	GetNextLine = gets_fromFile;

    query[0] = '\0';
    in_quote = false;
    paren_level = 0;
    slashCmdStatus = -1;	/* set default */

    /* main loop for getting queries and executing them */
    while (!eof) {
	if (slashCmdStatus == 3) {
	    paren_level = 0;
	    line = strdup(query);
	    query[0] = '\0';
	} else {
	    if (interactive && !settings->quiet) {
	    	if (in_quote)
	    	    settings->prompt[strlen(settings->prompt)-3] = '\'';
	    	else if (query[0] != '\0' && !querySent)
	    	    settings->prompt[strlen(settings->prompt)-3] = '-';
	    	else
	    	    settings->prompt[strlen(settings->prompt)-3] = '=';
	    }
	    line = GetNextLine(settings->prompt, source);
	    if (interactive && settings->useReadline && line != NULL)
		add_history(line);	/* save non-empty lines in history */
	}

	query_start = line;

	if (line == NULL) {	/* No more input.  Time to quit */
	    printf("EOF\n");	/* Goes on prompt line */
	    eof = true;
	} else {
	    /* remove whitespaces on the right, incl. \n's */
	    line = rightTrim(line);

	    if (!interactive && !settings->singleStep && !settings->quiet)
		fprintf(stderr, "%s\n", line);

	    if (line[0] == '\0') {
		free(line);
		continue;
	    }

	    len = strlen(line);

	    if (settings->singleLineMode) {
		SendQuery(&success, settings, line, false, false, 0);
		successResult &= success;
		querySent = true;
	    } else {
		int             i;
		was_bslash = false;
		was_dash = false;

		for (i = 0; i < len; i++) {
		    if (!in_quote && line[i] == '\\') {
			char            hold_char = line[i];

			line[i] = '\0';
			if (query_start[0] != '\0') {
			    if (query[0] != '\0') {
				strcat(query, "\n");
				strcat(query, query_start);
			    } else
				strcpy(query, query_start);
			}
			line[i] = hold_char;
			query_start = line + i;
			break;	/* handle command */
		    }
		    if (querySent && !isspace(line[i])) {
			query[0] = '\0';
			querySent = false;
		    }
		    if (!in_quote && was_dash && line[i] == '-') {
			/* print comment at top of query */
			if (settings->singleStep)
			    fprintf(stdout, "%s\n", line + i - 1);
			line[i - 1] = '\0';	/* remove comment */
			break;
		    }
		    was_dash = false;

		    if (!in_quote && !paren_level &&
			line[i] == ';') {
			char            hold_char = line[i + 1];

			line[i + 1] = '\0';
			if (query_start[0] != '\0') {
			    if (query[0] != '\0') {
				strcat(query, "\n");
				strcat(query, query_start);
			    } else
				strcpy(query, query_start);
			}
			SendQuery(&success, settings, query, false, false, 0);
			successResult &= success;
			line[i + 1] = hold_char;
			query_start = line + i + 1;
			querySent = true;
		    }
		    if (was_bslash)
			was_bslash = false;
		    else if (line[i] == '\\')
			was_bslash = true;
		    else if (line[i] == '\'')
			in_quote ^= 1;
		    else if (!in_quote && line[i] == '(')
			paren_level++;
		    else if (!in_quote && paren_level && line[i] == ')')
			paren_level--;
		    else if (!in_quote && line[i] == '-')
			was_dash = true;
		}
	    }

	    slashCmdStatus = -1;
	    /* slash commands have to be on their own line */
	    if (!in_quote && query_start[0] == '\\') {
		slashCmdStatus = HandleSlashCmds(settings,
						 query_start,
						 query);
		if (slashCmdStatus == 1) {
		    if (query[0] == '\0')
			paren_level = 0;
		    free(line);
		    continue;
		}
		if (slashCmdStatus == 2) {
		    free(line);
		    break;
		}
	    } else if (strlen(query) + strlen(query_start) > MAX_QUERY_BUFFER) {
		fprintf(stderr, "query buffer max length of %d exceeded\n",
			MAX_QUERY_BUFFER);
		fprintf(stderr, "query line ignored\n");
	    } else {
		if (query_start[0] != '\0') {
		    querySent = false;
		    if (query[0] != '\0') {
			strcat(query, "\n");
			strcat(query, query_start);
		    } else
			strcpy(query, query_start);
		}
	    }

	    if (slashCmdStatus == 0) {
		SendQuery(&success, settings, query, false, false, 0);
		successResult &= success;
		querySent = true;
	    }
	    free(line);		/* free storage malloc'd by GetNextLine */
	}
    }				/* while */
    return successResult;
}

int
main(int argc, char **argv)
{
    extern char    *optarg;
    extern int      optind;

    char           *dbname = NULL;
    char           *host = NULL;
    char           *port = NULL;
    char           *qfilename = NULL;
    char            errbuf[ERROR_MSG_LENGTH];

    PsqlSettings    settings;

    char           *singleQuery = NULL;

    bool            listDatabases = 0;
    int             successResult = 1;
    bool            singleSlashCmd = 0;
    int             c;

    memset(&settings, 0, sizeof settings);
    settings.opt.align = 1;
    settings.opt.header = 1;
    settings.queryFout = stdout;
    settings.opt.fieldSep = strdup(DEFAULT_FIELD_SEP);
    settings.opt.pager = 1;
    if (!isatty(0) || !isatty(1))
	settings.quiet = settings.notty = 1;
#ifndef NOREADLINE
    else
	settings.useReadline = 1;
#endif

    while ((c = getopt(argc, argv, "Aa:c:d:ef:F:lh:Hnso:p:qStT:x")) != EOF) {
	switch (c) {
	case 'A':
	    settings.opt.align = 0;
	    break;
	case 'a':
	    fe_setauthsvc(optarg, errbuf);
	    break;
	case 'c':
	    singleQuery = optarg;
	    if (singleQuery[0] == '\\') {
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
	    settings.opt.fieldSep = optarg;
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
	    settings.opt.tableOpt = optarg;
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

    settings.db = PQsetdb(host, port, NULL, NULL, dbname);
    dbname = PQdb(settings.db);

    if (PQstatus(settings.db) == CONNECTION_BAD) {
	fprintf(stderr, "Connection to database '%s' failed.\n", dbname);
	fprintf(stderr, "%s", PQerrorMessage(settings.db));
	exit(1);
    }
    if (listDatabases) {
	exit(listAllDbs(&settings));
    }
    if (!settings.quiet && !singleQuery && !qfilename) {
	printf("Welcome to the POSTGRESQL interactive sql monitor:\n");
	printf("  Please read the file COPYRIGHT for copyright terms "
	       "of POSTGRESQL\n\n");
	printf("   type \\? for help on slash commands\n");
	printf("   type \\q to quit\n");
	printf("   type \\g or terminate with semicolon to execute query\n");
	printf(" You are currently connected to the database: %s\n\n", dbname);
    }
    if (qfilename || singleSlashCmd) {
	/*
	 * read in a file full of queries instead of reading in queries
	 * interactively
	 */
	char           *line;

	if (singleSlashCmd) {
	    /* Not really a query, but "Do what I mean, not what I say." */
	    line = singleQuery;
	} else {
	    line = malloc(strlen(qfilename) + 5);
	    sprintf(line, "\\i %s", qfilename);
	}
	HandleSlashCmds(&settings, line, "");
    } else {
	if (singleQuery) {
	    bool            success;	/* The query succeeded at the backend */
	    SendQuery(&success, &settings, singleQuery, false, false, 0);
	    successResult = success;
	} else
	    successResult = MainLoop(&settings, stdin);
    }

    PQfinish(settings.db);

    return !successResult;
}

#define COPYBUFSIZ  8192

static void
handleCopyOut(PGresult * res, bool quiet, FILE * copystream)
{
    bool            copydone;
    char            copybuf[COPYBUFSIZ];
    int             ret;

    copydone = false;		/* Can't be done; haven't started. */

    while (!copydone) {
	ret = PQgetline(res->conn, copybuf, COPYBUFSIZ);

	if (copybuf[0] == '\\' &&
	    copybuf[1] == '.' &&
	    copybuf[2] == '\0') {
	    copydone = true;	/* don't print this... */
	} else {
	    fputs(copybuf, copystream);
	    switch (ret) {
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
handleCopyIn(PGresult * res, const bool mustprompt, FILE * copystream)
{
    bool            copydone = false;
    bool            firstload;
    bool            linedone;
    char            copybuf[COPYBUFSIZ];
    char           *s;
    int             buflen;
    int             c;

    if (mustprompt) {
	fputs("Enter info followed by a newline\n", stdout);
	fputs("End with a backslash and a "
	      "period on a line by itself.\n", stdout);
    }
    while (!copydone) {		/* for each input line ... */
	if (mustprompt) {
	    fputs(">> ", stdout);
	    fflush(stdout);
	}
	firstload = true;
	linedone = false;
	while (!linedone) {	/* for each buffer ... */
	    s = copybuf;
	    buflen = COPYBUFSIZ;
	    for (; buflen > 1 &&
		 !(linedone = (c = getc(copystream)) == '\n' || c == EOF);
		 --buflen) {
		*s++ = c;
	    }
	    if (c == EOF) {
		PQputline(res->conn, "\\.");
		copydone = true;
		break;
	    }
	    *s = '\0';
	    PQputline(res->conn, copybuf);
	    if (firstload) {
		if (!strcmp(copybuf, "\\.")) {
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

FILE           *
setFout(PsqlSettings * ps, char *fname)
{
    if (ps->queryFout && ps->queryFout != stdout) {
	if (ps->pipe)
	    pclose(ps->queryFout);
	else
	    fclose(ps->queryFout);
    }
    if (!fname) {
	ps->queryFout = stdout;
	pqsignal(SIGPIPE, SIG_DFL);
    }
    else {
	if (*fname == '|') {
	    pqsignal(SIGPIPE, SIG_IGN);
	    ps->queryFout = popen(fname + 1, "w");
	    ps->pipe = 1;
	} else {
	    ps->queryFout = fopen(fname, "w");
	    pqsignal(SIGPIPE, SIG_DFL);
	    ps->pipe = 0;
	}
	if (!ps->queryFout) {
	    perror(fname);
	    ps->queryFout = stdout;
	}
    }
    return ps->queryFout;
}
