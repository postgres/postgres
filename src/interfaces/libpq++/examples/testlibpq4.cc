/*
 * testlibpq4.cc
 * 	Test of the asynchronous notification interface
 *
   populate a test database with the following (use testlibpq4.sql):

CREATE TABLE TBL1 (i int4);

CREATE TABLE TBL2 (i int4);

CREATE RULE r1 AS ON INSERT TO TBL1 DO [INSERT INTO TBL2 values (new.i); NOTIFY TBL2];

 * Then start up this program
 * After the program has begun, do

INSERT INTO TBL1 values (10);

 *
 *
 */
#include <iostream.h>
#include "libpq++.h"
#include <stdlib.h>

int main()
{
  // Begin, by connecting to the backend using hardwired constants
  // and a test database created by the user prior to the invokation
  // of this test program.
  const char* dbName = "dbname=template1";
  PgDatabase data(dbName);

  // Check to see that the backend connection was successfully made
  if ( data.ConnectionBad() ) {
    cerr << "Connection to database '" << dbName << "' failed." << endl
         << data.ErrorMessage() << endl;
    exit(1);
  }

  // Listen to a table
  if ( !data.ExecCommandOk("LISTEN TBL2") ) {
    cerr << "LISTEN command failed" << endl;
    exit(1);
  }

  // Test asynchronous notification
  while (1) {
      // check for asynchronous returns
      PGnotify* notify = data.Notifies();
      if (notify) {
	  cerr << "ASYNC NOTIFY of '" << notify->relname 
	       << "' from backend pid '" << notify->be_pid 
	       << "' received" << endl;
	  free(notify);
	  break;
      }
  }
  return 0;
}
