/*
 * testlibpq4.cc
 * 	Test the C++ version of LIBPQ, the POSTGRES frontend library.
 * tests the copy in features
 *
 */
#include <iostream.h>
#include <libpq++.H>
#include <stdlib.h>

main()
{
  // Begin, by connecting to the backend using hardwired constants
  // and a test database created by the user prior to the invokation
  // of this test program.  Connect using transaction interface.
  char* dbName = "dbname=template1";
  PgTransaction data(dbName);

  // check to see that the backend connection was successfully made
  if ( data.ConnectionBad() ) {
    cerr << "Connection to database '" << dbName << "' failed." << endl
         << data.ErrorMessage();
    exit(1);
  }
  else cout << "Connected to database '" << dbName << "'..." << endl;

  // Create a new table
  if ( !data.ExecCommandOk("CREATE TABLE foo (a int4, b char16, d float8)") ) {
      cerr << "CREATE TABLE foo command failed" << endl;
      exit(1);
  }
  else cout << "CREATEd TABLE foo successfully.." <<  endl;

  // Initiate Copy command
  if ( data.ExecCommandOk("COPY foo FROM STDIN") ) {
      cerr << "COPY foo FROM STDIN" << endl;
      exit(1);      
  }
  else cout << "COPY foo FROM STDIN was successful.." <<  endl;

  // Put some test data into the table
  data.PutLine("3\thello world\t4.5\n");
  cout << "Line: \"3\thello world\t4.5\" copied..." << endl;
  data.PutLine("4\tgoodbye word\t7.11\n");
  cout << "Line: \"4\tgoodbye word\t7.11\" copied..." << endl;
  data.PutLine("\\.\n");
  cout << "Line: \"\\.\" copied..." << endl;
  if ( !data.EndCopy() )
       cout << "Ended COPY succesfully..." << endl;
  else cerr << "End Copy failed..." << endl;
  
  // Print the data that was inserted into the table
  if ( data.ExecTuplesOk("SELECT * FROM foo") )
       data.PrintTuples();
  else cerr << "SELECT * FROM foo failed..." << endl;
  
  // Drop the test table
  data.Exec("DROP TABLE foo");
}
