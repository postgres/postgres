/*-------------------------------------------------------------------------
 *
 * typcache.c
 *	  POSTGRES type cache code
 *
 * The type cache exists to speed lookup of certain information about data
 * types that is not directly available from a type's pg_type row.  In
 * particular, we use a type's default btree opclass, or the default hash
 * opclass if no btree opclass exists, to determine which operators should
 * be used for grouping and sorting the type (GROUP BY, ORDER BY ASC/DESC).
 *
 * Several seemingly-odd choices have been made to support use of the type
 * cache by the generic array comparison routines array_eq() and array_cmp().
 * Because these routines are used as index support operations, they cannot
 * leak memory.  To allow them to execute efficiently, all information that
 * either of them would like to re-use across calls is made available in the
 * type cache.
 *
 * Once created, a type cache entry lives as long as the backend does, so
 * there is no need for a call to release a cache entry.  (For present uses,
 * it would be okay to flush type cache entries at the ends of transactions,
 * if we needed to reclaim space.)
 *
 * There is presently no provision for clearing out a cache entry if the
 * stored data becomes obsolete.  (The code will work if a type acquires
 * opclasses it didn't have before while a backend runs --- but not if the
 * definition of an existing opclass is altered.)  However, the relcache
 * doesn't cope with opclasses changing under it, either, so this seems
 * a low-priority problem.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/cache/typcache.c,v 1.1 2003/08/17 19:58:06 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/hash.h"
#include "access/nbtree.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_am.h"
#include "catalog/pg_opclass.h"
#include "parser/parse_coerce.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"


static HTAB *TypeCacheHash = NULL;


static Oid lookup_default_opclass(Oid type_id, Oid am_id);


/*
 * lookup_type_cache
 *
 * Fetch the type cache entry for the specified datatype, and make sure that
 * all the fields requested by bits in 'flags' are valid.
 *
 * The result is never NULL --- we will elog() if the passed type OID is
 * invalid.  Note however that we may fail to find one or more of the
 * requested opclass-dependent fields; the caller needs to check whether
 * the fields are InvalidOid or not.
 */
