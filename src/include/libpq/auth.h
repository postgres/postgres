/*-------------------------------------------------------------------------
 *
 * auth.h--
 *	  Definitions for network authentication routines
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: auth.h,v 1.10 1998/09/01 04:36:23 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef AUTH_H
#define AUTH_H

#include "libpq/libpq-be.h"

/*----------------------------------------------------------------
 * Common routines and definitions
 *----------------------------------------------------------------
 */

void		be_recvauth(Port *port);
void		auth_failed(Port *port);

#define PG_KRB4_VERSION "PGVER4.1"		/* at most KRB_SENDAUTH_VLEN chars */
#define PG_KRB5_VERSION "PGVER5.1"

#endif	 /* AUTH_H */
