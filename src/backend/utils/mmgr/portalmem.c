/*-------------------------------------------------------------------------
 *
 * portalmem.c
 *	  backend portal memory context management stuff
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/mmgr/portalmem.c,v 1.18 1999/02/13 23:20:12 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * NOTES
 *		Do not confuse "Portal" with "PortalEntry" (or "PortalBuffer").
 *		When a PQexec() routine is run, the resulting tuples
 *		find their way into a "PortalEntry".  The contents of the resulting
 *		"PortalEntry" can then be inspected by other PQxxx functions.
 *
 *		A "Portal" is a structure used to keep track of queries of the
 *		form:
 *				retrieve portal FOO ( blah... ) where blah...
 *
 *		When the backend sees a "retrieve portal" query, it allocates
 *		a "PortalD" structure, plans the query and then stores the query
 *		in the portal without executing it.  Later, when the backend
 *		sees a
 *				fetch 1 into FOO
 *
 *		the system looks up the portal named "FOO" in the portal table,
 *		gets the planned query and then calls the executor with a feature of
 *		'(EXEC_FOR 1).  The executor then runs the query and returns a single
 *		tuple.	The problem is that we have to hold onto the state of the
 *		portal query until we see a "close p".	This means we have to be
 *		careful about memory management.
 *
 *		Having said all that, here is what a PortalD currently looks like:
 *
 * struct PortalD {
 *		char*							name;
 *		classObj(PortalVariableMemory)	variable;
 *		classObj(PortalHeapMemory)		heap;
 *		List							queryDesc;
 *		EState							state;
 *		void							(*cleanup) ARGS((Portal portal));
 * };
 *
 *		I hope this makes things clearer to whoever reads this -cim 2/22/91
 *
 *		Here is an old comment taken from nodes/memnodes.h
 *
 * MemoryContext 
 *		A logical context in which memory allocations occur.
 *
 * The types of memory contexts can be thought of as members of the
 * following inheritance hierarchy with properties summarized below.
 *
 *						Node
 *						|
 *				MemoryContext___
 *				/				\
 *		GlobalMemory	PortalMemoryContext
 *						/				\
 *		PortalVariableMemory	PortalHeapMemory
 *
 *						Flushed at		Flushed at		Checkpoints
 *						Transaction		Portal
 *						Commit			Close
 *
 * GlobalMemory					n				n				n
 * PortalVariableMemory			n				y				n
 * PortalHeapMemory				y				y				y *
 *
 */
#include <stdio.h>				/* for sprintf() */
#include <string.h>				/* for strlen, strncpy */

#include "postgres.h"

#include "lib/hasht.h"
#include "utils/module.h"
#include "utils/excid.h"		/* for Unimplemented */
#include "utils/mcxt.h"
#include "utils/hsearch.h"

#include "nodes/memnodes.h"
#include "nodes/nodes.h"
#include "nodes/pg_list.h"
#include "nodes/execnodes.h"	/* for EState */

#include "utils/portal.h"

static void CollectNamedPortals(Portal *portalP, int destroy);
static Portal PortalHeapMemoryGetPortal(PortalHeapMemory context);
static PortalVariableMemory PortalHeapMemoryGetVariableMemory(PortalHeapMemory context);
static void PortalResetHeapMemory(Portal portal);
static Portal PortalVariableMemoryGetPortal(PortalVariableMemory context);

/* ----------------
 *		ALLOCFREE_ERROR_ABORT
 *		define this if you want a core dump when you try to
 *		free memory already freed -cim 2/9/91
 * ----------------
 */
#undef ALLOCFREE_ERROR_ABORT

/* ----------------
 *		Global state
 * ----------------
 */

static int	PortalManagerEnableCount = 0;

#define MAX_PORTALNAME_LEN		64		/* XXX LONGALIGNable value */

typedef struct portalhashent
{
	char		portalname[MAX_PORTALNAME_LEN];
	Portal		portal;
} PortalHashEnt;

#define PortalManagerEnabled	(PortalManagerEnableCount >= 1)

static HTAB *PortalHashTable = NULL;

