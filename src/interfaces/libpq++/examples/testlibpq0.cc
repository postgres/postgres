/*-------------------------------------------------------------------------
 *
 * testlibpq0.c--
 *    small test program for libpq++, 
 * small interactive loop where queries can be entered interactively
 * and sent to the backend
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/interfaces/libpq++/examples/Attic/testlibpq0.cc,v 1.3 1997/02/13 10:00:42 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <iostream.h>
#include <libpq++.h>

int main()
{
  // Open the connection to the database and make sure it's OK
  PgDatabase data("template1");
  if ( data.ConnectionBad() ) {
      cout << "Connection was unsuccessful..." << endl
           << "Error message returned: " << data.ErrorMessage() << endl;
      return 1;
  }
  else
      cout << "Connection successful...  Enter queries below:" << endl;
    
  // Interactively obtain and execute queries
  ExecStatusType status;
  string buf;
  int done = 0;
  while (!done)
    {
      cout << "> ";
      cout.flush();
      getline(cin, buf);
      if ( buf != "" )
	       if ( (status = data.Exec( buf.c_str() )) == PGRES_TUPLES_OK ) 
	          data.DisplayTuples();
	       else
	          cout << "No tuples returned..." << endl
	               << "status = " << status << endl
	               << "Error returned: " << data.ErrorMessage() << endl;
      else
	       done = 1;
      }
  return 0;
} // End main()
