/*-------------------------------------------------------------------------
 *
 * pg_inherits.c
 *	  routines to support manipulation of the pg_inherits relation
 *
 * Note: currently, this module mostly contains inquiry functions; actual
 * creation and deletion of pg_inherits entries is mostly done in tablecmds.c.
 * Perhaps someday that code should be moved here, but it'd have to be
 * disentangled from other stuff such as pg_depend updates.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_inherits.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/pg_inherits.h"
#include "parser/parse_type.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

/*
 * Entry of a hash table used in find_all_inheritors. See below.
 */
typedef struct SeenRelsEntry
{
	Oid			rel_id;			/* relation oid */
	int			list_index;		/* its position in output list(s) */
} SeenRelsEntry;

/*
 * find_inheritance_children
 *
 * Returns a list containing the OIDs of all relations which
 * inherit *directly* from the relation with OID 'parentrelId'.
 *
 * The specified lock type is acquired on each child relation (but not on the
 * given rel; caller should already have locked it).  If lockmode is NoLock
 * then no locks are acquired, but caller must beware of race conditions
 * against possible DROPs of child relations.
 */
List *
find_inheritance_children(Oid parentrelId, LOCKMODE lockmode)
{
	List	   *list = NIL;
	Relation	relation;
	SysScanDesc scan;
	ScanKeyData key[1];
	HeapTuple	inheritsTuple;
	Oid			inhrelid;
	Oid		   *oidarr;
	int			maxoids,
				numoids,
				i;

	/*
	 * Can skip the scan if pg_class shows the relation has never had a
	 * subclass.
	 */
	if (!has_subclass(parentrelId))
		return NIL;

	/*
	 * Scan pg_inherits and build a working array of subclass OIDs.
	 */
	maxoids = 32;
	oidarr = (Oid *) palloc(maxoids * sizeof(Oid));
	numoids = 0;

	relation = table_open(InheritsRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_inherits_inhparent,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(parentrelId));

	scan = systable_beginscan(relation, InheritsParentIndexId, true,
							  NULL, 1, key);

	while ((inheritsTuple = systable_getnext(scan)) != NULL)
	{
		inhrelid = ((Form_pg_inherits) GETSTRUCT(inheritsTuple))->inhrelid;
		if (numoids >= maxoids)
		{
			maxoids *= 2;
			oidarr = (Oid *) repalloc(oidarr, maxoids * sizeof(Oid));
		}
		oidarr[numoids++] = inhrelid;
	}

	systable_endscan(scan);

	table_close(relation, AccessShareLock);

	/*
	 * If we found more than one child, sort them by OID.  This ensures
	 * reasonably consistent behavior regardless of the vagaries of an
	 * indexscan.  This is important since we need to be sure all backends
	 * lock children in the same order to avoid needless deadlocks.
	 */
	if (numoids > 1)
		qsort(oidarr, numoids, sizeof(Oid), oid_cmp);

	/*
	 * Acquire locks and build the result list.
	 */
	for (i = 0; i < numoids; i++)
	{
		inhrelid = oidarr[i];

		if (lockmode != NoLock)
		{
			/* Get the lock to synchronize against concurrent drop */
			LockRelationOid(inhrelid, lockmode);

			/*
			 * Now that we have the lock, double-check to see if the relation
			 * really exists or not.  If not, assume it was dropped while we
			 * waited to acquire lock, and ignore it.
			 */
			if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(inhrelid)))
			{
				/* Release useless lock */
				UnlockRelationOid(inhrelid, lockmode);
				/* And ignore this relation */
				continue;
			}
		}

		list = lappend_oid(list, inhrelid);
	}

	pfree(oidarr);

	return list;
}


/*
 * find_all_inheritors -
 *		Returns a list of relation OIDs including the given rel plus
 *		all relations that inherit from it, directly or indirectly.
 *		Optionally, it also returns the number of parents found for
 *		each such relation within the inheritance tree rooted at the
 *		given rel.
 *
 * The specified lock type is acquired on all child relations (but not on the
 * given rel; caller should already have locked it).  If lockmode is NoLock
 * then no locks are acquired, but caller must beware of race conditions
 * against possible DROPs of child relations.
 */
