/*-------------------------------------------------------------------------
 *
 * rewriteRemove.c
 *	  routines for removing rewrite rules
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/rewrite/rewriteRemove.c,v 1.49 2002/04/27 03:45:03 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_rewrite.h"
#include "commands/comment.h"
#include "miscadmin.h"
#include "rewrite/rewriteRemove.h"
#include "rewrite/rewriteSupport.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/syscache.h"


/*
 * RemoveRewriteRule
 *
 * Delete a rule given its name.
 */
void
RemoveRewriteRule(Oid owningRel, const char *ruleName)
{
	Relation	RewriteRelation;
	Relation	event_relation;
	HeapTuple	tuple;
	Oid			ruleId;
	Oid			eventRelationOid;
	bool		hasMoreRules;
	AclResult	aclresult;

	/*
	 * Open the pg_rewrite relation.
	 */
	RewriteRelation = heap_openr(RewriteRelationName, RowExclusiveLock);

	/*
	 * Find the tuple for the target rule.
	 */
	tuple = SearchSysCacheCopy(RULERELNAME,
							   ObjectIdGetDatum(owningRel),
							   PointerGetDatum(ruleName),
							   0, 0);

	/*
	 * complain if no rule with such name existed
	 */
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "Rule \"%s\" not found", ruleName);

	/*
	 * Save the OID of the rule (i.e. the tuple's OID) and the event
	 * relation's OID
	 */
	ruleId = tuple->t_data->t_oid;
	eventRelationOid = ((Form_pg_rewrite) GETSTRUCT(tuple))->ev_class;
	Assert(eventRelationOid == owningRel);

	/*
	 * We had better grab AccessExclusiveLock so that we know no other
	 * rule additions/deletions are going on for this relation.  Else we
	 * cannot set relhasrules correctly.  Besides, we don't want to be
	 * changing the ruleset while queries are executing on the rel.
	 */
	event_relation = heap_open(eventRelationOid, AccessExclusiveLock);

	/*
	 * Verify user has appropriate permissions.
	 */
	aclresult = pg_class_aclcheck(eventRelationOid, GetUserId(), ACL_RULE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, RelationGetRelationName(event_relation));

	/* do not allow the removal of a view's SELECT rule */
	if (event_relation->rd_rel->relkind == RELKIND_VIEW &&
		((Form_pg_rewrite) GETSTRUCT(tuple))->ev_type == '1')
		elog(ERROR, "Cannot remove a view's SELECT rule");

	hasMoreRules = event_relation->rd_rules != NULL &&
		event_relation->rd_rules->numLocks > 1;

	/*
	 * Delete any comments associated with this rule
	 */
	DeleteComments(ruleId, RelationGetRelid(RewriteRelation));

	/*
	 * Now delete the pg_rewrite tuple for the rule
	 */
	simple_heap_delete(RewriteRelation, &tuple->t_self);

	heap_freetuple(tuple);

	heap_close(RewriteRelation, RowExclusiveLock);

	/*
	 * Set pg_class 'relhasrules' field correctly for event relation.
	 *
	 * Important side effect: an SI notice is broadcast to force all backends
	 * (including me!) to update relcache entries with the new rule set.
	 * Therefore, must do this even if relhasrules is still true!
	 */
	SetRelationRuleStatus(eventRelationOid, hasMoreRules, false);

	/* Close rel, but keep lock till commit... */
	heap_close(event_relation, NoLock);
}

/*
 * RelationRemoveRules -
 *	  removes all rules associated with the relation when the relation is
 *	  being removed.
 */
void
RelationRemoveRules(Oid relid)
{
	Relation	RewriteRelation;
	SysScanDesc scanDesc;
	ScanKeyData scanKeyData;
	HeapTuple	tuple;

	/*
	 * Open the pg_rewrite relation.
	 */
	RewriteRelation = heap_openr(RewriteRelationName, RowExclusiveLock);

	/*
	 * Scan pg_rewrite for all the tuples that have the same ev_class
	 * as relid (the relation to be removed).
	 */
	ScanKeyEntryInitialize(&scanKeyData,
						   0,
						   Anum_pg_rewrite_ev_class,
						   F_OIDEQ,
						   ObjectIdGetDatum(relid));

	scanDesc = systable_beginscan(RewriteRelation,
								  RewriteRelRulenameIndex,
								  true, SnapshotNow,
								  1, &scanKeyData);

	while (HeapTupleIsValid(tuple = systable_getnext(scanDesc)))
	{
		/* Delete any comments associated with this rule */
		DeleteComments(tuple->t_data->t_oid, RelationGetRelid(RewriteRelation));

		simple_heap_delete(RewriteRelation, &tuple->t_self);
	}

	systable_endscan(scanDesc);

	heap_close(RewriteRelation, RowExclusiveLock);
}
