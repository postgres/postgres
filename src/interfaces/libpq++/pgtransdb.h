/*-------------------------------------------------------------------------
 *
 * pgtransdb.h
 *    
 *
 *   DESCRIPTION
 *		Postgres Transaction Database Class: 
 *		   Query Postgres backend using a transaction block
 *
 *   NOTES
 *      Currently under construction.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 *  $Id: pgtransdb.h,v 1.6 2001/01/24 19:43:32 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
 
#ifndef PGTRANSDB_H
#define PGTRANSDB_H

#ifndef PGDATABASE_H
#include "pgdatabase.h"
#endif

// ****************************************************************
//
// PgTransaction - a class for running transactions against databases
//
// ****************************************************************
// This is the database access class that keeps an open
// transaction block during its lifetime.  The block is ENDed when
// the object is destroyed.
class PgTransaction : public PgDatabase {
public:
  PgTransaction(const char* conninfo);	// use reasonable & environment defaults
  // connect to the database with given environment and database name
  // PgTransaction(const PgConnection&);
  ~PgTransaction();	// close connection and clean up
  
protected:
  ExecStatusType BeginTransaction();
  ExecStatusType EndTransaction();
  
protected:
  PgTransaction() : PgDatabase() {}	// Do not connect

private:
// We don't support copying of PgTransaction objects,
// so make copy constructor and assignment op private.
   PgTransaction(const PgTransaction&);
   PgTransaction& operator= (const PgTransaction&);
}; // End PgTransaction Class Declaration

#endif	// PGTRANSDB_H