#define PortalHashTableLookup(NAME, PORTAL) \
do { \
	PortalHashEnt *hentry; bool found; char key[MAX_PORTALNAME_LEN]; \
	\
	MemSet(key, 0, MAX_PORTALNAME_LEN); \
	snprintf(key, MAX_PORTALNAME_LEN - 1, "%s", NAME); \
	hentry = (PortalHashEnt*)hash_search(PortalHashTable, \
										 key, HASH_FIND, &found); \
	if (hentry == NULL) \
		elog(FATAL, "error in PortalHashTable"); \
	if (found) \
		PORTAL = hentry->portal; \
	else \
		PORTAL = NULL; \
} while(0)

#define PortalHashTableInsert(PORTAL) \
do { \
	PortalHashEnt *hentry; bool found; char key[MAX_PORTALNAME_LEN]; \
	\
	MemSet(key, 0, MAX_PORTALNAME_LEN); \
	snprintf(key, MAX_PORTALNAME_LEN - 1, "%s", PORTAL->name); \
	hentry = (PortalHashEnt*)hash_search(PortalHashTable, \
										 key, HASH_ENTER, &found); \
	if (hentry == NULL) \
		elog(FATAL, "error in PortalHashTable"); \
	if (found) \
		elog(NOTICE, "trying to insert a portal name that exists."); \
	hentry->portal = PORTAL; \
} while(0)

#define PortalHashTableDelete(PORTAL) \
{ \
	PortalHashEnt *hentry; bool found; char key[MAX_PORTALNAME_LEN]; \
	\
	MemSet(key, 0, MAX_PORTALNAME_LEN); \
	snprintf(key, MAX_PORTALNAME_LEN - 1, "%s", PORTAL->name); \
	hentry = (PortalHashEnt*)hash_search(PortalHashTable, \
										 key, HASH_REMOVE, &found); \
	if (hentry == NULL) \
		elog(FATAL, "error in PortalHashTable"); \
	if (!found) \
		elog(NOTICE, "trying to delete portal name that does not exist."); \
} while(0)

static GlobalMemory PortalMemory = NULL;
static char PortalMemoryName[] = "Portal";

static Portal BlankPortal = NULL;

/* ----------------
 *		Internal class definitions
 * ----------------
 */
typedef struct HeapMemoryBlockData
{
	AllocSetData setData;
	FixedItemData itemData;
} HeapMemoryBlockData;

typedef HeapMemoryBlockData *HeapMemoryBlock;

#define HEAPMEMBLOCK(context) \
	((HeapMemoryBlock)(context)->block)

/* ----------------------------------------------------------------
 *				  Variable and heap memory methods
 * ----------------------------------------------------------------
 */
/* ----------------
 *		PortalVariableMemoryAlloc
 * ----------------
 */
static Pointer
PortalVariableMemoryAlloc(PortalVariableMemory this,
						  Size size)
{
	return AllocSetAlloc(&this->setData, size);
}

/* ----------------
 *		PortalVariableMemoryFree
 * ----------------
 */
static void
PortalVariableMemoryFree(PortalVariableMemory this,
						 Pointer pointer)
{
	AllocSetFree(&this->setData, pointer);
}

/* ----------------
 *		PortalVariableMemoryRealloc
 * ----------------
 */
static Pointer
PortalVariableMemoryRealloc(PortalVariableMemory this,
							Pointer pointer,
							Size size)
{
	return AllocSetRealloc(&this->setData, pointer, size);
}

/* ----------------
 *		PortalVariableMemoryGetName
 * ----------------
 */
static char *
PortalVariableMemoryGetName(PortalVariableMemory this)
{
	return form("%s-var", PortalVariableMemoryGetPortal(this)->name);
}

/* ----------------
 *		PortalVariableMemoryDump
 * ----------------
 */
static void
PortalVariableMemoryDump(PortalVariableMemory this)
{
	printf("--\n%s:\n", PortalVariableMemoryGetName(this));

	AllocSetDump(&this->setData);		/* XXX is this the right interface */
}

/* ----------------
 *		PortalHeapMemoryAlloc
 * ----------------
 */
static Pointer
PortalHeapMemoryAlloc(PortalHeapMemory this,
					  Size size)
{
	HeapMemoryBlock block = HEAPMEMBLOCK(this);

	AssertState(PointerIsValid(block));

	return AllocSetAlloc(&block->setData, size);
}

/* ----------------
 *		PortalHeapMemoryFree
 * ----------------
 */
