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
 *    $Header: /cvsroot/pgsql/src/interfaces/libpq++/examples/Attic/testlibpq0.cc,v 1.2 1996/11/18 01:44:23 bryanh Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include "libpq++.H"

int 
main()
{
  ExecStatusType status;
  PGenv env;
  PGdatabase* data;

  char buf[10000];
  int done = 0;
 
  data = new PGdatabase(&env, "template1");

  if (data->status() == CONNECTION_BAD)
    printf("connection was unsuccessful\n%s\n", data->errormessage());
  else
    printf("connection successful\n");

  while (!done)
    {
      printf("> ");fflush(stdout);
      if (gets(buf) && buf[0]!='\0')
	if((status = data->exec(buf)) == PGRES_TUPLES_OK) 
	     data->printtuples(stdout, 1, "|", 1, 0);
	else
	     printf("status = %d\nerrorMessage = %s\n", status,
						   data->errormessage());
      else
	done = 1;
    }
}
