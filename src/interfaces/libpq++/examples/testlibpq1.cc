/*
 * testlibpq1.cc
 * 	Test the C++ version of LIBPQ, the POSTGRES frontend library.
 *
 *  queries the template1 database for a list of database names 
 *
 */

#include <iostream.h>
#include <iomanip.h>
#include <libpq++.h>

int main()
{
  // Begin, by establishing a connection to the backend.
  // When no parameters are given then the system will
  // try to use reasonable defaults by looking up environment variables 
  // or, failing that, using hardwired constants
  const char* dbName = "dbname=template1";
  PgDatabase data(dbName);

  // check to see that the backend connection was successfully made
  if ( data.ConnectionBad() ) {
      cerr << "Connection to database '" << dbName << "' failed." << endl
           << "Error returned: " << data.ErrorMessage() << endl;
      exit(1);
  }

  // start a transaction block
  if ( !data.ExecCommandOk("BEGIN") ) {
    cerr << "BEGIN command failed" << endl;
    exit(1);
  }

  // submit command to the backend
  if ( !data.ExecCommandOk("DECLARE myportal CURSOR FOR select * from pg_database") ) {
    cerr << "DECLARE CURSOR command failed" << endl;
    exit(1);
  }

  // fetch instances from the pg_database, the system catalog of databases
  if ( !data.ExecTuplesOk("FETCH ALL in myportal") ) {
    cerr << "FETCH ALL command didn't return tuples properly" << endl;
    exit(1);
  }
 
  // first, print out the attribute names
  int nFields = data.Fields();
  for (int i=0; i < nFields; i++)
      cout << setiosflags(ios::right) << setw(15) << data.FieldName(i);
  cout << endl << endl;

  // next, print out the instances
  for (int i=0; i < data.Tuples(); i++) {
       for (int j=0; j < nFields; j++)
            cout << setiosflags(ios::right) << setw(15) << data.GetValue(i,j);
       cout << endl;
  }

  // Close the portal
  data.Exec("CLOSE myportal");

  // End the transaction
  data.Exec("END");
  return 0;
}
  

