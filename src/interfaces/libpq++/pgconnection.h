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
 * Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */
 
#ifndef PGCONN_H
#define PGCONN_H

#include <stdio.h>
#include "pgenv.h"

extern "C" {
#include "libpq-fe.h"
}


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
  PgEnv pgEnv;		// Current connection environment
  PGconn* pgConn;	// Connection Structures
  PGresult* pgResult;	// Query Result
  string pgErrorMessage; // Error messages container
  int pgCloseConnection; // Flag indicating whether the connection should be closed or not
  
public:
   PgConnection(const char* dbName); // use reasonable defaults
   PgConnection(const PgEnv& env, const char* dbName); // connect to the database with 
                                    // given environment and database name
   virtual ~PgConnection(); // close connection and clean up
   
   // Connection status and error messages
   ConnStatusType Status();
   int ConnectionBad() { return Status() == CONNECTION_BAD; }
   const char* ErrorMessage() const { return pgErrorMessage.c_str(); }
  
   // returns the database name of the connection
   const char* DBName() const { return PQdb(pgConn); }

   // Query Execution interface
   ExecStatusType Exec(const char* query);  // send a query to the backend
   int ExecCommandOk(const char* query);    // send a command and check if it's OK
   int ExecTuplesOk(const char* query);     // send a command and check if tuples are returned
   PGnotify* Notifies();
    
protected:
   ConnStatusType Connect(const char* dbName);
   void SetErrorMessage(const string&, int append = 0);
   string IntToString(int);
   
protected:
   PgConnection();
   PgConnection(const PgConnection&);
};

#endif	// PGCONN_H
