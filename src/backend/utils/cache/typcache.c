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
 * We do support clearing the tuple descriptor part of a rowtype's cache
 * entry, since that may need to change as a consequence of ALTER TABLE.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/cache/typcache.c,v 1.22 2006/10/04 00:30:01 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "access/heapam.h"
#include "access/nbtree.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


/* The main type cache hashtable searched by lookup_type_cache */
static HTAB *TypeCacheHash = NULL;

/*
 * We use a separate table for storing the definitions of non-anonymous
 * record types.  Once defined, a record type will be remembered for the
 * life of the backend.  Subsequent uses of the "same" record type (where
 * sameness means equalTupleDescs) will refer to the existing table entry.
 *
 * Stored record types are remembered in a linear array of TupleDescs,
 * which can be indexed quickly with the assigned typmod.  There is also
 * a hash table to speed searches for matching TupleDescs.	The hash key
 * uses just the first N columns' type OIDs, and so we may have multiple
 * entries with the same hash key.
 */
#define REC_HASH_KEYS	16		/* use this many columns in hash key */

typedef struct RecordCacheEntry
{
	/* the hash lookup key MUST BE FIRST */
	Oid			hashkey[REC_HASH_KEYS]; /* column type IDs, zero-filled */

	/* list of TupleDescs for record types with this hashkey */
	List	   *tupdescs;
} RecordCacheEntry;

static HTAB *RecordCacheHash = NULL;

static TupleDesc *RecordCacheArray = NULL;
static int32 RecordCacheArrayLen = 0;	/* allocated length of array */
static int32 NextRecordTypmod = 0;		/* number of entries used */


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
		ctl.hash = oid_hash;
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
		 * If we didn't find one, we want to make one.  But first look up the
		 * pg_type row, just to make sure we don't make a cache entry for an
		 * invalid type OID.
		 */
		HeapTuple	tp;
		Form_pg_type typtup;

		tp = SearchSysCache(TYPEOID,
							ObjectIdGetDatum(type_id),
							0, 0, 0);
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for type %u", type_id);
		typtup = (Form_pg_type) GETSTRUCT(tp);
		if (!typtup->typisdefined)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("type \"%s\" is only a shell",
							NameStr(typtup->typname))));

		/* Now make the typcache entry */
		typentry = (TypeCacheEntry *) hash_search(TypeCacheHash,
												  (void *) &type_id,
												  HASH_ENTER, &found);
		Assert(!found);			/* it wasn't there a moment ago */

		MemSet(typentry, 0, sizeof(TypeCacheEntry));
		typentry->type_id = type_id;
		typentry->typlen = typtup->typlen;
		typentry->typbyval = typtup->typbyval;
		typentry->typalign = typtup->typalign;
		typentry->typtype = typtup->typtype;
		typentry->typrelid = typtup->typrelid;

		ReleaseSysCache(tp);
	}

	/* If we haven't already found the opclass, try to do so */
	if ((flags & (TYPECACHE_EQ_OPR | TYPECACHE_LT_OPR | TYPECACHE_GT_OPR |
				  TYPECACHE_CMP_PROC |
				  TYPECACHE_EQ_OPR_FINFO | TYPECACHE_CMP_PROC_FINFO)) &&
		typentry->btree_opc == InvalidOid)
	{
		typentry->btree_opc = GetDefaultOpClass(type_id,
												BTREE_AM_OID);
		/* Only care about hash opclass if no btree opclass... */
		if (typentry->btree_opc == InvalidOid)
		{
			if (typentry->hash_opc == InvalidOid)
				typentry->hash_opc = GetDefaultOpClass(type_id,
													   HASH_AM_OID);
		}
		else
		{
			/*
			 * If we find a btree opclass where previously we only found a
			 * hash opclass, forget the hash equality operator so we can use
			 * the btree operator instead.
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
												  InvalidOid,
												  BTEqualStrategyNumber);
		if (typentry->eq_opr == InvalidOid &&
			typentry->hash_opc != InvalidOid)
			typentry->eq_opr = get_opclass_member(typentry->hash_opc,
												  InvalidOid,
												  HTEqualStrategyNumber);
	}
	if ((flags & TYPECACHE_LT_OPR) && typentry->lt_opr == InvalidOid)
	{
		if (typentry->btree_opc != InvalidOid)
			typentry->lt_opr = get_opclass_member(typentry->btree_opc,
												  InvalidOid,
												  BTLessStrategyNumber);
	}
	if ((flags & TYPECACHE_GT_OPR) && typentry->gt_opr == InvalidOid)
	{
		if (typentry->btree_opc != InvalidOid)
			typentry->gt_opr = get_opclass_member(typentry->btree_opc,
												  InvalidOid,
												  BTGreaterStrategyNumber);
	}
	if ((flags & (TYPECACHE_CMP_PROC | TYPECACHE_CMP_PROC_FINFO)) &&
		typentry->cmp_proc == InvalidOid)
	{
		if (typentry->btree_opc != InvalidOid)
			typentry->cmp_proc = get_opclass_proc(typentry->btree_opc,
												  InvalidOid,
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
		Oid			eq_opr_func;

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

	/*
	 * If it's a composite type (row type), get tupdesc if requested
	 */
	if ((flags & TYPECACHE_TUPDESC) &&
		typentry->tupDesc == NULL &&
		typentry->typtype == 'c')
	{
		Relation	rel;

		if (!OidIsValid(typentry->typrelid))	/* should not happen */
			elog(ERROR, "invalid typrelid for composite type %u",
				 typentry->type_id);
		rel = relation_open(typentry->typrelid, AccessShareLock);
		Assert(rel->rd_rel->reltype == typentry->type_id);

		/*
		 * Link to the tupdesc and increment its refcount (we assert it's a
		 * refcounted descriptor).	We don't use IncrTupleDescRefCount() for
		 * this, because the reference mustn't be entered in the current
		 * resource owner; it can outlive the current query.
		 */
		typentry->tupDesc = RelationGetDescr(rel);

		Assert(typentry->tupDesc->tdrefcount > 0);
		typentry->tupDesc->tdrefcount++;

		relation_close(rel, AccessShareLock);
	}

	return typentry;
}

/*
 * lookup_rowtype_tupdesc_internal --- internal routine to lookup a rowtype
 *
 * Same API as lookup_rowtype_tupdesc_noerror, but the returned tupdesc
 * hasn't had its refcount bumped.
 */
static TupleDesc
lookup_rowtype_tupdesc_internal(Oid type_id, int32 typmod, bool noError)
{
	if (type_id != RECORDOID)
	{
		/*
		 * It's a named composite type, so use the regular typcache.
		 */
		TypeCacheEntry *typentry;

		typentry = lookup_type_cache(type_id, TYPECACHE_TUPDESC);
		if (typentry->tupDesc == NULL && !noError)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("type %s is not composite",
							format_type_be(type_id))));
		return typentry->tupDesc;
	}
	else
	{
		/*
		 * It's a transient record type, so look in our record-type table.
		 */
		if (typmod < 0 || typmod >= NextRecordTypmod)
		{
			if (!noError)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("record type has not been registered")));
			return NULL;
		}
		return RecordCacheArray[typmod];
	}
}

