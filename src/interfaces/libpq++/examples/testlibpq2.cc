/*
 * testlibpq2.cc
 * 	Test of the asynchronous notification interface
 *
   populate a database with the following:

CREATE TABLE TBL1 (i int4);

CREATE TABLE TBL2 (i int4);

CREATE RULE r1 AS ON INSERT TO TBL1 DO [INSERT INTO TBL2 values (new.i); NOTIFY TBL2];

 * Then start up this program
 * After the program has begun, do

INSERT INTO TBL1 values (10);

 *
 *
 */
#include <stdio.h>
#include "libpq++.H"

main()
{
  char* dbName;

  /* begin, by creating the parameter environtment for a backend
     connection. When no parameters are given then the system will
     try to use reasonable defaults by looking up environment variables 
     or, failing that, using hardwired constants */
  PGenv env;
  PGdatabase* data;
  PGnotify* notify;

  dbName = getenv("USER"); /* change this to the name of your test database */

  /* make a connection to the database */
  data = new PGdatabase(&env, dbName);

  /* check to see that the backend connection was successfully made */
  if (data->status() == CONNECTION_BAD) {
    fprintf(stderr,"Connection to database '%s' failed.\n", dbName);
    fprintf(stderr,"%s",data->errormessage());
    delete data;
    exit(1);
  }

  if (data->exec("LISTEN TBL2") != PGRES_COMMAND_OK) {
    fprintf(stderr,"LISTEN command failed\n");
    delete data;
    exit(1);
  }

  while (1) {
      /* check for asynchronous returns */
      notify = data->notifies();
      if (notify) {
	  fprintf(stderr,
		  "ASYNC NOTIFY of '%s' from backend pid '%d' received\n",
		  notify->relname, notify->be_pid);
	  free(notify);
	  break;
      }
  }
      
  /* close the connection to the database and cleanup */
  delete data;
}
