/*-------------------------------------------------------------------------
 *
 * syscache.c
 *	  System cache management routines
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/cache/syscache.c,v 1.55 2000/06/20 01:41:22 tgl Exp $
 *
 * NOTES
 *	  These routines allow the parser/planner/executor to perform
 *	  rapid lookups on the contents of the system catalogs.
 *
 *	  see catalog/syscache.h for a list of the cache id's
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/transam.h"
#include "utils/builtins.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_group.h"
#include "catalog/pg_index.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_language.h"
#include "catalog/pg_listener.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_shadow.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "utils/catcache.h"
#include "utils/syscache.h"
#include "utils/temprel.h"
#include "miscadmin.h"


/*---------------------------------------------------------------------------

	Adding system caches:

	Add your new cache to the list in include/utils/syscache.h.  Keep
	the list sorted alphabetically and adjust the cache numbers
	accordingly.

	Add your entry to the cacheinfo[] array below.	All cache lists are
	alphabetical, so add it in the proper place.  Specify the relation
	name, number of arguments, argument attribute numbers, index name,
	and index lookup function.

	In include/catalog/indexing.h, add a define for the number of indexes
	on the relation, add define(s) for the index name(s), add an extern
	array to hold the index names, define the index lookup function
	prototype, and use DECLARE_UNIQUE_INDEX to define the index.  Cache
	lookups return only one row, so the index should be unique.

	In backend/catalog/indexing.c, initialize the relation array with
	the index names for the relation, and create the index lookup function.
	Pick one that has similar arguments and copy that one, but keep the
	function names in the same order as the cache list for clarity.

	Finally, any place your relation gets heap_insert() or
	heap_update calls, include code to do a CatalogIndexInsert() to update
	the system indexes.  The heap_* calls do not update indexes.

	bjm 1999/11/22

  ---------------------------------------------------------------------------
*/

/* ----------------
 *		struct cachedesc: information defining a single syscache
 * ----------------
 */
struct cachedesc
{
	char	   *name;			/* name of the relation being cached */
	int			nkeys;			/* # of keys needed for cache lookup */
	int			key[4];			/* attribute numbers of key attrs */
	char	   *indname;		/* name of index relation for this cache */
	ScanFunc	iScanFunc;		/* function to handle index scans */
};

