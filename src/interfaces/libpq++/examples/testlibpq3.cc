/*
 * testlibpq3.cc
 * 	Test the C++ version of LIBPQ, the POSTGRES frontend library.
 *
 *  queries the template1 database for a list of database names using transaction block
 *  and cursor interface.
 *
 */

#include <iostream.h>
#include <iomanip.h>
#include <libpq++.H>

int main()
{
  // Begin, by establishing a connection to the backend.
  // When no parameters are given then the system will
  // try to use reasonable defaults by looking up environment variables 
  // or, failing that, using hardwired constants.
  // Create a cursor database query object.
  // All queries using cursor will be performed through this object.
  const char* dbName = "dbname=template1";
  PgCursor cData(dbName, "myportal");

  // check to see that the backend connection was successfully made
  if ( cData.ConnectionBad() ) {
      cerr << "Connection to database '" << dbName << "' failed." << endl
           << "Error returned: " << cData.ErrorMessage() << endl;
      exit(1);
  }
  
  // submit command to the backend
  if ( !cData.Declare("select * from pg_database") ) {
    cerr << "DECLARE CURSOR command failed" << endl;
    exit(1);
  }

  // fetch instances from the pg_cDatabase, the system catalog of cDatabases
  if ( !cData.Fetch() ) {
    cerr << "FETCH ALL command didn't return tuples properly" << endl;
    exit(1);
  }
 
  // first, print out the attribute names
  int nFields = cData.Fields();
  for (int i=0; i < nFields; i++)
      cout << setiosflags(ios::right) << setw(15) << cData.FieldName(i);
  cout << endl << endl;

  // next, print out the instances
  for (int i=0; i < cData.Tuples(); i++) {
       for (int j=0; j < nFields; j++)
            cout << setiosflags(ios::right) << setw(15) << cData.GetValue(i,j);
       cout << endl;
  }
}
