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
 * Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */
 
#ifndef PGDATABASE_H
#define PGDATABASE_H
 
#include "pgconnection.h"


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
  PgDatabase(const char* dbName) : PgConnection(dbName) {} // use reasonable defaults
  // connect to the database with given environment and database name
  PgDatabase(const PgEnv& env, const char* dbName) : PgConnection(env, dbName) {}
  PgDatabase(const PgConnection& conn) : PgConnection(conn) {pgCloseConnection = 0;}
  ~PgDatabase() {} // close connection and clean up
  
  // query result access
  int Tuples()
    { return PQntuples(pgResult); }
  int Fields()
    { return PQnfields(pgResult); }
  const char* FieldName(int field_num)
    { return PQfname(pgResult, field_num); }
  int FieldNum(const char* field_name)
    { return PQfnumber(pgResult, field_name); }
  Oid FieldType(int field_num)
    { return PQftype(pgResult, field_num); }
  Oid FieldType(const char* field_name)
    { return PQftype(pgResult, FieldNum(field_name)); }
  short FieldSize(int field_num)
    { return PQfsize(pgResult, field_num); }
  short FieldSize(const char* field_name)
    { return PQfsize(pgResult, FieldNum(field_name)); }
  const char* GetValue(int tup_num, int field_num)
    { return PQgetvalue(pgResult, tup_num, field_num); }
  const char* GetValue(int tup_num, const char* field_name)
    { return PQgetvalue(pgResult, tup_num, FieldNum(field_name)); }
  int GetLength(int tup_num, int field_num)
    { return PQgetlength(pgResult, tup_num, field_num); }
  int GetLength(int tup_num, const char* field_name)
    { return PQgetlength(pgResult, tup_num, FieldNum(field_name)); }
  void DisplayTuples(FILE *out = 0, int fillAlign = 1, const char* fieldSep = "|",
		   int printHeader = 1, int quiet = 0)
    { PQdisplayTuples(pgResult, (out ? out : stdout), fillAlign, fieldSep, printHeader, quiet); }
  void PrintTuples(FILE *out = 0, int printAttName = 1, int terseOutput = 0, int width = 0)
    { PQprintTuples(pgResult, (out ? out : stdout), printAttName, terseOutput, width); }

  // copy command related access
  int GetLine(char* string, int length)
    { return PQgetline(pgConn, string, length); }
  void PutLine(const char* string)
    { PQputline(pgConn, string); }
  const char* OidStatus()
    { return PQoidStatus(pgResult); }
  int EndCopy()
    { return PQendcopy(pgConn); }
    
protected:
  PgDatabase() : PgConnection() {}	// Do not connect
};

#endif	// PGDATABASE_H
