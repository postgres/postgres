/*-------------------------------------------------------------------------
 *
 *   FILE
 *	pgconnection.cc
 *
 *   DESCRIPTION
 *      implementation of the PGconnection class.
 *   PGconnection encapsulates a frontend to backend connection
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/interfaces/libpq++/Attic/pgconnection.cc,v 1.1.1.1 1996/07/09 06:22:18 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "libpq++.H"

// default constructor
// checks environment variable for database name
PGconnection::PGconnection()
{
  char* name;
  PGenv* newenv;

  conn = NULL;
  result = NULL;
  errorMessage[0] = '\0';

  newenv = new PGenv(); // use reasonable defaults for the environment
  if (!(name = getenv(ENV_DEFAULT_DBASE)))
    return;
  connect(newenv, name);
}

// constructor -- for given environment and database name
PGconnection::PGconnection(PGenv* env, char* dbName)
{
  conn = NULL;
  result = NULL;
  errorMessage[0] = '\0';
  connect(env, dbName);
}

// destructor - closes down the connection and cleanup
PGconnection::~PGconnection()
{
  if (result)	PQclear(result);
  if (conn)	PQfinish(conn);
}

// PGconnection::connect
// establish a connection to a backend
ConnStatusType
PGconnection::connect(PGenv* newenv, char* dbName)
{
#if 0
    FILE *debug;
    debug = fopen("/tmp/trace.out","w");
    PQtrace(conn, debug);
#endif

    env = newenv;
    fe_setauthsvc(env->pgauth, errorMessage); 
    conn = PQsetdb(env->pghost, env->pgport, env->pgoption, env->pgtty, dbName);
    if(strlen(errorMessage))
      return CONNECTION_BAD;
    else
      return status();
}

// PGconnection::status -- return connection or result status
ConnStatusType
PGconnection::status()
{
    return PQstatus(conn);
}

// PGconnection::exec  -- send a query to the backend
ExecStatusType
PGconnection::exec(char* query)
{
  if (result)
    PQclear(result); 

  result = PQexec(conn, query);
  if (result)
      return PQresultStatus(result);
  else {
      strcpy(errorMessage, PQerrorMessage(conn));
      return PGRES_FATAL_ERROR;
  }
}
