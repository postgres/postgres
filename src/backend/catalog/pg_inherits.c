/*-------------------------------------------------------------------------
 *
 * pg_inherits.c
 *	  routines to support manipulation of the pg_inherits relation
 *
 * Note: currently, this module only contains inquiry functions; the actual
 * creation and deletion of pg_inherits entries is done in tablecmds.c.
 * Perhaps someday that code should be moved here, but it'd have to be
 * disentangled from other stuff such as pg_depend updates.
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
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
#include "access/heapam.h"
#include "access/htup_details.h"
#include "catalog/indexing.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_inherits_fn.h"
#include "parser/parse_type.h"
#include "storage/lmgr.h"
#include "utils/fmgroids.h"
#include "utils/syscache.h"
#include "utils/tqual.h"

static int	oid_cmp(const void *p1, const void *p2);


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

	relation = heap_open(InheritsRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_inherits_inhparent,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(parentrelId));

	scan = systable_beginscan(relation, InheritsParentIndexId, true,
							  SnapshotNow, 1, key);

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

	heap_close(relation, AccessShareLock);

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
	List	   *rels_list,
			   *rel_numparents;
	ListCell   *l;

	/*
	 * We build a list starting with the given rel and adding all direct and
	 * indirect children.  We can use a single list as both the record of
	 * already-found rels and the agenda of rels yet to be scanned for more
	 * children.  This is a bit tricky but works because the foreach() macro
	 * doesn't fetch the next list element until the bottom of the loop.
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
			bool		found = false;
			ListCell   *lo;
			ListCell   *li;

			/* if the rel is already there, bump number-of-parents counter */
			forboth(lo, rels_list, li, rel_numparents)
			{
				if (lfirst_oid(lo) == child_oid)
				{
					lfirst_int(li)++;
					found = true;
					break;
				}
			}

			/* if it's not there, add it. expect 1 parent, initially. */
			if (!found)
			{
				rels_list = lappend_oid(rels_list, child_oid);
				rel_numparents = lappend_int(rel_numparents, 1);
			}
		}
	}

	if (numparents)
		*numparents = rel_numparents;
	else
		list_free(rel_numparents);
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
 * Given two type OIDs, determine whether the first is a complex type
 * (class type) that inherits from the second.
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
	subclassRelid = typeidTypeRelid(subclassTypeId);
	if (subclassRelid == InvalidOid)
		return false;			/* not a complex type */
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

	inhrel = heap_open(InheritsRelationId, AccessShareLock);

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
									 SnapshotNow, 1, &skey);

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
	heap_close(inhrel, AccessShareLock);

	list_free(visited);
	list_free(queue);

	return result;
}


/* qsort comparison function */
static int
oid_cmp(const void *p1, const void *p2)
{
	Oid			v1 = *((const Oid *) p1);
	Oid			v2 = *((const Oid *) p2);

	if (v1 < v2)
		return -1;
	if (v1 > v2)
		return 1;
	return 0;
}
