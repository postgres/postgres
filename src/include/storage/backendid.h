/*-------------------------------------------------------------------------
 *
 * backendid.h--
 *	  POSTGRES backend id communication definitions
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: backendid.h,v 1.4 1997/09/08 02:38:53 momjian Exp $
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

#endif							/* BACKENDID_H */
