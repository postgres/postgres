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
 *    $Header: /cvsroot/pgsql/src/interfaces/libpq++/Attic/pgtransdb.cc,v 1.1 1997/02/13 10:00:36 scrappy Exp $
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
PgTransaction::PgTransaction(const char* dbName)
   : PgDatabase(dbName)
{
	BeginTransaction();
}

// Make a connection to the specified database with the given environment
PgTransaction::PgTransaction(const PgEnv& env, const char* dbName)
   : PgDatabase(env, dbName)
{
	BeginTransaction();
}

// Do not make a connection to the backend -- just query
// Connection should not be closed after the object destructs since some
// other object is using the connection
PgTransaction::PgTransaction(const PgConnection& conn) 
   : PgDatabase(conn) 
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
