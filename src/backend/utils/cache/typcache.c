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
 * record_cmp(), and hash_array().  Because those routines are used as index
 * support operations, they cannot leak memory.  To allow them to execute
 * efficiently, all information that they would like to re-use across calls
 * is kept in the type cache.
 *
 * Once created, a type cache entry lives as long as the backend does, so
 * there is no need for a call to release a cache entry.  If the type is
 * dropped, the cache entry simply becomes wasted storage.  This is not
 * expected to happen often, and assuming that typcache entries are good
 * permanently allows caching pointers to them in long-lived places.
 *
 * We have some provisions for updating cache entries if the stored data
 * becomes obsolete.  Information dependent on opclasses is cleared if we
 * detect updates to pg_opclass.  We also support clearing the tuple
 * descriptor and operator/function parts of a rowtype's cache entry,
 * since those may need to change as a consequence of ALTER TABLE.
 * Domain constraint changes are also tracked properly.
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
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
#include "catalog/pg_constraint.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_range.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "executor/executor.h"
#include "optimizer/planner.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


/* The main type cache hashtable searched by lookup_type_cache */
static HTAB *TypeCacheHash = NULL;

/* List of type cache entries for domain types */
static TypeCacheEntry *firstDomainTypeEntry = NULL;

/* Private flag bits in the TypeCacheEntry.flags field */
#define TCFLAGS_CHECKED_BTREE_OPCLASS		0x0001
#define TCFLAGS_CHECKED_HASH_OPCLASS		0x0002
#define TCFLAGS_CHECKED_EQ_OPR				0x0004
#define TCFLAGS_CHECKED_LT_OPR				0x0008
#define TCFLAGS_CHECKED_GT_OPR				0x0010
#define TCFLAGS_CHECKED_CMP_PROC			0x0020
#define TCFLAGS_CHECKED_HASH_PROC			0x0040
#define TCFLAGS_CHECKED_ELEM_PROPERTIES		0x0080
#define TCFLAGS_HAVE_ELEM_EQUALITY			0x0100
#define TCFLAGS_HAVE_ELEM_COMPARE			0x0200
#define TCFLAGS_HAVE_ELEM_HASHING			0x0400
#define TCFLAGS_CHECKED_FIELD_PROPERTIES	0x0800
#define TCFLAGS_HAVE_FIELD_EQUALITY			0x1000
#define TCFLAGS_HAVE_FIELD_COMPARE			0x2000
#define TCFLAGS_CHECKED_DOMAIN_CONSTRAINTS	0x4000

/*
 * Data stored about a domain type's constraints.  Note that we do not create
 * this struct for the common case of a constraint-less domain; we just set
 * domainData to NULL to indicate that.
 *
 * Within a DomainConstraintCache, we abuse the DomainConstraintState node
 * type a bit: check_expr fields point to expression plan trees, not plan
 * state trees.  When needed, expression state trees are built by flat-copying
 * the DomainConstraintState nodes and applying ExecInitExpr to check_expr.
 * Such a state tree is not part of the DomainConstraintCache, but is
 * considered to belong to a DomainConstraintRef.
 */
struct DomainConstraintCache
{
	List	   *constraints;	/* list of DomainConstraintState nodes */
	MemoryContext dccContext;	/* memory context holding all associated data */
	long		dccRefCount;	/* number of references to this struct */
};

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
	EnumItem	enum_values[FLEXIBLE_ARRAY_MEMBER];
} TypeCacheEnumData;