List *
find_all_inheritors(Oid parentrelId, LOCKMODE lockmode, List **numparents)
{
	/* hash table for O(1) rel_oid -> rel_numparents cell lookup */
	HTAB	   *seen_rels;
	HASHCTL		ctl;
	List	   *rels_list,
			   *rel_numparents;
	ListCell   *l;

	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(SeenRelsEntry);
	ctl.hcxt = CurrentMemoryContext;

	seen_rels = hash_create("find_all_inheritors temporary table",
							32, /* start small and extend */
							&ctl,
							HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/*
	 * We build a list starting with the given rel and adding all direct and
	 * indirect children.  We can use a single list as both the record of
	 * already-found rels and the agenda of rels yet to be scanned for more
	 * children.  This is a bit tricky but works because the foreach() macro
	 * doesn't fetch the next list element until the bottom of the loop.  Note
	 * that we can't keep pointers into the output lists; but an index is
	 * sufficient.
	 */
	rels_list = list_make1_oid(parentrelId);
	rel_numparents = list_make1_int(0);

	foreach(l, rels_list)
	{
		Oid			currentrel = lfirst_oid(l);
		List	   *currentchildren;
		ListCell   *lc;

		/* Get the direct children of this rel */
		currentchildren = find_inheritance_children(currentrel, lockmode);

		/*
		 * Add to the queue only those children not already seen. This avoids
		 * making duplicate entries in case of multiple inheritance paths from
		 * the same parent.  (It'll also keep us from getting into an infinite
		 * loop, though theoretically there can't be any cycles in the
		 * inheritance graph anyway.)
		 */
		foreach(lc, currentchildren)
		{
			Oid			child_oid = lfirst_oid(lc);
			bool		found;
			SeenRelsEntry *hash_entry;

			hash_entry = hash_search(seen_rels, &child_oid, HASH_ENTER, &found);
			if (found)
			{
				/* if the rel is already there, bump number-of-parents counter */
				ListCell   *numparents_cell;

				numparents_cell = list_nth_cell(rel_numparents,
												hash_entry->list_index);
				lfirst_int(numparents_cell)++;
			}
			else
			{
				/* if it's not there, add it. expect 1 parent, initially. */
				hash_entry->list_index = list_length(rels_list);
				rels_list = lappend_oid(rels_list, child_oid);
				rel_numparents = lappend_int(rel_numparents, 1);
			}
		}
	}

	if (numparents)
		*numparents = rel_numparents;
	else
		list_free(rel_numparents);

	hash_destroy(seen_rels);

	return rels_list;
}


/*
 * has_subclass - does this relation have any children?
 *
 * In the current implementation, has_subclass returns whether a
 * particular class *might* have a subclass. It will not return the
 * correct result if a class had a subclass which was later dropped.
 * This is because relhassubclass in pg_class is not updated immediately
 * when a subclass is dropped, primarily because of concurrency concerns.
 *
 * Currently has_subclass is only used as an efficiency hack to skip
 * unnecessary inheritance searches, so this is OK.  Note that ANALYZE
 * on a childless table will clean up the obsolete relhassubclass flag.
 *
 * Although this doesn't actually touch pg_inherits, it seems reasonable
 * to keep it here since it's normally used with the other routines here.
 */
bool
has_subclass(Oid relationId)
{
	HeapTuple	tuple;
	bool		result;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relationId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relationId);

	result = ((Form_pg_class) GETSTRUCT(tuple))->relhassubclass;
	ReleaseSysCache(tuple);
	return result;
}

/*
 * has_superclass - does this relation inherit from another?
 *
 * Unlike has_subclass, this can be relied on to give an accurate answer.
 * However, the caller must hold a lock on the given relation so that it
 * can't be concurrently added to or removed from an inheritance hierarchy.
 */
bool
has_superclass(Oid relationId)
{
	Relation	catalog;
	SysScanDesc scan;
	ScanKeyData skey;
	bool		result;

	catalog = table_open(InheritsRelationId, AccessShareLock);
	ScanKeyInit(&skey, Anum_pg_inherits_inhrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(relationId));
	scan = systable_beginscan(catalog, InheritsRelidSeqnoIndexId, true,
							  NULL, 1, &skey);
	result = HeapTupleIsValid(systable_getnext(scan));
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);

	return result;
}

/*
 * Given two type OIDs, determine whether the first is a complex type
 * (class type) that inherits from the second.
 *
 * This essentially asks whether the first type is guaranteed to be coercible
 * to the second.  Therefore, we allow the first type to be a domain over a
 * complex type that inherits from the second; that creates no difficulties.
 * But the second type cannot be a domain.
 */
