/*-------------------------------------------------------------------------
 *
 * portal.h
 *	  POSTGRES portal definitions.
 *
 * A portal is an abstraction which represents the execution state of
 * a running or runnable query.  Portals support both SQL-level CURSORs
 * and protocol-level portals.
 *
 * Scrolling (nonsequential access) and suspension of execution are allowed
 * only for portals that contain a single SELECT-type query.  We do not want
 * to let the client suspend an update-type query partway through!	Because
 * the query rewriter does not allow arbitrary ON SELECT rewrite rules,
 * only queries that were originally update-type could produce multiple
 * parse/plan trees; so the restriction to a single query is not a problem
 * in practice.
 *
 * For SQL cursors, we support three kinds of scroll behavior:
 *
 * (1) Neither NO SCROLL nor SCROLL was specified: to remain backward
 *	   compatible, we allow backward fetches here, unless it would
 *	   impose additional runtime overhead to do so.
 *
 * (2) NO SCROLL was specified: don't allow any backward fetches.
 *
 * (3) SCROLL was specified: allow all kinds of backward fetches, even
 *	   if we need to take a performance hit to do so.  (The planner sticks
 *	   a Materialize node atop the query plan if needed.)
 *
 * Case #1 is converted to #2 or #3 by looking at the query itself and
 * determining if scrollability can be supported without additional
 * overhead.
 *
 * Protocol-level portals have no nonsequential-fetch API and so the
 * distinction doesn't matter for them.  They are always initialized
 * to look like NO SCROLL cursors.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: portal.h,v 1.47 2003/08/08 21:42:55 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PORTAL_H
#define PORTAL_H

#include "executor/execdesc.h"
#include "nodes/memnodes.h"
#include "utils/tuplestore.h"


/*
 * We have several execution strategies for Portals, depending on what
 * query or queries are to be executed.  (Note: in all cases, a Portal
 * executes just a single source-SQL query, and thus produces just a
 * single result from the user's viewpoint.  However, the rule rewriter
 * may expand the single source query to zero or many actual queries.)
 *
 * PORTAL_ONE_SELECT: the portal contains one single SELECT query.	We run
 * the Executor incrementally as results are demanded.	This strategy also
 * supports holdable cursors (the Executor results can be dumped into a
 * tuplestore for access after transaction completion).
 *
 * PORTAL_UTIL_SELECT: the portal contains a utility statement that returns
 * a SELECT-like result (for example, EXPLAIN or SHOW).  On first execution,
 * we run the statement and dump its results into the portal tuplestore;
 * the results are then returned to the client as demanded.
 *
 * PORTAL_MULTI_QUERY: all other cases.  Here, we do not support partial
 * execution: the portal's queries will be run to completion on first call.
 */

typedef enum PortalStrategy
{
	PORTAL_ONE_SELECT,
	PORTAL_UTIL_SELECT,
	PORTAL_MULTI_QUERY
} PortalStrategy;

/*
 * Note: typedef Portal is declared in tcop/dest.h as
 *		typedef struct PortalData *Portal;
 */

typedef struct PortalData
{
	/* Bookkeeping data */
	const char *name;			/* portal's name */
	MemoryContext heap;			/* subsidiary memory for portal */
	void		(*cleanup) (Portal portal, bool isError);		/* cleanup hook */
	TransactionId createXact;	/* the xid of the creating xact */

	/* The query or queries the portal will execute */
	const char *sourceText;		/* text of query, if known (may be NULL) */
	const char *commandTag;		/* command tag for original query */
	List	   *parseTrees;		/* parse tree(s) */
	List	   *planTrees;		/* plan tree(s) */
	MemoryContext queryContext; /* where the above trees live */

	/*
	 * Note: queryContext effectively identifies which prepared statement
	 * the portal depends on, if any.  The queryContext is *not* owned by
	 * the portal and is not to be deleted by portal destruction.  (But
	 * for a cursor it is the same as "heap", and that context is deleted
	 * by portal destruction.)
	 */
	ParamListInfo portalParams; /* params to pass to query */

	/* Features/options */
	PortalStrategy strategy;	/* see above */
	int			cursorOptions;	/* DECLARE CURSOR option bits */

	/* Status data */
	bool		portalReady;	/* PortalStart complete? */
	bool		portalUtilReady;	/* PortalRunUtility complete? */
	bool		portalActive;	/* portal is running (can't delete it) */
	bool		portalDone;		/* portal is finished (don't re-run it) */

	/* If not NULL, Executor is active; call ExecutorEnd eventually: */
	QueryDesc  *queryDesc;		/* info needed for executor invocation */

	/* If portal returns tuples, this is their tupdesc: */
	TupleDesc	tupDesc;		/* descriptor for result tuples */
	/* and these are the format codes to use for the columns: */
	int16	   *formats;		/* a format code for each column */

	/*
	 * Where we store tuples for a held cursor or a PORTAL_UTIL_SELECT
	 * query. (A cursor held past the end of its transaction no longer has
	 * any active executor state.)
	 */
	Tuplestorestate *holdStore; /* store for holdable cursors */
	MemoryContext holdContext;	/* memory containing holdStore */

	/*
	 * atStart, atEnd and portalPos indicate the current cursor position.
	 * portalPos is zero before the first row, N after fetching N'th row
	 * of query.  After we run off the end, portalPos = # of rows in
	 * query, and atEnd is true.  If portalPos overflows, set posOverflow
	 * (this causes us to stop relying on its value for navigation).  Note
	 * that atStart implies portalPos == 0, but not the reverse (portalPos
	 * could have overflowed).
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


/* Prototypes for functions in utils/mmgr/portalmem.c */
extern void EnablePortalManager(void);
extern void AtCommit_Portals(void);
extern void AtAbort_Portals(void);
extern void AtCleanup_Portals(void);
extern Portal CreatePortal(const char *name, bool allowDup, bool dupSilent);
extern Portal CreateNewPortal(void);
extern void PortalDrop(Portal portal, bool isError);
extern void DropDependentPortals(MemoryContext queryContext);
extern Portal GetPortalByName(const char *name);
extern void PortalDefineQuery(Portal portal,
				  const char *sourceText,
				  const char *commandTag,
				  List *parseTrees,
				  List *planTrees,
				  MemoryContext queryContext);
extern void PortalCreateHoldStore(Portal portal);

#endif   /* PORTAL_H */
