/*-------------------------------------------------------------------------
 *
 * typcache.c
 *	  POSTGRES type cache code
 *
 * The type cache exists to speed lookup of certain information about data
 * types that is not directly available from a type's pg_type row.  For
 * example, we use a type's default btree opclass, or the default hash
 * opclass if no btree opclass exists, to determine which operators should
 * be used for grouping and sorting the type (GROUP BY, ORDER BY ASC/DESC).
 *
 * Several seemingly-odd choices have been made to support use of the type
 * cache by generic array and record handling routines, such as array_eq(),
 * record_cmp(), and hash_array().	Because those routines are used as index
 * support operations, they cannot leak memory.  To allow them to execute
 * efficiently, all information that they would like to re-use across calls
 * is kept in the type cache.
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
 * We do support clearing the tuple descriptor and operator/function parts
 * of a rowtype's cache entry, since those may need to change as a consequence
 * of ALTER TABLE.
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/typcache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/hash.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "catalog/indexing.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_range.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


/* The main type cache hashtable searched by lookup_type_cache */
static HTAB *TypeCacheHash = NULL;

/* Private flag bits in the TypeCacheEntry.flags field */
#define TCFLAGS_CHECKED_ELEM_PROPERTIES		0x0001
#define TCFLAGS_HAVE_ELEM_EQUALITY			0x0002
#define TCFLAGS_HAVE_ELEM_COMPARE			0x0004
#define TCFLAGS_HAVE_ELEM_HASHING			0x0008
#define TCFLAGS_CHECKED_FIELD_PROPERTIES	0x0010
#define TCFLAGS_HAVE_FIELD_EQUALITY			0x0020
#define TCFLAGS_HAVE_FIELD_COMPARE			0x0040

/* Private information to support comparisons of enum values */
typedef struct
{
	Oid			enum_oid;		/* OID of one enum value */
	float4		sort_order;		/* its sort position */
} EnumItem;

typedef struct TypeCacheEnumData
{
	Oid			bitmap_base;	/* OID corresponding to bit 0 of bitmapset */
	Bitmapset  *sorted_values;	/* Set of OIDs known to be in order */
	int			num_values;		/* total number of values in enum */
	EnumItem	enum_values[1]; /* VARIABLE LENGTH ARRAY */
} TypeCacheEnumData;

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

