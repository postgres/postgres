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
 *  $Id: pgcursordb.h,v 1.6 2001/01/24 19:43:32 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
 
#ifndef PGCURSORDB_H
#define PGCURSORDB_H

#ifndef PGTRANSDB_H
#include "pgtransdb.h"
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
  int Declare(const string& query, int binary = 0);	// Declare a cursor with given name
  int Fetch(const char* dir = "FORWARD");		// Fetch ALL tuples in given direction
  int Fetch(unsigned num, const char* dir = "FORWARD");	// Fetch specified amount of tuples
  int Close();	// Close the cursor
  
  // Accessors to the cursor name
  const char* Cursor() const { return pgCursor.c_str(); }
  void Cursor(const string& cursor) { pgCursor = cursor; }
  
protected:
  int Fetch(const string& num, const string& dir);
  
protected:
  string pgCursor;
  
protected:
  PgCursor() : PgTransaction() {}	// Do not connect

private:
// We don't support copying of PgCursor objects,
// so make copy constructor and assignment op private.
   PgCursor(const PgCursor&);
   PgCursor& operator= (const PgCursor&);
}; // End PgCursor Class Declaration

#endif	// PGCURSORDB_H