/*
 * We use a separate table for storing the definitions of non-anonymous
 * record types.  Once defined, a record type will be remembered for the
 * life of the backend.  Subsequent uses of the "same" record type (where
 * sameness means equalTupleDescs) will refer to the existing table entry.
 *
 * Stored record types are remembered in a linear array of TupleDescs,
 * which can be indexed quickly with the assigned typmod.  There is also
 * a hash table to speed searches for matching TupleDescs.  The hash key
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
static void load_domaintype_info(TypeCacheEntry *typentry);
static int	dcs_cmp(const void *a, const void *b);
static void decr_dcc_refcount(DomainConstraintCache *dcc);
static void dccref_deletion_callback(void *arg);
static List *prep_domain_constraints(List *constraints, MemoryContext execctx);
static bool array_element_has_equality(TypeCacheEntry *typentry);
static bool array_element_has_compare(TypeCacheEntry *typentry);
static bool array_element_has_hashing(TypeCacheEntry *typentry);
static void cache_array_element_properties(TypeCacheEntry *typentry);
static bool record_fields_have_equality(TypeCacheEntry *typentry);
static bool record_fields_have_compare(TypeCacheEntry *typentry);
static void cache_record_field_properties(TypeCacheEntry *typentry);
static void TypeCacheRelCallback(Datum arg, Oid relid);
static void TypeCacheOpcCallback(Datum arg, int cacheid, uint32 hashvalue);
static void TypeCacheConstrCallback(Datum arg, int cacheid, uint32 hashvalue);
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
		TypeCacheHash = hash_create("Type information cache", 64,
									&ctl, HASH_ELEM | HASH_BLOBS);

		/* Also set up callbacks for SI invalidations */
		CacheRegisterRelcacheCallback(TypeCacheRelCallback, (Datum) 0);
		CacheRegisterSyscacheCallback(CLAOID, TypeCacheOpcCallback, (Datum) 0);
		CacheRegisterSyscacheCallback(CONSTROID, TypeCacheConstrCallback, (Datum) 0);
		CacheRegisterSyscacheCallback(TYPEOID, TypeCacheConstrCallback, (Datum) 0);

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

		/* If it's a domain, immediately thread it into the domain cache list */
		if (typentry->typtype == TYPTYPE_DOMAIN)
		{
			typentry->nextDomain = firstDomainTypeEntry;
			firstDomainTypeEntry = typentry;
		}

		ReleaseSysCache(tp);
	}

	/*
	 * Look up opclasses if we haven't already and any dependent info is
	 * requested.
	 */
	if ((flags & (TYPECACHE_EQ_OPR | TYPECACHE_LT_OPR | TYPECACHE_GT_OPR |
				  TYPECACHE_CMP_PROC |
				  TYPECACHE_EQ_OPR_FINFO | TYPECACHE_CMP_PROC_FINFO |
				  TYPECACHE_BTREE_OPFAMILY)) &&
		!(typentry->flags & TCFLAGS_CHECKED_BTREE_OPCLASS))
	{
		Oid			opclass;

		opclass = GetDefaultOpClass(type_id, BTREE_AM_OID);
		if (OidIsValid(opclass))
		{
			typentry->btree_opf = get_opclass_family(opclass);
			typentry->btree_opintype = get_opclass_input_type(opclass);
		}
		else
		{
			typentry->btree_opf = typentry->btree_opintype = InvalidOid;
		}

		/*
		 * Reset information derived from btree opclass.  Note in particular
		 * that we'll redetermine the eq_opr even if we previously found one;
		 * this matters in case a btree opclass has been added to a type that
		 * previously had only a hash opclass.
		 */
		typentry->flags &= ~(TCFLAGS_CHECKED_EQ_OPR |
							 TCFLAGS_CHECKED_LT_OPR |
							 TCFLAGS_CHECKED_GT_OPR |
							 TCFLAGS_CHECKED_CMP_PROC);
		typentry->flags |= TCFLAGS_CHECKED_BTREE_OPCLASS;
	}

	/*
	 * If we need to look up equality operator, and there's no btree opclass,
	 * force lookup of hash opclass.
	 */
	if ((flags & (TYPECACHE_EQ_OPR | TYPECACHE_EQ_OPR_FINFO)) &&
		!(typentry->flags & TCFLAGS_CHECKED_EQ_OPR) &&
		typentry->btree_opf == InvalidOid)
		flags |= TYPECACHE_HASH_OPFAMILY;

	if ((flags & (TYPECACHE_HASH_PROC | TYPECACHE_HASH_PROC_FINFO |
				  TYPECACHE_HASH_OPFAMILY)) &&
		!(typentry->flags & TCFLAGS_CHECKED_HASH_OPCLASS))
	{
		Oid			opclass;

		opclass = GetDefaultOpClass(type_id, HASH_AM_OID);
		if (OidIsValid(opclass))
		{
			typentry->hash_opf = get_opclass_family(opclass);
			typentry->hash_opintype = get_opclass_input_type(opclass);
		}
		else
		{
			typentry->hash_opf = typentry->hash_opintype = InvalidOid;
		}

		/*
		 * Reset information derived from hash opclass.  We do *not* reset the
		 * eq_opr; if we already found one from the btree opclass, that
		 * decision is still good.
		 */
		typentry->flags &= ~(TCFLAGS_CHECKED_HASH_PROC);
		typentry->flags |= TCFLAGS_CHECKED_HASH_OPCLASS;
	}

	/*
	 * Look for requested operators and functions, if we haven't already.
	 */
	if ((flags & (TYPECACHE_EQ_OPR | TYPECACHE_EQ_OPR_FINFO)) &&
		!(typentry->flags & TCFLAGS_CHECKED_EQ_OPR))
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

		/* Force update of eq_opr_finfo only if we're changing state */
		if (typentry->eq_opr != eq_opr)
			typentry->eq_opr_finfo.fn_oid = InvalidOid;

		typentry->eq_opr = eq_opr;

		/*
		 * Reset info about hash function whenever we pick up new info about
		 * equality operator.  This is so we can ensure that the hash function
		 * matches the operator.
		 */
		typentry->flags &= ~(TCFLAGS_CHECKED_HASH_PROC);
		typentry->flags |= TCFLAGS_CHECKED_EQ_OPR;
	}
	if ((flags & TYPECACHE_LT_OPR) &&
		!(typentry->flags & TCFLAGS_CHECKED_LT_OPR))
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
		typentry->flags |= TCFLAGS_CHECKED_LT_OPR;
	}
	if ((flags & TYPECACHE_GT_OPR) &&
		!(typentry->flags & TCFLAGS_CHECKED_GT_OPR))
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
		typentry->flags |= TCFLAGS_CHECKED_GT_OPR;
	}
	if ((flags & (TYPECACHE_CMP_PROC | TYPECACHE_CMP_PROC_FINFO)) &&
		!(typentry->flags & TCFLAGS_CHECKED_CMP_PROC))
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

		/* Force update of cmp_proc_finfo only if we're changing state */
		if (typentry->cmp_proc != cmp_proc)
			typentry->cmp_proc_finfo.fn_oid = InvalidOid;

		typentry->cmp_proc = cmp_proc;
		typentry->flags |= TCFLAGS_CHECKED_CMP_PROC;
	}
	if ((flags & (TYPECACHE_HASH_PROC | TYPECACHE_HASH_PROC_FINFO)) &&
		!(typentry->flags & TCFLAGS_CHECKED_HASH_PROC))
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

		/* Force update of hash_proc_finfo only if we're changing state */
		if (typentry->hash_proc != hash_proc)
			typentry->hash_proc_finfo.fn_oid = InvalidOid;

		typentry->hash_proc = hash_proc;
		typentry->flags |= TCFLAGS_CHECKED_HASH_PROC;
	}

	/*
	 * Set up fmgr lookup info as requested
	 *
	 * Note: we tell fmgr the finfo structures live in CacheMemoryContext,
	 * which is not quite right (they're really in the hash table's private
	 * memory context) but this will do for our purposes.
	 *
	 * Note: the code above avoids invalidating the finfo structs unless the
	 * referenced operator/function OID actually changes.  This is to prevent
	 * unnecessary leakage of any subsidiary data attached to an finfo, since
	 * that would cause session-lifespan memory leaks.
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

	/*
	 * If requested, get information about a domain type
	 */
	if ((flags & TYPECACHE_DOMAIN_INFO) &&
		(typentry->flags & TCFLAGS_CHECKED_DOMAIN_CONSTRAINTS) == 0 &&
		typentry->typtype == TYPTYPE_DOMAIN)
	{
		load_domaintype_info(typentry);
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
	 * refcounted descriptor).  We don't use IncrTupleDescRefCount() for this,
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
 * load_domaintype_info --- helper routine to set up domain constraint info
 *
 * Note: we assume we're called in a relatively short-lived context, so it's
 * okay to leak data into the current context while scanning pg_constraint.
 * We build the new DomainConstraintCache data in a context underneath
 * CurrentMemoryContext, and reparent it under CacheMemoryContext when
 * complete.
 */
