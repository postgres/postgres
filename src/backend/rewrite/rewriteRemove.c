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
 *	  $Header: /cvsroot/pgsql/src/backend/rewrite/rewriteRemove.c,v 1.44 2001/03/22 03:59:44 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */


#include "postgres.h"

#include "utils/builtins.h"
#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/pg_rewrite.h"
#include "commands/comment.h"
#include "rewrite/rewriteRemove.h"
#include "rewrite/rewriteSupport.h"
#include "utils/fmgroids.h"
#include "utils/syscache.h"

/*-----------------------------------------------------------------------
 * RewriteGetRuleEventRel
 *-----------------------------------------------------------------------
 */
char *
RewriteGetRuleEventRel(char *rulename)
{
	HeapTuple	htup;
	Oid			eventrel;
	char	   *result;

	htup = SearchSysCache(RULENAME,
						  PointerGetDatum(rulename),
						  0, 0, 0);
	if (!HeapTupleIsValid(htup))
		elog(ERROR, "Rule or view \"%s\" not found",
			 ((strncmp(rulename, "_RET", 4) == 0) ? (rulename + 4) : rulename));
	eventrel = ((Form_pg_rewrite) GETSTRUCT(htup))->ev_class;
	ReleaseSysCache(htup);

	htup = SearchSysCache(RELOID,
						  PointerGetDatum(eventrel),
						  0, 0, 0);
	if (!HeapTupleIsValid(htup))
		elog(ERROR, "Relation %u not found", eventrel);

	result = pstrdup(NameStr(((Form_pg_class) GETSTRUCT(htup))->relname));
	ReleaseSysCache(htup);
	return result;
}

/*
 * RemoveRewriteRule
 *
 * Delete a rule given its rulename.
 */
void
RemoveRewriteRule(char *ruleName)
{
	Relation	RewriteRelation;
	Relation	event_relation;
	HeapTuple	tuple;
	Oid			ruleId;
	Oid			eventRelationOid;
	bool		hasMoreRules;

	/*
	 * Open the pg_rewrite relation.
	 */
	RewriteRelation = heap_openr(RewriteRelationName, RowExclusiveLock);

	/*
	 * Find the tuple for the target rule.
	 */
	tuple = SearchSysCacheCopy(RULENAME,
							   PointerGetDatum(ruleName),
							   0, 0, 0);

	/*
	 * complain if no rule with such name existed
	 */
	if (!HeapTupleIsValid(tuple))
	{
		heap_close(RewriteRelation, RowExclusiveLock);
		elog(ERROR, "Rule \"%s\" not found", ruleName);
	}

	/*
	 * Save the OID of the rule (i.e. the tuple's OID) and the event
	 * relation's OID
	 */
	ruleId = tuple->t_data->t_oid;
	eventRelationOid = ((Form_pg_rewrite) GETSTRUCT(tuple))->ev_class;

	/*
	 * We had better grab AccessExclusiveLock so that we know no other
	 * rule additions/deletions are going on for this relation.  Else we
	 * cannot set relhasrules correctly.  Besides, we don't want to be
	 * changing the ruleset while queries are executing on the rel.
	 */
	event_relation = heap_open(eventRelationOid, AccessExclusiveLock);

	/* do not allow the removal of a view's SELECT rule */
	if (event_relation->rd_rel->relkind == RELKIND_VIEW &&
		((Form_pg_rewrite) GETSTRUCT(tuple))->ev_type == '1')
		elog(ERROR, "Cannot remove a view's SELECT rule");

	hasMoreRules = event_relation->rd_rules != NULL &&
		event_relation->rd_rules->numLocks > 1;

	/*
	 * Delete any comments associated with this rule
	 */
	DeleteComments(ruleId);

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
	Relation	RewriteRelation = NULL;
	HeapScanDesc scanDesc = NULL;
	ScanKeyData scanKeyData;
	HeapTuple	tuple = NULL;

	/*
	 * Open the pg_rewrite relation.
	 */
	RewriteRelation = heap_openr(RewriteRelationName, RowExclusiveLock);

	/*
	 * Scan the RuleRelation ('pg_rewrite') for all the tuples that has
	 * the same ev_class as relid (the relation to be removed).
	 */
	ScanKeyEntryInitialize(&scanKeyData,
						   0,
						   Anum_pg_rewrite_ev_class,
						   F_OIDEQ,
						   ObjectIdGetDatum(relid));
	scanDesc = heap_beginscan(RewriteRelation,
							  0, SnapshotNow, 1, &scanKeyData);

	while (HeapTupleIsValid(tuple = heap_getnext(scanDesc, 0)))
	{

		/*** Delete any comments associated with this relation ***/

		DeleteComments(tuple->t_data->t_oid);

		simple_heap_delete(RewriteRelation, &tuple->t_self);
	}

	heap_endscan(scanDesc);
	heap_close(RewriteRelation, RowExclusiveLock);
}
