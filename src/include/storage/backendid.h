/*-------------------------------------------------------------------------
 *
 * backendid.h
 *	  POSTGRES backend id communication definitions
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: backendid.h,v 1.8 2000/11/12 20:51:52 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef BACKENDID_H
#define BACKENDID_H

/* ----------------
 *		-cim 8/17/90
 * ----------------
 */
typedef int BackendId;			/* unique currently active backend
								 * identifier */

#define InvalidBackendId		(-1)

extern BackendId MyBackendId;	/* backend id of this backend */

#endif	 /* BACKENDID_H */
