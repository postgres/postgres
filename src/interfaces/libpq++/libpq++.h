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
 *      This is intended to be included by client applications.
 *	 It will not work as an inclusion in the libpq++ sources, since
 *	 in the build environment the individual include files are not
 *   yet installed in a subdirectory.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: libpq++.h,v 1.9 2000/04/22 22:39:15 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef LIBPQXX_H
#define LIBPQXX_H

#include "libpq++/pgconnection.h"
#include "libpq++/pgdatabase.h"
#include "libpq++/pglobject.h"
#include "libpq++/pgtransdb.h"
#include "libpq++/pgcursordb.h"

#endif /* LIBPQXX_H */
