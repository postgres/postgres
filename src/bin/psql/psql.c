/*-------------------------------------------------------------------------
 *
 * psql.c--
 *    an interactive front-end to postgres95
 *
 * Copyright (c) 1996, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/bin/psql/Attic/psql.c,v 1.3 1996/07/16 06:58:12 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
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
#include <history.h>
#endif
#endif

#define MAX_QUERY_BUFFER 20000
#define MAX_FIELD_SEP_LENGTH 40

#define COPYBUFSIZ	8192

#define DEFAULT_FIELD_SEP " "
#define DEFAULT_EDITOR  "vi"
#define DEFAULT_SHELL  "/bin/sh"

typedef struct _psqlSettings {
    int echoQuery;        /* if 1, echo the query before sending it */
    int quiet;            /* run quietly, no messages, no promt */
    int singleStep;       /* if 1, prompt for each query */ 
    int singleLineMode;   /* if 1, query terminated by newline */
    int useReadline;      /* use the readline routines or not */
    int printHeader;      /* print output field headers or not */
    int fillAlign;        /* fill align the fields */
    FILE *queryFout;      /* where to send the query results */
    char fieldSep[MAX_FIELD_SEP_LENGTH];  /* field separator */
} PsqlSettings;

/* declarations for functions in this file */
static void usage(char* progname);
static void slashUsage();
static void handleCopyOut(PGresult *res, int quiet);
static void handleCopyIn(PGresult *res, int quiet);
static int tableList(PGconn* conn, int deep_tablelist);
static int tableDesc(PGconn* conn, char* table);

char* gets_noreadline(char* prompt, FILE* source);
char* gets_readline(char* prompt, FILE* source);
char* gets_fromFile(char* prompt, FILE* source);
int listAllDbs(PGconn *db, PsqlSettings *settings);
int SendQuery(PGconn* db, char* query, PsqlSettings *settings);
int HandleSlashCmds(PGconn** db_ptr,  
		    char *line,
		    char** prompt_ptr,
		    char *query,
		    PsqlSettings *settings);
int MainLoop(PGconn** db_ptr, FILE *source, PsqlSettings *settings);
FILE* setFout(char *fname);


/*
 * usage 
 *   print out usage for command line arguments 
 */

static void  
usage(char* progname)
{
  fprintf(stderr,"Usage: %s [options] [dbname]\n",progname);
  fprintf(stderr,"\t -a authsvc              set authentication service\n"); 
  fprintf(stderr,"\t -A                      turn off fill-justification when printing out attributes\n");
  fprintf(stderr,"\t -c query                run single query (slash commands too)\n");
  fprintf(stderr,"\t -d dbName               specify database name\n");
  fprintf(stderr,"\t -e                      echo the query sent to the backend\n");
  fprintf(stderr,"\t -f filename             use file as a source of queries\n");
  fprintf(stderr,"\t -F sep                  set the field separator (default is " ")\n");
  fprintf(stderr,"\t -h                      help information\n");
  fprintf(stderr,"\t -H host                 set database server host\n");
  fprintf(stderr,"\t -l                      list available databases\n");
  fprintf(stderr,"\t -n                      don't use readline library\n");
  fprintf(stderr,"\t -o filename             send output to filename\n");
  fprintf(stderr,"\t -p port                 set port number\n");
  fprintf(stderr,"\t -q                      run quietly (no messages, no prompts)\n");
  fprintf(stderr,"\t -s                      single step mode (prompts for each query)\n");
  fprintf(stderr,"\t -S                      single line mode (i.e. query terminated by newline)\n");
  fprintf(stderr,"\t -T                      turn off printing of attribute names\n");
  exit(1);
}

/*
 * slashUsage
 *    print out usage for the backslash commands 
 */

