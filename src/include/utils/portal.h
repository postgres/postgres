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
 * $Id: portal.h,v 1.38 2003/03/10 03:53:52 tgl Exp $
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
	bool		backwardOK;		/* is fetch backwards allowed at all? */
	bool		atStart;		/* T => fetch backwards is not allowed now */
	bool		atEnd;			/* T => fetch forwards is not allowed now */
	void		(*cleanup) (Portal);	/* Cleanup routine (optional) */
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