static void load_typcache_tupdesc(TypeCacheEntry *typentry);
static void load_rangetype_info(TypeCacheEntry *typentry);
static bool array_element_has_equality(TypeCacheEntry *typentry);
static bool array_element_has_compare(TypeCacheEntry *typentry);
static bool array_element_has_hashing(TypeCacheEntry *typentry);
static void cache_array_element_properties(TypeCacheEntry *typentry);
static bool record_fields_have_equality(TypeCacheEntry *typentry);
static bool record_fields_have_compare(TypeCacheEntry *typentry);
static void cache_record_field_properties(TypeCacheEntry *typentry);
static void TypeCacheRelCallback(Datum arg, Oid relid);
static void load_enum_cache_data(TypeCacheEntry *tcache);
static EnumItem *find_enumitem(TypeCacheEnumData *enumdata, Oid arg);
static int	enum_oid_cmp(const void *left, const void *right);


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

		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(TypeCacheEntry);
		ctl.hash = oid_hash;
		TypeCacheHash = hash_create("Type information cache", 64,
									&ctl, HASH_ELEM | HASH_FUNCTION);

		/* Also set up a callback for relcache SI invalidations */
		CacheRegisterRelcacheCallback(TypeCacheRelCallback, (Datum) 0);

		/* Also make sure CacheMemoryContext exists */
		if (!CacheMemoryContext)
			CreateCacheMemoryContext();
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

		tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_id));
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
		typentry->typstorage = typtup->typstorage;
		typentry->typtype = typtup->typtype;
		typentry->typrelid = typtup->typrelid;

		ReleaseSysCache(tp);
	}

	/*
	 * If we haven't already found the opclasses, try to do so
	 */
	if ((flags & (TYPECACHE_EQ_OPR | TYPECACHE_LT_OPR | TYPECACHE_GT_OPR |
				  TYPECACHE_CMP_PROC |
				  TYPECACHE_EQ_OPR_FINFO | TYPECACHE_CMP_PROC_FINFO |
				  TYPECACHE_BTREE_OPFAMILY)) &&
		typentry->btree_opf == InvalidOid)
	{
		Oid			opclass;

		opclass = GetDefaultOpClass(type_id, BTREE_AM_OID);
		if (OidIsValid(opclass))
		{
			typentry->btree_opf = get_opclass_family(opclass);
			typentry->btree_opintype = get_opclass_input_type(opclass);
		}
		/* If no btree opclass, we force lookup of the hash opclass */
		if (typentry->btree_opf == InvalidOid)
		{
			if (typentry->hash_opf == InvalidOid)
			{
				opclass = GetDefaultOpClass(type_id, HASH_AM_OID);
				if (OidIsValid(opclass))
				{
					typentry->hash_opf = get_opclass_family(opclass);
					typentry->hash_opintype = get_opclass_input_type(opclass);
				}
			}
		}
		else
		{
			/*
			 * In case we find a btree opclass where previously we only found
			 * a hash opclass, reset eq_opr and derived information so that we
			 * can fetch the btree equality operator instead of the hash
			 * equality operator.  (They're probably the same operator, but we
			 * don't assume that here.)
			 */
			typentry->eq_opr = InvalidOid;
			typentry->eq_opr_finfo.fn_oid = InvalidOid;
			typentry->hash_proc = InvalidOid;
			typentry->hash_proc_finfo.fn_oid = InvalidOid;
		}
	}

	if ((flags & (TYPECACHE_HASH_PROC | TYPECACHE_HASH_PROC_FINFO |
				  TYPECACHE_HASH_OPFAMILY)) &&
		typentry->hash_opf == InvalidOid)
	{
		Oid			opclass;

		opclass = GetDefaultOpClass(type_id, HASH_AM_OID);
		if (OidIsValid(opclass))
		{
			typentry->hash_opf = get_opclass_family(opclass);
			typentry->hash_opintype = get_opclass_input_type(opclass);
		}
	}

	/* Look for requested operators and functions */
	if ((flags & (TYPECACHE_EQ_OPR | TYPECACHE_EQ_OPR_FINFO)) &&
		typentry->eq_opr == InvalidOid)
	{
		Oid			eq_opr = InvalidOid;

		if (typentry->btree_opf != InvalidOid)
			eq_opr = get_opfamily_member(typentry->btree_opf,
										 typentry->btree_opintype,
										 typentry->btree_opintype,
										 BTEqualStrategyNumber);
		if (eq_opr == InvalidOid &&
			typentry->hash_opf != InvalidOid)
			eq_opr = get_opfamily_member(typentry->hash_opf,
										 typentry->hash_opintype,
										 typentry->hash_opintype,
										 HTEqualStrategyNumber);

		/*
		 * If the proposed equality operator is array_eq or record_eq, check
		 * to see if the element type or column types support equality. If
		 * not, array_eq or record_eq would fail at runtime, so we don't want
		 * to report that the type has equality.
		 */
		if (eq_opr == ARRAY_EQ_OP &&
			!array_element_has_equality(typentry))
			eq_opr = InvalidOid;
		else if (eq_opr == RECORD_EQ_OP &&
				 !record_fields_have_equality(typentry))
			eq_opr = InvalidOid;

		typentry->eq_opr = eq_opr;

		/*
		 * Reset info about hash function whenever we pick up new info about
		 * equality operator.  This is so we can ensure that the hash function
		 * matches the operator.
		 */
		typentry->hash_proc = InvalidOid;
		typentry->hash_proc_finfo.fn_oid = InvalidOid;
	}
	if ((flags & TYPECACHE_LT_OPR) && typentry->lt_opr == InvalidOid)
	{
		Oid			lt_opr = InvalidOid;

		if (typentry->btree_opf != InvalidOid)
			lt_opr = get_opfamily_member(typentry->btree_opf,
										 typentry->btree_opintype,
										 typentry->btree_opintype,
										 BTLessStrategyNumber);

		/* As above, make sure array_cmp or record_cmp will succeed */
		if (lt_opr == ARRAY_LT_OP &&
			!array_element_has_compare(typentry))
			lt_opr = InvalidOid;
		else if (lt_opr == RECORD_LT_OP &&
				 !record_fields_have_compare(typentry))
			lt_opr = InvalidOid;

		typentry->lt_opr = lt_opr;
	}
	if ((flags & TYPECACHE_GT_OPR) && typentry->gt_opr == InvalidOid)
	{
		Oid			gt_opr = InvalidOid;

		if (typentry->btree_opf != InvalidOid)
			gt_opr = get_opfamily_member(typentry->btree_opf,
										 typentry->btree_opintype,
										 typentry->btree_opintype,
										 BTGreaterStrategyNumber);

		/* As above, make sure array_cmp or record_cmp will succeed */
		if (gt_opr == ARRAY_GT_OP &&
			!array_element_has_compare(typentry))
			gt_opr = InvalidOid;
		else if (gt_opr == RECORD_GT_OP &&
				 !record_fields_have_compare(typentry))
			gt_opr = InvalidOid;

		typentry->gt_opr = gt_opr;
	}
	if ((flags & (TYPECACHE_CMP_PROC | TYPECACHE_CMP_PROC_FINFO)) &&
		typentry->cmp_proc == InvalidOid)
	{
		Oid			cmp_proc = InvalidOid;

		if (typentry->btree_opf != InvalidOid)
			cmp_proc = get_opfamily_proc(typentry->btree_opf,
										 typentry->btree_opintype,
										 typentry->btree_opintype,
										 BTORDER_PROC);

		/* As above, make sure array_cmp or record_cmp will succeed */
		if (cmp_proc == F_BTARRAYCMP &&
			!array_element_has_compare(typentry))
			cmp_proc = InvalidOid;
		else if (cmp_proc == F_BTRECORDCMP &&
				 !record_fields_have_compare(typentry))
			cmp_proc = InvalidOid;

		typentry->cmp_proc = cmp_proc;
	}
	if ((flags & (TYPECACHE_HASH_PROC | TYPECACHE_HASH_PROC_FINFO)) &&
		typentry->hash_proc == InvalidOid)
	{
		Oid			hash_proc = InvalidOid;

		/*
		 * We insist that the eq_opr, if one has been determined, match the
		 * hash opclass; else report there is no hash function.
		 */
		if (typentry->hash_opf != InvalidOid &&
			(!OidIsValid(typentry->eq_opr) ||
			 typentry->eq_opr == get_opfamily_member(typentry->hash_opf,
													 typentry->hash_opintype,
													 typentry->hash_opintype,
													 HTEqualStrategyNumber)))
			hash_proc = get_opfamily_proc(typentry->hash_opf,
										  typentry->hash_opintype,
										  typentry->hash_opintype,
										  HASHPROC);

		/*
		 * As above, make sure hash_array will succeed.  We don't currently
		 * support hashing for composite types, but when we do, we'll need
		 * more logic here to check that case too.
		 */
		if (hash_proc == F_HASH_ARRAY &&
			!array_element_has_hashing(typentry))
			hash_proc = InvalidOid;

		typentry->hash_proc = hash_proc;
	}

	/*
	 * Set up fmgr lookup info as requested
	 *
	 * Note: we tell fmgr the finfo structures live in CacheMemoryContext,
	 * which is not quite right (they're really in the hash table's private
	 * memory context) but this will do for our purposes.
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
	if ((flags & TYPECACHE_HASH_PROC_FINFO) &&
		typentry->hash_proc_finfo.fn_oid == InvalidOid &&
		typentry->hash_proc != InvalidOid)
	{
		fmgr_info_cxt(typentry->hash_proc, &typentry->hash_proc_finfo,
					  CacheMemoryContext);
	}

	/*
	 * If it's a composite type (row type), get tupdesc if requested
	 */
	if ((flags & TYPECACHE_TUPDESC) &&
		typentry->tupDesc == NULL &&
		typentry->typtype == TYPTYPE_COMPOSITE)
	{
		load_typcache_tupdesc(typentry);
	}

	/*
	 * If requested, get information about a range type
	 */
	if ((flags & TYPECACHE_RANGE_INFO) &&
		typentry->rngelemtype == NULL &&
		typentry->typtype == TYPTYPE_RANGE)
	{
		load_rangetype_info(typentry);
	}

	return typentry;
}

