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
 * $Id: pgconnection.h,v 1.7 2000/04/22 22:39:15 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
 
#ifndef PGCONNECTION_H
#define PGCONNECTION_H

extern "C" {
#include "config.h"
}

/* We assume that the C++ compiler will have these keywords, even though
 * config.h may have #define'd them to empty because C compiler doesn't.
 */
#undef const
#undef inline
#undef signed
#undef volatile

#ifdef HAVE_CXX_STRING_HEADER
#include <string>
#endif

extern "C" {
#include "postgres.h"
#include "libpq-fe.h"
}

#ifdef HAVE_NAMESPACE_STD
using namespace std;
#endif


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
  PGconn* pgConn;				// Connection Structure
  PGresult* pgResult;			// Current Query Result
  int pgCloseConnection; // TRUE if connection should be closed by destructor
  
public:
   PgConnection(const char* conninfo); 	// use reasonable & environment defaults
   virtual ~PgConnection(); 			// close connection and clean up
   
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
   // Default constructor is only available to subclasses
   PgConnection();

private:
// We don't support copying of PgConnection objects,
// so make copy constructor and assignment op private.
   PgConnection(const PgConnection&);
   PgConnection& operator= (const PgConnection&);
};

#endif	// PGCONNECTION_H
