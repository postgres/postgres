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
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 *  $Id: pgtransdb.h,v 1.3 1999/06/01 02:37:33 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
 
#ifndef PGTRANSDB_H
#define PGTRANSDB_H

#include "pgdatabase.h"

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
}; // End PgTransaction Class Declaration

#endif	// PGTRANSDB_H
