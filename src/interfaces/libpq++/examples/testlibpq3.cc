/*
 * testlibpq3.cc
 * 	Test the C++ version of LIBPQ, the POSTGRES frontend library.
 *   tests the binary cursor interface
 *
 *
 *
 populate a database by doing the following:
 
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
#include <stdio.h>
#include "libpq++.H"
extern "C" {
#include "utils/geo-decls.h" /* for the POLYGON type */
}

main()
{
  char* dbName;
  int nFields;
  int i,j;
  int i_fnum, d_fnum, p_fnum;

  /* begin, by creating the parameter environtment for a backend
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
  if (data->exec("BEGIN") != PGRES_COMMAND_OK) {
    fprintf(stderr,"BEGIN command failed\n");
    delete data;
    exit(1);
  }

  /* fetch instances from the pg_database, the system catalog of databases*/
  if (data->exec("DECLARE mycursor BINARY CURSOR FOR select * from test1")
      != PGRES_COMMAND_OK) {
    fprintf(stderr,"DECLARE CURSOR command failed\n");
    delete data;
    exit(1);
  }

  if (data->exec("FETCH ALL in mycursor") != PGRES_TUPLES_OK) {
    fprintf(stderr,"FETCH ALL command didn't return tuples properly\n");
    delete data;
    exit(1);
  }
 
  i_fnum = data->fieldnum("i");
  d_fnum = data->fieldnum("d");
  p_fnum = data->fieldnum("p");
  
/*
  for (i=0;i<3;i++) {
      printf("type[%d] = %d, size[%d] = %d\n",
	     i, data->fieldtype(i), 
	     i, data->fieldsize(i));
  }
*/

  for (i=0; i < data->ntuples(); i++) {
    int *ival; 
    float *dval;
    int plen;
    POLYGON* pval;
    /* we hard-wire this to the 3 fields we know about */
    ival = (int*)data->getvalue(i,i_fnum);
    dval = (float*)data->getvalue(i,d_fnum);
    plen = data->getlength(i,p_fnum);

    /* plen doesn't include the length field so need to increment by VARHDSZ*/
    pval = (POLYGON*) malloc(plen + VARHDRSZ); 
    pval->size = plen;
    memmove((char*)&pval->npts, data->getvalue(i,p_fnum), plen);
    printf("tuple %d: got\n", i);
    printf(" i = (%d bytes) %d,\n",
	   data->getlength(i,i_fnum), *ival);
    printf(" d = (%d bytes) %f,\n",
	   data->getlength(i,d_fnum), *dval);
    printf(" p = (%d bytes) %d points \tboundbox = (hi=%f/%f, lo = %f,%f)\n",
	   data->getlength(i,d_fnum),
	   pval->npts,
	   pval->boundbox.xh,
	   pval->boundbox.yh,
	   pval->boundbox.xl,
	   pval->boundbox.yl);
  }
  
  /* close the portal */
  data->exec("CLOSE mycursor");

  /* end the transaction */
  data->exec("END");

  /* close the connection to the database and cleanup */
  delete data;
}
