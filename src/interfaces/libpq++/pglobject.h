/*-------------------------------------------------------------------------
 *
 *   FILE
 *	pglobject.h
 *
 *   DESCRIPTION
 *      declaration of the PGlobj class.
 *   PGlobj encapsulates a large object interface to Postgres backend 
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/interfaces/libpq++/Attic/pglobject.h,v 1.1 1997/02/13 10:00:35 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
 
#ifndef PGLOBJ_H
#define PGLOBJ_H

#include "pgconnection.h"

// buffer size
#define BUFSIZE 1024


// ****************************************************************
//
// PgLargeObject - a class for accessing Large Object in a database
//
// ****************************************************************
class PgLargeObject : public PgConnection {
private:
  int pgFd;
  Oid pgObject;

public:
  PgLargeObject(const char* dbName = 0);   // use reasonable defaults and create large object
  PgLargeObject(Oid lobjId, const char* dbName = 0); // use reasonable defaults and open large object
  PgLargeObject(const PgEnv& env, const char* dbName);            // create large object
  PgLargeObject(const PgEnv& env, const char* dbName, Oid lobjId); // open large object
  ~PgLargeObject(); // close connection and clean up
  
  void Create();
  void Open();
  void Close()
    { if (pgFd >= 0) lo_close(pgConn, pgFd); }
  int Read(char* buf, int len)
    { return lo_read(pgConn, pgFd, buf, len); }
  int Write(const char* buf, int len)
    { return lo_write(pgConn, pgFd, (char*)buf, len); }
  int LSeek(int offset, int whence)
    { return lo_lseek(pgConn, pgFd, offset, whence); }
  int Tell()
    { return lo_tell(pgConn, pgFd); }
  int Unlink();
  Oid Import(const char* filename) { return pgObject = lo_import(pgConn, (char*)filename); }
  int Export(const char* filename) { return lo_export(pgConn, pgObject, (char*)filename); }
  
private:
   void Init(Oid lobjId = 0);
};

#endif	// PGLOBJ_H