static void
PortalHeapMemoryFree(PortalHeapMemory this,
					 Pointer pointer)
{
	HeapMemoryBlock block = HEAPMEMBLOCK(this);

	AssertState(PointerIsValid(block));

	if (AllocSetContains(&block->setData, pointer))
		AllocSetFree(&block->setData, pointer);
	else
	{
		elog(NOTICE,
			 "PortalHeapMemoryFree: 0x%x not in alloc set!",
			 pointer);
#ifdef ALLOCFREE_ERROR_ABORT
		Assert(AllocSetContains(&block->setData, pointer));
#endif	 /* ALLOCFREE_ERROR_ABORT */
	}
}

/* ----------------
 *		PortalHeapMemoryRealloc
 * ----------------
 */
static Pointer
PortalHeapMemoryRealloc(PortalHeapMemory this,
						Pointer pointer,
						Size size)
{
	HeapMemoryBlock block = HEAPMEMBLOCK(this);

	AssertState(PointerIsValid(block));

	return AllocSetRealloc(&block->setData, pointer, size);
}

/* ----------------
 *		PortalHeapMemoryGetName
 * ----------------
 */
static char *
PortalHeapMemoryGetName(PortalHeapMemory this)
{
	return form("%s-heap", PortalHeapMemoryGetPortal(this)->name);
}

/* ----------------
 *		PortalHeapMemoryDump
 * ----------------
 */
static void
PortalHeapMemoryDump(PortalHeapMemory this)
{
	HeapMemoryBlock block;

	printf("--\n%s:\n", PortalHeapMemoryGetName(this));

	/* XXX is this the right interface */
	if (PointerIsValid(this->block))
		AllocSetDump(&HEAPMEMBLOCK(this)->setData);

	/* dump the stack too */
	for (block = (HeapMemoryBlock) FixedStackGetTop(&this->stackData);
		 PointerIsValid(block);
		 block = (HeapMemoryBlock)
		 FixedStackGetNext(&this->stackData, (Pointer) block))
	{

		printf("--\n");
		AllocSetDump(&block->setData);
	}
}

/* ----------------------------------------------------------------
 *				variable / heap context method tables
 * ----------------------------------------------------------------
 */
static struct MemoryContextMethodsData PortalVariableContextMethodsData = {
	PortalVariableMemoryAlloc,	/* Pointer (*)(this, uint32)	palloc */
	PortalVariableMemoryFree,	/* void (*)(this, Pointer)		pfree */
	PortalVariableMemoryRealloc,/* Pointer (*)(this, Pointer)	repalloc */
	PortalVariableMemoryGetName,/* char* (*)(this)				getName */
	PortalVariableMemoryDump	/* void (*)(this)				dump */
};

static struct MemoryContextMethodsData PortalHeapContextMethodsData = {
	PortalHeapMemoryAlloc,		/* Pointer (*)(this, uint32)	palloc */
	PortalHeapMemoryFree,		/* void (*)(this, Pointer)		pfree */
	PortalHeapMemoryRealloc,	/* Pointer (*)(this, Pointer)	repalloc */
	PortalHeapMemoryGetName,	/* char* (*)(this)				getName */
	PortalHeapMemoryDump		/* void (*)(this)				dump */
};


/* ----------------------------------------------------------------
 *				  private internal support routines
 * ----------------------------------------------------------------
 */
/* ----------------
 *		CreateNewBlankPortal
 * ----------------
 */
static void
CreateNewBlankPortal()
{
	Portal		portal;

	AssertState(!PortalIsValid(BlankPortal));

	/*
	 * make new portal structure
	 */
	portal = (Portal)
		MemoryContextAlloc((MemoryContext) PortalMemory, sizeof *portal);

	/*
	 * initialize portal variable context
	 */
	NodeSetTag((Node *) &portal->variable, T_PortalVariableMemory);
	AllocSetInit(&portal->variable.setData, DynamicAllocMode, (Size) 0);
	portal->variable.method = &PortalVariableContextMethodsData;

	/*
	 * initialize portal heap context
	 */
	NodeSetTag((Node *) &portal->heap, T_PortalHeapMemory);
	portal->heap.block = NULL;
	FixedStackInit(&portal->heap.stackData,
				   offsetof(HeapMemoryBlockData, itemData));
	portal->heap.method = &PortalHeapContextMethodsData;

	/*
	 * set bogus portal name
	 */
	portal->name = "** Blank Portal **";

	/* initialize portal query */
	portal->queryDesc = NULL;
	portal->attinfo = NULL;
	portal->state = NULL;
	portal->cleanup = NULL;

	/*
	 * install blank portal
	 */
	BlankPortal = portal;
}

