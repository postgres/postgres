/*-------------------------------------------------------------------------
 *
 * rewriteSupport.c
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/rewrite/rewriteSupport.c,v 1.50 2002/04/18 20:01:09 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "rewrite/rewriteSupport.h"
#include "utils/syscache.h"

#ifdef MULTIBYTE
#include "mb/pg_wchar.h"
#endif


/*
 * Is there a rule by the given name?
 */
bool
IsDefinedRewriteRule(Oid owningRel, const char *ruleName)
{
	return SearchSysCacheExists(RULERELNAME,
								ObjectIdGetDatum(owningRel),
								PointerGetDatum(ruleName),
								0, 0);
}

/*
 * makeViewRetrieveRuleName
 *
 * Given a view name, returns the name for the associated ON SELECT rule.
 *
 * XXX this is not the only place in the backend that knows about the _RET
 * name-forming convention.
 */
char *
MakeRetrieveViewRuleName(const char *viewName)
{
	char	   *buf;
	int			buflen,
				maxlen;

	buflen = strlen(viewName) + 5;
	buf = palloc(buflen);
	snprintf(buf, buflen, "_RET%s", viewName);
	/* clip to less than NAMEDATALEN bytes, if necessary */
#ifdef MULTIBYTE
	maxlen = pg_mbcliplen(buf, strlen(buf), NAMEDATALEN - 1);
#else
	maxlen = NAMEDATALEN - 1;
#endif
	if (maxlen < buflen)
		buf[maxlen] = '\0';

	return buf;
}

/*
 * SetRelationRuleStatus
 *		Set the value of the relation's relhasrules field in pg_class;
 *		if the relation is becoming a view, also adjust its relkind.
 *
 * NOTE: caller must be holding an appropriate lock on the relation.
 *
 * NOTE: an important side-effect of this operation is that an SI invalidation
 * message is sent out to all backends --- including me --- causing relcache
 * entries to be flushed or updated with the new set of rules for the table.
 * Therefore, we execute the update even if relhasrules has the right value
 * already.  Possible future improvement: skip the disk update and just send
 * an SI message in that case.
 */
void
SetRelationRuleStatus(Oid relationId, bool relHasRules,
					  bool relIsBecomingView)
{
	Relation	relationRelation;
	HeapTuple	tuple;
	Relation	idescs[Num_pg_class_indices];

	/*
	 * Find the tuple to update in pg_class, using syscache for the
	 * lookup.
	 */
	relationRelation = heap_openr(RelationRelationName, RowExclusiveLock);
	tuple = SearchSysCacheCopy(RELOID,
							   ObjectIdGetDatum(relationId),
							   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "SetRelationRuleStatus: cache lookup failed for relation %u", relationId);

	/* Do the update */
	((Form_pg_class) GETSTRUCT(tuple))->relhasrules = relHasRules;
	if (relIsBecomingView)
		((Form_pg_class) GETSTRUCT(tuple))->relkind = RELKIND_VIEW;

	simple_heap_update(relationRelation, &tuple->t_self, tuple);

	/* Keep the catalog indices up to date */
	CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, idescs);
	CatalogIndexInsert(idescs, Num_pg_class_indices, relationRelation, tuple);
	CatalogCloseIndices(Num_pg_class_indices, idescs);

	heap_freetuple(tuple);
	heap_close(relationRelation, RowExclusiveLock);
}