TypeCacheEntry *
lookup_type_cache(Oid type_id, int flags)
{
	TypeCacheEntry *typentry;
	bool		found;

	if (TypeCacheHash == NULL)
	{
		/* First time through: initialize the hash table */
		HASHCTL		ctl;

		if (!CacheMemoryContext)
			CreateCacheMemoryContext();

		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(TypeCacheEntry);
		ctl.hash = tag_hash;
		TypeCacheHash = hash_create("Type information cache", 64,
									&ctl, HASH_ELEM | HASH_FUNCTION);
	}

	/* Try to look up an existing entry */
	typentry = (TypeCacheEntry *) hash_search(TypeCacheHash,
											  (void *) &type_id,
											  HASH_FIND, NULL);
	if (typentry == NULL)
	{
		/*
		 * If we didn't find one, we want to make one.  But first get the
		 * required info from the pg_type row, just to make sure we don't
		 * make a cache entry for an invalid type OID.
		 */
		int16	typlen;
		bool	typbyval;
		char	typalign;

		get_typlenbyvalalign(type_id, &typlen, &typbyval, &typalign);

		typentry = (TypeCacheEntry *) hash_search(TypeCacheHash,
												  (void *) &type_id,
												  HASH_ENTER, &found);
		if (typentry == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		Assert(!found);			/* it wasn't there a moment ago */

		MemSet(typentry, 0, sizeof(TypeCacheEntry));
		typentry->type_id = type_id;
		typentry->typlen = typlen;
		typentry->typbyval = typbyval;
		typentry->typalign = typalign;
	}

	/* If we haven't already found the opclass, try to do so */
	if (flags != 0 && typentry->btree_opc == InvalidOid)
	{
		typentry->btree_opc = lookup_default_opclass(type_id,
													 BTREE_AM_OID);
		/* Only care about hash opclass if no btree opclass... */
		if (typentry->btree_opc == InvalidOid)
		{
			if (typentry->hash_opc == InvalidOid)
				typentry->hash_opc = lookup_default_opclass(type_id,
															HASH_AM_OID);
		}
		else
		{
			/*
			 * If we find a btree opclass where previously we only found
			 * a hash opclass, forget the hash equality operator so we
			 * can use the btree operator instead.
			 */
			typentry->eq_opr = InvalidOid;
			typentry->eq_opr_finfo.fn_oid = InvalidOid;
		}
	}

	/* Look for requested operators and functions */
	if ((flags & (TYPECACHE_EQ_OPR | TYPECACHE_EQ_OPR_FINFO)) &&
		typentry->eq_opr == InvalidOid)
	{
		if (typentry->btree_opc != InvalidOid)
			typentry->eq_opr = get_opclass_member(typentry->btree_opc,
												  BTEqualStrategyNumber);
		if (typentry->eq_opr == InvalidOid &&
			typentry->hash_opc != InvalidOid)
			typentry->eq_opr = get_opclass_member(typentry->hash_opc,
												  HTEqualStrategyNumber);
	}
	if ((flags & TYPECACHE_LT_OPR) && typentry->lt_opr == InvalidOid)
	{
		if (typentry->btree_opc != InvalidOid)
			typentry->lt_opr = get_opclass_member(typentry->btree_opc,
												  BTLessStrategyNumber);
	}
	if ((flags & TYPECACHE_GT_OPR) && typentry->gt_opr == InvalidOid)
	{
		if (typentry->btree_opc != InvalidOid)
			typentry->gt_opr = get_opclass_member(typentry->btree_opc,
												  BTGreaterStrategyNumber);
	}
	if ((flags & (TYPECACHE_CMP_PROC | TYPECACHE_CMP_PROC_FINFO)) &&
		typentry->cmp_proc == InvalidOid)
	{
		if (typentry->btree_opc != InvalidOid)
			typentry->cmp_proc = get_opclass_proc(typentry->btree_opc,
												  BTORDER_PROC);
	}

	/*
	 * Set up fmgr lookup info as requested
	 *
	 * Note: we tell fmgr the finfo structures live in CacheMemoryContext,
	 * which is not quite right (they're really in DynaHashContext) but this
	 * will do for our purposes.
	 */
	if ((flags & TYPECACHE_EQ_OPR_FINFO) &&
		typentry->eq_opr_finfo.fn_oid == InvalidOid &&
		typentry->eq_opr != InvalidOid)
	{
		Oid		eq_opr_func;

		eq_opr_func = get_opcode(typentry->eq_opr);
		if (eq_opr_func != InvalidOid)
			fmgr_info_cxt(eq_opr_func, &typentry->eq_opr_finfo,
						  CacheMemoryContext);
	}
	if ((flags & TYPECACHE_CMP_PROC_FINFO) &&
		typentry->cmp_proc_finfo.fn_oid == InvalidOid &&
		typentry->cmp_proc != InvalidOid)
	{
		fmgr_info_cxt(typentry->cmp_proc, &typentry->cmp_proc_finfo,
					  CacheMemoryContext);
	}

	return typentry;
}

/*
 * lookup_default_opclass
 *
 * Given the OIDs of a datatype and an access method, find the default
 * operator class, if any.  Returns InvalidOid if there is none.
 */
static Oid
lookup_default_opclass(Oid type_id, Oid am_id)
{
	int			nexact = 0;
	int			ncompatible = 0;
	Oid			exactOid = InvalidOid;
	Oid			compatibleOid = InvalidOid;
	Relation	rel;
	ScanKeyData skey[1];
	SysScanDesc scan;
	HeapTuple	tup;

	/* If it's a domain, look at the base type instead */
	type_id = getBaseType(type_id);

	/*
	 * We scan through all the opclasses available for the access method,
	 * looking for one that is marked default and matches the target type
	 * (either exactly or binary-compatibly, but prefer an exact match).
	 *
	 * We could find more than one binary-compatible match, in which case we
	 * require the user to specify which one he wants.	If we find more
	 * than one exact match, then someone put bogus entries in pg_opclass.
	 *
	 * This is the same logic as GetDefaultOpClass() in indexcmds.c, except
	 * that we consider all opclasses, regardless of the current search path.
	 */
	rel = heap_openr(OperatorClassRelationName, AccessShareLock);

	ScanKeyEntryInitialize(&skey[0], 0x0,
						   Anum_pg_opclass_opcamid, F_OIDEQ,
						   ObjectIdGetDatum(am_id));

	scan = systable_beginscan(rel, OpclassAmNameNspIndex, true,
							  SnapshotNow, 1, skey);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_opclass opclass = (Form_pg_opclass) GETSTRUCT(tup);

		if (opclass->opcdefault)
		{
			if (opclass->opcintype == type_id)
			{
				nexact++;
				exactOid = HeapTupleGetOid(tup);
			}
			else if (IsBinaryCoercible(type_id, opclass->opcintype))
			{
				ncompatible++;
				compatibleOid = HeapTupleGetOid(tup);
			}
		}
	}

	systable_endscan(scan);

	heap_close(rel, AccessShareLock);

	if (nexact == 1)
		return exactOid;
	if (nexact != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("there are multiple default operator classes for data type %s",
						format_type_be(type_id))));
	if (ncompatible == 1)
		return compatibleOid;

	return InvalidOid;
}