/*
 * lookup_rowtype_tupdesc
 *
 * Given a typeid/typmod that should describe a known composite type,
 * return the tuple descriptor for the type.  Will ereport on failure.
 *
 * Note: on success, we increment the refcount of the returned TupleDesc,
 * and log the reference in CurrentResourceOwner.  Caller should call
 * ReleaseTupleDesc or DecrTupleDescRefCount when done using the tupdesc.
 */
TupleDesc
lookup_rowtype_tupdesc(Oid type_id, int32 typmod)
{
	TupleDesc	tupDesc;

	tupDesc = lookup_rowtype_tupdesc_internal(type_id, typmod, false);
	IncrTupleDescRefCount(tupDesc);
	return tupDesc;
}

/*
 * lookup_rowtype_tupdesc_noerror
 *
 * As above, but if the type is not a known composite type and noError
 * is true, returns NULL instead of ereport'ing.  (Note that if a bogus
 * type_id is passed, you'll get an ereport anyway.)
 */
TupleDesc
lookup_rowtype_tupdesc_noerror(Oid type_id, int32 typmod, bool noError)
{
	TupleDesc	tupDesc;

	tupDesc = lookup_rowtype_tupdesc_internal(type_id, typmod, noError);
	if (tupDesc != NULL)
		IncrTupleDescRefCount(tupDesc);
	return tupDesc;
}

/*
 * lookup_rowtype_tupdesc_copy
 *
 * Like lookup_rowtype_tupdesc(), but the returned TupleDesc has been
 * copied into the CurrentMemoryContext and is not reference-counted.
 */
TupleDesc
lookup_rowtype_tupdesc_copy(Oid type_id, int32 typmod)
{
	TupleDesc	tmp;

	tmp = lookup_rowtype_tupdesc_internal(type_id, typmod, false);
	return CreateTupleDescCopyConstr(tmp);
}


