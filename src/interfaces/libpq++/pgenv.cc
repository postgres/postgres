/*-------------------------------------------------------------------------
 *
 *   FILE
 *	PGenv.cc
 *
 *   DESCRIPTION
 *      PGenv is the environment for setting up a connection to a 
 *   postgres backend,  captures the host, port, tty, options and
 *   authentication type.
 *
 *   NOTES
 *      Currently under construction.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/interfaces/libpq++/Attic/pgenv.cc,v 1.1.1.1 1996/07/09 06:22:18 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <stdlib.h>
#include "libpq++.H"

#define DefaultAuth DEFAULT_CLIENT_AUTHSVC 
#define DefaultPort POSTPORT

// default constructor for PGenv
// checks the environment variables
PGenv::PGenv()
{
  char* temp;

  pgauth = NULL;
  pghost = NULL;
  pgport = NULL;
  pgoption = NULL;
  pgtty = NULL;

  setValues(getenv(ENV_DEFAULT_AUTH), getenv(ENV_DEFAULT_HOST),
            getenv(ENV_DEFAULT_PORT), getenv(ENV_DEFAULT_OPTION),
	    getenv(ENV_DEFAULT_TTY));
}

// constructor for given environment
PGenv::PGenv(char* auth, char* host, char* port, char* option, char* tty)
{
  pgauth = NULL;
  pghost = NULL;
  pgport = NULL;
  pgoption = NULL;
  pgtty = NULL;

  setValues(auth, host, port, option, tty);
}

// allocate memory and set internal structures to match
// required environment
void
PGenv::setValues(char* auth, char* host, char* port, char* option, char* tty)
{
  char* temp;

  temp = (auth) ? auth : DefaultAuth;

  if (pgauth)
    free(pgauth);
  pgauth = strdup(temp);

  temp = (host) ? host : DefaultHost;

  if (pghost)
    free(pghost);
  pghost = strdup(temp);

  temp = (port) ? port : DefaultPort;

  if (pgport)
    free(pgport);
  pgport = strdup(temp);
  
  temp = (option) ? option : DefaultOption;

  if (pgoption)
    free(pgoption);
  pgoption = strdup(temp);

  temp = (tty) ? tty : DefaultTty;

  if (pgtty)
    free(pgtty);
  pgtty = strdup(temp);
}

// default destrutor
// frees allocated memory for internal structures
PGenv::~PGenv()
{
  if (pgauth)
    free(pgauth);
  if (pghost)
    free(pghost);
  if (pgport)
    free(pgport);
  if (pgoption)
    free(pgoption);
  if (pgtty)
    free(pgtty);
}
