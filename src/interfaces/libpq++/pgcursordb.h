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
 * Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */
 
#ifndef PGCURSOR_H
#define PGCURSOR_H

#include "pgtransdb.h"


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
  PgCursor(const char* dbName, const char* cursor);	// use reasonable defaults
  // connect to the database with given environment and database name
  PgCursor(const PgEnv& env, const char* dbName, const char* cursor);
  PgCursor(const PgConnection&, const char* cursor);
  virtual ~PgCursor();	// close connection and clean up
  
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
}; // End PgCursor Class Declaration

#endif	// PGCURSOR_H
