/*-------------------------------------------------------------------------
 * relation.c
 *	   PostgreSQL logical replication
 *
 * Copyright (c) 2016-2017, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/relation.c
 *
 * NOTES
 *	  This file contains helper functions for logical replication relation
 *	  mapping cache.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/sysattr.h"
#include "catalog/namespace.h"
#include "catalog/pg_subscription_rel.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "replication/logicalrelation.h"
#include "replication/worker_internal.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

static MemoryContext LogicalRepRelMapContext = NULL;

static HTAB *LogicalRepRelMap = NULL;
static HTAB *LogicalRepTypMap = NULL;

static void logicalrep_typmap_invalidate_cb(Datum arg, int cacheid,
								uint32 hashvalue);

/*
 * Relcache invalidation callback for our relation map cache.
 */
static void
logicalrep_relmap_invalidate_cb(Datum arg, Oid reloid)
{
	LogicalRepRelMapEntry *entry;

	/* Just to be sure. */
	if (LogicalRepRelMap == NULL)
		return;

	if (reloid != InvalidOid)
	{
		HASH_SEQ_STATUS status;

		hash_seq_init(&status, LogicalRepRelMap);

		/* TODO, use inverse lookup hashtable? */
		while ((entry = (LogicalRepRelMapEntry *) hash_seq_search(&status)) != NULL)
		{
			if (entry->localreloid == reloid)
			{
				entry->localreloid = InvalidOid;
				hash_seq_term(&status);
				break;
			}
		}
	}
	else
	{
		/* invalidate all cache entries */
		HASH_SEQ_STATUS status;

		hash_seq_init(&status, LogicalRepRelMap);

		while ((entry = (LogicalRepRelMapEntry *) hash_seq_search(&status)) != NULL)
			entry->localreloid = InvalidOid;
	}
}

/*
 * Initialize the relation map cache.
 */
