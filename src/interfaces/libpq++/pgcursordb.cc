/*-------------------------------------------------------------------------
 *
 *   FILE
 *	pgcursordb.cpp
 *
 *   DESCRIPTION
 *      implementation of the PgCursor class.
 *   PgCursor encapsulates a cursor interface to the backend
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpq++/Attic/pgcursordb.cc,v 1.7 2002/06/15 18:49:29 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
 
#include "pgcursordb.h"
 
#ifdef HAVE_NAMESPACE_STD
using namespace std;
#endif


// ****************************************************************
//
// PgCursor Implementation
//
// ****************************************************************
// Make a connection to the specified database with default environment
// See PQconnectdb() for conninfo usage
PgCursor::PgCursor(const char* conninfo, const char* cursor)
   : PgTransaction(conninfo), pgCursor(cursor)
{}

// Do not make a connection to the backend -- just query
// Connection should not be closed after the object destructs since some
// other object is using the connection
//PgCursor::PgCursor(const PgConnection& conn, const char* cursor)
//   : PgTransaction(conn), pgCursor(cursor)
//{}

// Destructor: End the transaction block
PgCursor::~PgCursor()
{
	Close();
}


// ****************************************************************
//
// PgCursor: Cursor Interface Implementation
//
// ****************************************************************
// Declare a cursor: name has already been supplied in the constructor
int PgCursor::Declare(string query, bool binary)
{
	string cmd = "DECLARE " + pgCursor;
	if ( binary )
	     cmd += " BINARY";
	cmd += " CURSOR FOR " + query;
	return ExecCommandOk( cmd.c_str() );
}

// Fetch ALL tuples in given direction
int PgCursor::Fetch(const char* dir)
{
	return Fetch("ALL", dir);
}

// Fetch specified amount of tuples in given direction
int PgCursor::Fetch(unsigned num, const char* dir)
{
	return Fetch( IntToString(num), dir );
}

// Create and execute the actual fetch command with the given arguments
int PgCursor::Fetch(string num, string dir)
{
	string cmd = "FETCH " + dir + " " + num + " IN " + pgCursor;
	return ExecTuplesOk( cmd.c_str() );
}

// Close the cursor: no more queries using the cursor should be allowed
// Actually, the backend should take care of it.
int PgCursor::Close()
{
	string cmd = "CLOSE " + pgCursor;
	return ExecCommandOk( cmd.c_str() );
}
