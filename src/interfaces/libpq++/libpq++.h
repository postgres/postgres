
/*-------------------------------------------------------------------------
 *
 * libpq++.h
 *    
 *
 *   DESCRIPTION
 *	C++ client interface to Postgres
 *   used for building front-end applications
 *
 *   NOTES
 *      Currently under construction.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 *-------------------------------------------------------------------------
 */

#ifndef LIBPQXX_H
#define LIBPQXX_H

#include <stdio.h>
#include <strings.h>
#include <string>

extern "C" {
#include "config.h"
#include "postgres.h"
#include "libpq-fe.h"
}

static char rcsid[] = "$Id: libpq++.h,v 1.3 1999/12/03 17:35:05 momjian Exp $";


// ****************************************************************
//
// PgConnection - a connection made to a postgres backend
//
// ****************************************************************
class PgConnection {
protected:
  PGconn* pgConn;	// Connection Structures
  PGresult* pgResult;	// Query Result
  int pgCloseConnection; // Flag indicating whether the connection should be closed
  ConnStatusType Connect(const char* conninfo);
  string IntToString(int);
  PgConnection();
  
public:
  PgConnection(const char* conninfo); // use reasonable and environment defaults 
  ~PgConnection(); // close connection and clean up
  
  ConnStatusType Status();
  int ConnectionBad();
  const char* ErrorMessage();
  
  // returns the database name of the connection
  const char* DBName(); 
  
  ExecStatusType Exec(const char* query);  // send a query to the backend
  int ExecCommandOk(const char* query);    // send a command and check if it's
  int ExecTuplesOk(const char* query);     // send a command and check if tuple
  PGnotify* Notifies();
};

// ****************************************************************
//
// PgDatabase - a class for accessing databases
//
// ****************************************************************
class PgDatabase : public PgConnection {
protected:
  PgDatabase() : PgConnection() {}	// Do not connect

public:
  // connect to the database with conninfo
  PgDatabase(const char *conninfo) : PgConnection(conninfo) {};
  ~PgDatabase() {}; // close connection and clean up
  // query result access
  int Tuples();
  int CmdTuples();
  int Fields();
  const char* FieldName(int field_num);
  int FieldNum(const char *field_name);
  Oid FieldType(int field_num);
  Oid FieldType(const char *field_name);
  short FieldSize(int field_num);
  short FieldSize(const char *field_name);
  const char* GetValue(int tup_num, int field_num);
  const char* GetValue(int tup_num, const char *field_name);
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
  const char *OidStatus();
  int EndCopy();
};



// ****************************************************************
//
// PGLargeObject - a class for accessing Large Object in a database
//
// ****************************************************************
class PgLargeObject : public PgConnection {
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
};


// ****************************************************************
//
// PgTransaction - a class for running transactions against databases
//
// ****************************************************************
class PgTransaction : public PgDatabase {
protected:
  ExecStatusType BeginTransaction();
  ExecStatusType EndTransaction();
  PgTransaction() : PgDatabase() {}	// Do not connect

public:
  PgTransaction(const char* conninfo);  // use reasonable & environment defaults
  // connect to the database with given environment and database name
  PgTransaction(const PgConnection&);
  virtual ~PgTransaction();     // close connection and clean up

};


// ****************************************************************
//
// PgCursor - a class for querying databases using a cursor
//
// ****************************************************************
class PgCursor : public PgTransaction {
protected:
  int Fetch(const string& num, const string& dir);
  string pgCursor;
  PgCursor() : PgTransaction() {}	// Do not connect

public:
  PgCursor(const char* dbName, const char* cursor);     // use reasonable & environment defaults
  // connect to the database with given environment and database name
  PgCursor(const PgConnection&, const char* cursor);
  virtual ~PgCursor();  // close connection and clean up

  // Commands associated with cursor interface
  int Declare(const string& query, int binary = 0);     // Declare a cursor with given name
  int Fetch(const char* dir = "FORWARD");               // Fetch ALL tuples in given direction
  int Fetch(unsigned num, const char* dir = "FORWARD"); // Fetch specified amount of tuples
  int Close();  // Close the cursor

  // Accessors to the cursor name
  const char* Cursor();
  void Cursor(const string& cursor);
};



// buffer size
#define BUFSIZE 1024

#endif /* LIBPQXX_H */


