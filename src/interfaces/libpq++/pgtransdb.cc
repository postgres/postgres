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
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpq++/Attic/pgtransdb.cc,v 1.3 1999/05/30 15:17:58 tgl Exp $
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
   : PgDatabase(conninfo)
{
	BeginTransaction();
}

// Destructor: End the transaction block
PgTransaction::~PgTransaction()
{
	EndTransaction();
}

// Begin the transaction block
ExecStatusType PgTransaction::BeginTransaction()
{
	return Exec("BEGIN");
} // End BeginTransaction()

// Begin the transaction block
ExecStatusType PgTransaction::EndTransaction()
{
	return Exec("END");
} // End EndTransaction()
