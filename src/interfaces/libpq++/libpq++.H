/*-------------------------------------------------------------------------
 *
 * libpq++.H
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
 *   IDENTIFICATION
 *
 * $Id: libpq++.H,v 1.3 1996/11/12 11:42:27 bryanh Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef LIBPQXX_H
#define LIBPQXX_H

#include <stdio.h>
#include <strings.h>

extern "C" {
#include "config.h"
#include "postgres.h"
#include "libpq-fe.h"
#include "fe-auth.h"
}

// ****************************************************************
//
// PGenv - the environment for setting up a connection to postgres
//
// ****************************************************************
class PGenv {
  friend class PGconnection;
  char* pgauth;
  char* pghost;
  char* pgport;
  char* pgoption;
  char* pgtty;
public:
  PGenv();  // default ctor will use reasonable defaults
            // will use environment  variables PGHOST, PGPORT,
	    // PGOPTION, PGTTY
  PGenv(char* auth, char* host, char* port, char* option, char* tty);
   void setValues(char* auth, char* host, char* port, char* option, char* tty);
   ~PGenv();
};

// ****************************************************************
//
// PGconnection - a connection made to a postgres backend
//
// ****************************************************************
class PGconnection {
  friend class PGdatabase;
  friend class PGlobj;
  PGenv* env; 
  PGconn* conn; 
  PGresult* result;
  
  char errorMessage[ERROR_MSG_LENGTH];
public:
   PGconnection(); // use reasonable defaults
   PGconnection(PGenv* env, char* dbName); // connect to the database with 
                                    // given environment and database name
   ConnStatusType status();
   char* errormessage() {return errorMessage;};
  
  // returns the database name of the connection
    char* dbName() {return PQdb(conn);}; 

    ExecStatusType exec(char* query);  // send a query to the backend
    PGnotify* notifies() {exec(" "); return PQnotifies(conn);};
    ~PGconnection(); // close connection and clean up
protected:
  ConnStatusType connect(PGenv* env, char* dbName);
};

// ****************************************************************
//
// PGdatabase - a class for accessing databases
//
// ****************************************************************
class PGdatabase : public PGconnection {
public:
  PGdatabase() : PGconnection() {}; // use reasonable defaults
  // connect to the database with 
  PGdatabase(PGenv* env, char* dbName) : PGconnection(env, dbName) {};
  // query result access
  int ntuples()
    {return PQntuples(result);};
  int nfields()
    {return PQnfields(result);};
  char* fieldname(int field_num)
    {return PQfname(result, field_num);};
  int fieldnum(char* field_name)
    {return PQfnumber(result, field_name);};
  Oid fieldtype(int field_num)
    {return PQftype(result, field_num);};
  Oid fieldtype(char* field_name)
    {return PQftype(result, fieldnum(field_name));};
  int2 fieldsize(int field_num)
    {return PQfsize(result, field_num);};
  int2 fieldsize(char* field_name)
    {return PQfsize(result, fieldnum(field_name));};
  char* getvalue(int tup_num, int field_num)
    {return PQgetvalue(result, tup_num, field_num);};
  char* getvalue(int tup_num, char* field_name)
    {return PQgetvalue(result, tup_num, fieldnum(field_name));};
  int getlength(int tup_num, int field_num)
    {return PQgetlength(result, tup_num, field_num);};
  int getlength(int tup_num, char* field_name)
    {return PQgetlength(result, tup_num, fieldnum(field_name));};
  void printtuples(FILE *out, int fillAlign, char *fieldSep,
		   int printHeader, int quiet)
    {PQdisplayTuples(result, out, fillAlign, fieldSep, printHeader, quiet);};
  // copy command related access
  int getline(char* string, int length)
    {return PQgetline(conn, string, length);};
  void putline(char* string)
    {PQputline(conn, string);};
  const char *OidStatus()
  {
    return PQoidStatus(result);
  }
  int endcopy()
    {return PQendcopy(conn);};
  ~PGdatabase() {}; // close connection and clean up
};

// ****************************************************************
//
// PGlobj - a class for accessing Large Object in a database
//
// ****************************************************************
class PGlobj : public PGconnection {
  int fd;
  Oid object;
public:
  PGlobj();          // use reasonable defaults and create large object
  PGlobj(Oid lobjId); // use reasonable defaults and open large object
  PGlobj(PGenv* env, char* dbName);            // create large object
  PGlobj(PGenv* env, char* dbName, Oid lobjId); // open large object
   int read(char* buf, int len)
    {return lo_read(conn, fd, buf, len);};
   int write(char* buf, int len)
    {return lo_write(conn, fd, buf, len);};
   int lseek(int offset, int whence)
    {return lo_lseek(conn, fd, offset, whence);};
   int tell()
    {return lo_tell(conn, fd);};
   int unlink();
   int import(char* filename);
   int export(char* filename);
   ~PGlobj(); // close connection and clean up
};

//
// these are the environment variables used for getting defaults
//

#define ENV_DEFAULT_AUTH   "PGAUTH"
#define ENV_DEFAULT_DBASE  "PGDATABASE"
#define ENV_DEFAULT_HOST   "PGHOST"
#define ENV_DEFAULT_OPTION "PGOPTION"
#define ENV_DEFAULT_PORT   "PGPORT"
#define ENV_DEFAULT_TTY    "PGTTY"

// buffer size
#define BUFSIZE 1024

#endif /* LIBPQXX_H */
