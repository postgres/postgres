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
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * 
 * $Id: pgconnection.h,v 1.15 2002/01/22 19:02:40 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
 
#ifndef PGCONNECTION_H
#define PGCONNECTION_H

extern "C" {
#include "pg_config.h"
}

/* We assume that the C++ compiler will have these keywords, even though
 * pg_config.h may have #define'd them to empty because C compiler doesn't.
 */
#undef const
#undef inline
#undef signed
#undef volatile

#ifdef HAVE_CXX_STRING_HEADER
#include <string>
#endif

extern "C" {
#include "libpq-fe.h"
}

#ifdef HAVE_NAMESPACE_STD
#define PGSTD std::
#else
#define PGSTD
#endif


// ****************************************************************
//
// PgConnection - a connection made to a postgres backend
//
// ****************************************************************
// This class contains all the information about the connection
// to the backend process.  All the database classes should be
// derived from this class to obtain the connection interface.
class DLLIMPORT PgConnection {
protected:
  PGconn* pgConn;			// Connection Structure
  PGresult* pgResult;			// Current Query Result
  bool pgCloseConnection; // true if connection should be closed by destructor
  
public:
   explicit PgConnection(const char* conninfo); // use reasonable & environment defaults
   virtual ~PgConnection(); 			// close connection and clean up
   
   // Connection status and error messages
   ConnStatusType Status() const;
   bool ConnectionBad() const;
   const char* ErrorMessage() const;
  
   // returns the database name of the connection
   const char* DBName() const;

   // Query Execution interface
   ExecStatusType Exec(const char* query);  // send a query to the backend
   int ExecCommandOk(const char* query);    // send a command and check if it's OK
   int ExecTuplesOk(const char* query);     // send a command and check if tuples are returned
   PGnotify* Notifies();
    
protected:
   ConnStatusType Connect(const char* conninfo);
   void CloseConnection();
   static PGSTD string IntToString(int);
   // Default constructor is only available to subclasses
   PgConnection();

private:
// We don't support copying of PgConnection objects,
// so make copy constructor and assignment op private.
   PgConnection(const PgConnection&);
   PgConnection& operator= (const PgConnection&);
};


#undef PGSTD

#endif	// PGCONNECTION_H
