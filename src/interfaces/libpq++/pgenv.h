/*-------------------------------------------------------------------------
 *
 * pgenv.h
 *    
 *
 *   DESCRIPTION
 *		Postgres Environment Class: manages and stores all the required
 *		connection variables.
 *
 *   NOTES
 *      Currently under construction.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */
 
#ifndef PGENV_H
#define PGENV_H

#include <string>


//
// these are the environment variables used for getting defaults
//

#define ENV_DEFAULT_AUTH   "PGAUTH"
#define ENV_DEFAULT_DBASE  "PGDATABASE"
#define ENV_DEFAULT_HOST   "PGHOST"
#define ENV_DEFAULT_OPTION "PGOPTION"
#define ENV_DEFAULT_PORT   "PGPORT"
#define ENV_DEFAULT_TTY    "PGTTY"
 

// ****************************************************************
//
// PgEnv - the environment for setting up a connection to postgres
//
// ****************************************************************
class PgEnv {
private:
  string pgAuth;
  string pgHost;
  string pgPort;
  string pgOption;
  string pgTty;
  
public:
  PgEnv();  // default ctor will use reasonable defaults
            // will use environment  variables PGHOST, PGPORT,
            // PGOPTION, PGTTY
  PgEnv(const string& auth, const string& host, const string& port, 
        const string& option, const string& tty);
  
  // Access methods to all the environment variables
  const char* Auth() { return pgAuth.c_str(); }
  void Auth(const string& auth) { pgAuth = auth; }
  
  const char* Host() { return pgHost.c_str(); }
  void Host(const string& host) { pgHost = host; }
  
  const char* Port() { return pgPort.c_str(); }
  void Port(const string& port) { pgPort = port; }
  
  const char* Option() { return pgOption.c_str(); }
  void Option(const string& option) { pgOption = option; }
  
  const char* TTY() { return pgTty.c_str(); }
  void TTY(const string& tty) { pgTty = tty; }
  
  void SetValues(const string& auth, const string& host, const string& port, 
                 const string& option, const string& tty);
                 
protected:
  string getenv(const char*);
};

#endif	// PGENV_H