/*
 * load_typcache_tupdesc --- helper routine to set up composite type's tupDesc
 */
static void
load_typcache_tupdesc(TypeCacheEntry *typentry)
{
	Relation	rel;

	if (!OidIsValid(typentry->typrelid))		/* should not happen */
		elog(ERROR, "invalid typrelid for composite type %u",
			 typentry->type_id);
	rel = relation_open(typentry->typrelid, AccessShareLock);
	Assert(rel->rd_rel->reltype == typentry->type_id);

	/*
	 * Link to the tupdesc and increment its refcount (we assert it's a
	 * refcounted descriptor).	We don't use IncrTupleDescRefCount() for this,
	 * because the reference mustn't be entered in the current resource owner;
	 * it can outlive the current query.
	 */
	typentry->tupDesc = RelationGetDescr(rel);

	Assert(typentry->tupDesc->tdrefcount > 0);
	typentry->tupDesc->tdrefcount++;

	relation_close(rel, AccessShareLock);
}

/*
 * load_rangetype_info --- helper routine to set up range type information
 */
static void
load_rangetype_info(TypeCacheEntry *typentry)
{
	Form_pg_range pg_range;
	HeapTuple	tup;
	Oid			subtypeOid;
	Oid			opclassOid;
	Oid			canonicalOid;
	Oid			subdiffOid;
	Oid			opfamilyOid;
	Oid			opcintype;
	Oid			cmpFnOid;

	/* get information from pg_range */
	tup = SearchSysCache1(RANGETYPE, ObjectIdGetDatum(typentry->type_id));
	/* should not fail, since we already checked typtype ... */
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for range type %u",
			 typentry->type_id);
	pg_range = (Form_pg_range) GETSTRUCT(tup);

	subtypeOid = pg_range->rngsubtype;
	typentry->rng_collation = pg_range->rngcollation;
	opclassOid = pg_range->rngsubopc;
	canonicalOid = pg_range->rngcanonical;
	subdiffOid = pg_range->rngsubdiff;

	ReleaseSysCache(tup);

	/* get opclass properties and look up the comparison function */
	opfamilyOid = get_opclass_family(opclassOid);
	opcintype = get_opclass_input_type(opclassOid);

	cmpFnOid = get_opfamily_proc(opfamilyOid, opcintype, opcintype,
								 BTORDER_PROC);
	if (!RegProcedureIsValid(cmpFnOid))
		elog(ERROR, "missing support function %d(%u,%u) in opfamily %u",
			 BTORDER_PROC, opcintype, opcintype, opfamilyOid);

	/* set up cached fmgrinfo structs */
	fmgr_info_cxt(cmpFnOid, &typentry->rng_cmp_proc_finfo,
				  CacheMemoryContext);
	if (OidIsValid(canonicalOid))
		fmgr_info_cxt(canonicalOid, &typentry->rng_canonical_finfo,
					  CacheMemoryContext);
	if (OidIsValid(subdiffOid))
		fmgr_info_cxt(subdiffOid, &typentry->rng_subdiff_finfo,
					  CacheMemoryContext);

	/* Lastly, set up link to the element type --- this marks data valid */
	typentry->rngelemtype = lookup_type_cache(subtypeOid, 0);
}


