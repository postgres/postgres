/*-------------------------------------------------------------------------
 *
 * portalmem.c
 *	  backend portal memory context management stuff
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/mmgr/portalmem.c,v 1.54 2003/03/27 16:51:29 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * NOTES
 *		A "Portal" is a structure used to keep track of cursor queries.
 *
 *		When the backend sees a "declare cursor" query, it allocates a
 *		"PortalData" structure, plans the query and then stores the query
 *		in the portal without executing it.  Later, when the backend
 *		sees a
 *				fetch 1 from foo
 *		the system looks up the portal named "foo" in the portal table,
 *		gets the planned query and then calls the executor with a count
 *		of 1.  The executor then runs the query and returns a single
 *		tuple.	The problem is that we have to hold onto the state of the
 *		portal query until we see a "close".  This means we have to be
 *		careful about memory management.
 *
 *		I hope this makes things clearer to whoever reads this -cim 2/22/91
 */

#include "postgres.h"

#include "executor/executor.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/portal.h"

/*
 * estimate of the maximum number of open portals a user would have,
 * used in initially sizing the PortalHashTable in EnablePortalManager()
 */
#define PORTALS_PER_USER	   64


/* ----------------
 *		Global state
 * ----------------
 */

#define MAX_PORTALNAME_LEN		NAMEDATALEN

typedef struct portalhashent
{
	char		portalname[MAX_PORTALNAME_LEN];
	Portal		portal;
} PortalHashEnt;

static HTAB *PortalHashTable = NULL;

#define PortalHashTableLookup(NAME, PORTAL) \
do { \
	PortalHashEnt *hentry; char key[MAX_PORTALNAME_LEN]; \
	\
	MemSet(key, 0, MAX_PORTALNAME_LEN); \
	snprintf(key, MAX_PORTALNAME_LEN - 1, "%s", NAME); \
	hentry = (PortalHashEnt*)hash_search(PortalHashTable, \
										 key, HASH_FIND, NULL); \
	if (hentry) \
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
		elog(ERROR, "out of memory in PortalHashTable"); \
	if (found) \
		elog(WARNING, "trying to insert a portal name that exists."); \
	hentry->portal = PORTAL; \
} while(0)

#define PortalHashTableDelete(PORTAL) \
do { \
	PortalHashEnt *hentry; char key[MAX_PORTALNAME_LEN]; \
	\
	MemSet(key, 0, MAX_PORTALNAME_LEN); \
	snprintf(key, MAX_PORTALNAME_LEN - 1, "%s", PORTAL->name); \
	hentry = (PortalHashEnt*)hash_search(PortalHashTable, \
										 key, HASH_REMOVE, NULL); \
	if (hentry == NULL) \
		elog(WARNING, "trying to delete portal name that does not exist."); \
} while(0)

static MemoryContext PortalMemory = NULL;


/* ----------------------------------------------------------------
 *				   public portal interface functions
 * ----------------------------------------------------------------
 */

/*
 * EnablePortalManager
 *		Enables the portal management module at backend startup.
 */
void
EnablePortalManager(void)
{
	HASHCTL		ctl;

	Assert(PortalMemory == NULL);

	PortalMemory = AllocSetContextCreate(TopMemoryContext,
										 "PortalMemory",
										 ALLOCSET_DEFAULT_MINSIZE,
										 ALLOCSET_DEFAULT_INITSIZE,
										 ALLOCSET_DEFAULT_MAXSIZE);

	ctl.keysize = MAX_PORTALNAME_LEN;
	ctl.entrysize = sizeof(PortalHashEnt);

	/*
	 * use PORTALS_PER_USER as a guess of how many hash table entries to
	 * create, initially
	 */
	PortalHashTable = hash_create("Portal hash", PORTALS_PER_USER,
								  &ctl, HASH_ELEM);
}

/*
 * GetPortalByName
 *		Returns a portal given a portal name, or NULL if name not found.
 */
Portal
GetPortalByName(const char *name)
{
	Portal		portal;

	if (PointerIsValid(name))
		PortalHashTableLookup(name, portal);
	else
		portal = NULL;

	return portal;
}

/*
 * PortalSetQuery
 *		Attaches a "query" to the specified portal. Note that in the
 *		case of DECLARE CURSOR, some Portal options have already been
 *		set based upon the parsetree of the original DECLARE statement.
 */
void
PortalSetQuery(Portal portal,
			   QueryDesc *queryDesc,
			   void (*cleanup) (Portal portal))
{
	AssertArg(PortalIsValid(portal));

	/*
	 * If the user didn't specify a SCROLL type, allow or disallow
	 * scrolling based on whether it would require any additional
	 * runtime overhead to do so.
	 */
	if (portal->scrollType == DEFAULT_SCROLL)
	{
		bool backwardPlan;

		backwardPlan = ExecSupportsBackwardScan(queryDesc->plantree);

		if (backwardPlan)
			portal->scrollType = ENABLE_SCROLL;
		else
			portal->scrollType = DISABLE_SCROLL;
	}

	portal->queryDesc = queryDesc;
	portal->cleanup = cleanup;
	portal->atStart = true;
	portal->atEnd = false;		/* allow fetches */
	portal->portalPos = 0;
	portal->posOverflow = false;
}

