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
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpq++/Attic/pgconnection.cc,v 1.11 2001/05/09 17:29:10 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "pgconnection.h"

using namespace std;


// ****************************************************************
//
// PgConnection Implementation
//
// ****************************************************************
// default constructor -- initialize everything
PgConnection::PgConnection()
	: pgConn(NULL), pgResult(NULL), pgCloseConnection(false)
{}


// constructor -- checks environment variable for database name
// Now uses PQconnectdb
PgConnection::PgConnection(const char* conninfo)
	: pgConn(NULL), pgResult(NULL), pgCloseConnection(true)
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
  CloseConnection();
}


// PgConnection::CloseConnection()
// close down the connection if there is one
void PgConnection::CloseConnection() 
{
  // if the connection is open, close it first
  if ( pgCloseConnection ) {    
       if(pgResult) PQclear(pgResult);
       pgResult=NULL;
       if(pgConn) PQfinish(pgConn);
       pgConn=NULL;
       pgCloseConnection=false;
  }
}


// PgConnection::connect
// establish a connection to a backend
ConnStatusType PgConnection::Connect(const char conninfo[])
{
  // if the connection is open, close it first
  CloseConnection();

  // Connect to the database
  pgConn = PQconnectdb(conninfo);

  // Now we have a connection we must close (even if it's bad!)
  pgCloseConnection = true;
  
  // Status will return either CONNECTION_OK or CONNECTION_BAD
  return Status();
}

// PgConnection::status -- return connection or result status
ConnStatusType PgConnection::Status() const
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
  char buffer [4*sizeof(n) + 2];
  sprintf(buffer, "%d", n);
  return buffer;
}



bool PgConnection::ConnectionBad() const
{ 
return Status() == CONNECTION_BAD; 
}


const char* PgConnection::ErrorMessage() const
{ 
return (const char *)PQerrorMessage(pgConn); 
}
  

const char* PgConnection::DBName() const
{ 
return (const char *)PQdb(pgConn); 
}


