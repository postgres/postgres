/*-------------------------------------------------------------------------
 *
 * portalcmds.h
 *	  prototypes for portalcmds.c.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: portalcmds.h,v 1.5 2003/03/10 03:53:51 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PORTALCMDS_H
#define PORTALCMDS_H

#include "utils/portal.h"


extern void PerformCursorOpen(DeclareCursorStmt *stmt, CommandDest dest);

extern void PerformPortalFetch(char *name, bool forward, long count,
				   CommandDest dest, char *completionTag);

extern long DoPortalFetch(Portal portal, bool forward, long count,
						  CommandDest dest);

extern void PerformPortalClose(char *name);

extern void PortalCleanup(Portal portal);

#endif   /* PORTALCMDS_H */