bool
PortalNameIsSpecial(char *pname)
{
	if (strcmp(pname, VACPNAME) == 0)
		return true;
	return false;
}

/*
 * This routine is used to collect all portals created in this xaction
 * and then destroy them.  There is a little trickiness required as a
 * result of the dynamic hashing interface to getting every hash entry
 * sequentially.  Its use of static variables requires that we get every
 * entry *before* we destroy anything (destroying updates the hashtable
 * and screws up the sequential walk of the table). -mer 17 Aug 1992
 */
static void
CollectNamedPortals(Portal *portalP, int destroy)
{
	static Portal *portalList = (Portal *) NULL;
	static int	listIndex = 0;
	static int	maxIndex = 9;

	if (portalList == (Portal *) NULL)
		portalList = (Portal *) malloc(10 * sizeof(Portal));

	if (destroy != 0)
	{
		int			i;

		for (i = 0; i < listIndex; i++)
			PortalDestroy(&portalList[i]);
		listIndex = 0;
	}
	else
	{
		Assert(portalP);
		Assert(*portalP);

		/*
		 * Don't delete special portals, up to portal creator to do this
		 */
		if (PortalNameIsSpecial((*portalP)->name))
			return;

		portalList[listIndex] = *portalP;
		listIndex++;
		if (listIndex == maxIndex)
		{
			portalList = (Portal *)
				realloc(portalList, (maxIndex + 11) * sizeof(Portal));
			maxIndex += 10;
		}
	}
	return;
}

void
AtEOXact_portals()
{
	HashTableWalk(PortalHashTable, CollectNamedPortals, 0);
	CollectNamedPortals(NULL, 1);
}

/* ----------------
 *		PortalDump
 * ----------------
 */
#ifdef NOT_USED
static void
PortalDump(Portal *thisP)
{
	/* XXX state/argument checking here */

	PortalVariableMemoryDump(PortalGetVariableMemory(*thisP));
	PortalHeapMemoryDump(PortalGetHeapMemory(*thisP));
}

#endif

/* ----------------
 *		DumpPortals
 * ----------------
 */
#ifdef NOT_USED
static void
DumpPortals()
{
	/* XXX state checking here */

	HashTableWalk(PortalHashTable, PortalDump, 0);
}

#endif

/* ----------------------------------------------------------------
 *				   public portal interface functions
 * ----------------------------------------------------------------
 */
/*
 * EnablePortalManager 
 *		Enables/disables the portal management module.
 */
void
EnablePortalManager(bool on)
{
	static bool processing = false;
	HASHCTL		ctl;

	AssertState(!processing);
	AssertArg(BoolIsValid(on));

	if (BypassEnable(&PortalManagerEnableCount, on))
		return;

	processing = true;

	if (on)
	{							/* initialize */
		EnableMemoryContext(true);

		PortalMemory = CreateGlobalMemory(PortalMemoryName);

		ctl.keysize = MAX_PORTALNAME_LEN;
		ctl.datasize = sizeof(Portal);

		/*
		 * use PORTALS_PER_USER, defined in utils/portal.h as a guess of
		 * how many hash table entries to create, initially
		 */
		PortalHashTable = hash_create(PORTALS_PER_USER * 3, &ctl, HASH_ELEM);

		CreateNewBlankPortal();

	}
	else
	{							/* cleanup */
		if (PortalIsValid(BlankPortal))
		{
			PortalDestroy(&BlankPortal);
			MemoryContextFree((MemoryContext) PortalMemory,
							  (Pointer) BlankPortal);
			BlankPortal = NULL;
		}

		/*
		 * Each portal must free its non-memory resources specially.
		 */
		HashTableWalk(PortalHashTable, PortalDestroy, 0);
		hash_destroy(PortalHashTable);
		PortalHashTable = NULL;

		GlobalMemoryDestroy(PortalMemory);
		PortalMemory = NULL;

		EnableMemoryContext(true);
	}

	processing = false;
}

