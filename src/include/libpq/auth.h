/*-------------------------------------------------------------------------
 *
 * auth.h
 *	  Definitions for network authentication routines
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: auth.h,v 1.12 1999/05/26 12:56:29 momjian Exp $
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

#define PG_KRB4_VERSION "PGVER4.1"		/* at most KRB_SENDAUTH_VLEN chars */
#define PG_KRB5_VERSION "PGVER5.1"

#endif	 /* AUTH_H */