static void
load_domaintype_info(TypeCacheEntry *typentry)
{
	Oid			typeOid = typentry->type_id;
	DomainConstraintCache *dcc;
	bool		notNull = false;
	DomainConstraintState **ccons;
	int			cconslen;
	Relation	conRel;
	MemoryContext oldcxt;

	/*
	 * If we're here, any existing constraint info is stale, so release it.
	 * For safety, be sure to null the link before trying to delete the data.
	 */
	if (typentry->domainData)
	{
		dcc = typentry->domainData;
		typentry->domainData = NULL;
		decr_dcc_refcount(dcc);
	}

	/*
	 * We try to optimize the common case of no domain constraints, so don't
	 * create the dcc object and context until we find a constraint.  Likewise
	 * for the temp sorting array.
	 */
	dcc = NULL;
	ccons = NULL;
	cconslen = 0;

	/*
	 * Scan pg_constraint for relevant constraints.  We want to find
	 * constraints for not just this domain, but any ancestor domains, so the
	 * outer loop crawls up the domain stack.
	 */
	conRel = heap_open(ConstraintRelationId, AccessShareLock);

	for (;;)
	{
		HeapTuple	tup;
		HeapTuple	conTup;
		Form_pg_type typTup;
		int			nccons = 0;
		ScanKeyData key[1];
		SysScanDesc scan;

		tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typeOid));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for type %u", typeOid);
		typTup = (Form_pg_type) GETSTRUCT(tup);

		if (typTup->typtype != TYPTYPE_DOMAIN)
		{
			/* Not a domain, so done */
			ReleaseSysCache(tup);
			break;
		}

		/* Test for NOT NULL Constraint */
		if (typTup->typnotnull)
			notNull = true;

		/* Look for CHECK Constraints on this domain */
		ScanKeyInit(&key[0],
					Anum_pg_constraint_contypid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(typeOid));

		scan = systable_beginscan(conRel, ConstraintTypidIndexId, true,
								  NULL, 1, key);

		while (HeapTupleIsValid(conTup = systable_getnext(scan)))
		{
			Form_pg_constraint c = (Form_pg_constraint) GETSTRUCT(conTup);
			Datum		val;
			bool		isNull;
			char	   *constring;
			Expr	   *check_expr;
			DomainConstraintState *r;

			/* Ignore non-CHECK constraints (presently, shouldn't be any) */
			if (c->contype != CONSTRAINT_CHECK)
				continue;

			/* Not expecting conbin to be NULL, but we'll test for it anyway */
			val = fastgetattr(conTup, Anum_pg_constraint_conbin,
							  conRel->rd_att, &isNull);
			if (isNull)
				elog(ERROR, "domain \"%s\" constraint \"%s\" has NULL conbin",
					 NameStr(typTup->typname), NameStr(c->conname));

			/* Convert conbin to C string in caller context */
			constring = TextDatumGetCString(val);

			/* Create the DomainConstraintCache object and context if needed */
			if (dcc == NULL)
			{
				MemoryContext cxt;

				cxt = AllocSetContextCreate(CurrentMemoryContext,
											"Domain constraints",
											ALLOCSET_SMALL_INITSIZE,
											ALLOCSET_SMALL_MINSIZE,
											ALLOCSET_SMALL_MAXSIZE);
				dcc = (DomainConstraintCache *)
					MemoryContextAlloc(cxt, sizeof(DomainConstraintCache));
				dcc->constraints = NIL;
				dcc->dccContext = cxt;
				dcc->dccRefCount = 0;
			}

			/* Create node trees in DomainConstraintCache's context */
			oldcxt = MemoryContextSwitchTo(dcc->dccContext);

			check_expr = (Expr *) stringToNode(constring);

			/* ExecInitExpr will assume we've planned the expression */
			check_expr = expression_planner(check_expr);

			r = makeNode(DomainConstraintState);
			r->constrainttype = DOM_CONSTRAINT_CHECK;
			r->name = pstrdup(NameStr(c->conname));
			/* Must cast here because we're not storing an expr state node */
			r->check_expr = (ExprState *) check_expr;

			MemoryContextSwitchTo(oldcxt);

			/* Accumulate constraints in an array, for sorting below */
			if (ccons == NULL)
			{
				cconslen = 8;
				ccons = (DomainConstraintState **)
					palloc(cconslen * sizeof(DomainConstraintState *));
			}
			else if (nccons >= cconslen)
			{
				cconslen *= 2;
				ccons = (DomainConstraintState **)
					repalloc(ccons, cconslen * sizeof(DomainConstraintState *));
			}
			ccons[nccons++] = r;
		}

		systable_endscan(scan);

		if (nccons > 0)
		{
			/*
			 * Sort the items for this domain, so that CHECKs are applied in a
			 * deterministic order.
			 */
			if (nccons > 1)
				qsort(ccons, nccons, sizeof(DomainConstraintState *), dcs_cmp);

			/*
			 * Now attach them to the overall list.  Use lcons() here because
			 * constraints of parent domains should be applied earlier.
			 */
			oldcxt = MemoryContextSwitchTo(dcc->dccContext);
			while (nccons > 0)
				dcc->constraints = lcons(ccons[--nccons], dcc->constraints);
			MemoryContextSwitchTo(oldcxt);
		}

		/* loop to next domain in stack */
		typeOid = typTup->typbasetype;
		ReleaseSysCache(tup);
	}

	heap_close(conRel, AccessShareLock);

	/*
	 * Only need to add one NOT NULL check regardless of how many domains in
	 * the stack request it.
	 */
	if (notNull)
	{
		DomainConstraintState *r;

		/* Create the DomainConstraintCache object and context if needed */
		if (dcc == NULL)
		{
			MemoryContext cxt;

			cxt = AllocSetContextCreate(CurrentMemoryContext,
										"Domain constraints",
										ALLOCSET_SMALL_INITSIZE,
										ALLOCSET_SMALL_MINSIZE,
										ALLOCSET_SMALL_MAXSIZE);
			dcc = (DomainConstraintCache *)
				MemoryContextAlloc(cxt, sizeof(DomainConstraintCache));
			dcc->constraints = NIL;
			dcc->dccContext = cxt;
			dcc->dccRefCount = 0;
		}

		/* Create node trees in DomainConstraintCache's context */
		oldcxt = MemoryContextSwitchTo(dcc->dccContext);

		r = makeNode(DomainConstraintState);

		r->constrainttype = DOM_CONSTRAINT_NOTNULL;
		r->name = pstrdup("NOT NULL");
		r->check_expr = NULL;

		/* lcons to apply the nullness check FIRST */
		dcc->constraints = lcons(r, dcc->constraints);

		MemoryContextSwitchTo(oldcxt);
	}

	/*
	 * If we made a constraint object, move it into CacheMemoryContext and
	 * attach it to the typcache entry.
	 */
	if (dcc)
	{
		MemoryContextSetParent(dcc->dccContext, CacheMemoryContext);
		typentry->domainData = dcc;
		dcc->dccRefCount++;		/* count the typcache's reference */
	}

	/* Either way, the typcache entry's domain data is now valid. */
	typentry->flags |= TCFLAGS_CHECKED_DOMAIN_CONSTRAINTS;
}

