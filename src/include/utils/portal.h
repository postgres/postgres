/*-------------------------------------------------------------------------
 *
 * portal.h
 *	  POSTGRES portal definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: portal.h,v 1.26 2001/02/27 22:07:34 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * Note:
 *		A portal is an abstraction which represents the execution state of
 * a running query (specifically, a CURSOR).
 */
#ifndef PORTAL_H
#define PORTAL_H

#include "executor/execdesc.h"
#include "nodes/memnodes.h"


typedef struct PortalData *Portal;

typedef struct PortalData
{
	char	   *name;			/* Portal's name */
	MemoryContext heap;			/* subsidiary memory */
	QueryDesc  *queryDesc;		/* Info about query associated with portal */
	TupleDesc	attinfo;
	EState	   *state;			/* Execution state of query */
	bool		atStart;		/* T => fetch backwards is not allowed */
	bool		atEnd;			/* T => fetch forwards is not allowed */
	void		(*cleanup) (Portal); /* Cleanup routine (optional) */
} PortalData;

/*
 * PortalIsValid
 *		True iff portal is valid.
 */
#define PortalIsValid(p) PointerIsValid(p)

/*
 * Access macros for Portal ... use these in preference to field access.
 */
#define PortalGetQueryDesc(portal)  ((portal)->queryDesc)
#define PortalGetTupleDesc(portal)  ((portal)->attinfo)
#define PortalGetState(portal)  ((portal)->state)
#define PortalGetHeapMemory(portal)  ((portal)->heap)

/*
 * estimate of the maximum number of open portals a user would have,
 * used in initially sizing the PortalHashTable in EnablePortalManager()
 */
#define PORTALS_PER_USER	   10


extern void EnablePortalManager(void);
extern void AtEOXact_portals(void);
extern Portal CreatePortal(char *name);
extern void PortalDrop(Portal *portalP);
extern Portal GetPortalByName(char *name);
extern void PortalSetQuery(Portal portal, QueryDesc *queryDesc,
			   TupleDesc attinfo, EState *state,
			   void (*cleanup) (Portal portal));


#endif	 /* PORTAL_H */