static void
logicalrep_relmap_init(void)
{
	HASHCTL		ctl;

	if (!LogicalRepRelMapContext)
		LogicalRepRelMapContext =
			AllocSetContextCreate(CacheMemoryContext,
								  "LogicalRepRelMapContext",
								  ALLOCSET_DEFAULT_SIZES);

	/* Initialize the relation hash table. */
	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(LogicalRepRelId);
	ctl.entrysize = sizeof(LogicalRepRelMapEntry);
	ctl.hcxt = LogicalRepRelMapContext;

	LogicalRepRelMap = hash_create("logicalrep relation map cache", 128, &ctl,
								   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/* Initialize the type hash table. */
	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(LogicalRepTyp);
	ctl.hcxt = LogicalRepRelMapContext;

	/* This will usually be small. */
	LogicalRepTypMap = hash_create("logicalrep type map cache", 2, &ctl,
								   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/* Watch for invalidation events. */
	CacheRegisterRelcacheCallback(logicalrep_relmap_invalidate_cb,
								  (Datum) 0);
	CacheRegisterSyscacheCallback(TYPEOID, logicalrep_typmap_invalidate_cb,
								  (Datum) 0);
}

/*
 * Free the entry of a relation map cache.
 */
static void
logicalrep_relmap_free_entry(LogicalRepRelMapEntry *entry)
{
	LogicalRepRelation *remoterel;

	remoterel = &entry->remoterel;

	pfree(remoterel->nspname);
	pfree(remoterel->relname);

	if (remoterel->natts > 0)
	{
		int			i;

		for (i = 0; i < remoterel->natts; i++)
			pfree(remoterel->attnames[i]);

		pfree(remoterel->attnames);
		pfree(remoterel->atttyps);
	}
	bms_free(remoterel->attkeys);

	if (entry->attrmap)
		pfree(entry->attrmap);
}

/*
 * Add new entry or update existing entry in the relation map cache.
 *
 * Called when new relation mapping is sent by the publisher to update
 * our expected view of incoming data from said publisher.
 */
void
logicalrep_relmap_update(LogicalRepRelation *remoterel)
{
	MemoryContext oldctx;
	LogicalRepRelMapEntry *entry;
	bool		found;
	int			i;

	if (LogicalRepRelMap == NULL)
		logicalrep_relmap_init();

	/*
	 * HASH_ENTER returns the existing entry if present or creates a new one.
	 */
	entry = hash_search(LogicalRepRelMap, (void *) &remoterel->remoteid,
						HASH_ENTER, &found);

	if (found)
		logicalrep_relmap_free_entry(entry);

	memset(entry, 0, sizeof(LogicalRepRelMapEntry));

	/* Make cached copy of the data */
	oldctx = MemoryContextSwitchTo(LogicalRepRelMapContext);
	entry->remoterel.remoteid = remoterel->remoteid;
	entry->remoterel.nspname = pstrdup(remoterel->nspname);
	entry->remoterel.relname = pstrdup(remoterel->relname);
	entry->remoterel.natts = remoterel->natts;
	entry->remoterel.attnames = palloc(remoterel->natts * sizeof(char *));
	entry->remoterel.atttyps = palloc(remoterel->natts * sizeof(Oid));
	for (i = 0; i < remoterel->natts; i++)
	{
		entry->remoterel.attnames[i] = pstrdup(remoterel->attnames[i]);
		entry->remoterel.atttyps[i] = remoterel->atttyps[i];
	}
	entry->remoterel.replident = remoterel->replident;
	entry->remoterel.attkeys = bms_copy(remoterel->attkeys);
	MemoryContextSwitchTo(oldctx);
}

/*
 * Find attribute index in TupleDesc struct by attribute name.
 *
 * Returns -1 if not found.
 */
static int
logicalrep_rel_att_by_name(LogicalRepRelation *remoterel, const char *attname)
{
	int			i;

	for (i = 0; i < remoterel->natts; i++)
	{
		if (strcmp(remoterel->attnames[i], attname) == 0)
			return i;
	}

	return -1;
}

/*
 * Open the local relation associated with the remote one.
 *
 * Optionally rebuilds the Relcache mapping if it was invalidated
 * by local DDL.
 */
LogicalRepRelMapEntry *
logicalrep_rel_open(LogicalRepRelId remoteid, LOCKMODE lockmode)
{
	LogicalRepRelMapEntry *entry;
	bool		found;

	if (LogicalRepRelMap == NULL)
		logicalrep_relmap_init();

	/* Search for existing entry. */
	entry = hash_search(LogicalRepRelMap, (void *) &remoteid,
						HASH_FIND, &found);

	if (!found)
		elog(ERROR, "no relation map entry for remote relation ID %u",
			 remoteid);

	/* Need to update the local cache? */
	if (!OidIsValid(entry->localreloid))
	{
		Oid			relid;
		int			i;
		int			found;
		Bitmapset  *idkey;
		TupleDesc	desc;
		LogicalRepRelation *remoterel;
		MemoryContext oldctx;

		remoterel = &entry->remoterel;

		/* Try to find and lock the relation by name. */
		relid = RangeVarGetRelid(makeRangeVar(remoterel->nspname,
											  remoterel->relname, -1),
								 lockmode, true);
		if (!OidIsValid(relid))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("logical replication target relation \"%s.%s\" does not exist",
							remoterel->nspname, remoterel->relname)));
		entry->localrel = heap_open(relid, NoLock);

		/* Check for supported relkind. */
		CheckSubscriptionRelkind(entry->localrel->rd_rel->relkind,
								 remoterel->nspname, remoterel->relname);

		/*
		 * Build the mapping of local attribute numbers to remote attribute
		 * numbers and validate that we don't miss any replicated columns as
		 * that would result in potentially unwanted data loss.
		 */
		desc = RelationGetDescr(entry->localrel);
		oldctx = MemoryContextSwitchTo(LogicalRepRelMapContext);
		entry->attrmap = palloc(desc->natts * sizeof(int));
		MemoryContextSwitchTo(oldctx);

		found = 0;
		for (i = 0; i < desc->natts; i++)
		{
			int			attnum;

			if (desc->attrs[i]->attisdropped)
			{
				entry->attrmap[i] = -1;
				continue;
			}

			attnum = logicalrep_rel_att_by_name(remoterel,
												NameStr(desc->attrs[i]->attname));

			entry->attrmap[i] = attnum;
			if (attnum >= 0)
				found++;
		}

		/* TODO, detail message with names of missing columns */
		if (found < remoterel->natts)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("logical replication target relation \"%s.%s\" is missing "
							"some replicated columns",
							remoterel->nspname, remoterel->relname)));

		/*
		 * Check that replica identity matches. We allow for stricter replica
		 * identity (fewer columns) on subscriber as that will not stop us
		 * from finding unique tuple. IE, if publisher has identity
		 * (id,timestamp) and subscriber just (id) this will not be a problem,
		 * but in the opposite scenario it will.
		 *
		 * Don't throw any error here just mark the relation entry as not
		 * updatable, as replica identity is only for updates and deletes but
		 * inserts can be replicated even without it.
		 */
		entry->updatable = true;
		idkey = RelationGetIndexAttrBitmap(entry->localrel,
										   INDEX_ATTR_BITMAP_IDENTITY_KEY);
		/* fallback to PK if no replica identity */
		if (idkey == NULL)
		{
			idkey = RelationGetIndexAttrBitmap(entry->localrel,
											   INDEX_ATTR_BITMAP_PRIMARY_KEY);

			/*
			 * If no replica identity index and no PK, the published table
			 * must have replica identity FULL.
			 */
			if (idkey == NULL && remoterel->replident != REPLICA_IDENTITY_FULL)
				entry->updatable = false;
		}

		i = -1;
		while ((i = bms_next_member(idkey, i)) >= 0)
		{
			int			attnum = i + FirstLowInvalidHeapAttributeNumber;

			if (!AttrNumberIsForUserDefinedAttr(attnum))
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("logical replication target relation \"%s.%s\" uses "
								"system columns in REPLICA IDENTITY index",
								remoterel->nspname, remoterel->relname)));

			attnum = AttrNumberGetAttrOffset(attnum);

			if (!bms_is_member(entry->attrmap[attnum], remoterel->attkeys))
			{
				entry->updatable = false;
				break;
			}
		}

		entry->localreloid = relid;
	}
	else
		entry->localrel = heap_open(entry->localreloid, lockmode);

	if (entry->state != SUBREL_STATE_READY)
		entry->state = GetSubscriptionRelState(MySubscription->oid,
											   entry->localreloid,
											   &entry->statelsn,
											   true);

	return entry;
}