/*
 * qsort comparator to sort DomainConstraintState pointers by name
 */
static int
dcs_cmp(const void *a, const void *b)
{
	const DomainConstraintState *const * ca = (const DomainConstraintState *const *) a;
	const DomainConstraintState *const * cb = (const DomainConstraintState *const *) b;

	return strcmp((*ca)->name, (*cb)->name);
}

/*
 * decr_dcc_refcount --- decrement a DomainConstraintCache's refcount,
 * and free it if no references remain
 */
static void
decr_dcc_refcount(DomainConstraintCache *dcc)
{
	Assert(dcc->dccRefCount > 0);
	if (--(dcc->dccRefCount) <= 0)
		MemoryContextDelete(dcc->dccContext);
}

/*
 * Context reset/delete callback for a DomainConstraintRef
 */
static void
dccref_deletion_callback(void *arg)
{
	DomainConstraintRef *ref = (DomainConstraintRef *) arg;
	DomainConstraintCache *dcc = ref->dcc;

	/* Paranoia --- be sure link is nulled before trying to release */
	if (dcc)
	{
		ref->constraints = NIL;
		ref->dcc = NULL;
		decr_dcc_refcount(dcc);
	}
}

/*
 * prep_domain_constraints --- prepare domain constraints for execution
 *
 * The expression trees stored in the DomainConstraintCache's list are
 * converted to executable expression state trees stored in execctx.
 */
