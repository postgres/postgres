/*-------------------------------------------------------------------------
 *
 *   FILE
 *	pglobject.h
 *
 *   DESCRIPTION
 *      declaration of the PGlobj class.
 *   PGlobj encapsulates a large object interface to Postgres backend 
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 *  $Id: pglobject.h,v 1.5 2000/04/22 22:39:15 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
 
#ifndef PGLOBJECT_H
#define PGLOBJECT_H
 
#ifndef PGCONNECTION_H
#include "pgconnection.h"
#endif


// ****************************************************************
//
// PgLargeObject - a class for accessing Large Object in a database
//
// ****************************************************************
class PgLargeObject : public PgConnection {
private:
  int pgFd;
  Oid pgObject;
  string loStatus;
  void Init(Oid lobjId = 0);

public:
  PgLargeObject(const char* conninfo = 0);   // use reasonable defaults and create large object
  PgLargeObject(Oid lobjId, const char* conninfo = 0); // use reasonable defaults and open large object
  ~PgLargeObject(); // close connection and clean up
  
  void Create();
  void Open();
  void Close();
  int Read(char* buf, int len);
  int Write(const char* buf, int len);
  int LSeek(int offset, int whence);
  int Tell();
  int Unlink();
  Oid LOid();
  Oid Import(const char* filename);
  int Export(const char* filename); 
  string Status();

private:
// We don't support copying of PgLargeObject objects,
// so make copy constructor and assignment op private.
   PgLargeObject(const PgLargeObject&);
   PgLargeObject& operator= (const PgLargeObject&);
};

#endif	// PGLOBJECT_H
