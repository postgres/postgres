/*-------------------------------------------------------------------------
 *
 * syscache.c
 *	  System cache management routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/cache/syscache.c,v 1.37 1999/09/30 10:31:43 wieck Exp $
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
#include "catalog/catname.h"
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
#include "catalog/pg_type.h"
#include "utils/catcache.h"
#include "utils/temprel.h"

extern bool AMI_OVERRIDE;		/* XXX style */

#include "utils/syscache.h"
#include "catalog/indexing.h"

typedef HeapTuple (*ScanFunc) ();

/* ----------------
 *		Warning:  cacheinfo[] below is changed, then be sure and
 *		update the magic constants in syscache.h!
 * ----------------
 */
static struct cachedesc cacheinfo[] = {
	{AccessMethodOperatorRelationName,	/* AMOPOPID */
		3,
		{
			Anum_pg_amop_amopclaid,
			Anum_pg_amop_amopopr,
			Anum_pg_amop_amopid,
			0
		},
		sizeof(FormData_pg_amop),
		NULL,
	(ScanFunc) NULL},
	{AccessMethodOperatorRelationName,	/* AMOPSTRATEGY */
		3,
		{
			Anum_pg_amop_amopid,
			Anum_pg_amop_amopclaid,
			Anum_pg_amop_amopstrategy,
			0
		},
		sizeof(FormData_pg_amop),
		NULL,
	(ScanFunc) NULL},
	{AttributeRelationName,		/* ATTNAME */
		2,
		{
			Anum_pg_attribute_attrelid,
			Anum_pg_attribute_attname,
			0,
			0
		},
		ATTRIBUTE_TUPLE_SIZE,
		AttributeNameIndex,
	(ScanFunc) AttributeNameIndexScan},
	{AttributeRelationName,		/* ATTNUM */
		2,
		{
			Anum_pg_attribute_attrelid,
			Anum_pg_attribute_attnum,
			0,
			0
		},
		ATTRIBUTE_TUPLE_SIZE,
		AttributeNumIndex,
	(ScanFunc) AttributeNumIndexScan},
	{IndexRelationName,			/* INDEXRELID */
		1,
		{
			Anum_pg_index_indexrelid,
			0,
			0,
			0
		},
		offsetof(FormData_pg_index, indpred),
		NULL,
	NULL},
	{LanguageRelationName,		/* LANNAME */
		1,
		{
			Anum_pg_language_lanname,
			0,
			0,
			0
		},
		offsetof(FormData_pg_language, lancompiler),
		NULL,
	NULL},
	{OperatorRelationName,		/* OPRNAME */
		4,
		{
			Anum_pg_operator_oprname,
			Anum_pg_operator_oprleft,
			Anum_pg_operator_oprright,
			Anum_pg_operator_oprkind
		},
		sizeof(FormData_pg_operator),
		NULL,
	NULL},
	{OperatorRelationName,		/* OPROID */
		1,
		{
			ObjectIdAttributeNumber,
			0,
			0,
			0
		},
		sizeof(FormData_pg_operator),
		NULL,
	(ScanFunc) NULL},
	{ProcedureRelationName,		/* PRONAME */
		3,
		{
			Anum_pg_proc_proname,
			Anum_pg_proc_pronargs,
			Anum_pg_proc_proargtypes,
			0
		},
		offsetof(FormData_pg_proc, prosrc),
		ProcedureNameIndex,
	(ScanFunc) ProcedureNameIndexScan},
	{ProcedureRelationName,		/* PROOID */
		1,
		{
			ObjectIdAttributeNumber,
			0,
			0,
			0
		},
		offsetof(FormData_pg_proc, prosrc),
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
		CLASS_TUPLE_SIZE,
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
		CLASS_TUPLE_SIZE,
		ClassOidIndex,
	(ScanFunc) ClassOidIndexScan},
	{TypeRelationName,			/* TYPNAME */
		1,
		{
			Anum_pg_type_typname,
			0,
			0,
			0
		},
		offsetof(FormData_pg_type, typalign) +sizeof(char),
		TypeNameIndex,
	TypeNameIndexScan},
	{TypeRelationName,			/* TYPOID */
		1,
		{
			ObjectIdAttributeNumber,
			0,
			0,
			0
		},
		offsetof(FormData_pg_type, typalign) +sizeof(char),
		TypeOidIndex,
	TypeOidIndexScan},
	{AccessMethodRelationName,	/* AMNAME */
		1,
		{
			Anum_pg_am_amname,
			0,
			0,
			0
		},
		sizeof(FormData_pg_am),
		NULL,
	NULL},
	{OperatorClassRelationName, /* CLANAME */
		1,
		{
			Anum_pg_opclass_opcname,
			0,
			0,
			0
		},
		sizeof(FormData_pg_opclass),
		NULL,
	NULL},
	{IndexRelationName,			/* INDRELIDKEY *//* never used */
		2,
		{
			Anum_pg_index_indrelid,
			Anum_pg_index_indkey,
			0,
			0
		},
		offsetof(FormData_pg_index, indpred),
		NULL,
	(ScanFunc) NULL},
	{InheritsRelationName,		/* INHRELID */
		2,
		{
			Anum_pg_inherits_inhrel,
			Anum_pg_inherits_inhseqno,
			0,
			0
		},
		sizeof(FormData_pg_inherits),
		NULL,
	(ScanFunc) NULL},
	{RewriteRelationName,		/* RULOID */
		1,
		{
			ObjectIdAttributeNumber,
			0,
			0,
			0
		},
		offsetof(FormData_pg_rewrite, ev_qual),
		NULL,
	(ScanFunc) NULL},
	{AggregateRelationName,		/* AGGNAME */
		2,
		{
			Anum_pg_aggregate_aggname,
			Anum_pg_aggregate_aggbasetype,
			0,
			0
		},
		offsetof(FormData_pg_aggregate, agginitval1),
		NULL,
	(ScanFunc) NULL},
	{ListenerRelationName,		/* LISTENREL */
		2,
		{
			Anum_pg_listener_relname,
			Anum_pg_listener_pid,
			0,
			0
		},
		sizeof(FormData_pg_listener),
		NULL,
	(ScanFunc) NULL},
	{ShadowRelationName,		/* USENAME */
		1,
		{
			Anum_pg_shadow_usename,
			0,
			0,
			0
		},
		sizeof(FormData_pg_shadow),
		NULL,
	(ScanFunc) NULL},
	{ShadowRelationName,		/* USESYSID */
		1,
		{
			Anum_pg_shadow_usesysid,
			0,
			0,
			0
		},
		sizeof(FormData_pg_shadow),
		NULL,
	(ScanFunc) NULL},
	{GroupRelationName,			/* GRONAME */
		1,
		{
			Anum_pg_group_groname,
			0,
			0,
			0
		},
		offsetof(FormData_pg_group, grolist[0]),
		NULL,
	(ScanFunc) NULL},
	{GroupRelationName,			/* GROSYSID */
		1,
		{
			Anum_pg_group_grosysid,
			0,
			0,
			0
		},
		offsetof(FormData_pg_group, grolist[0]),
		NULL,
	(ScanFunc) NULL},
	{RewriteRelationName,		/* REWRITENAME */
		1,
		{
			Anum_pg_rewrite_rulename,
			0,
			0,
			0
		},
		offsetof(FormData_pg_rewrite, ev_qual),
		NULL,
	(ScanFunc) NULL},
	{OperatorClassRelationName, /* CLADEFTYPE */
		1,
		{
			Anum_pg_opclass_opcdeftype,
			0,
			0,
			0
		},
		sizeof(FormData_pg_opclass),
		NULL,
	(ScanFunc) NULL},
	{LanguageRelationName,		/* LANOID */
		1,
		{
			ObjectIdAttributeNumber,
			0,
			0,
			0
		},
		offsetof(FormData_pg_language, lancompiler),
		NULL,
	NULL}
};