/*
 * array_element_has_equality and friends are helper routines to check
 * whether we should believe that array_eq and related functions will work
 * on the given array type or composite type.
 *
 * The logic above may call these repeatedly on the same type entry, so we
 * make use of the typentry->flags field to cache the results once known.
 * Also, we assume that we'll probably want all these facts about the type
 * if we want any, so we cache them all using only one lookup of the
 * component datatype(s).
 */

static bool
array_element_has_equality(TypeCacheEntry *typentry)
{
	if (!(typentry->flags & TCFLAGS_CHECKED_ELEM_PROPERTIES))
		cache_array_element_properties(typentry);
	return (typentry->flags & TCFLAGS_HAVE_ELEM_EQUALITY) != 0;
}

static bool
array_element_has_compare(TypeCacheEntry *typentry)
{
	if (!(typentry->flags & TCFLAGS_CHECKED_ELEM_PROPERTIES))
		cache_array_element_properties(typentry);
	return (typentry->flags & TCFLAGS_HAVE_ELEM_COMPARE) != 0;
}

static bool
array_element_has_hashing(TypeCacheEntry *typentry)
{
	if (!(typentry->flags & TCFLAGS_CHECKED_ELEM_PROPERTIES))
		cache_array_element_properties(typentry);
	return (typentry->flags & TCFLAGS_HAVE_ELEM_HASHING) != 0;
}