static struct cachedesc cacheinfo[] = {
	{AggregateRelationName,		/* AGGNAME */
		2,
		{
			Anum_pg_aggregate_aggname,
			Anum_pg_aggregate_aggbasetype,
			0,
			0
		},
	AggregateNameTypeIndex,
	(ScanFunc) AggregateNameTypeIndexScan},
	{AccessMethodRelationName,	/* AMNAME */
		1,
		{
			Anum_pg_am_amname,
			0,
			0,
			0
		},
	AmNameIndex,
	(ScanFunc) AmNameIndexScan},
	{AccessMethodOperatorRelationName,	/* AMOPOPID */
		3,
		{
			Anum_pg_amop_amopclaid,
			Anum_pg_amop_amopopr,
			Anum_pg_amop_amopid,
			0
		},
	AccessMethodOpidIndex,
	(ScanFunc) AccessMethodOpidIndexScan},
	{AccessMethodOperatorRelationName,	/* AMOPSTRATEGY */
		3,
		{
			Anum_pg_amop_amopid,
			Anum_pg_amop_amopclaid,
			Anum_pg_amop_amopstrategy,
			0
		},
	AccessMethodStrategyIndex,
	(ScanFunc) AccessMethodStrategyIndexScan},
	{AttributeRelationName,		/* ATTNAME */
		2,
		{
			Anum_pg_attribute_attrelid,
			Anum_pg_attribute_attname,
			0,
			0
		},
	AttributeRelidNameIndex,
	(ScanFunc) AttributeRelidNameIndexScan},
	{AttributeRelationName,		/* ATTNUM */
		2,
		{
			Anum_pg_attribute_attrelid,
			Anum_pg_attribute_attnum,
			0,
			0
		},
	AttributeRelidNumIndex,
	(ScanFunc) AttributeRelidNumIndexScan},
	{OperatorClassRelationName, /* CLADEFTYPE */
		1,
		{
			Anum_pg_opclass_opcdeftype,
			0,
			0,
			0
		},
	OpclassDeftypeIndex,
	(ScanFunc) OpclassDeftypeIndexScan},
	{OperatorClassRelationName, /* CLANAME */
		1,
		{
			Anum_pg_opclass_opcname,
			0,
			0,
			0
		},
	OpclassNameIndex,
	(ScanFunc) OpclassNameIndexScan},
	{GroupRelationName,			/* GRONAME */
		1,
		{
			Anum_pg_group_groname,
			0,
			0,
			0
		},
	GroupNameIndex,
	(ScanFunc) GroupNameIndexScan},
	{GroupRelationName,			/* GROSYSID */
		1,
		{
			Anum_pg_group_grosysid,
			0,
			0,
			0
		},
	GroupSysidIndex,
	(ScanFunc) GroupSysidIndexScan},
	{IndexRelationName,			/* INDEXRELID */
		1,
		{
			Anum_pg_index_indexrelid,
			0,
			0,
			0
		},
	IndexRelidIndex,
	(ScanFunc) IndexRelidIndexScan},
	{InheritsRelationName,		/* INHRELID */
		2,
		{
			Anum_pg_inherits_inhrelid,
			Anum_pg_inherits_inhseqno,
			0,
			0
		},
	InheritsRelidSeqnoIndex,
	(ScanFunc) InheritsRelidSeqnoIndexScan},
	{LanguageRelationName,		/* LANGNAME */
		1,
		{
			Anum_pg_language_lanname,
			0,
			0,
			0
		},
	LanguageNameIndex,
	(ScanFunc) LanguageNameIndexScan},
	{LanguageRelationName,		/* LANGOID */
		1,
		{
			ObjectIdAttributeNumber,
			0,
			0,
			0
		},
	LanguageOidIndex,
	(ScanFunc) LanguageOidIndexScan},
	{ListenerRelationName,		/* LISTENREL */
		2,
		{
			Anum_pg_listener_pid,
			Anum_pg_listener_relname,
			0,
			0
		},
	ListenerPidRelnameIndex,
	(ScanFunc) ListenerPidRelnameIndexScan},
	{OperatorRelationName,		/* OPERNAME */
		4,
		{
			Anum_pg_operator_oprname,
			Anum_pg_operator_oprleft,
			Anum_pg_operator_oprright,
			Anum_pg_operator_oprkind
		},
	OperatorNameIndex,
	(ScanFunc) OperatorNameIndexScan},
	{OperatorRelationName,		/* OPEROID */
		1,
		{
			ObjectIdAttributeNumber,
			0,
			0,
			0
		},
	OperatorOidIndex,
	(ScanFunc) OperatorOidIndexScan},
	{ProcedureRelationName,		/* PROCNAME */
		3,
		{
			Anum_pg_proc_proname,
			Anum_pg_proc_pronargs,
			Anum_pg_proc_proargtypes,
			0
		},
	ProcedureNameIndex,
	(ScanFunc) ProcedureNameIndexScan},
	{ProcedureRelationName,		/* PROCOID */
		1,
		{
			ObjectIdAttributeNumber,
			0,
			0,
			0
		},
	ProcedureOidIndex,
	(ScanFunc) ProcedureOidIndexScan},
	{RelationRelationName,		/* RELNAME */
		1,
		{
			Anum_pg_class_relname,
			0,
			0,
			0
		},
	ClassNameIndex,
	(ScanFunc) ClassNameIndexScan},
	{RelationRelationName,		/* RELOID */
		1,
		{
			ObjectIdAttributeNumber,
			0,
			0,
			0
		},
	ClassOidIndex,
	(ScanFunc) ClassOidIndexScan},
	{RewriteRelationName,		/* REWRITENAME */
		1,
		{
			Anum_pg_rewrite_rulename,
			0,
			0,
			0
		},
	RewriteRulenameIndex,
	(ScanFunc) RewriteRulenameIndexScan},
	{RewriteRelationName,		/* RULEOID */
		1,
		{
			ObjectIdAttributeNumber,
			0,
			0,
			0
		},
	RewriteOidIndex,
	(ScanFunc) RewriteOidIndexScan},
	{ShadowRelationName,		/* SHADOWNAME */
		1,
		{
			Anum_pg_shadow_usename,
			0,
			0,
			0
		},
	ShadowNameIndex,
	(ScanFunc) ShadowNameIndexScan},
	{ShadowRelationName,		/* SHADOWSYSID */
		1,
		{
			Anum_pg_shadow_usesysid,
			0,
			0,
			0
		},
	ShadowSysidIndex,
	(ScanFunc) ShadowSysidIndexScan},
	{StatisticRelationName,		/* STATRELID */
		2,
		{
			Anum_pg_statistic_starelid,
			Anum_pg_statistic_staattnum,
			0,
			0
		},
	StatisticRelidAttnumIndex,
	(ScanFunc) StatisticRelidAttnumIndexScan},
	{TypeRelationName,			/* TYPENAME */
		1,
		{
			Anum_pg_type_typname,
			0,
			0,
			0
		},
	TypeNameIndex,
	(ScanFunc) TypeNameIndexScan},
	{TypeRelationName,			/* TYPEOID */
		1,
		{
			ObjectIdAttributeNumber,
			0,
			0,
			0
		},
	TypeOidIndex,
	(ScanFunc) TypeOidIndexScan}
};

