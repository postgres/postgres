/*-------------------------------------------------------------------------
 *
 * portal.h
 *	  POSTGRES portal definitions.
 *
 * A portal is an abstraction which represents the execution state of
 * a running query (specifically, a CURSOR).
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: portal.h,v 1.41 2003/04/29 03:21:30 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PORTAL_H
#define PORTAL_H

#include "executor/execdesc.h"
#include "nodes/memnodes.h"
#include "utils/tuplestore.h"

/*
 * We support three kinds of scroll behavior:
 *
 * (1) Neither NO SCROLL nor SCROLL was specified: to remain backward
 *     compatible, we allow backward fetches here, unless it would
 *     impose additional runtime overhead to do so.
 *
 * (2) NO SCROLL was specified: don't allow any backward fetches.
 *
 * (3) SCROLL was specified: allow all kinds of backward fetches, even
 *     if we need to take a slight performance hit to do so.
 *
 * Case #1 is converted to #2 or #3 by looking at the query itself and
 * determining if scrollability can be supported without additional
 * overhead.
 */
typedef enum
{
	DEFAULT_SCROLL,
	DISABLE_SCROLL,
	ENABLE_SCROLL
} ScrollType;

typedef struct PortalData *Portal;

typedef struct PortalData
{
	char	   *name;			/* Portal's name */
	MemoryContext heap;			/* subsidiary memory */
	QueryDesc  *queryDesc;		/* Info about query associated with portal */
	void		(*cleanup) (Portal portal, bool isError);	/* Cleanup hook */
	ScrollType	scrollType;		/* Allow backward fetches? */
	bool		executorRunning;	/* T if we need to call ExecutorEnd */
	bool		holdOpen;		/* hold open after xact ends? */
	TransactionId createXact;	/* the xid of the creating xact */
	Tuplestorestate *holdStore;	/* store for holdable cursors */
	MemoryContext holdContext;  /* memory containing holdStore */

	/*
	 * atStart, atEnd and portalPos indicate the current cursor position.
	 * portalPos is zero before the first row, N after fetching N'th row of
	 * query.  After we run off the end, portalPos = # of rows in query, and
	 * atEnd is true.  If portalPos overflows, set posOverflow (this causes
	 * us to stop relying on its value for navigation).  Note that atStart
	 * implies portalPos == 0, but not the reverse (portalPos could have
	 * overflowed).
	 */
	bool		atStart;
	bool		atEnd;
	bool		posOverflow;
	long		portalPos;
} PortalData;

/*
 * PortalIsValid
 *		True iff portal is valid.
 */
#define PortalIsValid(p) PointerIsValid(p)

/*
 * Access macros for Portal ... use these in preference to field access.
 */
#define PortalGetQueryDesc(portal)	((portal)->queryDesc)
#define PortalGetHeapMemory(portal) ((portal)->heap)


extern void EnablePortalManager(void);
extern void AtEOXact_portals(bool isCommit);
extern Portal CreatePortal(const char *name);
extern void PortalDrop(Portal portal, bool isError);
extern Portal GetPortalByName(const char *name);
extern void PortalSetQuery(Portal portal, QueryDesc *queryDesc);
extern void PersistHoldablePortal(Portal portal);

#endif   /* PORTAL_H */