/*
 * GetPortalByName 
 *		Returns a portal given a portal name; returns blank portal given
 * NULL; returns invalid portal if portal not found.
 *
 * Exceptions:
 *		BadState if called when disabled.
 */
Portal
GetPortalByName(char *name)
{
	Portal		portal;

	AssertState(PortalManagerEnabled);

	if (PointerIsValid(name))
		PortalHashTableLookup(name, portal);
	else
	{
		if (!PortalIsValid(BlankPortal))
			CreateNewBlankPortal();
		portal = BlankPortal;
	}

	return portal;
}

/*
 * BlankPortalAssignName 
 *		Returns former blank portal as portal with given name.
 *
 * Side effect:
 *		All references to the former blank portal become incorrect.
 *
 * Exceptions:
 *		BadState if called when disabled.
 *		BadState if called without an intervening call to GetPortalByName(NULL).
 *		BadArg if portal name is invalid.
 *		"WARN" if portal name is in use.
 */
Portal
BlankPortalAssignName(char *name)		/* XXX PortalName */
{
	Portal		portal;
	uint16		length;

	AssertState(PortalManagerEnabled);
	AssertState(PortalIsValid(BlankPortal));
	AssertArg(PointerIsValid(name));	/* XXX PortalName */

	portal = GetPortalByName(name);
	if (PortalIsValid(portal))
	{
		elog(NOTICE, "BlankPortalAssignName: portal %s already exists", name);
		return portal;
	}

	/*
	 * remove blank portal
	 */
	portal = BlankPortal;
	BlankPortal = NULL;

	/*
	 * initialize portal name
	 */
	length = 1 + strlen(name);
	portal->name = (char *)
		MemoryContextAlloc((MemoryContext) &portal->variable, length);

	strncpy(portal->name, name, length);

	/*
	 * put portal in table
	 */
	PortalHashTableInsert(portal);

	return portal;
}

/*
 * PortalSetQuery 
 *		Attaches a "query" to portal.
 *
 * Exceptions:
 *		BadState if called when disabled.
 *		BadArg if portal is invalid.
 *		BadArg if queryDesc is "invalid."
 *		BadArg if state is "invalid."
 */
void
PortalSetQuery(Portal portal,
			   QueryDesc *queryDesc,
			   TupleDesc attinfo,
			   EState *state,
			   void (*cleanup) (Portal portal))
{
	AssertState(PortalManagerEnabled);
	AssertArg(PortalIsValid(portal));
	AssertArg(IsA((Node *) state, EState));

	portal->queryDesc = queryDesc;
	portal->state = state;
	portal->attinfo = attinfo;
	portal->cleanup = cleanup;
}

/*
 * PortalGetQueryDesc 
 *		Returns query attached to portal.
 *
 * Exceptions:
 *		BadState if called when disabled.
 *		BadArg if portal is invalid.
 */
QueryDesc  *
PortalGetQueryDesc(Portal portal)
{
	AssertState(PortalManagerEnabled);
	AssertArg(PortalIsValid(portal));

	return portal->queryDesc;
}

/*
 * PortalGetState 
 *		Returns state attached to portal.
 *
 * Exceptions:
 *		BadState if called when disabled.
 *		BadArg if portal is invalid.
 */
EState *
PortalGetState(Portal portal)
{
	AssertState(PortalManagerEnabled);
	AssertArg(PortalIsValid(portal));

	return portal->state;
}

/*
 * CreatePortal 
 *		Returns a new portal given a name.
 *
 * Note:
 *		This is expected to be of very limited usability.  See instead,
 * BlankPortalAssignName.
 *
 * Exceptions:
 *		BadState if called when disabled.
 *		BadArg if portal name is invalid.
 *		"WARN" if portal name is in use.
 */
