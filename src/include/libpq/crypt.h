/*-------------------------------------------------------------------------
 *
 * crypt.h
 *	  Interface to libpq/crypt.c
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/libpq/crypt.h,v 1.35 2006/06/20 19:56:52 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CRYPT_H
#define PG_CRYPT_H

#include "libpq/libpq-be.h"

extern int md5_crypt_verify(const Port *port, const char *user,
				 char *client_pass);

#endif
