/*-------------------------------------------------------------------------
 *
 * syscache.c
 *	  System cache management routines
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/cache/syscache.c,v 1.59 2001/02/22 18:39:20 momjian Exp $
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
	name, index name, number of keys, and key attribute numbers.

	In include/catalog/indexing.h, add a define for the number of indexes
	on the relation, add define(s) for the index name(s), add an extern
	array to hold the index names, and use DECLARE_UNIQUE_INDEX to define
	the index.  Cache lookups return only one row, so the index should be
	unique in most cases.

	In backend/catalog/indexing.c, initialize the relation array with
	the index names for the relation.

	Finally, any place your relation gets heap_insert() or
	heap_update calls, include code to do a CatalogIndexInsert() to update
	the system indexes.  The heap_* calls do not update indexes.

	bjm 1999/11/22

  ---------------------------------------------------------------------------
*/

/*
 *		struct cachedesc: information defining a single syscache
 *
 */
struct cachedesc
{
	char	   *name;			/* name of the relation being cached */
	char	   *indname;		/* name of index relation for this cache */
	int			nkeys;			/* # of keys needed for cache lookup */
	int			key[4];			/* attribute numbers of key attrs */
};

static struct cachedesc cacheinfo[] = {
	{AggregateRelationName,		/* AGGNAME */
	 AggregateNameTypeIndex,
		2,
		{
			Anum_pg_aggregate_aggname,
			Anum_pg_aggregate_aggbasetype,
			0,
			0
		}},
	{AccessMethodRelationName,	/* AMNAME */
	 AmNameIndex,
		1,
		{
			Anum_pg_am_amname,
			0,
			0,
			0
		}},
	{AccessMethodOperatorRelationName,	/* AMOPOPID */
	 AccessMethodOpidIndex,
		3,
		{
			Anum_pg_amop_amopclaid,
			Anum_pg_amop_amopopr,
			Anum_pg_amop_amopid,
			0
		}},
	{AccessMethodOperatorRelationName,	/* AMOPSTRATEGY */
	 AccessMethodStrategyIndex,
		3,
		{
			Anum_pg_amop_amopid,
			Anum_pg_amop_amopclaid,
			Anum_pg_amop_amopstrategy,
			0
		}},
	{AttributeRelationName,		/* ATTNAME */
	 AttributeRelidNameIndex,
		2,
		{
			Anum_pg_attribute_attrelid,
			Anum_pg_attribute_attname,
			0,
			0
		}},
	{AttributeRelationName,		/* ATTNUM */
	 AttributeRelidNumIndex,
		2,
		{
			Anum_pg_attribute_attrelid,
			Anum_pg_attribute_attnum,
			0,
			0
		}},
	{OperatorClassRelationName, /* CLADEFTYPE */
	 OpclassDeftypeIndex,
		1,
		{
			Anum_pg_opclass_opcdeftype,
			0,
			0,
			0
		}},
	{OperatorClassRelationName, /* CLANAME */
	 OpclassNameIndex,
		1,
		{
			Anum_pg_opclass_opcname,
			0,
			0,
			0
		}},
	{GroupRelationName,			/* GRONAME */
	 GroupNameIndex,
		1,
		{
			Anum_pg_group_groname,
			0,
			0,
			0
		}},
	{GroupRelationName,			/* GROSYSID */
	 GroupSysidIndex,
		1,
		{
			Anum_pg_group_grosysid,
			0,
			0,
			0
		}},
	{IndexRelationName,			/* INDEXRELID */
	 IndexRelidIndex,
		1,
		{
			Anum_pg_index_indexrelid,
			0,
			0,
			0
		}},
	{InheritsRelationName,		/* INHRELID */
	 InheritsRelidSeqnoIndex,
		2,
		{
			Anum_pg_inherits_inhrelid,
			Anum_pg_inherits_inhseqno,
			0,
			0
		}},
	{LanguageRelationName,		/* LANGNAME */
	 LanguageNameIndex,
		1,
		{
			Anum_pg_language_lanname,
			0,
			0,
			0
		}},
	{LanguageRelationName,		/* LANGOID */
	 LanguageOidIndex,
		1,
		{
			ObjectIdAttributeNumber,
			0,
			0,
			0
		}},
	{ListenerRelationName,		/* LISTENREL */
	 ListenerPidRelnameIndex,
		2,
		{
			Anum_pg_listener_pid,
			Anum_pg_listener_relname,
			0,
			0
		}},
	{OperatorRelationName,		/* OPERNAME */
	 OperatorNameIndex,
		4,
		{
			Anum_pg_operator_oprname,
			Anum_pg_operator_oprleft,
			Anum_pg_operator_oprright,
			Anum_pg_operator_oprkind
		}},
	{OperatorRelationName,		/* OPEROID */
	 OperatorOidIndex,
		1,
		{
			ObjectIdAttributeNumber,
			0,
			0,
			0
		}},
	{ProcedureRelationName,		/* PROCNAME */
	 ProcedureNameIndex,
		3,
		{
			Anum_pg_proc_proname,
			Anum_pg_proc_pronargs,
			Anum_pg_proc_proargtypes,
			0
		}},
	{ProcedureRelationName,		/* PROCOID */
	 ProcedureOidIndex,
		1,
		{
			ObjectIdAttributeNumber,
			0,
			0,
			0
		}},
	{RelationRelationName,		/* RELNAME */
	 ClassNameIndex,
		1,
		{
			Anum_pg_class_relname,
			0,
			0,
			0
		}},
	{RelationRelationName,		/* RELOID */
	 ClassOidIndex,
		1,
		{
			ObjectIdAttributeNumber,
			0,
			0,
			0
		}},
	{RewriteRelationName,		/* REWRITENAME */
	 RewriteRulenameIndex,
		1,
		{
			Anum_pg_rewrite_rulename,
			0,
			0,
			0
		}},
	{RewriteRelationName,		/* RULEOID */
	 RewriteOidIndex,
		1,
		{
			ObjectIdAttributeNumber,
			0,
			0,
			0
		}},
	{ShadowRelationName,		/* SHADOWNAME */
	 ShadowNameIndex,
		1,
		{
			Anum_pg_shadow_usename,
			0,
			0,
			0
		}},
	{ShadowRelationName,		/* SHADOWSYSID */
	 ShadowSysidIndex,
		1,
		{
			Anum_pg_shadow_usesysid,
			0,
			0,
			0
		}},
	{StatisticRelationName,		/* STATRELID */
	 StatisticRelidAttnumIndex,
		2,
		{
			Anum_pg_statistic_starelid,
			Anum_pg_statistic_staattnum,
			0,
			0
		}},
	{TypeRelationName,			/* TYPENAME */
	 TypeNameIndex,
		1,
		{
			Anum_pg_type_typname,
			0,
			0,
			0
		}},
	{TypeRelationName,			/* TYPEOID */
	 TypeOidIndex,
		1,
		{
			ObjectIdAttributeNumber,
			0,
			0,
			0
		}}
};

