/*-------------------------------------------------------------------------
 *
 * rewriteSupport.c
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/rewrite/rewriteSupport.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "catalog/indexing.h"
#include "catalog/pg_rewrite.h"
#include "rewrite/rewriteSupport.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/tqual.h"


/*
 * Is there a rule by the given name?
 */
bool
IsDefinedRewriteRule(Oid owningRel, const char *ruleName)
{
	return SearchSysCacheExists2(RULERELNAME,
								 ObjectIdGetDatum(owningRel),
								 PointerGetDatum(ruleName));
}


/*
 * SetRelationRuleStatus
 *		Set the value of the relation's relhasrules field in pg_class.
 *
 * NOTE: caller must be holding an appropriate lock on the relation.
 *
 * NOTE: an important side-effect of this operation is that an SI invalidation
 * message is sent out to all backends --- including me --- causing relcache
 * entries to be flushed or updated with the new set of rules for the table.
 * This must happen even if we find that no change is needed in the pg_class
 * row.
 */
void
SetRelationRuleStatus(Oid relationId, bool relHasRules)
{
	Relation	relationRelation;
	HeapTuple	tuple;
	Form_pg_class classForm;

	/*
	 * Find the tuple to update in pg_class, using syscache for the lookup.
	 */
	relationRelation = heap_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relationId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relationId);
	classForm = (Form_pg_class) GETSTRUCT(tuple);

	if (classForm->relhasrules != relHasRules)
	{
		/* Do the update */
		classForm->relhasrules = relHasRules;

		simple_heap_update(relationRelation, &tuple->t_self, tuple);

		/* Keep the catalog indexes up to date */
		CatalogUpdateIndexes(relationRelation, tuple);
	}
	else
	{
		/* no need to change tuple, but force relcache rebuild anyway */
		CacheInvalidateRelcacheByTuple(tuple);
	}

	heap_freetuple(tuple);
	heap_close(relationRelation, RowExclusiveLock);
}

/*
 * Find rule oid.
 *
 * If missing_ok is false, throw an error if rule name not found.  If
 * true, just return InvalidOid.
 */
Oid
get_rewrite_oid(Oid relid, const char *rulename, bool missing_ok)
{
	HeapTuple	tuple;
	Oid			ruleoid;

	/* Find the rule's pg_rewrite tuple, get its OID */
	tuple = SearchSysCache2(RULERELNAME,
							ObjectIdGetDatum(relid),
							PointerGetDatum(rulename));
	if (!HeapTupleIsValid(tuple))
	{
		if (missing_ok)
			return InvalidOid;
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("rule \"%s\" for relation \"%s\" does not exist",
						rulename, get_rel_name(relid))));
	}
	Assert(relid == ((Form_pg_rewrite) GETSTRUCT(tuple))->ev_class);
	ruleoid = HeapTupleGetOid(tuple);
	ReleaseSysCache(tuple);
	return ruleoid;
}

/*
 * Find rule oid, given only a rule name but no rel OID.
 *
 * If there's more than one, it's an error.  If there aren't any, that's an
 * error, too.	In general, this should be avoided - it is provided to support
 * syntax that is compatible with pre-7.3 versions of PG, where rule names
 * were unique across the entire database.
 */
Oid
get_rewrite_oid_without_relid(const char *rulename,
							  Oid *reloid, bool missing_ok)
{
	Relation	RewriteRelation;
	HeapScanDesc scanDesc;
	ScanKeyData scanKeyData;
	HeapTuple	htup;
	Oid			ruleoid;

	/* Search pg_rewrite for such a rule */
	ScanKeyInit(&scanKeyData,
				Anum_pg_rewrite_rulename,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(rulename));

	RewriteRelation = heap_open(RewriteRelationId, AccessShareLock);
	scanDesc = heap_beginscan(RewriteRelation, SnapshotNow, 1, &scanKeyData);

	htup = heap_getnext(scanDesc, ForwardScanDirection);
	if (!HeapTupleIsValid(htup))
	{
		if (!missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("rule \"%s\" does not exist", rulename)));
		ruleoid = InvalidOid;
	}
	else
	{
		ruleoid = HeapTupleGetOid(htup);
		if (reloid != NULL)
			*reloid = ((Form_pg_rewrite) GETSTRUCT(htup))->ev_class;

		htup = heap_getnext(scanDesc, ForwardScanDirection);
		if (HeapTupleIsValid(htup))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
				   errmsg("there are multiple rules named \"%s\"", rulename),
				errhint("Specify a relation name as well as a rule name.")));
	}
	heap_endscan(scanDesc);
	heap_close(RewriteRelation, AccessShareLock);

	return ruleoid;
}
