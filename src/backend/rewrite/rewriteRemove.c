/*-------------------------------------------------------------------------
 *
 * rewriteRemove.c
 *	  routines for removing rewrite rules
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/rewrite/rewriteRemove.c,v 1.36 2000/04/12 17:15:32 momjian Exp $
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

	htup = SearchSysCacheTuple(RULENAME,
							   PointerGetDatum(rulename),
							   0, 0, 0);
	if (!HeapTupleIsValid(htup))
		elog(ERROR, "Rule or view '%s' not found",
		  ((!strncmp(rulename, "_RET", 4)) ? (rulename + 4) : rulename));
	eventrel = ((Form_pg_rewrite) GETSTRUCT(htup))->ev_class;
	htup = SearchSysCacheTuple(RELOID,
							   PointerGetDatum(eventrel),
							   0, 0, 0);
	if (!HeapTupleIsValid(htup))
		elog(ERROR, "Class '%u' not found", eventrel);

	return NameStr(((Form_pg_class) GETSTRUCT(htup))->relname);
}

/* ----------------------------------------------------------------
 *
 * RemoveRewriteRule
 *
 * Delete a rule given its rulename.
 *
 * There are three steps.
 *	 1) Find the corresponding tuple in 'pg_rewrite' relation.
 *		Find the rule Id (i.e. the Oid of the tuple) and finally delete
 *		the tuple.
 *	 3) Delete the locks from the 'pg_class' relation.
 *
 *
 * ----------------------------------------------------------------
 */
void
RemoveRewriteRule(char *ruleName)
{
	Relation	RewriteRelation = NULL;
	HeapTuple	tuple = NULL;
	Oid			ruleId = (Oid) 0;
	Oid			eventRelationOid = (Oid) NULL;
	Datum		eventRelationOidDatum = (Datum) NULL;
	bool		isNull = false;

	/*
	 * Open the pg_rewrite relation.
	 */
	RewriteRelation = heap_openr(RewriteRelationName, RowExclusiveLock);

	/*
	 * Scan the RuleRelation ('pg_rewrite') until we find a tuple
	 */
	tuple = SearchSysCacheTupleCopy(RULENAME,
									PointerGetDatum(ruleName),
									0, 0, 0);

	/*
	 * complain if no rule with such name existed
	 */
	if (!HeapTupleIsValid(tuple))
	{
		heap_close(RewriteRelation, RowExclusiveLock);
		elog(ERROR, "Rule '%s' not found\n", ruleName);
	}

	/*
	 * Store the OID of the rule (i.e. the tuple's OID) and the event
	 * relation's OID
	 */
	ruleId = tuple->t_data->t_oid;
	eventRelationOidDatum = heap_getattr(tuple,
										 Anum_pg_rewrite_ev_class,
									   RelationGetDescr(RewriteRelation),
										 &isNull);
	if (isNull)
	{
		/* XXX strange!!! */
		heap_freetuple(tuple);
		elog(ERROR, "RemoveRewriteRule: internal error; null event target relation!");
	}
	eventRelationOid = DatumGetObjectId(eventRelationOidDatum);

	/*
	 * Now delete the relation level locks from the updated relation.
	 * (Make sure we do this before we remove the rule from pg_rewrite.
	 * Otherwise, heap_openr on eventRelationOid which reads pg_rwrite for
	 * the rules will fail.)
	 */
	prs2_deleteFromRelation(eventRelationOid, ruleId);

	/*
	 * Delete any comments associated with this rule
	 *
	 */

	DeleteComments(ruleId);

	/*
	 * Now delete the tuple...
	 */
	heap_delete(RewriteRelation, &tuple->t_self, NULL);

	heap_freetuple(tuple);
	heap_close(RewriteRelation, RowExclusiveLock);
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

		heap_delete(RewriteRelation, &tuple->t_self, NULL);

	}

	heap_endscan(scanDesc);
	heap_close(RewriteRelation, RowExclusiveLock);
}