static struct catcache *SysCache[lengthof(cacheinfo)];
static int32 SysCacheSize = lengthof(cacheinfo);


/*
 * zerocaches
 *
 *	  Make sure the SysCache structure is zero'd.
 */
void
zerocaches()
{
	MemSet((char *) SysCache, 0, SysCacheSize * sizeof(struct catcache *));
}

/*
 * Note:
 *		This function was written because the initialized catalog caches
 *		are used to determine which caches may contain tuples which need
 *		to be invalidated in other backends.
 */
void
InitCatalogCache()
{
	int			cacheId;		/* XXX type */

	if (!AMI_OVERRIDE)
	{
		for (cacheId = 0; cacheId < SysCacheSize; cacheId += 1)
		{

			Assert(!PointerIsValid((Pointer) SysCache[cacheId]));

			SysCache[cacheId] = InitSysCache(cacheinfo[cacheId].name,
											 cacheinfo[cacheId].indname,
											 cacheId,
											 cacheinfo[cacheId].nkeys,
											 cacheinfo[cacheId].key,
										   cacheinfo[cacheId].iScanFunc);
			if (!PointerIsValid((char *) SysCache[cacheId]))
			{
				elog(ERROR,
					 "InitCatalogCache: Can't init cache %s(%d)",
					 cacheinfo[cacheId].name,
					 cacheId);
			}

		}
	}
}

