/*
 * testlibpq4.cc
 * 	Test the C++ version of LIBPQ, the POSTGRES frontend library.
 * tests the copy in features
 *
 */
#include <stdio.h>
#include "libpq++.H"

#define DEBUG printf("Got here %d\n", __LINE__);
main()
{
  char* dbName;
  int nFields;
  int i,j;

  /* begin, by creating the parameter environment for a backend
     connection. When no parameters are given then the system will
     try to use reasonable defaults by looking up environment variables 
     or, failing that, using hardwired constants */
  PGenv env;
  PGdatabase* data;

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

  /* start a transaction block */
  if(data->exec("BEGIN") != PGRES_COMMAND_OK) {
    fprintf(stderr,"BEGIN command failed\n");
    delete data;
    exit(1);
  }

  if (data->exec("CREATE TABLE foo (a int4, b char16, d float8)") != 
      PGRES_COMMAND_OK) {
      fprintf(stderr,"CREATE TABLE foo command failed\n");
      delete data;
      exit(1);
  }

  if (data->exec("COPY foo FROM STDIN") != PGRES_COMMAND_OK) {
      fprintf(stderr,"COPY foo FROM STDIN\n");
      delete data;
      exit(1);      
  }

  data->putline("3\thello world\t4.5\n");
  data->putline("4\tgoodbye word\t7.11\n");
  data->putline(".\n");
  data->endcopy();
  data->exec("SELECT * FROM foo");
  data->printtuples(stdout,1,"|",1,0);
  data->exec("DROP TABLE foo");
  // end the transaction 
  data->exec("END");

  // close the connection to the database and cleanup 
  delete data;
}
