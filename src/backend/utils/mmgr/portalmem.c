/*-------------------------------------------------------------------------
 *
 * portalmem.c
 *	  backend portal memory context management stuff
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/mmgr/portalmem.c,v 1.40 2001/02/27 22:07:34 tgl Exp $
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
 *				fetch 1 from FOO
 *		the system looks up the portal named "FOO" in the portal table,
 *		gets the planned query and then calls the executor with a feature of
 *		'(EXEC_FOR 1).  The executor then runs the query and returns a single
 *		tuple.	The problem is that we have to hold onto the state of the
 *		portal query until we see a "close".  This means we have to be
 *		careful about memory management.
 *
 *		I hope this makes things clearer to whoever reads this -cim 2/22/91
 */

#include "postgres.h"

#include "lib/hasht.h"
#include "utils/memutils.h"
#include "utils/portal.h"

/* ----------------
 *		Global state
 * ----------------
 */

#define MAX_PORTALNAME_LEN		64		/* XXX LONGALIGNable value */

typedef struct portalhashent
{
	char		portalname[MAX_PORTALNAME_LEN];
	Portal		portal;
} PortalHashEnt;

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
do { \
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
	ctl.datasize = sizeof(Portal);

	/*
	 * use PORTALS_PER_USER, defined in utils/portal.h as a guess of
	 * how many hash table entries to create, initially
	 */
	PortalHashTable = hash_create(PORTALS_PER_USER * 3, &ctl, HASH_ELEM);
}

/*
 * GetPortalByName
 *		Returns a portal given a portal name, or NULL if name not found.
 */
Portal
GetPortalByName(char *name)
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
	AssertArg(PortalIsValid(portal));
	AssertArg(IsA((Node *) state, EState));

	portal->queryDesc = queryDesc;
	portal->attinfo = attinfo;
	portal->state = state;
	portal->atStart = true;		/* Allow fetch forward only */
	portal->atEnd = false;
	portal->cleanup = cleanup;
}

/*
 * CreatePortal
 *		Returns a new portal given a name.
 *
 * Exceptions:
 *		BadState if called when disabled.
 *		BadArg if portal name is invalid.
 *		"NOTICE" if portal name is in use (existing portal is returned!)
 */
Portal
CreatePortal(char *name)
{
	Portal		portal;

	AssertArg(PointerIsValid(name));

	portal = GetPortalByName(name);
	if (PortalIsValid(portal))
	{
		elog(NOTICE, "CreatePortal: portal \"%s\" already exists", name);
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
	portal->attinfo = NULL;
	portal->state = NULL;
	portal->atStart = true;		/* disallow fetches until query is set */
	portal->atEnd = true;
	portal->cleanup = NULL;

	/* put portal in table */
	PortalHashTableInsert(portal);

	return portal;
}

/*
 * PortalDrop
 *		Destroys portal.
 *
 * Exceptions:
 *		BadState if called when disabled.
 *		BadArg if portal is invalid.
 *
 * Note peculiar calling convention: pass a pointer to a portal pointer.
 * This is mainly so that this routine can be used as a hashtable walker.
 */
void
PortalDrop(Portal *portalP)
{
	Portal		portal = *portalP;

	AssertArg(PortalIsValid(portal));

	/* remove portal from hash table */
	PortalHashTableDelete(portal);

	/* reset portal */
	if (PointerIsValid(portal->cleanup))
		(*portal->cleanup) (portal);

	/* release subsidiary storage */
	MemoryContextDelete(PortalGetHeapMemory(portal));

	/* release name and portal data (both are in PortalMemory) */
	pfree(portal->name);
	pfree(portal);
}

/*
 * Destroy all portals created in the current transaction (ie, all of them).
 */
void
AtEOXact_portals(void)
{
	HashTableWalk(PortalHashTable, (HashtFunc) PortalDrop, 0);
}
