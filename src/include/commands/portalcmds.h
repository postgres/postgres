/*-------------------------------------------------------------------------
 *
 * portalcmds.h
 *	  prototypes for portalcmds.c.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: portalcmds.h,v 1.1 2002/04/15 05:22:03 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PORTALCMDS_H
#define PORTALCMDS_H

#include "utils/portal.h"

/*
 * PerformPortalFetch
 *		Performs the POSTQUEL function FETCH.  Fetches count (or all if 0)
 * tuples in portal with name in the forward direction iff goForward.
 *
 * Exceptions:
 *		BadArg if forward invalid.
 *		"ERROR" if portal not found.
 */
extern void PerformPortalFetch(char *name, bool forward, int count,
							   CommandDest dest, char *completionTag);

/*
 * PerformPortalClose
 *		Performs the POSTQUEL function CLOSE.
 */
extern void PerformPortalClose(char *name, CommandDest dest);

extern void PortalCleanup(Portal portal);

#endif  /* PORTALCMDS_H */