static void
cache_array_element_properties(TypeCacheEntry *typentry)
{
	Oid			elem_type = get_base_element_type(typentry->type_id);

	if (OidIsValid(elem_type))
	{
		TypeCacheEntry *elementry;

		elementry = lookup_type_cache(elem_type,
									  TYPECACHE_EQ_OPR |
									  TYPECACHE_CMP_PROC |
									  TYPECACHE_HASH_PROC);
		if (OidIsValid(elementry->eq_opr))
			typentry->flags |= TCFLAGS_HAVE_ELEM_EQUALITY;
		if (OidIsValid(elementry->cmp_proc))
			typentry->flags |= TCFLAGS_HAVE_ELEM_COMPARE;
		if (OidIsValid(elementry->hash_proc))
			typentry->flags |= TCFLAGS_HAVE_ELEM_HASHING;
	}
	typentry->flags |= TCFLAGS_CHECKED_ELEM_PROPERTIES;
}

static bool
record_fields_have_equality(TypeCacheEntry *typentry)
{
	if (!(typentry->flags & TCFLAGS_CHECKED_FIELD_PROPERTIES))
		cache_record_field_properties(typentry);
	return (typentry->flags & TCFLAGS_HAVE_FIELD_EQUALITY) != 0;
}

static bool
record_fields_have_compare(TypeCacheEntry *typentry)
{
	if (!(typentry->flags & TCFLAGS_CHECKED_FIELD_PROPERTIES))
		cache_record_field_properties(typentry);
	return (typentry->flags & TCFLAGS_HAVE_FIELD_COMPARE) != 0;
}

