/*
 * testlibpq.cc
 * 	Test the C++ version of LIBPQ, the POSTGRES frontend library.
 *
 *  queries the template1 database for a list of database names 
 *
 */
#include <stdio.h>
#include "libpq++.H"

main()
{
  char* dbName;
  int nFields;
  int i,j;

  /* begin, by creating the parameter environtment for a backend
     connection. When no parameters are given then the system will
     try to use reasonable defaults by looking up environment variables 
     or, failing that, using hardwired constants */
  PGenv env;
  PGdatabase* data;

  /* Select a database */
  dbName = "template1";

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

  /* fetch instances from the pg_database, the system catalog of databases*/
  if (data->exec("DECLARE myportal CURSOR FOR select * from pg_database")
      != PGRES_COMMAND_OK) {
    fprintf(stderr,"DECLARE CURSOR command failed\n");
    delete data;
    exit(1);
  }

  if(data->exec("FETCH ALL in myportal") != PGRES_TUPLES_OK) {
    fprintf(stderr,"FETCH ALL command didn't return tuples properly\n");
    delete data;
    exit(1);
  }
 
  /* first, print out the attribute names */
  nFields = data->nfields();
  for (i=0; i < nFields; i++) {
    printf("%-15s",data->fieldname(i));
  }
  printf("\n\n");

  /* next, print out the instances */
  for (i=0; i < data->ntuples(); i++) {
    for (j=0  ; j < nFields; j++) {
      printf("%-15s", data->getvalue(i,j));
    }
    printf("\n");
  }

  /* close the portal */
  data->exec("CLOSE myportal");

  /* end the transaction */
  data->exec("END");

  /* close the connection to the database and cleanup */
  delete data;
}
  

