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
 *
 *  $Id: pgdatabase.h,v 1.5 1999/10/06 03:00:16 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
 
#ifndef PGDATABASE_H
#define PGDATABASE_H
 
#include "pgconnection.h"

#include <stdlib.h>

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
  PgDatabase(const char* conninfo) : PgConnection(conninfo) {} // use reasonable defaults
  ~PgDatabase() {} ; // close connection and clean up
  
  // query result access
  int Tuples();
  int CmdTuples(); 
  int Fields();
  const char* FieldName(int field_num);
  int FieldNum(const char* field_name);
  Oid FieldType(int field_num);
  Oid FieldType(const char* field_name);
  short FieldSize(int field_num);
  short FieldSize(const char* field_name);
  const char* GetValue(int tup_num, int field_num);
  const char* GetValue(int tup_num, const char* field_name);
  int GetIsNull(int tup_num, int field_num);
  int GetIsNull(int tup_num, const char* field_name);
  int GetLength(int tup_num, int field_num);
  int GetLength(int tup_num, const char* field_name);
  void DisplayTuples(FILE *out = 0, int fillAlign = 1, 
	const char* fieldSep = "|",int printHeader = 1, int quiet = 0) ;
  void PrintTuples(FILE *out = 0, int printAttName = 1, 
	int terseOutput = 0, int width = 0) ;

  // copy command related access
  int GetLine(char* string, int length);
  void PutLine(const char* string);
  const char* OidStatus();
  int EndCopy();
    
protected:
  PgDatabase() : PgConnection() {}	// Do not connect
};

#endif	// PGDATABASE_H