Portal
CreatePortal(char *name)		/* XXX PortalName */
{
	Portal		portal;
	uint16		length;

	AssertState(PortalManagerEnabled);
	AssertArg(PointerIsValid(name));	/* XXX PortalName */

	portal = GetPortalByName(name);
	if (PortalIsValid(portal))
	{
		elog(NOTICE, "CreatePortal: portal %s already exists", name);
		return portal;
	}

	/* make new portal structure */
	portal = (Portal)
		MemoryContextAlloc((MemoryContext) PortalMemory, sizeof *portal);

	/* initialize portal variable context */
	NodeSetTag((Node *) &portal->variable, T_PortalVariableMemory);
	AllocSetInit(&portal->variable.setData, DynamicAllocMode, (Size) 0);
	portal->variable.method = &PortalVariableContextMethodsData;

	/* initialize portal heap context */
	NodeSetTag((Node *) &portal->heap, T_PortalHeapMemory);
	portal->heap.block = NULL;
	FixedStackInit(&portal->heap.stackData,
				   offsetof(HeapMemoryBlockData, itemData));
	portal->heap.method = &PortalHeapContextMethodsData;

	/* initialize portal name */
	length = 1 + strlen(name);
	portal->name = (char *)
		MemoryContextAlloc((MemoryContext) &portal->variable, length);
	strncpy(portal->name, name, length);

	/* initialize portal query */
	portal->queryDesc = NULL;
	portal->attinfo = NULL;
	portal->state = NULL;
	portal->cleanup = NULL;

	/* put portal in table */
	PortalHashTableInsert(portal);

	/* Trap(PointerIsValid(name), Unimplemented); */
	return portal;
}

/*
 * PortalDestroy 
 *		Destroys portal.
 *
 * Exceptions:
 *		BadState if called when disabled.
 *		BadArg if portal is invalid.
 */
void
PortalDestroy(Portal *portalP)
{
	Portal		portal = *portalP;

	AssertState(PortalManagerEnabled);
	AssertArg(PortalIsValid(portal));

	/* remove portal from table if not blank portal */
	if (portal != BlankPortal)
		PortalHashTableDelete(portal);

	/* reset portal */
	if (PointerIsValid(portal->cleanup))
		(*portal->cleanup) (portal);

	PortalResetHeapMemory(portal);
	MemoryContextFree((MemoryContext) &portal->variable,
					  (Pointer) portal->name);
	AllocSetReset(&portal->variable.setData);	/* XXX log */

	/*
	 * In the case of a transaction abort it is possible that
	 * we get called while one of the memory contexts of the portal
	 * we're destroying is the current memory context.
	 * 
	 * Don't know how to handle that cleanly because it is required
	 * to be in that context right now. This portal struct remains
	 * allocated in the PortalMemory context until backend dies.
	 *
	 * Not happy with that, but it's better to loose some bytes of
	 * memory than to have the backend dump core.
	 *
	 * --- Feb. 04, 1999 Jan Wieck
	 */
	if (CurrentMemoryContext == (MemoryContext)PortalGetHeapMemory(portal))
		return;
	if (CurrentMemoryContext == (MemoryContext)PortalGetVariableMemory(portal))
		return;

	if (portal != BlankPortal)
		MemoryContextFree((MemoryContext) PortalMemory, (Pointer) portal);
}

/* ----------------
 *		PortalResetHeapMemory 
 *				Resets portal's heap memory context.
 *
 * Someday, Reset, Start, and End can be optimized by keeping a global
 * portal module stack of free HeapMemoryBlock's.  This will make Start
 * and End be fast.
 *
 * Exceptions:
 *		BadState if called when disabled.
 *		BadState if called when not in PortalHeapMemory context.
 *		BadArg if mode is invalid.
 * ----------------
 */
static void
PortalResetHeapMemory(Portal portal)
{
	PortalHeapMemory context;
	MemoryContext currentContext;

	context = PortalGetHeapMemory(portal);

	if (PointerIsValid(context->block))
	{
		/* save present context */
		currentContext = MemoryContextSwitchTo((MemoryContext) context);

		do
		{
			EndPortalAllocMode();
		} while (PointerIsValid(context->block));

		/* restore context */
		MemoryContextSwitchTo(currentContext);
	}
}

/*
 * StartPortalAllocMode 
 *		Starts a new block of portal heap allocation using mode and limit;
 *		the current block is disabled until EndPortalAllocMode is called.
 *
 * Note:
 *		Note blocks may be stacked and restored arbitarily.
 *		The semantics of mode and limit are described in aset.h.
 *
 * Exceptions:
 *		BadState if called when disabled.
 *		BadState if called when not in PortalHeapMemory context.
 *		BadArg if mode is invalid.
 */
