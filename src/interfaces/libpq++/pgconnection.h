/*-------------------------------------------------------------------------
 *
 * pgconnection.h
 *    
 *
 *   DESCRIPTION
 *		Postgres Connection Class: 
 *		   Manage Postgres backend connection
 *
 *   NOTES
 *      Currently under construction.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 * 
 * $Id: pgconnection.h,v 1.4 2000/03/02 02:00:58 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
 
#ifndef PGCONN_H
#define PGCONN_H

#include <stdio.h>
#include <string>

extern "C" {
#include "libpq-fe.h"
}

using namespace std;

// ****************************************************************
//
// PgConnection - a connection made to a postgres backend
//
// ****************************************************************
// This class contains all the information about the connection
// to the backend process.  All the database classes should be
// derived from this class to obtain the connection interface.
class PgConnection {
protected:
  PGconn* pgConn;	// Connection Structures
  PGresult* pgResult;	// Query Result
  int pgCloseConnection; // Flag indicating whether the connection should be closed or not
  
public:
   PgConnection(const char* conninfo); 	// use reasonable & environment defaults
   ~PgConnection(); 			// close connection and clean up
   
   // Connection status and error messages
   ConnStatusType Status();
   int ConnectionBad();
   const char* ErrorMessage();
  
   // returns the database name of the connection
   const char* DBName();

   // Query Execution interface
   ExecStatusType Exec(const char* query);  // send a query to the backend
   int ExecCommandOk(const char* query);    // send a command and check if it's OK
   int ExecTuplesOk(const char* query);     // send a command and check if tuples are returned
   PGnotify* Notifies();
    
protected:
   ConnStatusType Connect(const char* conninfo);
   string IntToString(int);
   
protected:
   PgConnection();
};

#endif	// PGCONN_H