static void
cache_record_field_properties(TypeCacheEntry *typentry)
{
	/*
	 * For type RECORD, we can't really tell what will work, since we don't
	 * have access here to the specific anonymous type.  Just assume that
	 * everything will (we may get a failure at runtime ...)
	 */
	if (typentry->type_id == RECORDOID)
		typentry->flags |= (TCFLAGS_HAVE_FIELD_EQUALITY |
							TCFLAGS_HAVE_FIELD_COMPARE);
	else if (typentry->typtype == TYPTYPE_COMPOSITE)
	{
		TupleDesc	tupdesc;
		int			newflags;
		int			i;

		/* Fetch composite type's tupdesc if we don't have it already */
		if (typentry->tupDesc == NULL)
			load_typcache_tupdesc(typentry);
		tupdesc = typentry->tupDesc;

		/* Must bump the refcount while we do additional catalog lookups */
		IncrTupleDescRefCount(tupdesc);

		/* Have each property if all non-dropped fields have the property */
		newflags = (TCFLAGS_HAVE_FIELD_EQUALITY |
					TCFLAGS_HAVE_FIELD_COMPARE);
		for (i = 0; i < tupdesc->natts; i++)
		{
			TypeCacheEntry *fieldentry;

			if (tupdesc->attrs[i]->attisdropped)
				continue;

			fieldentry = lookup_type_cache(tupdesc->attrs[i]->atttypid,
										   TYPECACHE_EQ_OPR |
										   TYPECACHE_CMP_PROC);
			if (!OidIsValid(fieldentry->eq_opr))
				newflags &= ~TCFLAGS_HAVE_FIELD_EQUALITY;
			if (!OidIsValid(fieldentry->cmp_proc))
				newflags &= ~TCFLAGS_HAVE_FIELD_COMPARE;

			/* We can drop out of the loop once we disprove all bits */
			if (newflags == 0)
				break;
		}
		typentry->flags |= newflags;

		DecrTupleDescRefCount(tupdesc);
	}
	typentry->flags |= TCFLAGS_CHECKED_FIELD_PROPERTIES;
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

		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = REC_HASH_KEYS * sizeof(Oid);
		ctl.entrysize = sizeof(RecordCacheEntry);
		ctl.hash = tag_hash;
		RecordCacheHash = hash_create("Record information cache", 64,
									  &ctl, HASH_ELEM | HASH_FUNCTION);

		/* Also make sure CacheMemoryContext exists */
		if (!CacheMemoryContext)
			CreateCacheMemoryContext();
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
 * TypeCacheRelCallback
 *		Relcache inval callback function
 *
 * Delete the cached tuple descriptor (if any) for the given rel's composite
 * type, or for all composite types if relid == InvalidOid.  Also reset
 * whatever info we have cached about the composite type's comparability.
 *
 * This is called when a relcache invalidation event occurs for the given
 * relid.  We must scan the whole typcache hash since we don't know the
 * type OID corresponding to the relid.  We could do a direct search if this
 * were a syscache-flush callback on pg_type, but then we would need all
 * ALTER-TABLE-like commands that could modify a rowtype to issue syscache
 * invals against the rel's pg_type OID.  The extra SI signaling could very
 * well cost more than we'd save, since in most usages there are not very
 * many entries in a backend's typcache.  The risk of bugs-of-omission seems
 * high, too.
 *
 * Another possibility, with only localized impact, is to maintain a second
 * hashtable that indexes composite-type typcache entries by their typrelid.
 * But it's still not clear it's worth the trouble.
 */
static void
TypeCacheRelCallback(Datum arg, Oid relid)
{
	HASH_SEQ_STATUS status;
	TypeCacheEntry *typentry;

	/* TypeCacheHash must exist, else this callback wouldn't be registered */
	hash_seq_init(&status, TypeCacheHash);
	while ((typentry = (TypeCacheEntry *) hash_seq_search(&status)) != NULL)
	{
		if (typentry->typtype != TYPTYPE_COMPOSITE)
			continue;			/* skip non-composites */

		/* Skip if no match, unless we're zapping all composite types */
		if (relid != typentry->typrelid && relid != InvalidOid)
			continue;

		/* Delete tupdesc if we have it */
		if (typentry->tupDesc != NULL)
		{
			/*
			 * Release our refcount, and free the tupdesc if none remain.
			 * (Can't use DecrTupleDescRefCount because this reference is not
			 * logged in current resource owner.)
			 */
			Assert(typentry->tupDesc->tdrefcount > 0);
			if (--typentry->tupDesc->tdrefcount == 0)
				FreeTupleDesc(typentry->tupDesc);
			typentry->tupDesc = NULL;
		}

		/* Reset equality/comparison/hashing information */
		typentry->eq_opr = InvalidOid;
		typentry->lt_opr = InvalidOid;
		typentry->gt_opr = InvalidOid;
		typentry->cmp_proc = InvalidOid;
		typentry->hash_proc = InvalidOid;
		typentry->eq_opr_finfo.fn_oid = InvalidOid;
		typentry->cmp_proc_finfo.fn_oid = InvalidOid;
		typentry->hash_proc_finfo.fn_oid = InvalidOid;
		typentry->flags = 0;
	}
}


/*
 * Check if given OID is part of the subset that's sortable by comparisons
 */
static inline bool
enum_known_sorted(TypeCacheEnumData *enumdata, Oid arg)
{
	Oid			offset;

	if (arg < enumdata->bitmap_base)
		return false;
	offset = arg - enumdata->bitmap_base;
	if (offset > (Oid) INT_MAX)
		return false;
	return bms_is_member((int) offset, enumdata->sorted_values);
}


/*
 * compare_values_of_enum
 *		Compare two members of an enum type.
 *		Return <0, 0, or >0 according as arg1 <, =, or > arg2.
 *
 * Note: currently, the enumData cache is refreshed only if we are asked
 * to compare an enum value that is not already in the cache.  This is okay
 * because there is no support for re-ordering existing values, so comparisons
 * of previously cached values will return the right answer even if other
 * values have been added since we last loaded the cache.
 *
 * Note: the enum logic has a special-case rule about even-numbered versus
 * odd-numbered OIDs, but we take no account of that rule here; this
 * routine shouldn't even get called when that rule applies.
 */
int
compare_values_of_enum(TypeCacheEntry *tcache, Oid arg1, Oid arg2)
{
	TypeCacheEnumData *enumdata;
	EnumItem   *item1;
	EnumItem   *item2;

	/*
	 * Equal OIDs are certainly equal --- this case was probably handled by
	 * our caller, but we may as well check.
	 */
	if (arg1 == arg2)
		return 0;

	/* Load up the cache if first time through */
	if (tcache->enumData == NULL)
		load_enum_cache_data(tcache);
	enumdata = tcache->enumData;

	/*
	 * If both OIDs are known-sorted, we can just compare them directly.
	 */
	if (enum_known_sorted(enumdata, arg1) &&
		enum_known_sorted(enumdata, arg2))
	{
		if (arg1 < arg2)
			return -1;
		else
			return 1;
	}

	/*
	 * Slow path: we have to identify their actual sort-order positions.
	 */
	item1 = find_enumitem(enumdata, arg1);
	item2 = find_enumitem(enumdata, arg2);

	if (item1 == NULL || item2 == NULL)
	{
		/*
		 * We couldn't find one or both values.  That means the enum has
		 * changed under us, so re-initialize the cache and try again. We
		 * don't bother retrying the known-sorted case in this path.
		 */
		load_enum_cache_data(tcache);
		enumdata = tcache->enumData;

		item1 = find_enumitem(enumdata, arg1);
		item2 = find_enumitem(enumdata, arg2);

		/*
		 * If we still can't find the values, complain: we must have corrupt
		 * data.
		 */
		if (item1 == NULL)
			elog(ERROR, "enum value %u not found in cache for enum %s",
				 arg1, format_type_be(tcache->type_id));
		if (item2 == NULL)
			elog(ERROR, "enum value %u not found in cache for enum %s",
				 arg2, format_type_be(tcache->type_id));
	}

	if (item1->sort_order < item2->sort_order)
		return -1;
	else if (item1->sort_order > item2->sort_order)
		return 1;
	else
		return 0;
}

/*
 * Load (or re-load) the enumData member of the typcache entry.
 */
static void
load_enum_cache_data(TypeCacheEntry *tcache)
{
	TypeCacheEnumData *enumdata;
	Relation	enum_rel;
	SysScanDesc enum_scan;
	HeapTuple	enum_tuple;
	ScanKeyData skey;
	EnumItem   *items;
	int			numitems;
	int			maxitems;
	Oid			bitmap_base;
	Bitmapset  *bitmap;
	MemoryContext oldcxt;
	int			bm_size,
				start_pos;

	/* Check that this is actually an enum */
	if (tcache->typtype != TYPTYPE_ENUM)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is not an enum",
						format_type_be(tcache->type_id))));

	/*
	 * Read all the information for members of the enum type.  We collect the
	 * info in working memory in the caller's context, and then transfer it to
	 * permanent memory in CacheMemoryContext.	This minimizes the risk of
	 * leaking memory from CacheMemoryContext in the event of an error partway
	 * through.
	 */
	maxitems = 64;
	items = (EnumItem *) palloc(sizeof(EnumItem) * maxitems);
	numitems = 0;

	/*
	 * Scan pg_enum for the members of the target enum type.  We use a current
	 * MVCC snapshot, *not* SnapshotNow, so that we see a consistent set of
	 * rows even if someone commits a renumbering of the enum meanwhile. See
	 * comments for RenumberEnumType in catalog/pg_enum.c for more info.
	 */
	ScanKeyInit(&skey,
				Anum_pg_enum_enumtypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(tcache->type_id));

	enum_rel = heap_open(EnumRelationId, AccessShareLock);
	enum_scan = systable_beginscan(enum_rel,
								   EnumTypIdLabelIndexId,
								   true, GetLatestSnapshot(),
								   1, &skey);

	while (HeapTupleIsValid(enum_tuple = systable_getnext(enum_scan)))
	{
		Form_pg_enum en = (Form_pg_enum) GETSTRUCT(enum_tuple);

		if (numitems >= maxitems)
		{
			maxitems *= 2;
			items = (EnumItem *) repalloc(items, sizeof(EnumItem) * maxitems);
		}
		items[numitems].enum_oid = HeapTupleGetOid(enum_tuple);
		items[numitems].sort_order = en->enumsortorder;
		numitems++;
	}

	systable_endscan(enum_scan);
	heap_close(enum_rel, AccessShareLock);

	/* Sort the items into OID order */
	qsort(items, numitems, sizeof(EnumItem), enum_oid_cmp);

	/*
	 * Here, we create a bitmap listing a subset of the enum's OIDs that are
	 * known to be in order and can thus be compared with just OID comparison.
	 *
	 * The point of this is that the enum's initial OIDs were certainly in
	 * order, so there is some subset that can be compared via OID comparison;
	 * and we'd rather not do binary searches unnecessarily.
	 *
	 * This is somewhat heuristic, and might identify a subset of OIDs that
	 * isn't exactly what the type started with.  That's okay as long as the
	 * subset is correctly sorted.
	 */
	bitmap_base = InvalidOid;
	bitmap = NULL;
	bm_size = 1;				/* only save sets of at least 2 OIDs */

	for (start_pos = 0; start_pos < numitems - 1; start_pos++)
	{
		/*
		 * Identify longest sorted subsequence starting at start_pos
		 */
		Bitmapset  *this_bitmap = bms_make_singleton(0);
		int			this_bm_size = 1;
		Oid			start_oid = items[start_pos].enum_oid;
		float4		prev_order = items[start_pos].sort_order;
		int			i;

		for (i = start_pos + 1; i < numitems; i++)
		{
			Oid			offset;

			offset = items[i].enum_oid - start_oid;
			/* quit if bitmap would be too large; cutoff is arbitrary */
			if (offset >= 8192)
				break;
			/* include the item if it's in-order */
			if (items[i].sort_order > prev_order)
			{
				prev_order = items[i].sort_order;
				this_bitmap = bms_add_member(this_bitmap, (int) offset);
				this_bm_size++;
			}
		}

		/* Remember it if larger than previous best */
		if (this_bm_size > bm_size)
		{
			bms_free(bitmap);
			bitmap_base = start_oid;
			bitmap = this_bitmap;
			bm_size = this_bm_size;
		}
		else
			bms_free(this_bitmap);

		/*
		 * Done if it's not possible to find a longer sequence in the rest of
		 * the list.  In typical cases this will happen on the first
		 * iteration, which is why we create the bitmaps on the fly instead of
		 * doing a second pass over the list.
		 */
		if (bm_size >= (numitems - start_pos - 1))
			break;
	}

	/* OK, copy the data into CacheMemoryContext */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
	enumdata = (TypeCacheEnumData *)
		palloc(offsetof(TypeCacheEnumData, enum_values) +
			   numitems * sizeof(EnumItem));
	enumdata->bitmap_base = bitmap_base;
	enumdata->sorted_values = bms_copy(bitmap);
	enumdata->num_values = numitems;
	memcpy(enumdata->enum_values, items, numitems * sizeof(EnumItem));
	MemoryContextSwitchTo(oldcxt);

	pfree(items);
	bms_free(bitmap);

	/* And link the finished cache struct into the typcache */
	if (tcache->enumData != NULL)
		pfree(tcache->enumData);
	tcache->enumData = enumdata;
}

/*
 * Locate the EnumItem with the given OID, if present
 */
static EnumItem *
find_enumitem(TypeCacheEnumData *enumdata, Oid arg)
{
	EnumItem	srch;

	/* On some versions of Solaris, bsearch of zero items dumps core */
	if (enumdata->num_values <= 0)
		return NULL;

	srch.enum_oid = arg;
	return bsearch(&srch, enumdata->enum_values, enumdata->num_values,
				   sizeof(EnumItem), enum_oid_cmp);
}

/*
 * qsort comparison function for OID-ordered EnumItems
 */
static int
enum_oid_cmp(const void *left, const void *right)
{
	const EnumItem *l = (const EnumItem *) left;
	const EnumItem *r = (const EnumItem *) right;

	if (l->enum_oid < r->enum_oid)
		return -1;
	else if (l->enum_oid > r->enum_oid)
		return 1;
	else
		return 0;
}
