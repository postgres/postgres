/*-------------------------------------------------------------------------
 *
 * auth.h--
 *	  Definitions for network authentication routines
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: auth.h,v 1.9 1998/02/26 04:41:35 momjian Exp $
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

#endif							/* AUTH_H */
