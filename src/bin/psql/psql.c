/*-------------------------------------------------------------------------
 *
 * psql.c--
 *    an interactive front-end to postgres95
 *
 * Copyright (c) 1996, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/bin/psql/Attic/psql.c,v 1.14 1996/07/29 20:58:42 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include "libpq-fe.h"
#include "stringutils.h"

#include "psqlHelp.h"

#ifdef NOREADLINE
extern char *readline(char *);	/* in rlstubs.c */
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

#define MAX_QUERY_BUFFER 20000

#define COPYBUFSIZ	8192

#define DEFAULT_FIELD_SEP "|"
#define DEFAULT_EDITOR  "vi"
#define DEFAULT_SHELL  "/bin/sh"

typedef struct _psqlSettings {
    PGconn *db;		   /* connection to backend */
    FILE *queryFout;       /* where to send the query results */
    PQprintOpt opt;        /* options to be passed to PQprint */
    char *prompt;	   /* prompt to display */
    char *gfname;	   /* one-shot file output argument for \g */
    bool notty;		   /* input or output is not a tty */
    bool pipe;		   /* queryFout is from a popen() */
    bool echoQuery;        /* echo the query before sending it */
    bool quiet;            /* run quietly, no messages, no promt */
    bool singleStep;       /* prompt before for each query */ 
    bool singleLineMode;   /* query terminated by newline */
    bool useReadline;      /* use libreadline routines */
} PsqlSettings;

/* declarations for functions in this file */
static void usage(char *progname);
static void slashUsage();
static void handleCopyOut(PGresult *res, bool quiet);
static void handleCopyIn(PGresult *res, bool quiet);
static int tableList(PsqlSettings *ps, bool deep_tablelist);
static int tableDesc(PsqlSettings *ps, char *table);

char *gets_noreadline(char *prompt, FILE *source);
char *gets_readline(char *prompt, FILE *source);
char *gets_fromFile(char *prompt, FILE *source);
int listAllDbs(PsqlSettings *settings);
int SendQuery(PsqlSettings *settings, char *query);
int HandleSlashCmds(PsqlSettings *settings,
		    char *line,
		    char *query);
int MainLoop(PsqlSettings *settings, FILE *source);
/* probably should move this into libpq */
void PQprint(FILE *fp,
                     PGresult *res, 
		     PQprintOpt *po
		     );

FILE *setFout(PsqlSettings *ps, char *fname);

/*
 * usage 
 *   print out usage for command line arguments 
 */

static void  
usage(char *progname)
{
  fprintf(stderr,"Usage: %s [options] [dbname]\n",progname);
  fprintf(stderr,"\t -a authsvc              set authentication service\n"); 
  fprintf(stderr,"\t -A                      turn off alignment when printing out attributes\n");
  fprintf(stderr,"\t -c query                run single query (slash commands too)\n");
  fprintf(stderr,"\t -d dbName               specify database name\n");
  fprintf(stderr,"\t -e                      echo the query sent to the backend\n");
  fprintf(stderr,"\t -f filename             use file as a source of queries\n");
  fprintf(stderr,"\t -F sep                  set the field separator (default is " ")\n");
  fprintf(stderr,"\t -h host                 set database server host\n");
  fprintf(stderr,"\t -H                      turn on html3.0 table output\n");
  fprintf(stderr,"\t -l                      list available databases\n");
  fprintf(stderr,"\t -n                      don't use readline library\n");
  fprintf(stderr,"\t -o filename             send output to filename or (|pipe)\n");
  fprintf(stderr,"\t -p port                 set port number\n");
  fprintf(stderr,"\t -q                      run quietly (no messages, no prompts)\n");
  fprintf(stderr,"\t -s                      single step mode (prompts for each query)\n");
  fprintf(stderr,"\t -S                      single line mode (i.e. query terminated by newline)\n");
  fprintf(stderr,"\t -t                      turn off printing of attribute headers\n");
  fprintf(stderr,"\t -T html                 set html3.0 table command options (cf. -H)\n");
  fprintf(stderr,"\t -x                      turn on expanded output (field names on left)\n");
  exit(1);
}

/*
 * slashUsage
 *    print out usage for the backslash commands 
 */

char *on(bool f)
{
	return f? "on": "off";
}