static List *
prep_domain_constraints(List *constraints, MemoryContext execctx)
{
	List	   *result = NIL;
	MemoryContext oldcxt;
	ListCell   *lc;

	oldcxt = MemoryContextSwitchTo(execctx);

	foreach(lc, constraints)
	{
		DomainConstraintState *r = (DomainConstraintState *) lfirst(lc);
		DomainConstraintState *newr;

		newr = makeNode(DomainConstraintState);
		newr->constrainttype = r->constrainttype;
		newr->name = r->name;
		/* Must cast here because cache items contain expr plan trees */
		newr->check_expr = ExecInitExpr((Expr *) r->check_expr, NULL);

		result = lappend(result, newr);
	}

	MemoryContextSwitchTo(oldcxt);

	return result;
}

/*
 * InitDomainConstraintRef --- initialize a DomainConstraintRef struct
 *
 * Caller must tell us the MemoryContext in which the DomainConstraintRef
 * lives.  The ref will be cleaned up when that context is reset/deleted.
 */
void
InitDomainConstraintRef(Oid type_id, DomainConstraintRef *ref,
						MemoryContext refctx)
{
	/* Look up the typcache entry --- we assume it survives indefinitely */
	ref->tcache = lookup_type_cache(type_id, TYPECACHE_DOMAIN_INFO);
	/* For safety, establish the callback before acquiring a refcount */
	ref->refctx = refctx;
	ref->dcc = NULL;
	ref->callback.func = dccref_deletion_callback;
	ref->callback.arg = (void *) ref;
	MemoryContextRegisterResetCallback(refctx, &ref->callback);
	/* Acquire refcount if there are constraints, and set up exported list */
	if (ref->tcache->domainData)
	{
		ref->dcc = ref->tcache->domainData;
		ref->dcc->dccRefCount++;
		ref->constraints = prep_domain_constraints(ref->dcc->constraints,
												   ref->refctx);
	}
	else
		ref->constraints = NIL;
}

