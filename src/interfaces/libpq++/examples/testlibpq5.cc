/*
 * testlibpq5.cc
 * 	Test the C++ version of LIBPQ, the POSTGRES frontend library.
 *   tests the binary cursor interface
 *
 *
 *
 populate a database by doing the following (use testlibpq5.sql):
 
CREATE TABLE test1 (i int4, d float4, p polygon);

INSERT INTO test1 values (1, 3.567, '(3.0, 4.0, 1.0, 2.0)'::polygon);

INSERT INTO test1 values (2, 89.05, '(4.0, 3.0, 2.0, 1.0)'::polygon);

 the expected output is:

tuple 0: got
 i = (4 bytes) 1,
 d = (4 bytes) 3.567000,
 p = (4 bytes) 2 points         boundbox = (hi=3.000000/4.000000, lo = 1.000000,2.000000)
tuple 1: got
 i = (4 bytes) 2,
 d = (4 bytes) 89.050003,
 p = (4 bytes) 2 points         boundbox = (hi=4.000000/3.000000, lo = 2.000000,1.000000)

 *
 */
#include <iostream.h>
#include <libpq++.h>
#include <stdlib.h>
extern "C" {
#include "postgres.h"		// for Postgres types
#include "utils/geo_decls.h" // for the POLYGON type
}

main()
{
  // Begin, by connecting to the backend using hardwired constants
  // and a test database created by the user prior to the invokation
  // of this test program.  Connect using cursor interface.
  char* dbName = "dbname=template1"; // change this to the name of your test database
  PgCursor data(dbName, "mycursor");

  // check to see that the backend connection was successfully made
  if ( data.ConnectionBad() ) {
    cerr << "Connection to database '" << dbName << "' failed." << endl
         << data.ErrorMessage();
    exit(1);
  }

  // Declare a binary cursor for all the tuples in database 'test1'
  if ( !data.Declare("select * from test1", 1) ) {
    cerr << "DECLARE CURSOR command failed" << endl;
    exit(1);
  }

  // fetch all instances from the current cursor
  if ( !data.Fetch() ) {
    cerr << "FETCH ALL command didn't return tuples properly" << endl;
    exit(1);
  }
 
  // Find the field numbers for the columns 'i', 'd', and 'p'
  int i_fnum = data.FieldNum("i");
  int d_fnum = data.FieldNum("d");
  int p_fnum = data.FieldNum("p");
  
/*
  for (i=0;i<3;i++) {
      printf("type[%d] = %d, size[%d] = %d\n",
	     i, data.FieldType(i), 
	     i, data.FieldSize(i));
  }
*/

  // Print out the information about the extracted tuple
  for (int i=0; i < data.Tuples(); i++) {
    // we hard-wire this to the 3 fields we know about
    int* ival = (int*)data.GetValue(i,i_fnum);
    float* dval = (float*)data.GetValue(i,d_fnum);
    int plen = data.GetLength(i,p_fnum);

    // Allocate correct memory space for the Polygon struct and copy
    // the extracted data into it.
    // plen doesn't include the length field so need to increment by VARHDSZ
    POLYGON* pval = (POLYGON*) malloc(plen + VARHDRSZ); 
    pval->size = plen;
    memmove((char*)&pval->npts, data.GetValue(i,p_fnum), plen);
    
    // Display Polygon Information
    cout << "tuple " << i << ": got" << endl
         << " i = (" << data.GetLength(i,i_fnum) << " bytes) " << *ival << "," << endl
         << " d = (" << data.GetLength(i,d_fnum) << " bytes) " << *dval << "," << endl
         << " p = (" << data.GetLength(i,d_fnum) << " bytes) " << pval->npts << " points"
         << "\tboundbox = (hi=" << pval->boundbox.high.x << "/" << pval->boundbox.high.y << ","
         << "lo = " << pval->boundbox.low.x << "," << pval->boundbox.low.y << ")" << endl;
	   
    // Deallocate memory allocated for the Polygon structure
    free(pval);
  }
  return 0;
}