/*
 * assign_record_type_typmod
 *
 * Given a tuple descriptor for a RECORD type, find or create a cache entry
 * for the type, and set the tupdesc's tdtypmod field to a value that will
 * identify this cache entry to lookup_rowtype_tupdesc.
 */
void
assign_record_type_typmod(TupleDesc tupDesc)
{
	RecordCacheEntry *recentry;
	TupleDesc	entDesc;
	Oid			hashkey[REC_HASH_KEYS];
	bool		found;
	int			i;
	ListCell   *l;
	int32		newtypmod;
	MemoryContext oldcxt;

	Assert(tupDesc->tdtypeid == RECORDOID);

	if (RecordCacheHash == NULL)
	{
		/* First time through: initialize the hash table */
		HASHCTL		ctl;

		if (!CacheMemoryContext)
			CreateCacheMemoryContext();

		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = REC_HASH_KEYS * sizeof(Oid);
		ctl.entrysize = sizeof(RecordCacheEntry);
		ctl.hash = tag_hash;
		RecordCacheHash = hash_create("Record information cache", 64,
									  &ctl, HASH_ELEM | HASH_FUNCTION);
	}

	/* Find or create a hashtable entry for this hash class */
	MemSet(hashkey, 0, sizeof(hashkey));
	for (i = 0; i < tupDesc->natts; i++)
	{
		if (i >= REC_HASH_KEYS)
			break;
		hashkey[i] = tupDesc->attrs[i]->atttypid;
	}
	recentry = (RecordCacheEntry *) hash_search(RecordCacheHash,
												(void *) hashkey,
												HASH_ENTER, &found);
	if (!found)
	{
		/* New entry ... hash_search initialized only the hash key */
		recentry->tupdescs = NIL;
	}

	/* Look for existing record cache entry */
	foreach(l, recentry->tupdescs)
	{
		entDesc = (TupleDesc) lfirst(l);
		if (equalTupleDescs(tupDesc, entDesc))
		{
			tupDesc->tdtypmod = entDesc->tdtypmod;
			return;
		}
	}

	/* Not present, so need to manufacture an entry */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	if (RecordCacheArray == NULL)
	{
		RecordCacheArray = (TupleDesc *) palloc(64 * sizeof(TupleDesc));
		RecordCacheArrayLen = 64;
	}
	else if (NextRecordTypmod >= RecordCacheArrayLen)
	{
		int32		newlen = RecordCacheArrayLen * 2;

		RecordCacheArray = (TupleDesc *) repalloc(RecordCacheArray,
												  newlen * sizeof(TupleDesc));
		RecordCacheArrayLen = newlen;
	}

	/* if fail in subrs, no damage except possibly some wasted memory... */
	entDesc = CreateTupleDescCopy(tupDesc);
	recentry->tupdescs = lcons(entDesc, recentry->tupdescs);
	/* mark it as a reference-counted tupdesc */
	entDesc->tdrefcount = 1;
	/* now it's safe to advance NextRecordTypmod */
	newtypmod = NextRecordTypmod++;
	entDesc->tdtypmod = newtypmod;
	RecordCacheArray[newtypmod] = entDesc;

	/* report to caller as well */
	tupDesc->tdtypmod = newtypmod;

	MemoryContextSwitchTo(oldcxt);
}

/*
 * flush_rowtype_cache
 *
 * If a typcache entry exists for a rowtype, delete the entry's cached
 * tuple descriptor link.  This is called from relcache.c when a cached
 * relation tupdesc is about to be dropped.
 */
void
flush_rowtype_cache(Oid type_id)
{
	TypeCacheEntry *typentry;

	if (TypeCacheHash == NULL)
		return;					/* no table, so certainly no entry */

	typentry = (TypeCacheEntry *) hash_search(TypeCacheHash,
											  (void *) &type_id,
											  HASH_FIND, NULL);
	if (typentry == NULL)
		return;					/* no matching entry */
	if (typentry->tupDesc == NULL)
		return;					/* tupdesc hasn't been requested */

	/*
	 * Release our refcount and free the tupdesc if none remain. (Can't use
	 * DecrTupleDescRefCount because this reference is not logged in current
	 * resource owner.)
	 */
	Assert(typentry->tupDesc->tdrefcount > 0);
	if (--typentry->tupDesc->tdrefcount == 0)
		FreeTupleDesc(typentry->tupDesc);

	typentry->tupDesc = NULL;
}