/*
 * UpdateDomainConstraintRef --- recheck validity of domain constraint info
 *
 * If the domain's constraint set changed, ref->constraints is updated to
 * point at a new list of cached constraints.
 *
 * In the normal case where nothing happened to the domain, this is cheap
 * enough that it's reasonable (and expected) to check before *each* use
 * of the constraint info.
 */
void
UpdateDomainConstraintRef(DomainConstraintRef *ref)
{
	TypeCacheEntry *typentry = ref->tcache;

	/* Make sure typcache entry's data is up to date */
	if ((typentry->flags & TCFLAGS_CHECKED_DOMAIN_CONSTRAINTS) == 0 &&
		typentry->typtype == TYPTYPE_DOMAIN)
		load_domaintype_info(typentry);

	/* Transfer to ref object if there's new info, adjusting refcounts */
	if (ref->dcc != typentry->domainData)
	{
		/* Paranoia --- be sure link is nulled before trying to release */
		DomainConstraintCache *dcc = ref->dcc;

		if (dcc)
		{
			/*
			 * Note: we just leak the previous list of executable domain
			 * constraints.  Alternatively, we could keep those in a child
			 * context of ref->refctx and free that context at this point.
			 * However, in practice this code path will be taken so seldom
			 * that the extra bookkeeping for a child context doesn't seem
			 * worthwhile; we'll just allow a leak for the lifespan of refctx.
			 */
			ref->constraints = NIL;
			ref->dcc = NULL;
			decr_dcc_refcount(dcc);
		}
		dcc = typentry->domainData;
		if (dcc)
		{
			ref->dcc = dcc;
			dcc->dccRefCount++;
			ref->constraints = prep_domain_constraints(dcc->constraints,
													   ref->refctx);
		}
	}
}