bool
typeInheritsFrom(Oid subclassTypeId, Oid superclassTypeId)
{
	bool		result = false;
	Oid			subclassRelid;
	Oid			superclassRelid;
	Relation	inhrel;
	List	   *visited,
			   *queue;
	ListCell   *queue_item;

	/* We need to work with the associated relation OIDs */
	subclassRelid = typeOrDomainTypeRelid(subclassTypeId);
	if (subclassRelid == InvalidOid)
		return false;			/* not a complex type or domain over one */
	superclassRelid = typeidTypeRelid(superclassTypeId);
	if (superclassRelid == InvalidOid)
		return false;			/* not a complex type */

	/* No point in searching if the superclass has no subclasses */
	if (!has_subclass(superclassRelid))
		return false;

	/*
	 * Begin the search at the relation itself, so add its relid to the queue.
	 */
	queue = list_make1_oid(subclassRelid);
	visited = NIL;

	inhrel = table_open(InheritsRelationId, AccessShareLock);

	/*
	 * Use queue to do a breadth-first traversal of the inheritance graph from
	 * the relid supplied up to the root.  Notice that we append to the queue
	 * inside the loop --- this is okay because the foreach() macro doesn't
	 * advance queue_item until the next loop iteration begins.
	 */
	foreach(queue_item, queue)
	{
		Oid			this_relid = lfirst_oid(queue_item);
		ScanKeyData skey;
		SysScanDesc inhscan;
		HeapTuple	inhtup;

		/*
		 * If we've seen this relid already, skip it.  This avoids extra work
		 * in multiple-inheritance scenarios, and also protects us from an
		 * infinite loop in case there is a cycle in pg_inherits (though
		 * theoretically that shouldn't happen).
		 */
		if (list_member_oid(visited, this_relid))
			continue;

		/*
		 * Okay, this is a not-yet-seen relid. Add it to the list of
		 * already-visited OIDs, then find all the types this relid inherits
		 * from and add them to the queue.
		 */
		visited = lappend_oid(visited, this_relid);

		ScanKeyInit(&skey,
					Anum_pg_inherits_inhrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(this_relid));

		inhscan = systable_beginscan(inhrel, InheritsRelidSeqnoIndexId, true,
									 NULL, 1, &skey);

		while ((inhtup = systable_getnext(inhscan)) != NULL)
		{
			Form_pg_inherits inh = (Form_pg_inherits) GETSTRUCT(inhtup);
			Oid			inhparent = inh->inhparent;

			/* If this is the target superclass, we're done */
			if (inhparent == superclassRelid)
			{
				result = true;
				break;
			}

			/* Else add to queue */
			queue = lappend_oid(queue, inhparent);
		}

		systable_endscan(inhscan);

		if (result)
			break;
	}

	/* clean up ... */
	table_close(inhrel, AccessShareLock);

	list_free(visited);
	list_free(queue);

	return result;
}

/*
 * Create a single pg_inherits row with the given data
 */
void
StoreSingleInheritance(Oid relationId, Oid parentOid, int32 seqNumber)
{
	Datum		values[Natts_pg_inherits];
	bool		nulls[Natts_pg_inherits];
	HeapTuple	tuple;
	Relation	inhRelation;

	inhRelation = table_open(InheritsRelationId, RowExclusiveLock);

	/*
	 * Make the pg_inherits entry
	 */
	values[Anum_pg_inherits_inhrelid - 1] = ObjectIdGetDatum(relationId);
	values[Anum_pg_inherits_inhparent - 1] = ObjectIdGetDatum(parentOid);
	values[Anum_pg_inherits_inhseqno - 1] = Int32GetDatum(seqNumber);

	memset(nulls, 0, sizeof(nulls));

	tuple = heap_form_tuple(RelationGetDescr(inhRelation), values, nulls);

	CatalogTupleInsert(inhRelation, tuple);

	heap_freetuple(tuple);

	table_close(inhRelation, RowExclusiveLock);
}

/*
 * DeleteInheritsTuple
 *
 * Delete pg_inherits tuples with the given inhrelid.  inhparent may be given
 * as InvalidOid, in which case all tuples matching inhrelid are deleted;
 * otherwise only delete tuples with the specified inhparent.
 *
 * Returns whether at least one row was deleted.
 */
bool
DeleteInheritsTuple(Oid inhrelid, Oid inhparent)
{
	bool		found = false;
	Relation	catalogRelation;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	inheritsTuple;

	/*
	 * Find pg_inherits entries by inhrelid.
	 */
	catalogRelation = table_open(InheritsRelationId, RowExclusiveLock);
	ScanKeyInit(&key,
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(inhrelid));
	scan = systable_beginscan(catalogRelation, InheritsRelidSeqnoIndexId,
							  true, NULL, 1, &key);

	while (HeapTupleIsValid(inheritsTuple = systable_getnext(scan)))
	{
		Oid			parent;

		/* Compare inhparent if it was given, and do the actual deletion. */
		parent = ((Form_pg_inherits) GETSTRUCT(inheritsTuple))->inhparent;
		if (!OidIsValid(inhparent) || parent == inhparent)
		{
			CatalogTupleDelete(catalogRelation, &inheritsTuple->t_self);
			found = true;
		}
	}

	/* Done */
	systable_endscan(scan);
	table_close(catalogRelation, RowExclusiveLock);

	return found;
}
