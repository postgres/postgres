/*-------------------------------------------------------------------------
 *
 * rewriteRemove.c--
 *	  routines for removing rewrite rules
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/rewrite/rewriteRemove.c,v 1.4 1997/09/08 02:28:20 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/skey.h"
#include "catalog/pg_rewrite.h"
#include "catalog/catname.h"	/* for RewriteRelationName */
#include "utils/syscache.h"
#include "utils/elog.h"			/* for elog stuff */
#include "utils/palloc.h"
#include "utils/tqual.h"		/* 'NowTimeQual' defined here.. */
#include "access/heapam.h"		/* heap AM calls defined here */
#include "fmgr.h"				/* for CHAR_16_EQ */

#include "rewrite/rewriteRemove.h"		/* where the decls go */
#include "rewrite/rewriteSupport.h"

/*-----------------------------------------------------------------------
 * RewriteGetRuleEventRel
 *-----------------------------------------------------------------------
 */
char	   *
RewriteGetRuleEventRel(char *rulename)
{
	HeapTuple	htp;
	Oid			eventrel;

	htp = SearchSysCacheTuple(REWRITENAME, PointerGetDatum(rulename),
							  0, 0, 0);
	if (!HeapTupleIsValid(htp))
		elog(WARN, "RewriteGetRuleEventRel: rule \"%s\" not found",
			 rulename);
	eventrel = ((Form_pg_rewrite) GETSTRUCT(htp))->ev_class;
	htp = SearchSysCacheTuple(RELOID, PointerGetDatum(eventrel),
							  0, 0, 0);
	if (!HeapTupleIsValid(htp))
		elog(WARN, "RewriteGetRuleEventRel: class %d not found",
			 eventrel);
	return ((Form_pg_class) GETSTRUCT(htp))->relname.data;
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
	HeapScanDesc scanDesc = NULL;
	ScanKeyData scanKeyData;
	HeapTuple	tuple = NULL;
	Oid			ruleId = (Oid) 0;
	Oid			eventRelationOid = (Oid) NULL;
	Datum		eventRelationOidDatum = (Datum) NULL;
	Buffer		buffer = (Buffer) NULL;
	bool		isNull = false;

	/*
	 * Open the pg_rewrite relation.
	 */
	RewriteRelation = heap_openr(RewriteRelationName);

	/*
	 * Scan the RuleRelation ('pg_rewrite') until we find a tuple
	 */
	ScanKeyEntryInitialize(&scanKeyData, 0, Anum_pg_rewrite_rulename,
						   F_CHAR16EQ, NameGetDatum(ruleName));
	scanDesc = heap_beginscan(RewriteRelation,
							  0, NowTimeQual, 1, &scanKeyData);

	tuple = heap_getnext(scanDesc, 0, (Buffer *) NULL);

	/*
	 * complain if no rule with such name existed
	 */
	if (!HeapTupleIsValid(tuple))
	{
		heap_close(RewriteRelation);
		elog(WARN, "No rule with name = '%s' was found.\n", ruleName);
	}

	/*
	 * Store the OID of the rule (i.e. the tuple's OID) and the event
	 * relation's OID
	 */
	ruleId = tuple->t_oid;
	eventRelationOidDatum =
		PointerGetDatum(heap_getattr(tuple,
									 buffer,
									 Anum_pg_rewrite_ev_class,
							 RelationGetTupleDescriptor(RewriteRelation),
									 &isNull));
	if (isNull)
	{
		/* XXX strange!!! */
		elog(WARN, "RemoveRewriteRule: null event target relation!");
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
	 * Now delete the tuple...
	 */
	heap_delete(RewriteRelation, &(tuple->t_ctid));
	heap_close(RewriteRelation);
	heap_endscan(scanDesc);
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
	RewriteRelation = heap_openr(RewriteRelationName);

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
							  0, NowTimeQual, 1, &scanKeyData);

	for (;;)
	{
		tuple = heap_getnext(scanDesc, 0, (Buffer *) NULL);

		if (!HeapTupleIsValid(tuple))
		{
			break;				/* we're done */
		}

		/*
		 * delete the tuple...
		 */
		heap_delete(RewriteRelation, &(tuple->t_ctid));
	}

	heap_endscan(scanDesc);
	heap_close(RewriteRelation);
}