/*
 * DomainHasConstraints --- utility routine to check if a domain has constraints
 *
 * This is defined to return false, not fail, if type is not a domain.
 */
bool
DomainHasConstraints(Oid type_id)
{
	TypeCacheEntry *typentry;

	/*
	 * Note: a side effect is to cause the typcache's domain data to become
	 * valid.  This is fine since we'll likely need it soon if there is any.
	 */
	typentry = lookup_type_cache(type_id, TYPECACHE_DOMAIN_INFO);

	return (typentry->domainData != NULL);
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
		RecordCacheHash = hash_create("Record information cache", 64,
									  &ctl, HASH_ELEM | HASH_BLOBS);

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

		/* Reset equality/comparison/hashing validity information */
		typentry->flags = 0;
	}
}

/*
 * TypeCacheOpcCallback
 *		Syscache inval callback function
 *
 * This is called when a syscache invalidation event occurs for any pg_opclass
 * row.  In principle we could probably just invalidate data dependent on the
 * particular opclass, but since updates on pg_opclass are rare in production
 * it doesn't seem worth a lot of complication: we just mark all cached data
 * invalid.
 *
 * Note that we don't bother watching for updates on pg_amop or pg_amproc.
 * This should be safe because ALTER OPERATOR FAMILY ADD/DROP OPERATOR/FUNCTION
 * is not allowed to be used to add/drop the primary operators and functions
 * of an opclass, only cross-type members of a family; and the latter sorts
 * of members are not going to get cached here.
 */
static void
TypeCacheOpcCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS status;
	TypeCacheEntry *typentry;

	/* TypeCacheHash must exist, else this callback wouldn't be registered */
	hash_seq_init(&status, TypeCacheHash);
	while ((typentry = (TypeCacheEntry *) hash_seq_search(&status)) != NULL)
	{
		/* Reset equality/comparison/hashing validity information */
		typentry->flags = 0;
	}
}

/*
 * TypeCacheConstrCallback
 *		Syscache inval callback function
 *
 * This is called when a syscache invalidation event occurs for any
 * pg_constraint or pg_type row.  We flush information about domain
 * constraints when this happens.
 *
 * It's slightly annoying that we can't tell whether the inval event was for a
 * domain constraint/type record or not; there's usually more update traffic
 * for table constraints/types than domain constraints, so we'll do a lot of
 * useless flushes.  Still, this is better than the old no-caching-at-all
 * approach to domain constraints.
 */
static void
TypeCacheConstrCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	TypeCacheEntry *typentry;

	/*
	 * Because this is called very frequently, and typically very few of the
	 * typcache entries are for domains, we don't use hash_seq_search here.
	 * Instead we thread all the domain-type entries together so that we can
	 * visit them cheaply.
	 */
	for (typentry = firstDomainTypeEntry;
		 typentry != NULL;
		 typentry = typentry->nextDomain)
	{
		/* Reset domain constraint validity information */
		typentry->flags &= ~TCFLAGS_CHECKED_DOMAIN_CONSTRAINTS;
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
	 * permanent memory in CacheMemoryContext.  This minimizes the risk of
	 * leaking memory from CacheMemoryContext in the event of an error partway
	 * through.
	 */
	maxitems = 64;
	items = (EnumItem *) palloc(sizeof(EnumItem) * maxitems);
	numitems = 0;

	/* Scan pg_enum for the members of the target enum type. */
	ScanKeyInit(&skey,
				Anum_pg_enum_enumtypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(tcache->type_id));

	enum_rel = heap_open(EnumRelationId, AccessShareLock);
	enum_scan = systable_beginscan(enum_rel,
								   EnumTypIdLabelIndexId,
								   true, NULL,
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
