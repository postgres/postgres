/*-------------------------------------------------------------------------
 *
 *   FILE
 *	pglobject.cc
 *
 *   DESCRIPTION
 *      implementation of the PgLargeObject class.
 *   PgLargeObject encapsulates a frontend to backend connection
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/interfaces/libpq++/Attic/pglobject.cc,v 1.3 1997/02/13 10:00:34 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
 
extern "C" {
#include "libpq/libpq-fs.h"
}

#include "pglobject.h"



// ****************************************************************
//
// PgLargeObject Implementation
//
// ****************************************************************
// default constructor
// creates a large object in the default database
PgLargeObject::PgLargeObject(const char* dbName)
	: PgConnection(dbName)
{
  Init();
  Create();
  Open();
}

// constructor
// open an existing large object in the default database
PgLargeObject::PgLargeObject(Oid lobjId, const char* dbName) 
	: PgConnection(dbName)
{
  Init(lobjId);
  if ( !pgObject )
       Create();
  Open();
}

// constructor
// create a large object in the given database
PgLargeObject::PgLargeObject(const PgEnv& env, const char* dbName)
	: PgConnection(env, dbName)
{
  Init();
  Create();
  Open();
}

// constructor
// open an existing large object in the given database
PgLargeObject::PgLargeObject(const PgEnv& env, const char* dbName, Oid lobjId)
	: PgConnection(env, dbName)
{
  Init(lobjId);
  if ( !pgObject )
       Create();
  Open();
}

// destructor -- closes large object
PgLargeObject::~PgLargeObject()
{
  Close();
}

// PgLargeObject::Init
// Initialize the variables
void PgLargeObject::Init(Oid lobjId)
{
  pgFd = -1;
  pgObject = lobjId;
}

// PgLargeObject::create
// create large object and check for errors
void PgLargeObject::Create()
{
  // Create the object
  pgObject = lo_creat(pgConn, INV_READ|INV_WRITE);
  
  // Check for possible errors
  if (!pgObject)
      SetErrorMessage( "PgLargeObject: can't create large object" );
}

// PgLargeObject::open
// open large object and check for errors
void PgLargeObject::Open()
{
  // Open the object
  pgFd = lo_open(pgConn, pgObject, INV_READ|INV_WRITE);
  
  // Check for possible errors
  string objStr( IntToString(pgObject) );
  if (pgFd < 0)
      SetErrorMessage( "PgLargeObject: can't open large object " + objStr );
  else
      SetErrorMessage( "PgLargeObject: created and opened large object " + objStr );
}

// PgLargeObject::unlink
// destruct large object and delete from it from the database
int PgLargeObject::Unlink()
{
  // Unlink the object
  int temp = lo_unlink(pgConn, pgObject);
  
  // Initialize the large object upon success
  if (!temp) {
      Close();
      Init();
  }
  
  // Return the status
  return temp;
}
