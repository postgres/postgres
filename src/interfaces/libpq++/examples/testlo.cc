/*-------------------------------------------------------------------------
 *
 * lotest.cc--
 *    test using large objects with libpq
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/interfaces/libpq++/examples/Attic/testlo.cc,v 1.8 2000/05/29 21:25:04 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <iostream.h>
#include "libpq++.h"
#include <stdlib.h>

int main(int argc, char **argv)
{
    // Check if the program was invoked correctly; if not, signal error
    if (argc < 4 || argc > 5) {
	cerr << "Usage: " << argv[0] << " conninfo_str in_filename out_filename [oid]" << endl;
	exit(1);
    }

    // Get the arguments passed to the program
    char* conninfo = argv[1];
    char* in_filename = argv[2];
    char* out_filename = argv[3];

    // Set up the connection and create a large object
    int lobjId = ( argc == 4 ? 0 : atoi(argv[4]) );
    PgLargeObject object(lobjId, conninfo);

    // check to see that the backend connection was successfully made
    if ( object.ConnectionBad() ) {
         cerr << "Connection with conninfo '" << conninfo << "' failed." << endl
              << object.ErrorMessage();
	 exit(1);
    }

    // Test the import and export features of the Large Object interface
    object.Exec("BEGIN");
    cout << "Importing file \"" << in_filename << "\"..." << endl;
    object.Import(in_filename);
    cout << "Exporting large object to file \"" << out_filename << "\"..." << endl;
    object.Export(out_filename);
    object.Exec("END"); // WHY DOES IT CORE DUMP HERE ???
    return 0;
}