static void
slashUsage()
{
  fprintf(stderr,"\t \\a           -- toggle fill-justification of display of attributes\n");
  fprintf(stderr,"\t \\d [<table>] -- list tables in database or columns in <table>\n");
  fprintf(stderr,"\t \\d *         -- list tables in database and columns in all tables\n");
  fprintf(stderr,"\t \\e [<fname>] -- edit the current query buffer or <fname>\n");
  fprintf(stderr,"\t \\f <sep>     -- change field separator\n");
  fprintf(stderr,"\t \\g           -- query to backend\n");
  fprintf(stderr,"\t \\h <command> -- help on syntax of sql commands\n");
  fprintf(stderr,"\t \\h *         -- complete description of all sql commands\n");
  fprintf(stderr,"\t \\g           -- send query to backend\n");
  fprintf(stderr,"\t \\i <fname>   -- read queries from filename\n");
  fprintf(stderr,"\t \\l           -- list all databases\n");
  fprintf(stderr,"\t \\o [<fname>] -- send query results file named <fname> or stdout\n");
  fprintf(stderr,"\t \\p           -- print the current query buffer\n");
  fprintf(stderr,"\t \\q           -- quit\n");
  fprintf(stderr,"\t \\s [<fname>] -- save or print history\n");
  fprintf(stderr,"\t \\t           -- toggle output field headers (defaults to on)\n");
  fprintf(stderr,"\t \\! [<cmd>]   -- shell escape\n");
  fprintf(stderr,"\t \\?           -- help\n");
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
listAllDbs(PGconn *db, PsqlSettings *settings)
{
  PGresult *results;
  char* query = "select * from pg_database;";

  results = PQexec(db, query);
  if (results == NULL) {
    fprintf(stderr,"%s", PQerrorMessage(db));
    return 1;
  }

  if (PQresultStatus(results) != PGRES_TUPLES_OK)
    {
      fprintf(stderr,"Unexpected error from executing: %s\n", query);
      return 2;
    }
  else
    {
      PQdisplayTuples(results,
		      settings->queryFout, 
		      settings->fillAlign,
		      settings->fieldSep,
		      settings->printHeader,
		      settings->quiet);
      PQclear(results);
      return 0;
    }
}

/*
 * tableList (PGconn* conn)
 *
 * List The Database Tables
 *     returns 0 if all went well
 *    
 */
int
tableList (PGconn* conn, int deep_tablelist)
{
  char listbuf[256];
  int nColumns; 
  int i;
  char* ru;
  char* rk;
  char* rr;

  PGresult* res;

  listbuf[0] = '\0';
  strcat(listbuf,"SELECT usename, relname, relkind, relhasrules");
  strcat(listbuf,"  FROM pg_class, pg_user ");
  strcat(listbuf,"WHERE ( relkind = 'r' OR relkind = 'i') ");
  strcat(listbuf,"  and relname !~ '^pg_'");
  strcat(listbuf,"  and relname !~ '^Inv'");
/* the usesysid = relowner won't work on stock 1.0 dbs, need to 
   add in the int4oideq function */
  strcat(listbuf,"  and usesysid = relowner");
  strcat(listbuf,"  ORDER BY relname ");
  res = PQexec(conn,listbuf);
  if (res == NULL) {
      fprintf(stderr,"%s", PQerrorMessage(conn));
      return (-1);
  }

  if ((PQresultStatus(res) != PGRES_TUPLES_OK) || (PQntuples(res) <= 0)) {
      fprintf(stderr,"No tables found in database %s.\n", PQdb(conn));
      PQclear(res);
      return (-1);
  }

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
	  
	  /* load table table*/
	  for (i=0; i < nColumns; i++) {
	      table[i] = (char *) malloc(PQgetlength(res,i,1) * sizeof(char) + 1);
	      if ( table[i] == NULL )
		  perror("malloc");
	      strcpy(table[i],PQgetvalue(res,i,1));
	  }

 	PQclear(res);
 	for (i=0; i < nColumns; i++) {
 	   tableDesc(conn,table[i]);
 	}
 	free(table);
      }
      else {
 	/* Display the information */

 	printf ("\nDatabase    = %s\n", PQdb(conn));
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
 * Describe a table   (PGconn* conn, char* table)
 *
 * Describe the columns in a database table.
 *     returns 0 if all went well
 *    
 *
 */
int
tableDesc (PGconn* conn, char* table)
{
  char descbuf[256];
  int nColumns;
  char *rtype;
  int i;
  int rsize;

  PGresult* res;

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
  res = PQexec(conn,descbuf);
  if (res == NULL) {
    fprintf(stderr,"%s", PQerrorMessage(conn));
    return (-1);
   }
 if ((PQresultStatus(res) != PGRES_TUPLES_OK) || (PQntuples(res) <= 0)) {
   fprintf(stderr,"Couldn't find table %s!\n", table);
   PQclear(res);
   return (-1);
 }
  /* first, print out the attribute names */
  nColumns = PQntuples(res);
  if (nColumns > 0)
  {
    /*
    ** Display the information
    */

    printf ("\nTable    = %s\n", table);
    printf ("+----------------------------------+----------------------------------+--------+\n");
    printf ("|              Field               |              Type                | Length |\n");
    printf ("+----------------------------------+----------------------------------+--------+\n");

    /* next, print out the instances */
    for (i=0; i < PQntuples(res); i++) {
      printf ("| %-32.32s | ", PQgetvalue(res,i,1));
      rtype = PQgetvalue(res,i,2);
      rsize = atoi(PQgetvalue(res,i,3));
      if (strcmp(rtype, "text") == 0) {
        printf ("%-32.32s |", rtype);
        printf (" %-6s |",  "var" );
      }
      else if (strcmp(rtype, "bpchar") == 0) {
        printf ("%-32.32s |", "char");
        printf (" %-6i |", rsize > 0 ? rsize - 4 : 0 );
      }
      else if (strcmp(rtype, "varchar") == 0) {
        printf ("%-32.32s |", rtype);
        printf (" %-6i |", rsize > 0 ? rsize - 4 : 0 );
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
	    printf (" %-6i |", rsize);
	else
	    printf (" %-6s |", "var");
      }
      printf("\n");
    }
    printf ("+----------------------------------+----------------------------------+--------+\n");

    PQclear(res);
    return (0);
  
  } else {
      fprintf (stderr, "Couldn't find table %s!\n", table);
    return (-1);
  }
}

typedef char* (*READ_ROUTINE)(char* prompt, FILE* source);

/* gets_noreadline  prompt source
      gets a line of input without calling readline, the source is ignored
*/
char* 
gets_noreadline(char* prompt, FILE* source)
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
char* 
gets_readline(char* prompt, FILE* source)
{
  return (readline(prompt));
}


/*
 * gets_fromFile  prompt source
 *    
 * the routine to read from a file, the prompt argument is ignored
 * the source argument is a FILE* 
 */
char* 
gets_fromFile(char* prompt, FILE* source)
{
  char* line;
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
 * SendQuery:
     SendQuery: send the query string to the backend 
 *
 *  return 0 if the query executed successfully
 *  returns 1 otherwise
 */
int
SendQuery(PGconn* db, char* query, PsqlSettings *settings)
{
  PGresult* results;
  PGnotify* notify;
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

  results = PQexec(db, query);
  if (results == NULL) {
    fprintf(stderr,"%s",PQerrorMessage(db));
    return 1;
  }

  switch (PQresultStatus(results)) {
  case PGRES_TUPLES_OK:
      PQdisplayTuples(results,
		      settings->queryFout,
		      settings->fillAlign,
		      settings->fieldSep,
		      settings->printHeader,
		      settings->quiet);
      PQclear(results);
      break;
  case PGRES_EMPTY_QUERY:
    /* do nothing */
    break;
  case PGRES_COMMAND_OK:
    if (!settings->quiet)
      fprintf(stdout,"%s\n",PQcmdStatus(results));
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
    fprintf(stderr,"%s",PQerrorMessage(db));
    break;

  } 

  /* check for asynchronous returns */
  notify = PQnotifies(db);
  if (notify) {
      fprintf(stderr,"ASYNC NOTIFY of '%s' from backend pid '%d' received\n",
	      notify->relname, notify->be_pid);
      free(notify);
  }

  return status;

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
HandleSlashCmds(PGconn** db_ptr, 
		char* line, 
		char** prompt_ptr, 
		char *query,
		PsqlSettings *settings)
{
  int status = 0;
  PGconn* db = *db_ptr;
  char* dbname = PQdb(db);
  char *optarg = NULL;
  int len;

  len = strlen(line);
  if (len > 2)
      optarg = leftTrim(line+2);
  switch (line[1])
    {
    case 'a': /* toggles to fill fields on output */
	if (settings->fillAlign)
	    settings->fillAlign = 0;
	else 
	    settings->fillAlign = 1;
	if (!settings->quiet)
	    fprintf(stderr,"turning %s fill-justification\n",
		    (settings->fillAlign) ? "on" : "off" );
	break;
    case 'c':  /* \c means connect to new database */
      {
	  if (!optarg) {
	      fprintf(stderr,"\\c must be followed by a database name\n");
	      status = 1;
	      break;
	  }
	  if (strcmp(optarg, dbname) == 0)  {
	      fprintf(stderr,"already connected to %s\n", dbname);
	      status = 1;
	      break;
	  }
	  else {
	      PGconn *olddb;
	      
	      printf("closing connection to database:%s\n", dbname);
	      olddb = db;
	      db = PQsetdb(PQhost(olddb), PQport(olddb), NULL, NULL, optarg);
	      *db_ptr = db;
	      printf("connecting to new database: %s\n", optarg);
	      if (PQstatus(db) == CONNECTION_BAD) {
		  fprintf(stderr,"%s\n", PQerrorMessage(db));
		  printf("reconnecting to %s\n", dbname);
		  db = PQsetdb(PQhost(olddb), PQport(olddb), 
			       NULL, NULL, dbname);
		  *db_ptr = db;
		  if (PQstatus(db) == CONNECTION_BAD) {
		      fprintf(stderr, 
			      "could not reconnect to %s.  exiting\n", dbname);
		      exit(2);
		  }
		  status = 1;
		  break;
	      }
	      PQfinish(olddb);
	      free(*prompt_ptr);
	      *prompt_ptr = malloc(strlen(optarg) + 10);
	      sprintf(*prompt_ptr,"%s=> ", optarg);
	      status = 1;
	    break;
	  }
      }
      break;
    case 'd':     /* \d describe tables or columns in a table */
      {
	if (!optarg) {
          tableList(db,0);
	  status = 1;
	  break;
	}
 	if ( strcmp(optarg,"*") == 0 ) {
 	   tableList(db, 0);
 	   tableList(db, 1);
 	}
 	else {
 	   tableDesc(db,optarg);
 	}
 	status = 1;
 	break;
      }
    case 'e':
      {
	char s[256];
	int fd;
	int ql = strlen(query);
	int f_arg = 0;
	int cc;
        if (optarg)
        {
        	f_arg = 1;
        	strcpy(s, optarg);
        }
        else
        {
		sprintf(s, "/tmp/psql.%d.%d", getuid(), getpid());
		unlink(s);
		if (ql)
		{
			if ((fd=open(s, O_EXCL|O_CREAT|O_WRONLY, 0600))==-1)
			{
				perror(s);
				break;
			}
			if (query[ql-1]!='\n')
				strcat(query, "\n");
			if (write(fd, query, ql)!=ql)
			{
				perror(s);
				close(fd);
				unlink(s);
				break;
			}
			close(fd);
		}
	}
	{
	    char sys[256];
	    char *editorName;
	    editorName = getenv("EDITOR");
	    if (editorName == NULL)
		editorName = DEFAULT_EDITOR;
	    sprintf(sys, "exec %s %s", editorName, s);
	    system(sys);
        }
	if ((fd=open(s, O_RDONLY))==-1)
	{
		if (!f_arg)
			unlink(s);
		break;
	}
	if ((cc=read(fd, query, MAX_QUERY_BUFFER))==-1)
        {
		perror(s);
		close(fd);
		if (!f_arg)
			unlink(s);
		break;
	}	
	query[cc]='\0';
	close(fd);
	if (!f_arg)
		unlink(s);
	rightTrim(query);
	if (query[strlen(query)-1]==';')
		return 0;
	break;
      }
  case 'f':
      if (optarg)
	  strcpy(settings->fieldSep,optarg);
      else
          strcpy(settings->fieldSep,DEFAULT_FIELD_SEP);
      break;
  case 'g':  /* \g means send query */
      status = 0;     
	break;
    case 'i':     /* \i is include file */
      {
	FILE* fd;

	if (!optarg) {
	  fprintf(stderr,"\\i must be followed by a file name\n");
	  status = 1;
	  break;
	}

	if ( (fd = fopen(optarg, "r")) == NULL)
	  {
	    fprintf(stderr,"file named %s could not be opened\n",optarg);
	    status = 1;
	    break;
	  }
	MainLoop(&db, fd, settings);
	fclose(fd);
	status = 1;
	break;
      }
    case 'h':
      {
	char* cmd;
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

 	  if ( strcmp(cmd,"*") == 0 ) {
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
	status = 1;
	break;
      }
    case 'l':     /* \l is list database */
      listAllDbs(db,settings);
      status = 1;
      break;
    case 'o':
      settings->queryFout = setFout(optarg);
      break;
    case 'p':
	if (query) {
	    fputs(query, stdout);
	    fputc('\n', stdout);
	    status = 1;
	}
	break;
    case 'r':
        query[0] = '\0';
        status = 1;
        break;          
    case 'q': /* \q is quit */
      status = 2;
      break;
    case 's': /* \s is save history to a file */
      {
	char* fname;

	if (!optarg) {
	  fprintf(stderr,"\\s must be followed by a file name\n");
	  status = 1;
	  break;
	}

	fname = optarg;
	if (write_history(fname) != 0)
	  {
	    fprintf(stderr,"cannot write history to %s\n",fname);
	  }
	status = 1;
	break;
      }
    case 't':
	if ( settings->printHeader )
	     settings->printHeader = 0;
	else
	     settings->printHeader = 1;
	if (!settings->quiet)
	    fprintf(stderr,"turning %s printing of field headers\n",
		    (settings->printHeader) ? "on" : "off" );
	break;
    case '!':
      if (!optarg) {
	  char sys[256];
	  char *shellName;
	  shellName = getenv("SHELL");
	  if (shellName == NULL) 
	      shellName = DEFAULT_SHELL;
	  sprintf(sys,"exec %s", shellName);
	  system(sys);
      }
      else
	  system(optarg);
      break;
    default:
    case '?':     /* \? is help */
      slashUsage();
      status = 1;
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
MainLoop(PGconn** db_ptr, 
	 FILE* source,
	 PsqlSettings *settings)
{
  char* prompt;                 /* readline prompt */
  char* line;                   /* line of input*/
  int len;                      /* length of the line */
  char query[MAX_QUERY_BUFFER]; /* multi-line query storage */
  PGconn* db = *db_ptr;
  char* dbname = PQdb(db);
  int exitStatus = 0;

  int slashCmdStatus = 0;
 /* slashCmdStatus can be:
       0 - send currently constructed query to backend (i.e. we got a \g)
       1 - skip processing of this line, continue building up query
       2 - terminate processing of this query entirely
  */

  int send_query = 0;
  int interactive;
  READ_ROUTINE GetNextLine;

  interactive = (source == stdin);

  if (interactive) {
    prompt = malloc(strlen(dbname) + 10);
    if (settings->quiet)
      prompt[0] = '\0';
    else
      sprintf(prompt,"%s=> ", dbname);
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
  while ((line = GetNextLine(prompt, source)) != NULL)
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
	 -- in your quoted string, be careful and don't start a line with it*/
      if (line[0] == '-' && line[1] == '-') {
	  if (settings->singleStep) /* in single step mode, show comments */
	      fprintf(stdout,"%s\n",line);
	  free(line);
	  continue;
      }

      len = strlen(line);

      if (interactive && settings->useReadline)
	  add_history(line);      /* save non-empty lines in history */
      
      /* do the query immediately if we are doing single line queries 
       or if the last character is a semicolon */
      send_query = settings->singleLineMode || (line[len-1] == ';') ;

      /* normally, \ commands have to be start the line,
	 but for backwards compatibility with monitor,
	 check for \g at the end of line */
      if (len > 2 && !send_query) 
	{
	  if (line[len-1]=='g' && line[len-2]=='\\')
	    {
	    send_query = 1;
	    line[len-2]='\0';
	  }
	}
      
      /* slash commands have to be on their own line */
      if (line[0] == '\\') {
	  slashCmdStatus = HandleSlashCmds(db_ptr, 
					   line, 
					   &prompt, 
					   query,
					   settings);
	db = *db_ptr; /* in case \c changed the database */
	if (slashCmdStatus == 1)
	  continue;
	if (slashCmdStatus == 2)
	  break;
	if (slashCmdStatus == 0)
	  send_query = 1;
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
      
      if (send_query && query[0] != '\0')
	{
	    /* echo the line read from the file,
	     unless we are in single_step mode, because single_step mode
	     will echo anyway */
	  if (!interactive && !settings->singleStep) 
	    fprintf(stderr,"%s\n",query);

	  exitStatus = SendQuery(db, query, settings);
	  query[0] = '\0';
	}
      
       free(line); /* free storage malloc'd by GetNextLine */
    } /* while */
  return exitStatus;
} 

int
main(int argc, char** argv)
{
  extern char* optarg;
  extern int optind, opterr;
  
  PGconn *db;
  char* dbname = NULL;
  char* host = NULL;
  char* port = NULL;
  char* qfilename = NULL;
  char errbuf[ERROR_MSG_LENGTH];

  PsqlSettings settings;

  char* singleQuery = NULL;

  int listDatabases = 0 ;
  int exitStatus = 0;
  int singleSlashCmd = 0;
  int c;


#ifdef NOREADLINE
  settings.useReadline = 0;
#else
  settings.useReadline = 1;
#endif

  settings.quiet = 0;
  settings.fillAlign = 1;
  settings.printHeader = 1;
  settings.echoQuery = 0;
  settings.singleStep = 0;
  settings.singleLineMode = 0;
  settings.queryFout = stdout;
  strcpy(settings.fieldSep, DEFAULT_FIELD_SEP);

  while ((c = getopt(argc, argv, "Aa:c:d:ef:F:lhH:nso:p:qST")) != EOF) {
    switch (c) {
    case 'A':
	settings.fillAlign = 0;
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
	strncpy(settings.fieldSep,optarg,MAX_FIELD_SEP_LENGTH); 
	break;
    case 'l':
      listDatabases = 1;
      break;
    case 'H':
      host = optarg;
      break;
    case 'n':
	settings.useReadline = 0;
	break;
    case 'o':
	settings.queryFout = setFout(optarg);
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
    case 'T':
	settings.printHeader = 0;
	break;
    case 'h':
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
  
  db = PQsetdb(host, port, NULL, NULL, dbname);
  dbname = PQdb(db);

  if (PQstatus(db) == CONNECTION_BAD) {
    fprintf(stderr,"Connection to database '%s' failed.\n", dbname);
    fprintf(stderr,"%s",PQerrorMessage(db));
    exit(1);
  }
  if (listDatabases) {
      exit(listAllDbs(db,&settings));
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
      char prompt[100];

      if ( singleSlashCmd ) {
 	/* Not really a query, but "Do what I mean, not what I say." */
 	line = singleQuery;
      }
      else {
 	line = malloc(strlen(qfilename) + 5);
 	sprintf(line,"\\i %s", qfilename);
      }
      HandleSlashCmds(&db, line, (char**)prompt, "", &settings);
      
   } else {
       if (singleQuery) {
	   exitStatus = SendQuery(db, singleQuery, &settings);
       }
       else 
	   exitStatus = MainLoop(&db, stdin, &settings);
   }

  PQfinish(db);

  return exitStatus;
}


static void
handleCopyOut(PGresult *res, int quiet)
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
handleCopyIn(PGresult *res, int quiet)
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


/* try to open fname and return a FILE*,
   if it fails, use stdout, instead */
FILE* 
setFout(char *fname)
{
    FILE *queryFout;

    if (!fname)
	queryFout = stdout;
    else {
	queryFout = fopen(fname, "w");
	if (!queryFout) {
	    perror(fname);
	    queryFout = stdout;
	}
    }

    return queryFout;
}
 
