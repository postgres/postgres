/*-------------------------------------------------------------------------
 *
 * syscache.c
 *	  System cache management routines
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/cache/syscache.c,v 1.91 2003/09/24 18:54:01 tgl Exp $
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
#include "catalog/pg_amproc.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_group.h"
#include "catalog/pg_index.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_shadow.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "utils/catcache.h"
#include "utils/syscache.h"
#include "miscadmin.h"


/*---------------------------------------------------------------------------

	Adding system caches:

	Add your new cache to the list in include/utils/syscache.h.  Keep
	the list sorted alphabetically and adjust the cache numbers
	accordingly.

	Add your entry to the cacheinfo[] array below.	All cache lists are
	alphabetical, so add it in the proper place.  Specify the relation
	name, index name, number of keys, and key attribute numbers.  If the
	relation contains tuples that are associated with a particular relation
	(for example, its attributes, rules, triggers, etc) then specify the
	attribute number that contains the OID of the associated relation.
	This is used by CatalogCacheFlushRelation() to remove the correct
	tuples during a table drop or relcache invalidation event.

	There must be a unique index underlying each syscache (ie, an index
	whose key is the same as that of the cache).  If there is not one
	already, add definitions for it to include/catalog/indexing.h: you
	need a #define for the index name and a DECLARE_UNIQUE_INDEX macro
	with the actual declaration.  (This will require a catversion.h update,
	while simply adding/deleting caches only requires a recompile.)

	Finally, any place your relation gets heap_insert() or
	heap_update calls, make sure there is a CatalogUpdateIndexes() or
	similar call.  The heap_* calls do not update indexes.

	bjm 1999/11/22

  ---------------------------------------------------------------------------
*/

/*
 *		struct cachedesc: information defining a single syscache
 */
struct cachedesc
{
	const char *name;			/* name of the relation being cached */
	const char *indname;		/* name of index relation for this cache */
	int			reloidattr;		/* attr number of rel OID reference, or 0 */
	int			nkeys;			/* # of keys needed for cache lookup */
	int			key[4];			/* attribute numbers of key attrs */
};

