/*-------------------------------------------------------------------------
 *
 * rewriteRemove.c
 *	  routines for removing rewrite rules
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/rewrite/rewriteRemove.c,v 1.56 2003/08/04 02:40:03 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_rewrite.h"
#include "miscadmin.h"
#include "rewrite/rewriteRemove.h"
#include "rewrite/rewriteSupport.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/*
 * RemoveRewriteRule
 *
 * Delete a rule given its name.
 */
void
RemoveRewriteRule(Oid owningRel, const char *ruleName, DropBehavior behavior)
{
	HeapTuple	tuple;
	Oid			eventRelationOid;
	AclResult	aclresult;
	ObjectAddress object;

	/*
	 * Find the tuple for the target rule.
	 */
	tuple = SearchSysCache(RULERELNAME,
						   ObjectIdGetDatum(owningRel),
						   PointerGetDatum(ruleName),
						   0, 0);

	/*
	 * complain if no rule with such name exists
	 */
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("rule \"%s\" for relation \"%s\" does not exist",
						ruleName, get_rel_name(owningRel))));

	/*
	 * Verify user has appropriate permissions.
	 */
	eventRelationOid = ((Form_pg_rewrite) GETSTRUCT(tuple))->ev_class;
	Assert(eventRelationOid == owningRel);
	aclresult = pg_class_aclcheck(eventRelationOid, GetUserId(), ACL_RULE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_CLASS,
					   get_rel_name(eventRelationOid));

	/*
	 * Do the deletion
	 */
	object.classId = get_system_catalog_relid(RewriteRelationName);
	object.objectId = HeapTupleGetOid(tuple);
	object.objectSubId = 0;

	ReleaseSysCache(tuple);

	performDeletion(&object, behavior);
}


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
	bool		hasMoreRules;

	/*
	 * Open the pg_rewrite relation.
	 */
	RewriteRelation = heap_openr(RewriteRelationName, RowExclusiveLock);

	/*
	 * Find the tuple for the target rule.
	 */
	ScanKeyEntryInitialize(&skey[0], 0x0,
						   ObjectIdAttributeNumber, F_OIDEQ,
						   ObjectIdGetDatum(ruleOid));

	rcscan = systable_beginscan(RewriteRelation, RewriteOidIndex, true,
								SnapshotNow, 1, skey);

	tuple = systable_getnext(rcscan);

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for rule %u", ruleOid);

	/*
	 * We had better grab AccessExclusiveLock so that we know no other
	 * rule additions/deletions are going on for this relation.  Else we
	 * cannot set relhasrules correctly.  Besides, we don't want to be
	 * changing the ruleset while queries are executing on the rel.
	 */
	eventRelationOid = ((Form_pg_rewrite) GETSTRUCT(tuple))->ev_class;
	event_relation = heap_open(eventRelationOid, AccessExclusiveLock);

	hasMoreRules = event_relation->rd_rules != NULL &&
		event_relation->rd_rules->numLocks > 1;

	/*
	 * Now delete the pg_rewrite tuple for the rule
	 */
	simple_heap_delete(RewriteRelation, &tuple->t_self);

	systable_endscan(rcscan);

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
