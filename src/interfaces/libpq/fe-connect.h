/*-------------------------------------------------------------------------
 *
 * fe-connect.h
 *
 *	  Definitions related to setting up a connection to the backend
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: fe-connect.h,v 1.6 1998/01/26 01:42:30 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FE_CONNECT_H
#define		   FE_CONNECT_H

#include <sys/types.h>

#include "libpq-fe.h"


/*----------------------------------------------------------------
 * Common routines and definitions
 *----------------------------------------------------------------
 */

int packetSend(PGconn *conn, char *buf, size_t len);

#endif							/* FE_CONNECT_H */
