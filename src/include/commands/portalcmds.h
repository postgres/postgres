/*-------------------------------------------------------------------------
 *
 * portalcmds.h
 *	  prototypes for portalcmds.c.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: portalcmds.h,v 1.10 2003/05/06 20:26:27 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PORTALCMDS_H
#define PORTALCMDS_H

#include "utils/portal.h"


extern void PerformCursorOpen(DeclareCursorStmt *stmt);

extern void PerformPortalFetch(FetchStmt *stmt, DestReceiver *dest,
							   char *completionTag);

extern void PerformPortalClose(const char *name);

extern void PortalCleanup(Portal portal, bool isError);

extern void PersistHoldablePortal(Portal portal);

#endif   /* PORTALCMDS_H */