/*
 * SearchSysCacheTupleCopy
 *
 *	This is like SearchSysCacheTuple, except it returns a copy of the tuple
 *	that the user is required to pfree().
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
 * SearchSysCacheTuple
 *
 *	A layer on top of SearchSysCache that does the initialization and
 *	key-setting for you.
 *
 *	Returns the cache copy of the tuple if one is found, NULL if not.
 *	The tuple is the 'cache' copy.
 *
 *	XXX The tuple that is returned is NOT supposed to be pfree'd!
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

	/* temp table name remapping */
	if (cacheId == RELNAME)
	{
		char *nontemp_relname;

		if ((nontemp_relname =
			 get_temp_rel_by_name(DatumGetPointer(key1))) != NULL)
			key1 = PointerGetDatum(nontemp_relname);
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
 * SearchSysCacheStruct
 *	  Fills 's' with the information retrieved by calling SearchSysCache()
 *	  with arguments key1...key4.  Retrieves only the portion of the tuple
 *	  which is not variable-length.
 *
 * NOTE: we are assuming that non-variable-length fields in the system
 *		 catalogs will always be defined!
 *
 * Returns 1L if a tuple was found, 0L if not.
 */
int32
SearchSysCacheStruct(int cacheId,		/* cache selection code */
					 char *returnStruct,		/* (preallocated!) */
					 Datum key1,
					 Datum key2,
					 Datum key3,
					 Datum key4)
{
	HeapTuple	tp;

	if (!PointerIsValid(returnStruct))
	{
		elog(ERROR, "SearchSysCacheStruct: No receiving struct");
		return 0;
	}
	tp = SearchSysCacheTuple(cacheId, key1, key2, key3, key4);
	if (!HeapTupleIsValid(tp))
		return 0;
	memcpy(returnStruct, (char *) GETSTRUCT(tp), cacheinfo[cacheId].size);
	return 1;
}


/*
 * SearchSysCacheGetAttribute
 *	  Returns the attribute corresponding to 'attributeNumber' for
 *	  a given cached tuple.  This routine usually needs to be used for
 *	  attributes that might be NULL or might be at a variable offset
 *	  in the tuple.
 *
 * XXX This re-opens the relation, so this is slower than just pulling
 * fixed-location fields out of the struct returned by SearchSysCacheTuple.
 *
 * [callers all assume this returns a (struct varlena *). -ay 10/94]
 */
void *
SearchSysCacheGetAttribute(int cacheId,
						   AttrNumber attributeNumber,
						   Datum key1,
						   Datum key2,
						   Datum key3,
						   Datum key4)
{
	HeapTuple	tp;
	char	   *cacheName;
	Relation	relation;
	int32		attributeLength,
				attributeByValue;
	bool		isNull;
	Datum		attributeValue;
	void	   *returnValue;

	/*
	 * Open the relation first, to ensure we are in sync with SI inval
	 * events --- we don't want the tuple found in the cache to be
	 * invalidated out from under us.
	 */
	cacheName = cacheinfo[cacheId].name;
	relation = heap_openr(cacheName, AccessShareLock);

	tp = SearchSysCacheTuple(cacheId, key1, key2, key3, key4);

	if (!HeapTupleIsValid(tp))
	{
		heap_close(relation, AccessShareLock);
#ifdef	CACHEDEBUG
		elog(DEBUG,
			 "SearchSysCacheGetAttribute: Lookup in %s(%d) failed",
			 cacheName, cacheId);
#endif	 /* defined(CACHEDEBUG) */
		return NULL;
	}

	if (attributeNumber < 0 &&
		attributeNumber > FirstLowInvalidHeapAttributeNumber)
	{
		attributeLength = heap_sysattrlen(attributeNumber);
		attributeByValue = heap_sysattrbyval(attributeNumber);
	}
	else if (attributeNumber > 0 &&
			 attributeNumber <= relation->rd_rel->relnatts)
	{
		attributeLength = relation->rd_att->attrs[attributeNumber - 1]->attlen;
		attributeByValue = relation->rd_att->attrs[attributeNumber - 1]->attbyval;
	}
	else
	{
		heap_close(relation, AccessShareLock);
		elog(ERROR,
			 "SearchSysCacheGetAttribute: Bad attr # %d in %s(%d)",
			 attributeNumber, cacheName, cacheId);
		return NULL;
	}

	attributeValue = heap_getattr(tp,
								  attributeNumber,
								  RelationGetDescr(relation),
								  &isNull);

	if (isNull)
	{
		/*
		 * Used to be an elog(DEBUG, ...) here and a claim that it should
		 * be a FATAL error, I don't think either is warranted -mer 6/9/92
		 */
		heap_close(relation, AccessShareLock);
		return NULL;
	}

	if (attributeByValue)
		returnValue = (void *) attributeValue;
	else
	{
		char	   *tmp;
		int			size = (attributeLength < 0)
		? VARSIZE((struct varlena *) attributeValue)	/* variable length */
		: attributeLength;		/* fixed length */

		tmp = (char *) palloc(size);
		memcpy(tmp, (void *) attributeValue, size);
		returnValue = (void *) tmp;
	}

	heap_close(relation, AccessShareLock);
	return returnValue;
}
