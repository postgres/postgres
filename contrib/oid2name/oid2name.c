/* 
  oid2name; a postgresql 7.1 (+?) app to map OIDs on the filesystem
   to table and database names.  

  b. palmer, bpalmer@crimelabs.net 1-17-2001

 */

#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "libpq-fe.h"

/* these are the opts structures for command line params */
struct options {
  int getdatabase;
  int gettable;
  int getoid;
  
  int systables;

  int remotehost;
  int remoteport;
  int remoteuser;
  int remotepass;

  int _oid;
  char _dbname[128];
  char _tbname[128];

  char _hostname[128];
  char _port[6];
  char _username[128];
  char _password[128];
};

/* function prototypes */
void get_opts(int, char **, struct options *);
PGconn *sql_conn(char *, struct options *);
void sql_exec_error (int);
int sql_exec(PGconn *, char *, int);
void sql_exec_dumpdb(PGconn *);
void sql_exec_dumptable(PGconn *, int);
void sql_exec_searchtable(PGconn *, char *);
void sql_exec_searchoid(PGconn *, int);

/* fuction to parse command line options and check for some usage errors. */
void get_opts(int argc, char **argv, struct options *my_opts)
{
  char c;

  /* set the defaults */
  my_opts->getdatabase = 0;
  my_opts->gettable = 0;
  my_opts->getoid = 0;

  my_opts->systables = 0;

  my_opts->remotehost = 0;
  my_opts->remoteport = 0;
  my_opts->remoteuser = 0;
  my_opts->remotepass = 0;

  /* get opts */
  while( (c = getopt(argc, argv, "H:p:U:P:d:t:o:xh?")) != EOF)
    {
      switch(c)
	{
	  /* specify the database */
	case 'd':
	  my_opts->getdatabase = 1;
	  sscanf(optarg, "%s", my_opts->_dbname);
	  break;

	  /* specify the table name */
	case 't':
	  /* make sure we set the database first */
	  if(!my_opts->getdatabase)
	    {
	      fprintf(stderr, "Sorry,  but you must specify a database to dump from.\n");
	      exit(1);
	    }
	  /* make sure we don't try to do a -o also */
	  if(my_opts->getoid)
	    {
	      fprintf(stderr, "Sorry, you can only specify either oid or table\n");
	      exit(1);
	    }

	  my_opts->gettable = 1;
	  sscanf(optarg, "%s", my_opts->_tbname);

	  break;

	  /* specify the oid int */
	case 'o':
	  /* make sure we set the database first */
	  if(!my_opts->getdatabase)
	    {
	      fprintf(stderr, "Sorry,  but you must specify a database to dump from.\n");
	      exit(1);
	    }
	  /* make sure we don't try to do a -t also */
	  if(my_opts->gettable)
	    {
	      fprintf(stderr, "Sorry, you can only specify either oid or table\n");
	      exit(1);
	    }

	  my_opts->getoid = 1;
	  sscanf(optarg, "%i", &my_opts->_oid);

	  break;

	  /* host to connect to */
	case 'H':
	  my_opts->remotehost = 1;
	  sscanf(optarg, "%s", my_opts->_hostname);
	  break;

	  /* port to connect to on remote host */
	case 'p':
	  my_opts->remoteport = 1;
	  sscanf(optarg, "%s", my_opts->_port);
	  break;

	  /* username */
	case 'U':
	  my_opts->remoteuser = 1;
	  sscanf(optarg, "%s", my_opts->_username);
	  break;

	  /* password */
	case 'P':
	  my_opts->remotepass = 1;
	  sscanf(optarg, "%s", my_opts->_password);
	  break;

	  /* display system tables */
	case 'x':

	  my_opts->systables = 1;
	  break;

	  /* help! (ugly in code for easier editing) */
	case '?':
	case 'h':
	  fprintf(stderr, "\n\
Usage: pg_oid2name [-d database [-x] ] [-t table | -o oid] \n\
        dafault action        display all databases
        -d database           database to oid2name\n\
        -x                    display system tables\n\
        -t table | -o oid     search for table name (-t) or\n\
                               oid (-o) in -d database
        -H host               connect to remote host\n\
        -p port               host port to connect to\n\
        -U username           username to connect with\n\
        -P password           password for username\n\n\
");
	  exit(1);
	  break;
	}
    }
}

/* establish connection with database. */
PGconn *sql_conn(char *dbName, struct options *my_opts)
{
  char *pghost, *pgport;
  char *pgoptions, *pgtty;
  char *pguser, *pgpass;

  PGconn *conn;

  pghost = NULL;
  pgport = NULL;
  
  pgoptions = NULL;           /* special options to start up the backend
                                 * server */
  pgtty = NULL;               /* debugging tty for the backend server */

  pguser = NULL;
  pgpass = NULL;

  /* override the NULLs with the user params if passed */
  if(my_opts->remotehost)
    {
      pghost = (char *) malloc (128);
      sscanf(my_opts->_hostname, "%s", pghost);
    }
    
  if(my_opts->remoteport)
    {
      pgport = (char *) malloc (6);
      sscanf(my_opts->_port, "%s", pgport);
    }

  if(my_opts->remoteuser)
    {
      pguser = (char *) malloc (128);
      sscanf(my_opts->_username, "%s", pguser);
    }

  if(my_opts->remotepass)
    {
      pgpass = (char *) malloc (128);
      sscanf(my_opts->_password, "%s", pgpass);
    }

  /* login */
  conn = PQsetdbLogin(pghost, pgport, pgoptions, pgtty, dbName, pguser, pgpass);

  /* deal with errors */
  if (PQstatus(conn) == CONNECTION_BAD)
    {
      fprintf(stderr, "Connection to database '%s' failed.\n", dbName);
      fprintf(stderr, "%s", PQerrorMessage(conn));
      
      
      PQfinish(conn);
      exit(1);

    }

  /* return the conn if good */
  return conn;
}

/* If the sql_ command has an error,  this function looks up the error number and prints it out. */
void sql_exec_error (int error_number)
{
  fprintf(stderr, "Error number %i.\n", error_number);
  switch(error_number)
    {
    case 3:
      fprintf(stderr,  "Error:  PGRES_COPY_OUT\n");
      break;
      
    case 4:
      fprintf(stderr,  "Error:  PGRES_COPY_IN\n");
      break;
      
    case 5:
      fprintf(stderr,  "Error:  PGRES_BAD_RESPONCE\n");
      break;
      
    case 6:
      fprintf(stderr,  "Error:  PGRES_NONFATAL_ERROR\n");
      break;
      
    case 7:
      fprintf(stderr,  "Error:  PGRES_FATAL_ERROR\n");
      break;
    }
}

/* actual code to make call to the database and print the output data */
int sql_exec(PGconn *conn, char *todo, int match)
{
  PGresult *res;

  int numbfields;
  int error_number;
  int i, len;

  /* make the call */
  res = PQexec(conn, todo);

  /* check and deal with errors */
  if (!res || PQresultStatus(res) > 2)
    {
      error_number = PQresultStatus(res);
      fprintf(stderr, "There was an error in the SQL command:\n%s\n", todo);
      sql_exec_error(error_number);
      fprintf(stderr,  "PQerrorMessage = %s\n", PQerrorMessage(conn));
	    
      PQclear(res);
      PQfinish(conn);
      exit(-1);
    }

  /* get the number of fields */
  numbfields = PQntuples(res);

  /* if we only expect 1 and there mode than,  return -2 */
  if(match == 1 && numbfields > 1)
    return -2;

  /* return -1 if there aren't any returns */
  if(match == 1 && numbfields < 1)
    return -1;

  /* for each row,  dump the information */
  for(i = 0; i < numbfields; i++)
    {
      len = strlen(PQgetvalue(res, i, 0));

      fprintf(stdout, "%-6s = %s\n", PQgetvalue(res, i, 0), PQgetvalue(res, i, 1));
    }

  /* clean the PGconn once done */
  PQclear(res);

  return 0;
}

/* dump all databases know by the system table */
void sql_exec_dumpdb(PGconn *conn)
{
  char *todo;

  todo = (char *) malloc (1024);

  /* get the oid and database name from the system pg_database table */
  sprintf(todo, "select oid,datname from pg_database");

  sql_exec(conn, todo, NULL);
}

/* display all tables in whatever db we are connected to.  don't display the
   system tables by default */
void sql_exec_dumptable(PGconn *conn, int systables)
{
  char *todo;

  todo = (char *) malloc (1024);

  /* don't exclude the systables if this is set */
  if(systables == 1)
    sprintf(todo, "select relfilenode,relname from pg_class order by relname");
  else
    sprintf(todo, "select relfilenode,relname from pg_class where relname not like 'pg_%%' order by relname");

  sql_exec(conn, todo, NULL);
}

/* display the oid for a given tablename for whatever db we are connected
   to.  do we want to allow %bar% in the search?  Not now. */
void sql_exec_searchtable(PGconn *conn, char *tablename)
{
  int returnvalue;
  char *todo;

  todo = (char *) malloc (1024);

  /* get the oid and tablename where the name matches tablename */
  sprintf(todo, "select relfilenode,relname from pg_class where relname = '%s'", tablename);

  returnvalue = sql_exec(conn, todo, 1);

  /* deal with the return errors */
  if(returnvalue == -1)
    {
      printf("No tables with that name found\n");
    }
  
  if(returnvalue == -2)
    {
      printf("VERY scary:  more than one table with that name found!!\n");
    }
}

/* same as above */
void sql_exec_searchoid(PGconn *conn, int oid)
{
  int returnvalue;
  char *todo;

  todo = (char *) malloc (1024);

  sprintf(todo, "select relfilenode,relname from pg_class where oid = %i", oid);

  returnvalue = sql_exec(conn, todo, 1);

  if(returnvalue == -1)
    {
      printf("No tables with that oid found\n");
    }
  
  if(returnvalue == -2)
    {
      printf("VERY scary:  more than one table with that oid found!!\n");
    }
}

int main(int argc, char **argv)
{
  struct options *my_opts;
  PGconn *pgconn;

  my_opts = (struct options *) malloc (sizeof(struct options));

  /* parse the opts */
  get_opts(argc, argv, my_opts);

  /* display all the tables in the database */
  if(my_opts->getdatabase & my_opts->gettable)
    {
      printf("Oid of table %s from database \"%s\":\n", my_opts->_tbname, my_opts->_dbname);
      printf("_______________________________\n");

      pgconn = sql_conn(my_opts->_dbname, my_opts);
      sql_exec_searchtable(pgconn, my_opts->_tbname);
      PQfinish(pgconn);

      exit(1);
    }
  
  /* search for the tablename of the given OID */
  if(my_opts->getdatabase & my_opts->getoid)
    {
      printf("Tablename of oid %i from database \"%s\":\n", my_opts->_oid, my_opts->_dbname);
      printf("---------------------------------\n");

      pgconn = sql_conn(my_opts->_dbname, my_opts);
      sql_exec_searchoid(pgconn, my_opts->_oid);
      PQfinish(pgconn);

      exit(1);
    }

  /* search for the oid for the given tablename */
  if(my_opts->getdatabase)
    {
      printf("All tables from database \"%s\":\n", my_opts->_dbname);
      printf("---------------------------------\n");

      pgconn = sql_conn(my_opts->_dbname, my_opts);
      sql_exec_dumptable(pgconn, my_opts->systables);
      PQfinish(pgconn);

      exit(1);
    }

  /* display all the databases for the server we are connected to.. */
  printf("All databases:\n");
  printf("---------------------------------\n");  
  
  pgconn = sql_conn("template1", my_opts);
  sql_exec_dumpdb(pgconn);
  PQfinish(pgconn);

  exit(0);
}