/*
 * Close the previously opened logical relation.
 */
void
logicalrep_rel_close(LogicalRepRelMapEntry *rel, LOCKMODE lockmode)
{
	heap_close(rel->localrel, lockmode);
	rel->localrel = NULL;
}


/*
 * Type cache invalidation callback for our type map cache.
 */
static void
logicalrep_typmap_invalidate_cb(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS status;
	LogicalRepTyp *entry;

	/* Just to be sure. */
	if (LogicalRepTypMap == NULL)
		return;

	/* invalidate all cache entries */
	hash_seq_init(&status, LogicalRepTypMap);

	while ((entry = (LogicalRepTyp *) hash_seq_search(&status)) != NULL)
		entry->typoid = InvalidOid;
}

/*
 * Free the type map cache entry data.
 */
static void
logicalrep_typmap_free_entry(LogicalRepTyp *entry)
{
	pfree(entry->nspname);
	pfree(entry->typname);

	entry->typoid = InvalidOid;
}

/*
 * Add new entry or update existing entry in the type map cache.
 */
void
logicalrep_typmap_update(LogicalRepTyp *remotetyp)
{
	MemoryContext oldctx;
	LogicalRepTyp *entry;
	bool		found;

	if (LogicalRepTypMap == NULL)
		logicalrep_relmap_init();

	/*
	 * HASH_ENTER returns the existing entry if present or creates a new one.
	 */
	entry = hash_search(LogicalRepTypMap, (void *) &remotetyp->remoteid,
						HASH_ENTER, &found);

	if (found)
		logicalrep_typmap_free_entry(entry);

	/* Make cached copy of the data */
	entry->remoteid = remotetyp->remoteid;
	oldctx = MemoryContextSwitchTo(LogicalRepRelMapContext);
	entry->nspname = pstrdup(remotetyp->nspname);
	entry->typname = pstrdup(remotetyp->typname);
	MemoryContextSwitchTo(oldctx);
	entry->typoid = InvalidOid;
}

/*
 * Fetch type info from the cache.
 */
Oid
logicalrep_typmap_getid(Oid remoteid)
{
	LogicalRepTyp *entry;
	bool		found;
	Oid			nspoid;

	/* Internal types are mapped directly. */
	if (remoteid < FirstNormalObjectId)
	{
		if (!get_typisdefined(remoteid))
			ereport(ERROR,
					(errmsg("builtin type %u not found", remoteid),
					 errhint("This can be caused by having publisher with "
							 "higher major version than subscriber")));
		return remoteid;
	}

	if (LogicalRepTypMap == NULL)
		logicalrep_relmap_init();

	/* Try finding the mapping. */
	entry = hash_search(LogicalRepTypMap, (void *) &remoteid,
						HASH_FIND, &found);

	if (!found)
		elog(ERROR, "no type map entry for remote type %u",
			 remoteid);

	/* Found and mapped, return the oid. */
	if (OidIsValid(entry->typoid))
		return entry->typoid;

	/* Otherwise, try to map to local type. */
	nspoid = LookupExplicitNamespace(entry->nspname, true);
	if (OidIsValid(nspoid))
		entry->typoid = GetSysCacheOid2(TYPENAMENSP,
										PointerGetDatum(entry->typname),
										ObjectIdGetDatum(nspoid));
	else
		entry->typoid = InvalidOid;

	if (!OidIsValid(entry->typoid))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("data type \"%s.%s\" required for logical replication does not exist",
						entry->nspname, entry->typname)));

	return entry->typoid;
}
