/*-------------------------------------------------------------------------
 *
 *   FILE
 *	pglobject.h
 *
 *   DESCRIPTION
 *      declaration of the PGlobj class.
 *   PGlobj encapsulates a large object interface to Postgres backend 
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 *  $Id: pglobject.h,v 1.7 2001/05/09 17:29:10 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
 
#ifndef PGLOBJECT_H
#define PGLOBJECT_H
 
#ifndef PGCONNECTION_H
#include "pgconnection.h"
#endif

#ifdef HAVE_NAMESPACE_STD
#define PGSTD std::
#else
#define PGSTD
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
  PGSTD string loStatus;
  void Init(Oid lobjId = 0);

public:
  explicit PgLargeObject(const char* conninfo = 0);   // use reasonable defaults and create large object
  explicit PgLargeObject(Oid lobjId, const char* conninfo = 0); // use reasonable defaults and open large object
  ~PgLargeObject(); // close connection and clean up
  
  void Create();
  void Open();
  void Close();
  int Read(char* buf, int len);
  int Write(const char* buf, int len);
  int LSeek(int offset, int whence);
  int Tell() const;
  int Unlink();
  Oid LOid();
  Oid Import(const char* filename);
  int Export(const char* filename); 
  PGSTD string Status() const;

private:
// We don't support copying of PgLargeObject objects,
// so make copy constructor and assignment op private.
   PgLargeObject(const PgLargeObject&);
   PgLargeObject& operator= (const PgLargeObject&);
};


#ifdef HAVE_NAMESPACE_STD
#undef PGSTD
#endif


#endif	// PGLOBJECT_H