void
StartPortalAllocMode(AllocMode mode, Size limit)
{
	PortalHeapMemory context;

	AssertState(PortalManagerEnabled);
	AssertState(IsA(CurrentMemoryContext, PortalHeapMemory));
	/* AssertArg(AllocModeIsValid); */

	context = (PortalHeapMemory) CurrentMemoryContext;

	/* stack current mode */
	if (PointerIsValid(context->block))
		FixedStackPush(&context->stackData, context->block);

	/* allocate and initialize new block */
	context->block = MemoryContextAlloc(
			  (MemoryContext) PortalHeapMemoryGetVariableMemory(context),
						   sizeof(HeapMemoryBlockData));

	/* XXX careful, context->block has never been stacked => bad state */

	AllocSetInit(&HEAPMEMBLOCK(context)->setData, mode, limit);
}

/*
 * EndPortalAllocMode 
 *		Ends current block of portal heap allocation; previous block is
 *		reenabled.
 *
 * Note:
 *		Note blocks may be stacked and restored arbitarily.
 *
 * Exceptions:
 *		BadState if called when disabled.
 *		BadState if called when not in PortalHeapMemory context.
 */
void
EndPortalAllocMode()
{
	PortalHeapMemory context;

	AssertState(PortalManagerEnabled);
	AssertState(IsA(CurrentMemoryContext, PortalHeapMemory));

	context = (PortalHeapMemory) CurrentMemoryContext;
	AssertState(PointerIsValid(context->block));		/* XXX Trap(...) */

	/* free current mode */
	AllocSetReset(&HEAPMEMBLOCK(context)->setData);
	MemoryContextFree((MemoryContext) PortalHeapMemoryGetVariableMemory(context),
					  context->block);

	/* restore previous mode */
	context->block = FixedStackPop(&context->stackData);
}

/*
 * PortalGetVariableMemory 
 *		Returns variable memory context for a given portal.
 *
 * Exceptions:
 *		BadState if called when disabled.
 *		BadArg if portal is invalid.
 */
PortalVariableMemory
PortalGetVariableMemory(Portal portal)
{
	return &portal->variable;
}

/*
 * PortalGetHeapMemory 
 *		Returns heap memory context for a given portal.
 *
 * Exceptions:
 *		BadState if called when disabled.
 *		BadArg if portal is invalid.
 */
PortalHeapMemory
PortalGetHeapMemory(Portal portal)
{
	return &portal->heap;
}

/*
 * PortalVariableMemoryGetPortal 
 *		Returns portal containing given variable memory context.
 *
 * Exceptions:
 *		BadState if called when disabled.
 *		BadArg if context is invalid.
 */
static Portal
PortalVariableMemoryGetPortal(PortalVariableMemory context)
{
	return (Portal) ((char *) context - offsetof(PortalD, variable));
}

/*
 * PortalHeapMemoryGetPortal 
 *		Returns portal containing given heap memory context.
 *
 * Exceptions:
 *		BadState if called when disabled.
 *		BadArg if context is invalid.
 */
static Portal
PortalHeapMemoryGetPortal(PortalHeapMemory context)
{
	return (Portal) ((char *) context - offsetof(PortalD, heap));
}

/*
 * PortalVariableMemoryGetHeapMemory 
 *		Returns heap memory context associated with given variable memory.
 *
 * Exceptions:
 *		BadState if called when disabled.
 *		BadArg if context is invalid.
 */
#ifdef NOT_USED
PortalHeapMemory
PortalVariableMemoryGetHeapMemory(PortalVariableMemory context)
{
	return ((PortalHeapMemory) ((char *) context
								- offsetof(PortalD, variable)
								+offsetof(PortalD, heap)));
}

#endif

/*
 * PortalHeapMemoryGetVariableMemory 
 *		Returns variable memory context associated with given heap memory.
 *
 * Exceptions:
 *		BadState if called when disabled.
 *		BadArg if context is invalid.
 */
static PortalVariableMemory
PortalHeapMemoryGetVariableMemory(PortalHeapMemory context)
{
	return ((PortalVariableMemory) ((char *) context
									- offsetof(PortalD, heap)
									+offsetof(PortalD, variable)));
}
