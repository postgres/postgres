/*-------------------------------------------------------------------------
 *
 * portal.h
 *	  POSTGRES portal definitions.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: portal.h,v 1.23 2000/04/12 17:16:55 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * Note:
 *		A portal is an abstraction which represents the execution state of
 * a running query (or a fixed sequence of queries).  The "blank portal" is
 * a portal with an InvalidName.  This blank portal is in existance except
 * between calls to BlankPortalAssignName and GetPortalByName(NULL).
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

typedef struct PortalBlockData
{
	AllocSetData setData;
	FixedItemData itemData;
} PortalBlockData;

typedef PortalBlockData *PortalBlock;

typedef struct PortalD PortalD;
typedef PortalD *Portal;

struct PortalD
{
	char	   *name;			/* XXX PortalName */
	struct PortalVariableMemoryData variable;
	struct PortalHeapMemoryData heap;
	QueryDesc  *queryDesc;
	TupleDesc	attinfo;
	EState	   *state;
	void		(*cleanup) (Portal);
};

/*
 * PortalIsValid
 *		True iff portal is valid.
 */
#define PortalIsValid(p) PointerIsValid(p)

/*
 * Special portals (well, their names anyway)
 */
#define VACPNAME		"<vacuum>"
#define TRUNCPNAME				"<truncate>"

extern bool PortalNameIsSpecial(char *pname);
extern void AtEOXact_portals(void);
extern void EnablePortalManager(bool on);
extern Portal GetPortalByName(char *name);
extern Portal BlankPortalAssignName(char *name);
extern void PortalSetQuery(Portal portal, QueryDesc *queryDesc,
			   TupleDesc attinfo, EState *state,
			   void (*cleanup) (Portal portal));
extern QueryDesc *PortalGetQueryDesc(Portal portal);
extern EState *PortalGetState(Portal portal);
extern Portal CreatePortal(char *name);
extern void PortalDrop(Portal *portalP);
extern void StartPortalAllocMode(AllocMode mode, Size limit);
extern void EndPortalAllocMode(void);
extern void PortalResetHeapMemory(Portal portal);
extern PortalVariableMemory PortalGetVariableMemory(Portal portal);
extern PortalHeapMemory PortalGetHeapMemory(Portal portal);
extern void CommonSpecialPortalOpen(void);
extern void CommonSpecialPortalClose(void);
extern PortalVariableMemory CommonSpecialPortalGetMemory(void);
extern bool CommonSpecialPortalIsOpen(void);

/* estimate of the maximum number of open portals a user would have,
 * used in initially sizing the PortalHashTable in	EnablePortalManager()
 */
#define PORTALS_PER_USER	   10


#endif	 /* PORTAL_H */