static const struct cachedesc cacheinfo[] = {
	{AggregateRelationName,		/* AGGFNOID */
		AggregateFnoidIndex,
		0,
		1,
		{
			Anum_pg_aggregate_aggfnoid,
			0,
			0,
			0
	}},
	{AccessMethodRelationName,	/* AMNAME */
		AmNameIndex,
		0,
		1,
		{
			Anum_pg_am_amname,
			0,
			0,
			0
	}},
	{AccessMethodRelationName,	/* AMOID */
		AmOidIndex,
		0,
		1,
		{
			ObjectIdAttributeNumber,
			0,
			0,
			0
	}},
	{AccessMethodOperatorRelationName,	/* AMOPOPID */
		AccessMethodOperatorIndex,
		0,
		2,
		{
			Anum_pg_amop_amopopr,
			Anum_pg_amop_amopclaid,
			0,
			0
	}},
	{AccessMethodOperatorRelationName,	/* AMOPSTRATEGY */
		AccessMethodStrategyIndex,
		0,
		2,
		{
			Anum_pg_amop_amopclaid,
			Anum_pg_amop_amopstrategy,
			0,
			0
	}},
	{AccessMethodProcedureRelationName, /* AMPROCNUM */
		AccessMethodProcedureIndex,
		0,
		2,
		{
			Anum_pg_amproc_amopclaid,
			Anum_pg_amproc_amprocnum,
			0,
			0
	}},
	{AttributeRelationName,		/* ATTNAME */
		AttributeRelidNameIndex,
		Anum_pg_attribute_attrelid,
		2,
		{
			Anum_pg_attribute_attrelid,
			Anum_pg_attribute_attname,
			0,
			0
	}},
	{AttributeRelationName,		/* ATTNUM */
		AttributeRelidNumIndex,
		Anum_pg_attribute_attrelid,
		2,
		{
			Anum_pg_attribute_attrelid,
			Anum_pg_attribute_attnum,
			0,
			0
	}},
	{
		CastRelationName,		/* CASTSOURCETARGET */
		CastSourceTargetIndex,
		0,
		2,
		{
			Anum_pg_cast_castsource,
			Anum_pg_cast_casttarget,
			0,
			0
	}},
	{OperatorClassRelationName, /* CLAAMNAMENSP */
		OpclassAmNameNspIndex,
		0,
		3,
		{
			Anum_pg_opclass_opcamid,
			Anum_pg_opclass_opcname,
			Anum_pg_opclass_opcnamespace,
			0
	}},
	{OperatorClassRelationName, /* CLAOID */
		OpclassOidIndex,
		0,
		1,
		{
			ObjectIdAttributeNumber,
			0,
			0,
			0
	}},
	{ConversionRelationName,	/* CONDEFAULT */
		ConversionDefaultIndex,
		0,
		4,
		{
			Anum_pg_conversion_connamespace,
			Anum_pg_conversion_conforencoding,
			Anum_pg_conversion_contoencoding,
			ObjectIdAttributeNumber,
	}},
	{ConversionRelationName,	/* CONNAMENSP */
		ConversionNameNspIndex,
		0,
		2,
		{
			Anum_pg_conversion_conname,
			Anum_pg_conversion_connamespace,
			0,
			0
	}},
	{ConversionRelationName,	/* CONOID */
		ConversionOidIndex,
		0,
		1,
		{
			ObjectIdAttributeNumber,
			0,
			0,
			0
	}},
	{GroupRelationName,			/* GRONAME */
		GroupNameIndex,
		0,
		1,
		{
			Anum_pg_group_groname,
			0,
			0,
			0
	}},
	{GroupRelationName,			/* GROSYSID */
		GroupSysidIndex,
		0,
		1,
		{
			Anum_pg_group_grosysid,
			0,
			0,
			0
	}},
	{IndexRelationName,			/* INDEXRELID */
		IndexRelidIndex,
		Anum_pg_index_indrelid,
		1,
		{
			Anum_pg_index_indexrelid,
			0,
			0,
			0
	}},
	{InheritsRelationName,		/* INHRELID */
		InheritsRelidSeqnoIndex,
		Anum_pg_inherits_inhrelid,
		2,
		{
			Anum_pg_inherits_inhrelid,
			Anum_pg_inherits_inhseqno,
			0,
			0
	}},
	{LanguageRelationName,		/* LANGNAME */
		LanguageNameIndex,
		0,
		1,
		{
			Anum_pg_language_lanname,
			0,
			0,
			0
	}},
	{LanguageRelationName,		/* LANGOID */
		LanguageOidIndex,
		0,
		1,
		{
			ObjectIdAttributeNumber,
			0,
			0,
			0
	}},
	{NamespaceRelationName,		/* NAMESPACENAME */
		NamespaceNameIndex,
		0,
		1,
		{
			Anum_pg_namespace_nspname,
			0,
			0,
			0
	}},
	{NamespaceRelationName,		/* NAMESPACEOID */
		NamespaceOidIndex,
		0,
		1,
		{
			ObjectIdAttributeNumber,
			0,
			0,
			0
	}},
	{OperatorRelationName,		/* OPERNAMENSP */
		OperatorNameNspIndex,
		0,
		4,
		{
			Anum_pg_operator_oprname,
			Anum_pg_operator_oprleft,
			Anum_pg_operator_oprright,
			Anum_pg_operator_oprnamespace
	}},
	{OperatorRelationName,		/* OPEROID */
		OperatorOidIndex,
		0,
		1,
		{
			ObjectIdAttributeNumber,
			0,
			0,
			0
	}},
	{ProcedureRelationName,		/* PROCNAMENSP */
		ProcedureNameNspIndex,
		0,
		4,
		{
			Anum_pg_proc_proname,
			Anum_pg_proc_pronargs,
			Anum_pg_proc_proargtypes,
			Anum_pg_proc_pronamespace
	}},
	{ProcedureRelationName,		/* PROCOID */
		ProcedureOidIndex,
		0,
		1,
		{
			ObjectIdAttributeNumber,
			0,
			0,
			0
	}},
	{RelationRelationName,		/* RELNAMENSP */
		ClassNameNspIndex,
		ObjectIdAttributeNumber,
		2,
		{
			Anum_pg_class_relname,
			Anum_pg_class_relnamespace,
			0,
			0
	}},
	{RelationRelationName,		/* RELOID */
		ClassOidIndex,
		ObjectIdAttributeNumber,
		1,
		{
			ObjectIdAttributeNumber,
			0,
			0,
			0
	}},
	{RewriteRelationName,		/* RULERELNAME */
		RewriteRelRulenameIndex,
		Anum_pg_rewrite_ev_class,
		2,
		{
			Anum_pg_rewrite_ev_class,
			Anum_pg_rewrite_rulename,
			0,
			0
	}},
	{ShadowRelationName,		/* SHADOWNAME */
		ShadowNameIndex,
		0,
		1,
		{
			Anum_pg_shadow_usename,
			0,
			0,
			0
	}},
	{ShadowRelationName,		/* SHADOWSYSID */
		ShadowSysidIndex,
		0,
		1,
		{
			Anum_pg_shadow_usesysid,
			0,
			0,
			0
	}},
	{StatisticRelationName,		/* STATRELATT */
		StatisticRelidAttnumIndex,
		Anum_pg_statistic_starelid,
		2,
		{
			Anum_pg_statistic_starelid,
			Anum_pg_statistic_staattnum,
			0,
			0
	}},
	{TypeRelationName,			/* TYPENAMENSP */
		TypeNameNspIndex,
		Anum_pg_type_typrelid,
		2,
		{
			Anum_pg_type_typname,
			Anum_pg_type_typnamespace,
			0,
			0
	}},
	{TypeRelationName,			/* TYPEOID */
		TypeOidIndex,
		Anum_pg_type_typrelid,
		1,
		{
			ObjectIdAttributeNumber,
			0,
			0,
			0
	}}
};

