/*-------------------------------------------------------------------------
 *
 * backendid.h
 *	  POSTGRES backend id communication definitions
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/backendid.h,v 1.24 2010/08/14 13:37:21 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef BACKENDID_H
#define BACKENDID_H

/* ----------------
 *		-cim 8/17/90
 * ----------------
 */
typedef int BackendId;			/* unique currently active backend identifier */

#define InvalidBackendId		(-1)

extern PGDLLIMPORT BackendId MyBackendId;	/* backend id of this backend */

#endif   /* BACKENDID_H */