static CatCache *SysCache[lengthof(cacheinfo)];
static int32 SysCacheSize = lengthof(cacheinfo);
static bool CacheInitialized = false;


bool
IsCacheInitialized(void)
{
	return CacheInitialized;
}


/*
 * zerocaches
 *
 *	  Make sure the SysCache structure is zero'd.
 */
void
zerocaches()
{
	MemSet((char *) SysCache, 0, SysCacheSize * sizeof(CatCache *));
}


/*
 * InitCatalogCache - initialize the caches
 */
void
InitCatalogCache()
{
	int			cacheId;		/* XXX type */

	if (!AMI_OVERRIDE)
	{
		for (cacheId = 0; cacheId < SysCacheSize; cacheId += 1)
		{
			Assert(!PointerIsValid(SysCache[cacheId]));

			SysCache[cacheId] = InitSysCache(cacheinfo[cacheId].name,
											 cacheinfo[cacheId].indname,
											 cacheId,
											 cacheinfo[cacheId].nkeys,
											 cacheinfo[cacheId].key,
										   cacheinfo[cacheId].iScanFunc);
			if (!PointerIsValid(SysCache[cacheId]))
			{
				elog(ERROR,
					 "InitCatalogCache: Can't init cache %s (%d)",
					 cacheinfo[cacheId].name,
					 cacheId);
			}

		}
	}
	CacheInitialized = true;
}


/*
 * SearchSysCacheTuple
 *
 *	A layer on top of SearchSysCache that does the initialization and
 *	key-setting for you.
 *
 *	Returns the cache copy of the tuple if one is found, NULL if not.
 *	The tuple is the 'cache' copy.
 *
 *	CAUTION: The tuple that is returned must NOT be freed by the caller!
 *
 *	CAUTION: The returned tuple may be flushed from the cache during
 *	subsequent cache lookup operations, or by shared cache invalidation.
 *	Callers should not expect the pointer to remain valid for long.
 *
 *  XXX we ought to have some kind of referencecount mechanism for
 *  cache entries, to ensure entries aren't deleted while in use.
 */