static void
slashUsage(PsqlSettings *ps)
{
  fprintf(stderr,"\t \\a           -- toggle field-alignment (currenty %s)\n", on(ps->opt.align));
  fprintf(stderr,"\t \\C [<captn>] -- set html3 caption (currently '%s')\n", ps->opt.caption? ps->opt.caption: "");
  fprintf(stderr,"\t \\c <dbname>  -- connect to new database (currently '%s')\n", PQdb(ps->db));
  fprintf(stderr,"\t \\d [<table>] -- list tables in database or columns in <table>\n");
  fprintf(stderr,"\t \\d *         -- list tables in database and columns in all tables\n");
  fprintf(stderr,"\t \\e [<fname>] -- edit the current query buffer or <fname>\n");
  fprintf(stderr,"\t \\f [<sep>]   -- change field separater (currently '%s')\n", ps->opt.fieldSep);
  fprintf(stderr,"\t \\g [<fname>] -- send query to backend [and place results in <fname>]\n");
  fprintf(stderr,"\t \\g |<cmd>    -- send query to backend and pipe results into <cmd>\n");
  fprintf(stderr,"\t \\h [<cmd>]   -- help on syntax of sql commands\n");
  fprintf(stderr,"\t \\h *         -- complete description of all sql commands\n");
  fprintf(stderr,"\t \\H           -- toggle html3 output (currently %s)\n", on(ps->opt.html3));
  fprintf(stderr,"\t \\i <fname>   -- read and execute queries from filename\n");
  fprintf(stderr,"\t \\l           -- list all databases\n");
  fprintf(stderr,"\t \\m           -- toggle monitor-like type-setting (currently %s)\n", on(ps->opt.standard));
  fprintf(stderr,"\t \\o [<fname>] -- send all query results to <fname> or stdout\n");
  fprintf(stderr,"\t \\o |<cmd>    -- pipe all query results through <cmd>\n");
  fprintf(stderr,"\t \\p           -- print the current query buffer\n");
  fprintf(stderr,"\t \\q           -- quit\n");
  fprintf(stderr,"\t \\r [<fname>] -- edit <fname> then execute on save\n");
  fprintf(stderr,"\t \\s [<fname>] -- print history or save it in <fname>\n");
  fprintf(stderr,"\t \\t           -- toggle table output header (currently %s)\n", on(ps->opt.header));
  fprintf(stderr,"\t \\T [<html>]  -- set html3.0 <table ...> options (currently '%s')\n", ps->opt.tableOpt? ps->opt.tableOpt: "");
  fprintf(stderr,"\t \\x           -- toggle expanded output (currently %s)\n", on(ps->opt.expanded));
  fprintf(stderr,"\t \\z           -- zorch current query buffer (i.e clear it)\n");
  fprintf(stderr,"\t \\! [<cmd>]   -- shell escape or command\n");
  fprintf(stderr,"\t \\?           -- help\n");
}

