/*-------------------------------------------------------------------------
 *
 * portalcmds.h
 *	  prototypes for portalcmds.c.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: portalcmds.h,v 1.7 2003/04/29 03:21:30 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PORTALCMDS_H
#define PORTALCMDS_H

#include "utils/portal.h"


extern void PerformCursorOpen(DeclareCursorStmt *stmt, CommandDest dest);

extern void PerformPortalFetch(FetchStmt *stmt, CommandDest dest,
							   char *completionTag);

extern long DoPortalFetch(Portal portal,
						  FetchDirection fdirection,
						  long count,
						  CommandDest dest);

extern void PerformPortalClose(char *name);

extern void PortalCleanup(Portal portal, bool isError);

#endif   /* PORTALCMDS_H */