/*
 * CreatePortal
 *		Returns a new portal given a name.
 *
 *		An elog(WARNING) is emitted if portal name is in use (existing
 *		portal is returned!)
 */
Portal
CreatePortal(const char *name)
{
	Portal		portal;

	AssertArg(PointerIsValid(name));

	portal = GetPortalByName(name);
	if (PortalIsValid(portal))
	{
		elog(WARNING, "CreatePortal: portal \"%s\" already exists", name);
		return portal;
	}

	/* make new portal structure */
	portal = (Portal) MemoryContextAlloc(PortalMemory, sizeof *portal);

	/* initialize portal name */
	portal->name = MemoryContextStrdup(PortalMemory, name);

	/* initialize portal heap context */
	portal->heap = AllocSetContextCreate(PortalMemory,
										 "PortalHeapMemory",
										 ALLOCSET_DEFAULT_MINSIZE,
										 ALLOCSET_DEFAULT_INITSIZE,
										 ALLOCSET_DEFAULT_MAXSIZE);

	/* initialize portal query */
	portal->queryDesc = NULL;
	portal->cleanup = NULL;
	portal->scrollType = DEFAULT_SCROLL;
	portal->holdOpen = false;
	portal->holdStore = NULL;
	portal->holdContext = NULL;
	portal->createXact = GetCurrentTransactionId();
	portal->atStart = true;
	portal->atEnd = true;		/* disallow fetches until query is set */
	portal->portalPos = 0;
	portal->posOverflow = false;

	/* put portal in table */
	PortalHashTableInsert(portal);

	return portal;
}

/*
 * PortalDrop
 *		Destroy the portal.
 *
 *		keepHoldable: if true, holdable portals should not be removed by
 *		this function. More specifically, invoking this function with
 *		keepHoldable = true on a holdable portal prepares the portal for
 *		access outside of its creating transaction.
 */
void
PortalDrop(Portal portal, bool persistHoldable)
{
	AssertArg(PortalIsValid(portal));

	if (portal->holdOpen && persistHoldable)
	{
		/*
		 * We're "dropping" a holdable portal, but what we really need
		 * to do is prepare the portal for access outside of its
		 * creating transaction.
		 */

		/*
		 * Create the memory context that is used for storage of
		 * long-term (cross transaction) data needed by the holdable
		 * portal.
		 */
		portal->holdContext =
			AllocSetContextCreate(PortalMemory,
								  "PortalHeapMemory",
								  ALLOCSET_DEFAULT_MINSIZE,
								  ALLOCSET_DEFAULT_INITSIZE,
								  ALLOCSET_DEFAULT_MAXSIZE);

		/*
		 * Note that PersistHoldablePortal() releases any resources used
		 * by the portal that are local to the creating txn.
		 */
		PersistHoldablePortal(portal);

		return;
	}

	/* remove portal from hash table */
	PortalHashTableDelete(portal);

	/* reset portal */
	if (PointerIsValid(portal->cleanup))
		(*portal->cleanup) (portal);

	/*
	 * delete short-term memory context; in the case of a holdable
	 * portal, this has already been done
	 */
	if (PortalGetHeapMemory(portal))
		MemoryContextDelete(PortalGetHeapMemory(portal));

	/*
	 * delete long-term memory context; in the case of a non-holdable
	 * portal, this context has never been created, so we don't need to
	 * do anything
	 */
	if (portal->holdContext)
		MemoryContextDelete(portal->holdContext);

	/* release name and portal data (both are in PortalMemory) */
	pfree(portal->name);
	pfree(portal);
}

/*
 * Cleanup the portals created in the current transaction. If the
 * transaction was aborted, all the portals created in this transaction
 * should be removed. If the transaction was successfully committed, any
 * holdable cursors created in this transaction need to be kept
 * open. Only cursors created in the current transaction should be
 * removed in this fashion.
 *
 * XXX This assumes that portals can be deleted in a random order, ie,
 * no portal has a reference to any other (at least not one that will be
 * exercised during deletion).	I think this is okay at the moment, but
 * we've had bugs of that ilk in the past.  Keep a close eye on cursor
 * references...
 */
void
AtEOXact_portals(bool isCommit)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;
	TransactionId xact = GetCurrentTransactionId();

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		if (hentry->portal->createXact == xact)
			PortalDrop(hentry->portal, isCommit);
	}
}
