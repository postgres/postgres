/*-------------------------------------------------------------------------
 *
 *   FILE
 *	PgEnv.cc
 *
 *   DESCRIPTION
 *      PgEnv is the environment for setting up a connection to a 
 *   postgres backend,  captures the host, port, tty, options and
 *   authentication type.
 *
 *   NOTES
 *      Currently under construction.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/interfaces/libpq++/Attic/pgenv.cc,v 1.3 1997/02/13 10:00:33 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <stdlib.h>
#include "pgenv.h"


#define DefaultAuth DEFAULT_CLIENT_AUTHSVC
#define DefaultPort POSTPORT


// ****************************************************************
//
// PgEnv Implementation
//
// ****************************************************************
// Default constructor for PgEnv
// checks the environment variables
PgEnv::PgEnv()
{
  SetValues(getenv(ENV_DEFAULT_AUTH), getenv(ENV_DEFAULT_HOST),
            getenv(ENV_DEFAULT_PORT), getenv(ENV_DEFAULT_OPTION),
            getenv(ENV_DEFAULT_TTY));
}

// constructor for given environment
PgEnv::PgEnv(const string& auth, const string& host, const string& port, 
             const string& option, const string& tty)
{
  SetValues(auth, host, port, option, tty);
}

// allocate memory and set internal structures to match
// required environment
void PgEnv::SetValues(const string& auth, const string& host, const string& port, 
                      const string& option, const string& tty)
{
  Auth( auth );
  Host( host );
  Port( port );
  Option( option );
  TTY( tty );
}

// read a string from the environment and convert it to string
string PgEnv::getenv(const char* name)
{
  char* env = ::getenv(name);
  return (env ? env : "");
}
