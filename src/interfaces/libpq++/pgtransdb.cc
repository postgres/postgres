/*-------------------------------------------------------------------------
 *
 *   FILE
 *	pgtransdb.cpp
 *
 *   DESCRIPTION
 *      implementation of the PgTransaction class.
 *   PgConnection encapsulates a transaction querying to backend
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpq++/Attic/pgtransdb.cc,v 1.4 2001/05/09 17:29:10 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
 
#include "pgtransdb.h"

// ****************************************************************
//
// PgTransaction Implementation
//
// ****************************************************************
// Make a connection to the specified database with default environment
// See PQconnectdb() for conninfo usage. 
PgTransaction::PgTransaction(const char* conninfo)
   : PgDatabase(conninfo),
     pgCommitted(true)
{
	BeginTransaction();
}

// Destructor: End the transaction block
PgTransaction::~PgTransaction()
{
	if (!pgCommitted) Exec("ABORT");
}

// Begin the transaction block
ExecStatusType PgTransaction::BeginTransaction()
{
	pgCommitted = false;
	return Exec("BEGIN");
} // End BeginTransaction()

// Begin the transaction block
ExecStatusType PgTransaction::EndTransaction()
{
	pgCommitted = true;
	return Exec("END");
} // End EndTransaction()