HeapTuple
SearchSysCacheTuple(int cacheId,/* cache selection code */
					Datum key1,
					Datum key2,
					Datum key3,
					Datum key4)
{
	HeapTuple	tp;

	if (cacheId < 0 || cacheId >= SysCacheSize)
	{
		elog(ERROR, "SearchSysCacheTuple: Bad cache id %d", cacheId);
		return (HeapTuple) NULL;
	}

	Assert(AMI_OVERRIDE || PointerIsValid(SysCache[cacheId]));

	if (!PointerIsValid(SysCache[cacheId]))
	{
		SysCache[cacheId] = InitSysCache(cacheinfo[cacheId].name,
										 cacheinfo[cacheId].indname,
										 cacheId,
										 cacheinfo[cacheId].nkeys,
										 cacheinfo[cacheId].key,
										 cacheinfo[cacheId].iScanFunc);
		if (!PointerIsValid(SysCache[cacheId]))
			elog(ERROR,
				 "InitCatalogCache: Can't init cache %s(%d)",
				 cacheinfo[cacheId].name,
				 cacheId);
	}

	/*
	 * If someone tries to look up a relname, translate temp relation
	 * names to real names.  Less obviously, apply the same translation
	 * to type names, so that the type tuple of a temp table will be found
	 * when sought.  This is a kluge ... temp table substitution should be
	 * happening at a higher level ...
	 */
	if (cacheId == RELNAME || cacheId == TYPENAME)
	{
		char	   *nontemp_relname;

		nontemp_relname = get_temp_rel_by_username(DatumGetCString(key1));
		if (nontemp_relname != NULL)
			key1 = CStringGetDatum(nontemp_relname);
	}

	tp = SearchSysCache(SysCache[cacheId], key1, key2, key3, key4);
	if (!HeapTupleIsValid(tp))
	{
#ifdef CACHEDEBUG
		elog(DEBUG,
			 "SearchSysCacheTuple: Search %s(%d) %d %d %d %d failed",
			 cacheinfo[cacheId].name,
			 cacheId, key1, key2, key3, key4);
#endif
		return (HeapTuple) NULL;
	}
	return tp;
}


/*
 * SearchSysCacheTupleCopy
 *
 *	This is like SearchSysCacheTuple, except it returns a palloc'd copy of
 *	the tuple.  The caller should heap_freetuple() the returned copy when
 *	done with it.  This routine should be used when the caller intends to
 *	continue to access the tuple for more than a very short period of time.
 */
HeapTuple
SearchSysCacheTupleCopy(int cacheId,	/* cache selection code */
						Datum key1,
						Datum key2,
						Datum key3,
						Datum key4)
{
	HeapTuple	cachetup;

	cachetup = SearchSysCacheTuple(cacheId, key1, key2, key3, key4);
	if (PointerIsValid(cachetup))
		return heap_copytuple(cachetup);
	else
		return cachetup;		/* NULL */
}


/*
 * SysCacheGetAttr
 *
 *		Given a tuple previously fetched by SearchSysCacheTuple() or
 *		SearchSysCacheTupleCopy(), extract a specific attribute.
 *
 * This is equivalent to using heap_getattr() on a tuple fetched
 * from a non-cached relation.	Usually, this is only used for attributes
 * that could be NULL or variable length; the fixed-size attributes in
 * a system table are accessed just by mapping the tuple onto the C struct
 * declarations from include/catalog/.
 *
 * As with heap_getattr(), if the attribute is of a pass-by-reference type
 * then a pointer into the tuple data area is returned --- the caller must
 * not modify or pfree the datum!
 */
Datum
SysCacheGetAttr(int cacheId, HeapTuple tup,
				AttrNumber attributeNumber,
				bool *isNull)
{

	/*
	 * We just need to get the TupleDesc out of the cache entry, and then
	 * we can apply heap_getattr().  We expect that the cache control data
	 * is currently valid --- if the caller recently fetched the tuple, then
	 * it should be.
	 */
	if (cacheId < 0 || cacheId >= SysCacheSize)
		elog(ERROR, "SysCacheGetAttr: Bad cache id %d", cacheId);
	if (!PointerIsValid(SysCache[cacheId]) ||
		SysCache[cacheId]->relationId == InvalidOid ||
		!PointerIsValid(SysCache[cacheId]->cc_tupdesc))
		elog(ERROR, "SysCacheGetAttr: missing cache data for id %d", cacheId);

	return heap_getattr(tup, attributeNumber,
						SysCache[cacheId]->cc_tupdesc,
						isNull);
}
