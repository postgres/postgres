/*-------------------------------------------------------------------------
 *
 * portal.h
 *	  POSTGRES portal definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: portal.h,v 1.25 2001/01/24 19:43:28 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * Note:
 *		A portal is an abstraction which represents the execution state of
 * a running query (or a fixed sequence of queries).
 *
 * Note:
 *		now that PQ calls can be made from within a backend, a portal
 *		may also be used to keep track of the tuples resulting
 *		from the execution of a query.	In this case, entryIndex
 */
#ifndef PORTAL_H
#define PORTAL_H

#include "executor/execdesc.h"
#include "nodes/memnodes.h"


typedef struct PortalD *Portal;

typedef struct PortalD
{
	char	   *name;			/* Portal's name */
	MemoryContext heap;			/* subsidiary memory */
	QueryDesc  *queryDesc;		/* Info about query associated with portal */
	TupleDesc	attinfo;
	EState	   *state;
	void		(*cleanup) (Portal); /* Cleanup routine (optional) */
} PortalD;

/*
 * PortalIsValid
 *		True iff portal is valid.
 */
#define PortalIsValid(p) PointerIsValid(p)

extern void EnablePortalManager(void);
extern void AtEOXact_portals(void);
extern Portal CreatePortal(char *name);
extern void PortalDrop(Portal *portalP);
extern Portal GetPortalByName(char *name);
extern void PortalSetQuery(Portal portal, QueryDesc *queryDesc,
			   TupleDesc attinfo, EState *state,
			   void (*cleanup) (Portal portal));
extern QueryDesc *PortalGetQueryDesc(Portal portal);
extern EState *PortalGetState(Portal portal);
extern MemoryContext PortalGetHeapMemory(Portal portal);

/* estimate of the maximum number of open portals a user would have,
 * used in initially sizing the PortalHashTable in EnablePortalManager()
 */
#define PORTALS_PER_USER	   10


#endif	 /* PORTAL_H */
