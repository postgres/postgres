/*-------------------------------------------------------------------------
 * relation.c
 *	   PostgreSQL logical replication
 *
 * Copyright (c) 2016-2020, PostgreSQL Global Development Group
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

#include "access/relation.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_subscription_rel.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "replication/logicalrelation.h"
#include "replication/worker_internal.h"
#include "utils/inval.h"


static MemoryContext LogicalRepRelMapContext = NULL;

static HTAB *LogicalRepRelMap = NULL;

/*
 * Partition map (LogicalRepPartMap)
 *
 * When a partitioned table is used as replication target, replicated
 * operations are actually performed on its leaf partitions, which requires
 * the partitions to also be mapped to the remote relation.  Parent's entry
 * (LogicalRepRelMapEntry) cannot be used as-is for all partitions, because
 * individual partitions may have different attribute numbers, which means
 * attribute mappings to remote relation's attributes must be maintained
 * separately for each partition.
 */
static MemoryContext LogicalRepPartMapContext = NULL;
static HTAB *LogicalRepPartMap = NULL;
typedef struct LogicalRepPartMapEntry
{
	Oid			partoid;		/* LogicalRepPartMap's key */
	LogicalRepRelMapEntry relmapentry;
} LogicalRepPartMapEntry;

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
				entry->localrelvalid = false;
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
			entry->localrelvalid = false;
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

	/* Watch for invalidation events. */
	CacheRegisterRelcacheCallback(logicalrep_relmap_invalidate_cb,
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
		free_attrmap(entry->attrmap);
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
 * Check if replica identity matches and mark the updatable flag.
 *
 * We allow for stricter replica identity (fewer columns) on subscriber as
 * that will not stop us from finding unique tuple. IE, if publisher has
 * identity (id,timestamp) and subscriber just (id) this will not be a
 * problem, but in the opposite scenario it will.
 *
 * We just mark the relation entry as not updatable here if the local
 * replica identity is found to be insufficient for applying
 * updates/deletes (inserts don't care!) and leave it to
 * check_relation_updatable() to throw the actual error if needed.
 */
static void
logicalrep_rel_mark_updatable(LogicalRepRelMapEntry *entry)
{
	Bitmapset  *idkey;
	LogicalRepRelation *remoterel = &entry->remoterel;
	int			i;

	entry->updatable = true;

	idkey = RelationGetIndexAttrBitmap(entry->localrel,
									   INDEX_ATTR_BITMAP_IDENTITY_KEY);
	/* fallback to PK if no replica identity */
	if (idkey == NULL)
	{
		idkey = RelationGetIndexAttrBitmap(entry->localrel,
										   INDEX_ATTR_BITMAP_PRIMARY_KEY);

		/*
		 * If no replica identity index and no PK, the published table must
		 * have replica identity FULL.
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

		if (entry->attrmap->attnums[attnum] < 0 ||
			!bms_is_member(entry->attrmap->attnums[attnum], remoterel->attkeys))
		{
			entry->updatable = false;
			break;
		}
	}
}

/*
 * Open the local relation associated with the remote one.
 *
 * Rebuilds the Relcache mapping if it was invalidated by local DDL.
 */
LogicalRepRelMapEntry *
logicalrep_rel_open(LogicalRepRelId remoteid, LOCKMODE lockmode)
{
	LogicalRepRelMapEntry *entry;
	bool		found;
	LogicalRepRelation *remoterel;

	if (LogicalRepRelMap == NULL)
		logicalrep_relmap_init();

	/* Search for existing entry. */
	entry = hash_search(LogicalRepRelMap, (void *) &remoteid,
						HASH_FIND, &found);

	if (!found)
		elog(ERROR, "no relation map entry for remote relation ID %u",
			 remoteid);

	remoterel = &entry->remoterel;

	/* Ensure we don't leak a relcache refcount. */
	if (entry->localrel)
		elog(ERROR, "remote relation ID %u is already open", remoteid);

	/*
	 * When opening and locking a relation, pending invalidation messages are
	 * processed which can invalidate the relation.  Hence, if the entry is
	 * currently considered valid, try to open the local relation by OID and
	 * see if invalidation ensues.
	 */
	if (entry->localrelvalid)
	{
		entry->localrel = try_relation_open(entry->localreloid, lockmode);
		if (!entry->localrel)
		{
			/* Table was renamed or dropped. */
			entry->localrelvalid = false;
		}
		else if (!entry->localrelvalid)
		{
			/* Note we release the no-longer-useful lock here. */
			table_close(entry->localrel, lockmode);
			entry->localrel = NULL;
		}
	}

	/*
	 * If the entry has been marked invalid since we last had lock on it,
	 * re-open the local relation by name and rebuild all derived data.
	 */
	if (!entry->localrelvalid)
	{
		Oid			relid;
		int			found;
		TupleDesc	desc;
		MemoryContext oldctx;
		int			i;

		/* Release the no-longer-useful attrmap, if any. */
		if (entry->attrmap)
		{
			free_attrmap(entry->attrmap);
			entry->attrmap = NULL;
		}

		/* Try to find and lock the relation by name. */
		relid = RangeVarGetRelid(makeRangeVar(remoterel->nspname,
											  remoterel->relname, -1),
								 lockmode, true);
		if (!OidIsValid(relid))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("logical replication target relation \"%s.%s\" does not exist",
							remoterel->nspname, remoterel->relname)));
		entry->localrel = table_open(relid, NoLock);
		entry->localreloid = relid;

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
		entry->attrmap = make_attrmap(desc->natts);
		MemoryContextSwitchTo(oldctx);

		found = 0;
		for (i = 0; i < desc->natts; i++)
		{
			int			attnum;
			Form_pg_attribute attr = TupleDescAttr(desc, i);

			if (attr->attisdropped || attr->attgenerated)
			{
				entry->attrmap->attnums[i] = -1;
				continue;
			}

			attnum = logicalrep_rel_att_by_name(remoterel,
												NameStr(attr->attname));

			entry->attrmap->attnums[i] = attnum;
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
		 * Set if the table's replica identity is enough to apply
		 * update/delete.
		 */
		logicalrep_rel_mark_updatable(entry);

		entry->localrelvalid = true;
	}

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
	table_close(rel->localrel, lockmode);
	rel->localrel = NULL;
}

/*
 * Partition cache: look up partition LogicalRepRelMapEntry's
 *
 * Unlike relation map cache, this is keyed by partition OID, not remote
 * relation OID, because we only have to use this cache in the case where
 * partitions are not directly mapped to any remote relation, such as when
 * replication is occurring with one of their ancestors as target.
 */

/*
 * Relcache invalidation callback
 */
static void
logicalrep_partmap_invalidate_cb(Datum arg, Oid reloid)
{
	LogicalRepPartMapEntry *entry;

	/* Just to be sure. */
	if (LogicalRepPartMap == NULL)
		return;

	if (reloid != InvalidOid)
	{
		HASH_SEQ_STATUS status;

		hash_seq_init(&status, LogicalRepPartMap);

		/* TODO, use inverse lookup hashtable? */
		while ((entry = (LogicalRepPartMapEntry *) hash_seq_search(&status)) != NULL)
		{
			if (entry->relmapentry.localreloid == reloid)
			{
				entry->relmapentry.localrelvalid = false;
				hash_seq_term(&status);
				break;
			}
		}
	}
	else
	{
		/* invalidate all cache entries */
		HASH_SEQ_STATUS status;

		hash_seq_init(&status, LogicalRepPartMap);

		while ((entry = (LogicalRepPartMapEntry *) hash_seq_search(&status)) != NULL)
			entry->relmapentry.localrelvalid = false;
	}
}

/*
 * Reset the entries in the partition map that refer to remoterel.
 *
 * Called when new relation mapping is sent by the publisher to update our
 * expected view of incoming data from said publisher.
 *
 * Note that we don't update the remoterel information in the entry here,
 * we will update the information in logicalrep_partition_open to avoid
 * unnecessary work.
 */
void
logicalrep_partmap_reset_relmap(LogicalRepRelation *remoterel)
{
	HASH_SEQ_STATUS status;
	LogicalRepPartMapEntry *part_entry;
	LogicalRepRelMapEntry *entry;

	if (LogicalRepPartMap == NULL)
		return;

	hash_seq_init(&status, LogicalRepPartMap);
	while ((part_entry = (LogicalRepPartMapEntry *) hash_seq_search(&status)) != NULL)
	{
		entry = &part_entry->relmapentry;

		if (entry->remoterel.remoteid != remoterel->remoteid)
			continue;

		logicalrep_relmap_free_entry(entry);

		memset(entry, 0, sizeof(LogicalRepRelMapEntry));
	}
}

/*
 * Initialize the partition map cache.
 */
static void
logicalrep_partmap_init(void)
{
	HASHCTL		ctl;

	if (!LogicalRepPartMapContext)
		LogicalRepPartMapContext =
			AllocSetContextCreate(CacheMemoryContext,
								  "LogicalRepPartMapContext",
								  ALLOCSET_DEFAULT_SIZES);

	/* Initialize the relation hash table. */
	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(Oid);	/* partition OID */
	ctl.entrysize = sizeof(LogicalRepPartMapEntry);
	ctl.hcxt = LogicalRepPartMapContext;

	LogicalRepPartMap = hash_create("logicalrep partition map cache", 64, &ctl,
									HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/* Watch for invalidation events. */
	CacheRegisterRelcacheCallback(logicalrep_partmap_invalidate_cb,
								  (Datum) 0);
}

/*
 * logicalrep_partition_open
 *
 * Returned entry reuses most of the values of the root table's entry, save
 * the attribute map, which can be different for the partition.  However,
 * we must physically copy all the data, in case the root table's entry
 * gets freed/rebuilt.
 *
 * Note there's no logicalrep_partition_close, because the caller closes the
 * the component relation.
 */
LogicalRepRelMapEntry *
logicalrep_partition_open(LogicalRepRelMapEntry *root,
						  Relation partrel, AttrMap *map)
{
	LogicalRepRelMapEntry *entry;
	LogicalRepPartMapEntry *part_entry;
	LogicalRepRelation *remoterel = &root->remoterel;
	Oid			partOid = RelationGetRelid(partrel);
	AttrMap    *attrmap = root->attrmap;
	bool		found;
	MemoryContext oldctx;

	if (LogicalRepPartMap == NULL)
		logicalrep_partmap_init();

	/* Search for existing entry. */
	part_entry = (LogicalRepPartMapEntry *) hash_search(LogicalRepPartMap,
														(void *) &partOid,
														HASH_ENTER, &found);

	entry = &part_entry->relmapentry;

	/*
	 * We must always overwrite entry->localrel with the latest partition
	 * Relation pointer, because the Relation pointed to by the old value may
	 * have been cleared after the caller would have closed the partition
	 * relation after the last use of this entry.  Note that localrelvalid is
	 * only updated by the relcache invalidation callback, so it may still be
	 * true irrespective of whether the Relation pointed to by localrel has
	 * been cleared or not.
	 */
	if (found && entry->localrelvalid)
	{
		entry->localrel = partrel;
		return entry;
	}

	/* Switch to longer-lived context. */
	oldctx = MemoryContextSwitchTo(LogicalRepPartMapContext);

	if (!found)
	{
		memset(part_entry, 0, sizeof(LogicalRepPartMapEntry));
		part_entry->partoid = partOid;
	}

	/* Release the no-longer-useful attrmap, if any. */
	if (entry->attrmap)
	{
		free_attrmap(entry->attrmap);
		entry->attrmap = NULL;
	}

	if (!entry->remoterel.remoteid)
	{
		int			i;

		/* Remote relation is copied as-is from the root entry. */
		entry = &part_entry->relmapentry;
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
	}

	entry->localrel = partrel;
	entry->localreloid = partOid;

	/*
	 * If the partition's attributes don't match the root relation's, we'll
	 * need to make a new attrmap which maps partition attribute numbers to
	 * remoterel's, instead of the original which maps root relation's
	 * attribute numbers to remoterel's.
	 *
	 * Note that 'map' which comes from the tuple routing data structure
	 * contains 1-based attribute numbers (of the parent relation).  However,
	 * the map in 'entry', a logical replication data structure, contains
	 * 0-based attribute numbers (of the remote relation).
	 */
	if (map)
	{
		AttrNumber	attno;

		entry->attrmap = make_attrmap(map->maplen);
		for (attno = 0; attno < entry->attrmap->maplen; attno++)
		{
			AttrNumber	root_attno = map->attnums[attno];

			/* 0 means it's a dropped attribute.  See comments atop AttrMap. */
			if (root_attno == 0)
				entry->attrmap->attnums[attno] = -1;
			else
				entry->attrmap->attnums[attno] = attrmap->attnums[root_attno - 1];
		}
	}
	else
	{
		/* Lacking copy_attmap, do this the hard way. */
		entry->attrmap = make_attrmap(attrmap->maplen);
		memcpy(entry->attrmap->attnums, attrmap->attnums,
			   attrmap->maplen * sizeof(AttrNumber));
	}

	/* Set if the table's replica identity is enough to apply update/delete. */
	logicalrep_rel_mark_updatable(entry);

	entry->localrelvalid = true;

	/* state and statelsn are left set to 0. */
	MemoryContextSwitchTo(oldctx);

	return entry;
}
