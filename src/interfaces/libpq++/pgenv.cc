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
 *    $Header: /cvsroot/pgsql/src/interfaces/libpq++/Attic/pgenv.cc,v 1.4 1999/05/10 15:27:19 momjian Exp $
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


// Extract the PgEnv contents into a form suitable for PQconnectdb
// which happens to be readable, hence choice of <<
ostream& operator << (ostream &s, const PgEnv& a)
{
  s<<' '; // surround with whitespace, just in case
  if(a.pgHost.length()  !=0)s<<" host="   <<a.pgHost;
  if(a.pgPort.length()  !=0)s<<" port="   <<a.pgPort;
  // deprecated: if(a.pgAuth.length()!=0)s<<" authtype="<<a.pgAuth;
  if(a.pgOption.length()!=0)s<<" options="<<a.pgOption;
  if(a.pgTty.length()   !=0)s<<" tty="    <<a.pgTty;
  s<<' ';

  return s;
}
