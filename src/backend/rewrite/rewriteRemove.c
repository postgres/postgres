/*-------------------------------------------------------------------------
 *
 * rewriteRemove.c
 *	  routines for removing rewrite rules
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/rewrite/rewriteRemove.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_rewrite.h"
#include "miscadmin.h"
#include "rewrite/rewriteRemove.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/tqual.h"

/*
 * Guts of rule deletion.
 */
void
RemoveRewriteRuleById(Oid ruleOid)
{
	Relation	RewriteRelation;
	ScanKeyData skey[1];
	SysScanDesc rcscan;
	Relation	event_relation;
	HeapTuple	tuple;
	Oid			eventRelationOid;

	/*
	 * Open the pg_rewrite relation.
	 */
	RewriteRelation = heap_open(RewriteRelationId, RowExclusiveLock);

	/*
	 * Find the tuple for the target rule.
	 */
	ScanKeyInit(&skey[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ruleOid));

	rcscan = systable_beginscan(RewriteRelation, RewriteOidIndexId, true,
								SnapshotNow, 1, skey);

	tuple = systable_getnext(rcscan);

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for rule %u", ruleOid);

	/*
	 * We had better grab AccessExclusiveLock to ensure that no queries are
	 * going on that might depend on this rule.  (Note: a weaker lock would
	 * suffice if it's not an ON SELECT rule.)
	 */
	eventRelationOid = ((Form_pg_rewrite) GETSTRUCT(tuple))->ev_class;
	event_relation = heap_open(eventRelationOid, AccessExclusiveLock);

	/*
	 * Now delete the pg_rewrite tuple for the rule
	 */
	simple_heap_delete(RewriteRelation, &tuple->t_self);

	systable_endscan(rcscan);

	heap_close(RewriteRelation, RowExclusiveLock);

	/*
	 * Issue shared-inval notice to force all backends (including me!) to
	 * update relcache entries with the new rule set.
	 */
	CacheInvalidateRelcache(event_relation);

	/* Close rel, but keep lock till commit... */
	heap_close(event_relation, NoLock);
}
