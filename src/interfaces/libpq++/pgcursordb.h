/*-------------------------------------------------------------------------
 *
 * pgcursordb.h
 *    
 *
 *   DESCRIPTION
 *		Postgres Cursor Database Class: 
 *		   Query Postgres backend using a cursor
 *
 *   NOTES
 *      Currently under construction.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 *  $Id: pgcursordb.h,v 1.7 2001/05/09 17:29:10 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
 
#ifndef PGCURSORDB_H
#define PGCURSORDB_H

#ifndef PGTRANSDB_H
#include "pgtransdb.h"
#endif

#ifdef HAVE_NAMESPACE_STD
#define PGSTD std::
#else
#define PGSTD
#endif


// ****************************************************************
//
// PgCursor - a class for querying databases using a cursor
//
// ****************************************************************
// This is the database access class that declares a cursor and
// manipulates data through it.  The interface will introduce some
// ease of use through the methods that will allow cursor specific
// operations, like fetch, forward, etc.
class PgCursor : public PgTransaction {
public:
  PgCursor(const char* conninfo, const char* cursor);	// use reasonable & environment defaults
  // connect to the database with given environment and database name
  //  PgCursor(const PgConnection&, const char* cursor);
  ~PgCursor();	// close connection and clean up
  
  // Commands associated with cursor interface
  int Declare(PGSTD string query, bool binary=false);	// Declare a cursor with given name
  int Fetch(const char* dir = "FORWARD");		// Fetch ALL tuples in given direction
  int Fetch(unsigned num, const char* dir = "FORWARD");	// Fetch specified amount of tuples
  int Close();	// Close the cursor
  
  // Accessors to the cursor name
  const char* Cursor() const { return pgCursor.c_str(); }
  // TODO: Setter has same name as getter--ouch!
  // OBSOLESCENT
  void Cursor(PGSTD string cursor) { pgCursor = cursor; }
  
protected:
  int Fetch(PGSTD string num, PGSTD string dir);
  
protected:
  PGSTD string pgCursor;
  
protected:
  PgCursor() : PgTransaction() {}	// Do not connect

private:
// We don't support copying of PgCursor objects,
// so make copy constructor and assignment op private.
   PgCursor(const PgCursor&);
   PgCursor& operator= (const PgCursor&);
}; // End PgCursor Class Declaration


#ifdef HAVE_NAMESPACE_STD
#undef PGSTD
#endif

#endif	// PGCURSORDB_H