static CatCache *SysCache[lengthof(cacheinfo)];
static int	SysCacheSize = lengthof(cacheinfo);
static bool CacheInitialized = false;


/*
 * InitCatalogCache - initialize the caches
 *
 * Note that no database access is done here; we only allocate memory
 * and initialize the cache structure.	Interrogation of the database
 * to complete initialization of a cache happens upon first use
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
										 cacheinfo[cacheId].reloidattr,
										 cacheinfo[cacheId].nkeys,
										 cacheinfo[cacheId].key);
		if (!PointerIsValid(SysCache[cacheId]))
			elog(ERROR, "could not initialize cache %s (%d)",
				 cacheinfo[cacheId].name, cacheId);
	}
	CacheInitialized = true;
}


/*
 * InitCatalogCachePhase2 - finish initializing the caches
 *
 * Finish initializing all the caches, including necessary database
 * access.
 *
 * This is *not* essential; normally we allow syscaches to be initialized
 * on first use.  However, it is useful as a mechanism to preload the
 * relcache with entries for the most-commonly-used system catalogs.
 * Therefore, we invoke this routine when we need to write a new relcache
 * init file.
 */
void
InitCatalogCachePhase2(void)
{
	int			cacheId;

	Assert(CacheInitialized);

	for (cacheId = 0; cacheId < SysCacheSize; cacheId++)
		InitCatCachePhase2(SysCache[cacheId]);
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
	if (cacheId < 0 || cacheId >= SysCacheSize ||
		!PointerIsValid(SysCache[cacheId]))
		elog(ERROR, "invalid cache id: %d", cacheId);

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
 * SearchSysCacheExists
 *
 * A convenience routine that just probes to see if a tuple can be found.
 * No lock is retained on the syscache entry.
 */
bool
SearchSysCacheExists(int cacheId,
					 Datum key1,
					 Datum key2,
					 Datum key3,
					 Datum key4)
{
	HeapTuple	tuple;

	tuple = SearchSysCache(cacheId, key1, key2, key3, key4);
	if (!HeapTupleIsValid(tuple))
		return false;
	ReleaseSysCache(tuple);
	return true;
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
	result = HeapTupleGetOid(tuple);
	ReleaseSysCache(tuple);
	return result;
}


/*
 * SearchSysCacheAttName
 *
 * This routine is equivalent to SearchSysCache on the ATTNAME cache,
 * except that it will return NULL if the found attribute is marked
 * attisdropped.  This is convenient for callers that want to act as
 * though dropped attributes don't exist.
 */
HeapTuple
SearchSysCacheAttName(Oid relid, const char *attname)
{
	HeapTuple	tuple;

	tuple = SearchSysCache(ATTNAME,
						   ObjectIdGetDatum(relid),
						   CStringGetDatum(attname),
						   0, 0);
	if (!HeapTupleIsValid(tuple))
		return NULL;
	if (((Form_pg_attribute) GETSTRUCT(tuple))->attisdropped)
	{
		ReleaseSysCache(tuple);
		return NULL;
	}
	return tuple;
}

/*
 * SearchSysCacheCopyAttName
 *
 * As above, an attisdropped-aware version of SearchSysCacheCopy.
 */
HeapTuple
SearchSysCacheCopyAttName(Oid relid, const char *attname)
{
	HeapTuple	tuple,
				newtuple;

	tuple = SearchSysCacheAttName(relid, attname);
	if (!HeapTupleIsValid(tuple))
		return tuple;
	newtuple = heap_copytuple(tuple);
	ReleaseSysCache(tuple);
	return newtuple;
}

/*
 * SearchSysCacheExistsAttName
 *
 * As above, an attisdropped-aware version of SearchSysCacheExists.
 */
bool
SearchSysCacheExistsAttName(Oid relid, const char *attname)
{
	HeapTuple	tuple;

	tuple = SearchSysCacheAttName(relid, attname);
	if (!HeapTupleIsValid(tuple))
		return false;
	ReleaseSysCache(tuple);
	return true;
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
	 * is currently valid --- if the caller recently fetched the tuple,
	 * then it should be.
	 */
	if (cacheId < 0 || cacheId >= SysCacheSize)
		elog(ERROR, "invalid cache id: %d", cacheId);
	if (!PointerIsValid(SysCache[cacheId]) ||
		!PointerIsValid(SysCache[cacheId]->cc_tupdesc))
		elog(ERROR, "missing cache data for cache id %d", cacheId);

	return heap_getattr(tup, attributeNumber,
						SysCache[cacheId]->cc_tupdesc,
						isNull);
}

/*
 * List-search interface
 */
struct catclist *
SearchSysCacheList(int cacheId, int nkeys,
				   Datum key1, Datum key2, Datum key3, Datum key4)
{
	if (cacheId < 0 || cacheId >= SysCacheSize ||
		!PointerIsValid(SysCache[cacheId]))
		elog(ERROR, "invalid cache id: %d", cacheId);

	return SearchCatCacheList(SysCache[cacheId], nkeys,
							  key1, key2, key3, key4);
}
