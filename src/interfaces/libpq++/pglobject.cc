
/*-------------------------------------------------------------------------
 *
 *   FILE
 *	pglobject.cc
 *
 *   DESCRIPTION
 *      implementation of the PGlobj class.
 *   PGlobj encapsulates a frontend to backend connection
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/interfaces/libpq++/Attic/pglobject.cc,v 1.1.1.1 1996/07/09 06:22:18 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "libpq++.H"

extern "C" {
#include "libpq/libpq-fs.h"
}

// default constructor
// creates a large object in the default database
PGlobj::PGlobj() : PGconnection::PGconnection() {
  object = lo_creat(conn, INV_READ|INV_WRITE);
  if (object == 0) {
    sprintf(errorMessage, "PGlobj: can't create large object");
  }
  fd = lo_open(conn, object, INV_READ|INV_WRITE);
  if (fd < 0) {
    sprintf(errorMessage, "PGlobj: can't open large object %d", object);
  } else
    sprintf(errorMessage, "PGlobj: created and opened large object %d",
	    object);
   
}

// constructor
// open an existing large object in the default database
PGlobj::PGlobj(Oid lobjId) : PGconnection::PGconnection() {
  object = lobjId;
  fd = lo_open(conn, object, INV_READ|INV_WRITE);
  if (fd < 0) {
    sprintf(errorMessage, "PGlobj: can't open large object %d", object);
  } else
    sprintf(errorMessage, "PGlobj: opened large object %d",
	    object);
}

// constructor
// create a large object in the given database
PGlobj::PGlobj(PGenv* env, char* dbName) : PGconnection::PGconnection(env,dbName) {
  object = lo_creat(conn, INV_READ|INV_WRITE);
  if (object == 0) {
    sprintf(errorMessage, "PGlobj: can't create large object");
  }
  fd = lo_open(conn, object, INV_READ|INV_WRITE);
  if (fd < 0) {
    sprintf(errorMessage, "PGlobj: can't open large object %d", object);
  } else
    sprintf(errorMessage, "PGlobj: created and opened large object %d",
	    object);
}

// constructor
// open an existing large object in the given database
PGlobj::PGlobj(PGenv* env, char* dbName, Oid lobjId) : PGconnection::PGconnection(env,dbName) {
  object = lobjId;
  fd = lo_open(conn, object, INV_READ|INV_WRITE);
  if (fd < 0) {
    sprintf(errorMessage, "PGlobj: can't open large object %d", object);
  } else
    sprintf(errorMessage, "PGlobj: created and opened large object %d",
	    object);
}

// PGlobj::unlink
// destruct large object and delete from it from the database
int
PGlobj::unlink() {
  int temp = lo_unlink(conn, object);
  if (temp) {
    return temp;
  } else {
    delete this;
    return temp;
  }
}

// PGlobj::import -- import a given file into the large object
int
PGlobj::import(char* filename) {
    char buf[BUFSIZE];
    int nbytes, tmp;
    int in_fd;

    // open the file to be read in
    in_fd = open(filename, O_RDONLY, 0666);
    if (in_fd < 0)  {   /* error */
	sprintf(errorMessage, "PGlobj::import: can't open unix file\"%s\"", filename);
	return -1;
    }

    // read in from the Unix file and write to the inversion file
    while ((nbytes = ::read(in_fd, buf, BUFSIZE)) > 0) {
      tmp = lo_write(conn, fd, buf, nbytes);
      if (tmp < nbytes) {
       sprintf(errorMessage, "PGlobj::import: error while reading \"%s\"",
         filename);
	  return -1;
      }
    }
    
    (void) close(in_fd);
    return 0;
}

// PGlobj::export -- export large object to given file
int
PGlobj::export(char* filename) {
    int out_fd;
    char buf[BUFSIZE];
    int nbytes, tmp;

    // open the file to be written to
    out_fd = open(filename, O_CREAT|O_WRONLY, 0666);
    if (out_fd < 0)  {   /* error */
	sprintf(errorMessage, "PGlobj::export: can't open unix file\"%s\"",
		filename);
	return -1;
    }

    // read in from the Unix file and write to the inversion file
    while ((nbytes = lo_read(conn, fd, buf, BUFSIZE)) > 0) {
      tmp = ::write(out_fd, buf, nbytes);
      if (tmp < nbytes) {
        sprintf(errorMessage,"PGlobj::export: error while writing \"%s\"",
	    filename);
	return -1;
      }
    }
    (void) close(out_fd);
    return 0;
}

// default destructor -- closes large object
PGlobj::~PGlobj() {
  if (fd >= 0)
    lo_close(conn, fd);
}
