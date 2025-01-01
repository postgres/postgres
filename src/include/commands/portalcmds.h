/*-------------------------------------------------------------------------
 *
 * portalcmds.h
 *	  prototypes for portalcmds.c.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/portalcmds.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PORTALCMDS_H
#define PORTALCMDS_H

#include "nodes/parsenodes.h"
#include "parser/parse_node.h"
#include "utils/portal.h"


extern void PerformCursorOpen(ParseState *pstate, DeclareCursorStmt *cstmt, ParamListInfo params,
							  bool isTopLevel);

extern void PerformPortalFetch(FetchStmt *stmt, DestReceiver *dest,
							   QueryCompletion *qc);

extern void PerformPortalClose(const char *name);

extern void PortalCleanup(Portal portal);

extern void PersistHoldablePortal(Portal portal);

#endif							/* PORTALCMDS_H */
