/*-------------------------------------------------------------------------
 *
 * backendid.h
 *	  POSTGRES backend id communication definitions
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: backendid.h,v 1.7 2000/01/26 05:58:32 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef BACKENDID_H
#define BACKENDID_H

/* ----------------
 *		-cim 8/17/90
 * ----------------
 */
typedef int16 BackendId;		/* unique currently active backend
								 * identifier */

#define InvalidBackendId		(-1)

typedef int32 BackendTag;		/* unique backend identifier */

#define InvalidBackendTag		(-1)

extern BackendId MyBackendId;	/* backend id of this backend */
extern BackendTag MyBackendTag; /* backend tag of this backend */

#endif	 /* BACKENDID_H */