static CatCache *SysCache[lengthof(cacheinfo)];
static int SysCacheSize = lengthof(cacheinfo);
static bool CacheInitialized = false;


bool
IsCacheInitialized(void)
{
	return CacheInitialized;
}


/*
 * InitCatalogCache - initialize the caches
 *
 * Note that no database access is done here; we only allocate memory
 * and initialize the cache structure.  Interrogation of the database
 * to complete initialization of a cache happens only upon first use
 * of that cache.
 */
void
InitCatalogCache(void)
{
	int			cacheId;

	Assert(!CacheInitialized);

	MemSet((char *) SysCache, 0, sizeof(SysCache));

	for (cacheId = 0; cacheId < SysCacheSize; cacheId++)
	{
		SysCache[cacheId] = InitCatCache(cacheId,
										 cacheinfo[cacheId].name,
										 cacheinfo[cacheId].indname,
										 cacheinfo[cacheId].nkeys,
										 cacheinfo[cacheId].key);
		if (!PointerIsValid(SysCache[cacheId]))
			elog(ERROR, "InitCatalogCache: Can't init cache %s (%d)",
				 cacheinfo[cacheId].name, cacheId);
	}
	CacheInitialized = true;
}


/*
 * SearchSysCache
 *
 *	A layer on top of SearchCatCache that does the initialization and
 *	key-setting for you.
 *
 *	Returns the cache copy of the tuple if one is found, NULL if not.
 *	The tuple is the 'cache' copy and must NOT be modified!
 *
 *	When the caller is done using the tuple, call ReleaseSysCache()
 *	to release the reference count grabbed by SearchSysCache().  If this
 *	is not done, the tuple will remain locked in cache until end of
 *	transaction, which is tolerable but not desirable.
 *
 *	CAUTION: The tuple that is returned must NOT be freed by the caller!
 */
HeapTuple
SearchSysCache(int cacheId,
			   Datum key1,
			   Datum key2,
			   Datum key3,
			   Datum key4)
{
	if (cacheId < 0 || cacheId >= SysCacheSize)
	{
		elog(ERROR, "SearchSysCache: Bad cache id %d", cacheId);
		return (HeapTuple) NULL;
	}

	Assert(PointerIsValid(SysCache[cacheId]));

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

	return SearchCatCache(SysCache[cacheId], key1, key2, key3, key4);
}

/*
 * ReleaseSysCache
 *		Release previously grabbed reference count on a tuple
 */
void
ReleaseSysCache(HeapTuple tuple)
{
	ReleaseCatCache(tuple);
}

/*
 * SearchSysCacheCopy
 *
 * A convenience routine that does SearchSysCache and (if successful)
 * returns a modifiable copy of the syscache entry.  The original
 * syscache entry is released before returning.  The caller should
 * heap_freetuple() the result when done with it.
 */
HeapTuple
SearchSysCacheCopy(int cacheId,
				   Datum key1,
				   Datum key2,
				   Datum key3,
				   Datum key4)
{
	HeapTuple	tuple,
				newtuple;

	tuple = SearchSysCache(cacheId, key1, key2, key3, key4);
	if (!HeapTupleIsValid(tuple))
		return tuple;
	newtuple = heap_copytuple(tuple);
	ReleaseSysCache(tuple);
	return newtuple;
}

/*
 * GetSysCacheOid
 *
 * A convenience routine that does SearchSysCache and returns the OID
 * of the found tuple, or InvalidOid if no tuple could be found.
 * No lock is retained on the syscache entry.
 */
Oid
GetSysCacheOid(int cacheId,
			   Datum key1,
			   Datum key2,
			   Datum key3,
			   Datum key4)
{
	HeapTuple	tuple;
	Oid			result;

	tuple = SearchSysCache(cacheId, key1, key2, key3, key4);
	if (!HeapTupleIsValid(tuple))
		return InvalidOid;
	result = tuple->t_data->t_oid;
	ReleaseSysCache(tuple);
	return result;
}

/*
 * SysCacheGetAttr
 *
 *		Given a tuple previously fetched by SearchSysCache(),
 *		extract a specific attribute.
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
		!PointerIsValid(SysCache[cacheId]->cc_tupdesc))
		elog(ERROR, "SysCacheGetAttr: missing cache data for id %d", cacheId);

	return heap_getattr(tup, attributeNumber,
						SysCache[cacheId]->cc_tupdesc,
						isNull);
}