PGresult *
PSQLexec(PsqlSettings *ps, char *query)
{
	PGresult *res = PQexec(ps->db, query);
	if (!res)
   		fputs(PQerrorMessage(ps->db), stderr);
	else
	{
		if (PQresultStatus(res)==PGRES_COMMAND_OK ||
		    PQresultStatus(res)==PGRES_TUPLES_OK)
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
 * list all the databases in the system
 *     returns 0 if all went well
 *    
 *
 */

int
listAllDbs(PsqlSettings *ps)
{
  PGresult *results;
  char *query = "select * from pg_database;";

  if (!(results=PSQLexec(ps, query)))
    return 1;
  else
    {
      PQprint(ps->queryFout,
                      results,
		      &ps->opt);
      PQclear(results);
      return 0;
    }
}

/*
 *
 * List The Database Tables
 *     returns 0 if all went well
 *    
 */
int
tableList (PsqlSettings *ps, bool deep_tablelist)
{
  char listbuf[256];
  int nColumns; 
  int i;
  char *rk;
  char *rr;

  PGresult *res;

  listbuf[0] = '\0';
  strcat(listbuf,"SELECT usename, relname, relkind, relhasrules");
  strcat(listbuf,"  FROM pg_class, pg_user ");
  strcat(listbuf,"WHERE ( relkind = 'r' OR relkind = 'i') ");
  strcat(listbuf,"  and relname !~ '^pg_'");
  strcat(listbuf,"  and relname !~ '^Inv[0-9]+'");
/* the usesysid = relowner won't work on stock 1.0 dbs, need to 
   add in the int4oideq function */
  strcat(listbuf,"  and usesysid = relowner");
  strcat(listbuf,"  ORDER BY relname ");
  if (!(res=PSQLexec(ps, listbuf)))
	return -1;

  /* first, print out the attribute names */
  nColumns = PQntuples(res);
  if (nColumns > 0)
  {
      if ( deep_tablelist ) {
	  /* describe everything here */
	  char **table;
	  table = (char**)malloc(nColumns * sizeof(char*));
	  if ( table == NULL )
	      perror("malloc");
	  
	  /* load table table */
	  for (i=0; i < nColumns; i++) {
	      table[i] = (char *) malloc(PQgetlength(res,i,1) * sizeof(char) + 1);
	      if ( table[i] == NULL )
		  perror("malloc");
	      strcpy(table[i],PQgetvalue(res,i,1));
	  }

 	PQclear(res);
 	for (i=0; i < nColumns; i++) {
 	   tableDesc(ps, table[i]);
 	}
 	free(table);
      }
      else {
 	/* Display the information */

 	printf ("\nDatabase    = %s\n", PQdb(ps->db));
	printf (" +------------------+----------------------------------+----------+\n");
	printf (" |  Owner           |             Relation             |   Type   |\n");
	printf (" +------------------+----------------------------------+----------+\n");

 	/* next, print out the instances */
 	for (i=0; i < PQntuples(res); i++) {
	    printf (" | %-16.16s", PQgetvalue(res,i,0));
	    printf (" | %-32.32s | ", PQgetvalue(res,i,1));
	    rk =  PQgetvalue(res,i,2);
	    rr =  PQgetvalue(res,i,3);
	    if (strcmp(rk, "r") == 0)
		printf ("%-8.8s |", (rr[0] == 't') ? "view?" : "table" );
	    else
		printf ("%-8.8s |", "index");
	    printf("\n");
 	}
	printf (" +------------------+----------------------------------+----------+\n");
 	PQclear(res);
      }
      return (0);
  
  } else {
    fprintf (stderr, "Couldn't find any tables!\n");
    return (-1);
  }
}

/*
 * Describe a table
 *
 * Describe the columns in a database table.
 *     returns 0 if all went well
 *    
 *
 */
int
tableDesc (PsqlSettings *ps, char *table)
{
  char descbuf[256];
  int nColumns;
  char *rtype;
  int i;
  int rsize;

  PGresult *res;

  /* Build the query */

  descbuf[0] = '\0';
  strcat(descbuf,"SELECT a.attnum, a.attname, t.typname, a.attlen");
  strcat(descbuf,"  FROM pg_class c, pg_attribute a, pg_type t ");
  strcat(descbuf,"    WHERE c.relname = '");
  strcat(descbuf,table);
  strcat(descbuf,"'");
  strcat(descbuf,"    and a.attnum > 0 ");
  strcat(descbuf,"    and a.attrelid = c.oid ");
  strcat(descbuf,"    and a.atttypid = t.oid ");
  strcat(descbuf,"  ORDER BY attnum ");
  if (!(res = PSQLexec(ps, descbuf)))
	return -1;
  /* first, print out the attribute names */
  nColumns = PQntuples(res);
  if (nColumns > 0)
  {
    /*
    ** Display the information
    */

    printf ("\nTable    = %s\n", table);
    printf ("+----------------------------------+----------------------------------+-------+\n");
    printf ("|              Field               |              Type                | Length|\n");
    printf ("+----------------------------------+----------------------------------+-------+\n");

    /* next, print out the instances */
    for (i=0; i < PQntuples(res); i++) {
      printf ("| %-32.32s | ", PQgetvalue(res,i,1));
      rtype = PQgetvalue(res,i,2);
      rsize = atoi(PQgetvalue(res,i,3));
      if (strcmp(rtype, "text") == 0) {
        printf ("%-32.32s |", rtype);
        printf ("%6s |",  "var" );
      }
      else if (strcmp(rtype, "bpchar") == 0) {
        printf ("%-32.32s |", "char");
        printf ("%6i |", rsize > 0 ? rsize - 4 : 0 );
      }
      else if (strcmp(rtype, "varchar") == 0) {
        printf ("%-32.32s |", rtype);
        printf ("%6i |", rsize > 0 ? rsize - 4 : 0 );
      }
      else {
	  /* array types start with an underscore */
	  if (rtype[0] != '_')
	      printf ("%-32.32s |", rtype);
	  else  {
	      char *newname;
	      newname = malloc(strlen(rtype) + 2);
	      strcpy(newname, rtype+1);
	      strcat(newname, "[]");
	      printf ("%-32.32s |", newname);
	      free(newname);
	  }
	if (rsize > 0) 
	    printf ("%6i |", rsize);
	else
	    printf ("%6s |", "var");
      }
      printf("\n");
    }
    printf ("+----------------------------------+----------------------------------+-------+\n");

    PQclear(res);
    return (0);
  
  } else {
      fprintf (stderr, "Couldn't find table %s!\n", table);
    return (-1);
  }
}

typedef char *(*READ_ROUTINE)(char *prompt, FILE *source);

/* gets_noreadline  prompt source
      gets a line of input without calling readline, the source is ignored
*/
char *
gets_noreadline(char *prompt, FILE *source)
{
    fputs(prompt, stdout);
    fflush(stdout);
    return(gets_fromFile(prompt,stdin));
}

/*
 * gets_readline  prompt source
 *   the routine to get input from GNU readline(), the source is ignored 
 * the prompt argument is used as the prompting string
 */
char *
gets_readline(char *prompt, FILE *source)
{
  return (readline(prompt));
}

/*
 * gets_fromFile  prompt source
 *    
 * the routine to read from a file, the prompt argument is ignored
 * the source argument is a FILE *
 */
char *
gets_fromFile(char *prompt, FILE *source)
{
  char *line;
  int len;

  line = malloc(MAX_QUERY_BUFFER+1);

  /* read up to MAX_QUERY_BUFFER characters */
  if (fgets(line, MAX_QUERY_BUFFER, source) == NULL)
    return NULL;

  line[MAX_QUERY_BUFFER-1] = '\0';
  len = strlen(line);
  if (len == MAX_QUERY_BUFFER)
    {
      fprintf(stderr, "line read exceeds maximum length.  Truncating at %d\n", MAX_QUERY_BUFFER);
    }
  
  return line;
}

/*
 *  SendQuery: send the query string to the backend 
 *  return 0 if the query executed successfully
 *  returns 1 otherwise
 */
int
SendQuery(PsqlSettings *settings, char *query)
{
  PGresult *results;
  PGnotify *notify;
  int status = 0;

  if (settings->singleStep)
	fprintf(stdout, "\n*******************************************************************************\n");

  if (settings->echoQuery || settings->singleStep) {
      fprintf(stderr,"QUERY: %s\n",query);
      fflush(stderr);
  }

  if (settings->singleStep) {
	fprintf(stdout, "\n*******************************************************************************\n");
	fflush(stdout);
	printf("\npress return to continue ..\n");
	gets_fromFile("",stdin);
  }

  results = PQexec(settings->db, query);
  if (results == NULL) {
    fprintf(stderr,"%s",PQerrorMessage(settings->db));
    return 1;
  }

  switch (PQresultStatus(results)) {
  case PGRES_TUPLES_OK:
      if (settings->gfname)
      {
      		PsqlSettings ps=*settings;
		FILE *fp;
		ps.queryFout=stdout;
		fp=setFout(&ps, settings->gfname);
		if (!fp || fp==stdout)
		{
			status = 1;
			break;
		}
		PQprint(fp,
			results,
			&(settings->opt));
		if (ps.pipe)
			pclose(fp);
		else
			fclose(fp);
		settings->gfname=NULL;
		break;
	} else 
	{
	      PQprint(settings->queryFout,
			      results,
			      &(settings->opt));
	      fflush(settings->queryFout);
	}
      PQclear(results);
      break;
  case PGRES_EMPTY_QUERY:
    /* do nothing */
    break;
  case PGRES_COMMAND_OK:
    if (!settings->quiet)
      fprintf(stdout,"%s\n", PQcmdStatus(results));
    break;
  case PGRES_COPY_OUT:
    handleCopyOut(results, settings->quiet);
    break;
  case PGRES_COPY_IN:
    handleCopyIn(results, settings->quiet);
    break;
  case PGRES_NONFATAL_ERROR:
  case PGRES_FATAL_ERROR:
  case PGRES_BAD_RESPONSE:
    status = 1;
    fprintf(stderr,"%s",PQerrorMessage(settings->db));
    break;
  } 

  /* check for asynchronous returns */
  notify = PQnotifies(settings->db);
  if (notify) {
      fprintf(stderr,"ASYNC NOTIFY of '%s' from backend pid '%d' received\n",
	      notify->relname, notify->be_pid);
      free(notify);
  }

  return status;

}

void
editFile(char *fname)
{
    char *editorName;
    char *sys;
    editorName = getenv("EDITOR");
    if (!editorName)
	editorName = DEFAULT_EDITOR;
    sys=malloc(strlen(editorName)+strlen(fname)+32+1);
    if (!sys)
    {
	perror("malloc");
	exit(1);
    }
    sprintf(sys, "exec '%s' '%s'", editorName, fname);
    system(sys);
    free(sys);
}

bool
toggle(PsqlSettings *settings, bool *sw, char *msg)
{
	*sw= !*sw;
	if (!settings->quiet)
	    fprintf(stderr, "turned %s %s\n", on(*sw), msg);
	return *sw;
}

void
decode(char *s)
{
	char *p, *d;
	bool esc=0;
	for (d=p=s; *p; p++)
	{
		char c=*p;
		if (esc)
		{
			switch(*p)
			{
			case 'n':
				c='\n';
				break;
			case 'r':
				c='\r';
				break;
			case 't':
				c='\t';
				break;
			case 'f':
				c='\f';
				break;
			}
			esc=0;
		} else
			if (c=='\\')
			{
				esc=1;
				continue;
			}
		*d++=c;
	}
	*d='\0';
}

/*
  HandleSlashCmds:

  Handles all the different commands that start with \ 
     db_ptr is a pointer to the TgDb* structure
     line is the current input line
     prompt_ptr is a pointer to the prompt string,
                  a pointer is used because the prompt can be used with 
		  a connection to a new database
  returns a status:
       0 - send currently constructed query to backend (i.e. we got a \g)
       1 - skip processing of this line, continue building up query
       2 - terminate processing of this query entirely
*/
int
HandleSlashCmds(PsqlSettings *settings,
		char *line, 
		char *query)
{
  int status = 1;
  char *optarg = NULL;
  int len;

  len = strlen(line);
  if (len > 2)
  {
      optarg = leftTrim(line+2);
      decode(optarg);
  }
  switch (line[1])
    {
    case 'a': /* toggles to align fields on output */
        toggle(settings, &settings->opt.align, "field alignment");
	break;
    case 'C': /* define new caption */
    	if (settings->opt.caption)
		free(settings->opt.caption);
	if (!optarg)
		settings->opt.caption=NULL;
	else
		if (!(settings->opt.caption=dupstr(optarg)))
		{
			perror("malloc");
			exit(1);
		}
	break;
    case 'c':  /* \c means connect to new database */
      {
	  char *dbname=PQdb(settings->db);
	  if (!optarg) {
	      fprintf(stderr,"\\c must be followed by a database name\n");
	      break;
	  }
	  {
	      PGconn *olddb=settings->db;
	      
	      printf("closing connection to database: %s\n", dbname);
	      settings->db = PQsetdb(PQhost(olddb), PQport(olddb), NULL, NULL, optarg);
	      printf("connecting to new database: %s\n", optarg);
	      if (PQstatus(settings->db) == CONNECTION_BAD) {
		  fprintf(stderr,"%s\n", PQerrorMessage(settings->db));
		  printf("reconnecting to %s\n", dbname);
		  settings->db = PQsetdb(PQhost(olddb), PQport(olddb), 
			       NULL, NULL, dbname);
		  if (PQstatus(settings->db) == CONNECTION_BAD) {
		      fprintf(stderr, 
			      "could not reconnect to %s. exiting\n", dbname);
		      exit(2);
		  }
		  break;
	      }
	      PQfinish(olddb);
	      free(settings->prompt);
	      settings->prompt = malloc(strlen(PQdb(settings->db)) + 10);
	      sprintf(settings->prompt,"%s=> ", PQdb(settings->db));
	    break;
	  }
      }
      break;
    case 'd':     /* \d describe tables or columns in a table */
	if (!optarg) {
          tableList(settings, 0);
	  break;
	} 
 	if (strcmp(optarg, "*") == 0 ) {
 	   tableList(settings, 0);
 	   tableList(settings, 1);
 	}
 	else {
 	   tableDesc(settings, optarg);
 	}
 	break;
    case 'e':
      {
	int fd;
	char tmp[64];
	char *fname;
	int cc;
	int ql = strlen(query);
        if (optarg)
        	fname=optarg;
        else
	{
		sprintf(tmp, "/tmp/psql.%d.%d", geteuid(), getpid());
		fname=tmp;
		unlink(tmp);
		if (ql)
		{
			if ((fd=open(tmp, O_EXCL|O_CREAT|O_WRONLY, 0600))==-1)
			{
				perror(tmp);
				break;
			}
			if (query[ql-1]!='\n')
				strcat(query, "\n");
			if (write(fd, query, ql)!=ql)
			{
				perror(tmp);
				close(fd);
				unlink(tmp);
				break;
			}
			close(fd);
		}
	}
	editFile(fname);
	if ((fd=open(fname, O_RDONLY))==-1)
	{
		perror(fname);
		if (!optarg)
			unlink(fname);
		break;
	}
	if ((cc=read(fd, query, MAX_QUERY_BUFFER))==-1)
        {
		perror(fname);
		close(fd);
		if (!optarg)
			unlink(fname);
		break;
	}	
	query[cc]='\0';
	close(fd);
	if (!optarg)
		unlink(fname);
	rightTrim(query);
	if (query[strlen(query)-1]==';')
		return 0;
	break;
      }
    case 'f':
    {
        char *fs=DEFAULT_FIELD_SEP;
    	if (optarg)
		fs=optarg;
        if (settings->opt.fieldSep);
		free(settings->opt.fieldSep);
	if (!(settings->opt.fieldSep=dupstr(fs)))
	{
		perror("malloc");
		exit(1);
	}
	if (!settings->quiet)
		fprintf(stderr, "field separater changed to '%s'\n", settings->opt.fieldSep);
	break;
    }
  case 'g':  /* \g means send query */
      settings->gfname = optarg;
      status = 0;     
      break;
    case 'h':
      {
	char *cmd;
	int i, numCmds;
	int all_help = 0;

	if (!optarg) {
	    printf("type \\h <cmd> where <cmd> is one of the following:\n");
	    i = 0;
	    while (QL_HELP[i].cmd != NULL)
	      {
		printf("\t%s\n", QL_HELP[i].cmd);
		i++;
	      }
 	     printf("type \\h * for a complete description of all commands\n");
	  }
	else
	  {
	  cmd = optarg;

	  numCmds = 0;
	  while (QL_HELP[numCmds++].cmd != NULL);

	  numCmds = numCmds - 1;

 	  if (strcmp(cmd, "*") == 0 ) {
 	     all_help=1;
 	  }

	  for (i=0; i<numCmds;i++)  {
	      if (strcmp(QL_HELP[i].cmd, cmd) == 0 || all_help)    {
		printf("Command: %s\n",QL_HELP[i].cmd);
		printf("Description: %s\n", QL_HELP[i].help);
		printf("Syntax:\n");
		printf("%s\n", QL_HELP[i].syntax);
 		if ( all_help ) {
 		   printf("\n");
 		}
 		else {
 		   break;
	       }
	    }
	  }
	  if (i == numCmds && ! all_help)
	    printf("command not found,  try \\h with no arguments to see available help\n");
	}
	break;
      }
    case 'i':     /* \i is include file */
      {
	FILE *fd;

	if (!optarg) {
	  fprintf(stderr,"\\i must be followed by a file name\n");
	  break;
	}

	if ((fd = fopen(optarg, "r")) == NULL)
	  {
	    fprintf(stderr,"file named %s could not be opened\n",optarg);
	    break;
	  }
	MainLoop(settings, fd);
	fclose(fd);
	break;
      }
    case 'l':     /* \l is list database */
      listAllDbs(settings);
      break;
    case 'H':
      if (toggle(settings, &settings->opt.html3, "HTML3.0 tablular output"))
          settings->opt.standard = 0;
      break;
    case 'o':
      setFout(settings, optarg);
      break;
    case 'p':
      if (query)
      {
	      fputs(query, stdout);
	      fputc('\n', stdout);
      }
      break;
    case 'q': /* \q is quit */
      status = 2;
      break;
    case 'r':
    {
	FILE *fd;
	static char *lastfile;
	struct stat st, st2;
	if (optarg)
	{
		if (lastfile)
			free(lastfile);
		lastfile=malloc(strlen(optarg+1));
		if (!lastfile)
		{
			perror("malloc");
			exit(1);
		}
		strcpy(lastfile, optarg);
	} else if (!lastfile)
		{
	  		  fprintf(stderr,"\\r must be followed by a file name initially\n");
			  break;
		}
	stat(lastfile, &st);
	editFile(lastfile);
	if ((stat(lastfile, &st2) == -1) || ((fd = fopen(lastfile, "r")) == NULL))
	  {
	    perror(lastfile);
	    break;
	  }
	if (st2.st_mtime==st.st_mtime)
	{
		if (!settings->quiet)
			fprintf(stderr, "warning: %s not modified. query not executed\n", lastfile);
		fclose(fd);
		break;
	}
	MainLoop(settings, fd);
	fclose(fd);
	break;
      }
    case 's': /* \s is save history to a file */
	if (!optarg)
		optarg="/dev/tty";
	if (write_history(optarg) != 0)
	    fprintf(stderr,"cannot write history to %s\n",optarg);
	break;
    case 'm': /* monitor like type-setting */
      if (toggle(settings, &settings->opt.standard, "standard SQL separaters and padding"))
      {
          settings->opt.html3 = settings->opt.expanded = 0;
          settings->opt.align = settings->opt.header = 1;
	  free(settings->opt.fieldSep);
	  settings->opt.fieldSep=dupstr("|");
	  if (!settings->quiet)
	      fprintf(stderr, "field separater changed to '%s'\n", settings->opt.fieldSep);
      } else
      {
	  free(settings->opt.fieldSep);
	  settings->opt.fieldSep=dupstr(DEFAULT_FIELD_SEP);
	  if (!settings->quiet)
	      fprintf(stderr, "field separater changed to '%s'\n", settings->opt.fieldSep);
      }
      break;
    case 't': /* toggle headers */
      toggle(settings, &settings->opt.header, "output headers");
      break;
    case 'T': /* define html <table ...> option */
    	if (settings->opt.tableOpt)
		free(settings->opt.tableOpt);
	if (!optarg)
		settings->opt.tableOpt=NULL;
	else
		if (!(settings->opt.tableOpt=dupstr(optarg)))
		{
			perror("malloc");
			exit(1);
		}
	break;
    case 'x':
      toggle(settings, &settings->opt.expanded, "expanded table representation");
      break;
    case 'z': /* zorch buffer */
      query[0]='\0';
      if (!settings->quiet)
      	  fprintf(stderr, "zorched current query buffer\n");
      break;
    case '!':
      if (!optarg) {
	  char *sys;
	  char *shellName;
	  shellName = getenv("SHELL");
	  if (shellName == NULL) 
	      shellName = DEFAULT_SHELL;
	  sys = malloc(strlen(shellName)+16);
	  if (!sys)
	  {
	  	perror("malloc");
		exit(1);
	  }
	  sprintf(sys,"exec %s", shellName);
	  system(sys);
	  free(sys);
      }
      else
	  system(optarg);
      break;
    default:
    case '?':     /* \? is help */
      slashUsage(settings);
      break;
    }
  return status;
}

/* 
 MainLoop: main processing loop for reading lines of input
 and sending them to the backend

 this loop is re-entrant.  May be called by \i command
 which reads input from a file

 *db_ptr must be initialized and set
*/

int
MainLoop(PsqlSettings *settings, FILE *source)
{
  char *line;                   /* line of input */
  int len;                      /* length of the line */
  char query[MAX_QUERY_BUFFER]; /* multi-line query storage */
  int exitStatus = 0;
  int slashCmdStatus = 0;
 /* slashCmdStatus can be:
       0 - send currently constructed query to backend (i.e. we got a \g)
       1 - skip processing of this line, continue building up query
       2 - terminate processing of this query entirely
  */

  bool sendQuery = 0;
  bool querySent = 0;
  bool interactive;
  READ_ROUTINE GetNextLine;

  interactive = ((source == stdin) && !settings->notty);
#define PROMPT "=> "
  if (interactive) {
    if (settings->prompt)
	free(settings->prompt);
    settings->prompt = malloc(strlen(PQdb(settings->db)) + strlen(PROMPT) + 1);
    if (settings->quiet)
      settings->prompt[0] = '\0';
    else
      sprintf(settings->prompt,"%s%s", PQdb(settings->db), PROMPT);
    if (settings->useReadline) {
	using_history();
	GetNextLine = gets_readline;
    } else
	GetNextLine = gets_noreadline;

  }
  else
    GetNextLine = gets_fromFile;

  query[0] = '\0';
  
  /* main loop for getting queries and executing them */
  while ((line = GetNextLine(settings->prompt, source)) != NULL)
    {
	exitStatus = 0;
      line = rightTrim(line); /* remove whitespaces on the right, incl. \n's */

      if (line[0] == '\0') {
	  free(line);
	  continue;
      }

      /* filter out comment lines that begin with --,
         this could be incorrect if -- is part of a quoted string.
         But we won't go through the trouble of detecting that.  If you have
	 -- in your quoted string, be careful and don't start a line with it */
      if (line[0] == '-' && line[1] == '-') {
	  if (settings->singleStep) /* in single step mode, show comments */
	      fprintf(stdout,"%s\n",line);
	  free(line);
	  continue;
      }
      if (line[0] != '\\' && querySent)
      {
	  query[0]='\0';
          querySent = 0;
      }

      len = strlen(line);

      if (interactive && settings->useReadline)
	  add_history(line);      /* save non-empty lines in history */
      
      /* do the query immediately if we are doing single line queries 
       or if the last character is a semicolon */
      sendQuery = settings->singleLineMode || (line[len-1] == ';') ;

      /* normally, \ commands have to be start the line,
	 but for backwards compatibility with monitor,
	 check for \g at the end of line */
      if (len > 2 && !sendQuery) 
	{
	  if (line[len-1]=='g' && line[len-2]=='\\')
	    {
	    sendQuery = 1;
	    line[len-2]='\0';
	  }
	}
      
      /* slash commands have to be on their own line */
      if (line[0] == '\\') {
	  slashCmdStatus = HandleSlashCmds(settings,
					   line, 
					   query);
	if (slashCmdStatus == 1)
	  continue;
	if (slashCmdStatus == 2)
	  break;
	if (slashCmdStatus == 0)
	  sendQuery = 1;
      }
      else
	if (strlen(query) + len > MAX_QUERY_BUFFER)
	  {
	    fprintf(stderr,"query buffer max length of %d exceeded\n",MAX_QUERY_BUFFER);
	    fprintf(stderr,"query line ignored\n");
	  }
      else
	if (query[0]!='\0') {
	    strcat(query,"\n");
	    strcat(query,line);
	}
      else
	strcpy(query,line);
      
      if (sendQuery && query[0] != '\0')
	{
	    /* echo the line read from the file,
	     unless we are in single_step mode, because single_step mode
	     will echo anyway */
	  if (!interactive && !settings->singleStep && !settings->quiet) 
	    fprintf(stderr,"%s\n", query);

	  exitStatus = SendQuery(settings, query);
          querySent = 1;
	}
      
       free(line); /* free storage malloc'd by GetNextLine */
    } /* while */
    return exitStatus;
} 

int
main(int argc, char **argv)
{
  extern char *optarg;
  extern int optind;
  
  char *dbname = NULL;
  char *host = NULL;
  char *port = NULL;
  char *qfilename = NULL;
  char errbuf[ERROR_MSG_LENGTH];

  PsqlSettings settings;

  char *singleQuery = NULL;

  bool listDatabases = 0 ;
  int exitStatus = 0;
  bool singleSlashCmd = 0;
  int c;

  memset(&settings, 0, sizeof settings);
  settings.opt.align = 1;
  settings.opt.header = 1;
  settings.queryFout = stdout;
  settings.opt.fieldSep=dupstr(DEFAULT_FIELD_SEP);
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
	if ( singleQuery[0] == '\\' ) {
	    singleSlashCmd=1;
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
      settings.opt.fieldSep=optarg;
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
      settings.opt.expanded = 0;
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
    fprintf(stderr,"Connection to database '%s' failed.\n", dbname);
    fprintf(stderr,"%s",PQerrorMessage(settings.db));
    exit(1);
  }
  if (listDatabases) {
      exit(listAllDbs(&settings));
    }

  if (!settings.quiet && !singleQuery && !qfilename) {
    printf("Welcome to the POSTGRES95 interactive sql monitor:\n");
    printf("  Please read the file COPYRIGHT for copyright terms of POSTGRES95\n\n");
    printf("   type \\? for help on slash commands\n");
    printf("   type \\q to quit\n");
    printf("   type \\g or terminate with semicolon to execute query\n");
    printf(" You are currently connected to the database: %s\n\n", dbname);
     }

  if (qfilename || singleSlashCmd) {
      /* read in a file full of queries instead of reading in queries
	 interactively */
      char *line;

      if ( singleSlashCmd ) {
 	/* Not really a query, but "Do what I mean, not what I say." */
 	line = singleQuery;
      }
      else {
 	line = malloc(strlen(qfilename) + 5);
 	sprintf(line,"\\i %s", qfilename);
      }
      HandleSlashCmds(&settings, line, "");
      
   } else {
       if (singleQuery) {
	   exitStatus = SendQuery(&settings, singleQuery);
       }
       else 
	   exitStatus = MainLoop(&settings, stdin);
   }

  PQfinish(settings.db);

  return exitStatus;
}

#define COPYBUFSIZ	8192

static void
handleCopyOut(PGresult *res, bool quiet)
{
    bool copydone = false;
    char copybuf[COPYBUFSIZ];
    int ret;

    if (!quiet)
	fprintf(stdout, "Copy command returns...\n");
    
    while (!copydone) {
	ret = PQgetline(res->conn, copybuf, COPYBUFSIZ);
	
	if (copybuf[0] == '.' && copybuf[1] =='\0') {
	    copydone = true;	/* don't print this... */
	} else {
	    fputs(copybuf, stdout);
	    switch (ret) {
	    case EOF:
		copydone = true;
		/*FALLTHROUGH*/
	    case 0:
		fputc('\n', stdout);
		break;
	    case 1:
		break;
	    }
	}
    }
    fflush(stdout);
    PQendcopy(res->conn);
}


static void
handleCopyIn(PGresult *res, bool quiet)
{
    bool copydone = false;
    bool firstload;
    bool linedone;
    char copybuf[COPYBUFSIZ];
    char *s;
    int buflen;
    int c;
    
    if (!quiet) {
	fputs("Enter info followed by a newline\n", stdout);
	fputs("End with a dot on a line by itself.\n", stdout);
    }
    
    /*
     * eat extra newline still in input buffer
     *
     */
    fflush(stdin);
    if ((c = getc(stdin)) != '\n' && c != EOF) {
	(void) ungetc(c, stdin);
    }
    
    while (!copydone) {			/* for each input line ... */
	if (!quiet) {
	    fputs(">> ", stdout);
	    fflush(stdout);
	}
	firstload = true;
	linedone = false;
	while (!linedone) {		/* for each buffer ... */
	    s = copybuf;
	    buflen = COPYBUFSIZ;
	    for (; buflen > 1 &&
		 !(linedone = (c = getc(stdin)) == '\n' || c == EOF);
		 --buflen) {
		*s++ = c;
	    }
	    if (c == EOF) {
		/* reading from stdin, but from a file */
		PQputline(res->conn, ".");
		copydone = true;
		break;
	    }
	    *s = '\0';
	    PQputline(res->conn, copybuf);
	    if (firstload) {
		if (!strcmp(copybuf, ".")) {
		    copydone = true;
		}
		firstload = false;
	    }
	}
	PQputline(res->conn, "\n");
    }
    PQendcopy(res->conn);
}

/* try to open fname and return a FILE *,
   if it fails, use stdout, instead */

FILE *
setFout(PsqlSettings *ps, char *fname)
{
        if (ps->queryFout && ps->queryFout != stdout)
	{
		if (ps->pipe)
			pclose(ps->queryFout);
		else
			fclose(ps->queryFout);
	}
	if (!fname)
		ps->queryFout = stdout;
	else
	{
		if (*fname == '|')
		{
	                signal(SIGPIPE, SIG_IGN);
			ps->queryFout = popen(fname+1, "w");
			ps->pipe = 1;
		}
		else
		{
			ps->queryFout = fopen(fname, "w");
			ps->pipe = 0;
		}
		if (!ps->queryFout) {
		    perror(fname);
		    ps->queryFout = stdout;
		}
	}
    return ps->queryFout;
}
