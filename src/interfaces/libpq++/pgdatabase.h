/*-------------------------------------------------------------------------
 *
 * pgdatabase.h
 *    
 *
 *   DESCRIPTION
 *		Postgres Database Class: 
 *		   Query Postgres backend to obtain query results
 *
 *   NOTES
 *      Currently under construction.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 *  $Id: pgdatabase.h,v 1.11 2001/05/09 17:46:11 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
 
#ifndef PGDATABASE_H
#define PGDATABASE_H
 
#ifndef PGCONNECTION_H
#include "pgconnection.h"
#endif

// ****************************************************************
//
// PgDatabase - a class for accessing databases
//
// ****************************************************************
// This is the basic database access class.  Its interface should 
// be used only after a query has been sent to the backend and
// results are being received.
class PgDatabase : public PgConnection {
public:
  // connect to the database with conninfo
  explicit PgDatabase(const char* conninfo) : PgConnection(conninfo) {}

  ~PgDatabase() {}				// close connection and clean up

  typedef int size_type;
  
  // query result access
  size_type Tuples() const;
  size_type CmdTuples() const; 
  int Fields();
  const char* FieldName(int field_num) const;
  int FieldNum(const char* field_name) const;
  Oid FieldType(int field_num) const;
  Oid FieldType(const char* field_name) const;
  int FieldSize(int field_num) const;
  int FieldSize(const char* field_name) const;
  const char* GetValue(size_type tup_num, int field_num) const;
  const char* GetValue(size_type tup_num, const char* field_name) const;
  bool GetIsNull(size_type tup_num, int field_num) const;
  bool GetIsNull(size_type tup_num, const char* field_name) const;
  int GetLength(size_type tup_num, int field_num) const;
  int GetLength(size_type tup_num, const char* field_name) const;

  // OBSOLESCENT (use PQprint()):
  void DisplayTuples(FILE *out=0, bool fillAlign=true, 
	const char* fieldSep="|", bool printHeader=true, bool quiet=false) const;
  void PrintTuples(FILE *out=0, bool printAttName=true, 
	bool terseOutput=false, bool fillAlign=false) const;

  // copy command related access
  int GetLine(char str[], int length);
  void PutLine(const char str[]);
  const char* OidStatus() const;
  int EndCopy();
    
protected:
  PgDatabase() : PgConnection() {}	// Do not connect

private:
// We don't support copying of PgDatabase objects,
// so make copy constructor and assignment op private.
   PgDatabase(const PgDatabase&);
   PgDatabase& operator= (const PgDatabase&);
};

#endif	// PGDATABASE_H
