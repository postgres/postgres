/*-------------------------------------------------------------------------
 *
 *   FILE
 *	pgconnection.cc
 *
 *   DESCRIPTION
 *      implementation of the PgConnection class.
 *   PgConnection encapsulates a frontend to backend connection
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpq++/Attic/pgconnection.cc,v 1.9 2000/04/22 22:39:15 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "pgconnection.h"


// ****************************************************************
//
// PgConnection Implementation
//
// ****************************************************************
// default constructor -- initialize everything
PgConnection::PgConnection()
	: pgConn(NULL), pgResult(NULL), pgCloseConnection(0)
{}


// constructor -- checks environment variable for database name
// Now uses PQconnectdb
PgConnection::PgConnection(const char* conninfo)
	: pgConn(NULL), pgResult(NULL), pgCloseConnection(1)
{
    
  // Connect to the database
  Connect( conninfo );
}


// destructor - closes down the connection and cleanup
PgConnection::~PgConnection()
{
  // Close the connection only if needed
  // This feature will most probably be used by the derived classes that
  // need not close the connection after they are destructed.
  if ( pgCloseConnection ) {
       if(pgResult) PQclear(pgResult);
       if(pgConn) PQfinish(pgConn);
  }
}

// PgConnection::connect
// establish a connection to a backend
ConnStatusType PgConnection::Connect(const char* conninfo)
{
  // Connect to the database
  pgConn = PQconnectdb(conninfo);

  // Now we have a connection we must close (even if it's bad!)
  pgCloseConnection = 1;
  
  // Status will return either CONNECTION_OK or CONNECTION_BAD
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
  else 
	return PGRES_FATAL_ERROR;
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



// Don't know why these next two need to be part of Connection

// PgConnection::notifies() -- returns a notification from a list of unhandled notifications
PGnotify* PgConnection::Notifies()
{
  Exec(" "); 
  return PQnotifies(pgConn);
}


// From Integer To String Conversion Function
string PgConnection::IntToString(int n)
{
  char buffer [32];
  memset(buffer, 0, sizeof(buffer));
  sprintf(buffer, "%d", n);
  return buffer;
}



int PgConnection::ConnectionBad() 
{ 
return Status() == CONNECTION_BAD; 
}


const char* PgConnection::ErrorMessage() 
{ 
return (const char *)PQerrorMessage(pgConn); 
}
  

const char* PgConnection::DBName()
{ 
return (const char *)PQdb(pgConn); 
}


