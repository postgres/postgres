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
 * $Id: portal.h,v 1.39 2003/03/11 19:40:24 tgl Exp $
 *
 *-------------------------------------------------------------------------
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
	void		(*cleanup) (Portal);	/* Cleanup routine (optional) */
	bool		backwardOK;		/* is fetch backwards allowed? */
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
extern void AtEOXact_portals(void);
extern Portal CreatePortal(const char *name);
extern void PortalDrop(Portal portal);
extern Portal GetPortalByName(const char *name);
extern void PortalSetQuery(Portal portal, QueryDesc *queryDesc,
						   void (*cleanup) (Portal portal));

#endif   /* PORTAL_H */
