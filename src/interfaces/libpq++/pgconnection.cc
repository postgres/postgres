/*-------------------------------------------------------------------------
 *
 *   FILE
 *	pgconnection.cpp
 *
 *   DESCRIPTION
 *      implementation of the PgConnection class.
 *   PgConnection encapsulates a frontend to backend connection
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/interfaces/libpq++/Attic/pgconnection.cc,v 1.3 1999/05/10 15:27:18 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <stdlib.h>
#include <string.h>
#include <strstream>
#include "pgconnection.h"

extern "C" {
#include "fe-auth.h"
}


// ****************************************************************
//
// PgConnection Implementation
//
// ****************************************************************
// default constructor -- initialize everything
PgConnection::PgConnection()
	: pgConn(NULL), pgResult(NULL), pgCloseConnection(0)
{}

// copy constructor -- copy the pointers; no deep copy required
PgConnection::PgConnection(const PgConnection& conn)
	: pgEnv(conn.pgEnv), pgConn(conn.pgConn), pgResult(conn.pgResult), 
	  pgCloseConnection(conn.pgCloseConnection)
{}

// constructor -- checks environment variable for database name
PgConnection::PgConnection(const char* dbName)
	: pgConn(NULL), pgResult(NULL), pgCloseConnection(1)
{
  // Get a default database name to connect to
  char* defDB = (char*)dbName;
  if ( !dbName )
       if ( !(defDB = getenv(ENV_DEFAULT_DBASE)) )
            return;
    
  // Connect to the database
  Connect( defDB );
}

// constructor -- for given environment and database name
PgConnection::PgConnection(const PgEnv& env, const char* dbName)
	: pgEnv(env), pgConn(NULL), pgResult(NULL), pgCloseConnection(1)
{
  Connect( dbName );
}

// destructor - closes down the connection and cleanup
PgConnection::~PgConnection()
{
  // Terminate the debugging output if it was turned on
  #if defined(DEBUG)
  	PQuntrace(pgConn);
  #endif
  
  // Close the conneciton only if needed
  // This feature will most probably be used by the derived classes that
  // need not close the connection after they are destructed.
  if ( pgCloseConnection ) {
       if (pgResult)	PQclear(pgResult);
       if (pgConn)	PQfinish(pgConn);
  }
}

// PgConnection::connect
// establish a connection to a backend
ConnStatusType PgConnection::Connect(const char* dbName)
{
    // Turn the trace on
    #if defined(DEBUG)
	FILE *debug = fopen("/tmp/trace.out","w");
	PQtrace(pgConn, debug);
    #endif

    // Connect to the database
    ostrstream conninfo;
    conninfo << "dbname="<<dbName;
    conninfo << pgEnv;
    pgConn=PQconnectdb(conninfo.str());
    conninfo.freeze(0);
    
    if(ConnectionBad()) {
    	SetErrorMessage( PQerrorMessage(pgConn) );
    }

    return Status();
}

// PgConnection::status -- return connection or result status
ConnStatusType PgConnection::Status()
{
    return PQstatus(pgConn);
}

// PgConnection::exec  -- send a query to the backend
ExecStatusType PgConnection::Exec(const char* query)
{
  // Clear the Result Stucture if needed
  if (pgResult)
    PQclear(pgResult); 

  // Execute the given query
  pgResult = PQexec(pgConn, query);
  
  // Return the status
  if (pgResult)
      return PQresultStatus(pgResult);
  else {
      SetErrorMessage( PQerrorMessage(pgConn) );
      return PGRES_FATAL_ERROR;
  }
}

// Return true if the Postgres command was executed OK
int PgConnection::ExecCommandOk(const char* query)
{
	return Exec(query) == PGRES_COMMAND_OK;
} // End ExecCommandOk()

int PgConnection::ExecTuplesOk(const char* query)
{
	return Exec(query) == PGRES_TUPLES_OK;
} // End ExecTuplesOk()


// PgConnection::notifies() -- returns a notification from a list of unhandled notifications
PGnotify* PgConnection::Notifies()
{
  Exec(" "); 
  return PQnotifies(pgConn);
}

// PgConnection::SetErrorMessage
// sets error message to the given string
void PgConnection::SetErrorMessage(const string& msg, int append)
{
  if ( append )
     pgErrorMessage += msg;
  else
     pgErrorMessage = msg;
}

// From Integer To String Conversion Function
string PgConnection::IntToString(int n)
{
  char buffer [32];
  memset(buffer, 0, sizeof(buffer));
  sprintf(buffer, "%d", n);
  return buffer;
}
