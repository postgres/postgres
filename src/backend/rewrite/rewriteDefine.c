/*-------------------------------------------------------------------------
 *
 * rewriteDefine.c
 *	  routines for defining a rewrite rule
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/rewrite/rewriteDefine.c,v 1.52 2000/09/12 20:38:09 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "utils/builtins.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_rewrite.h"
#include "parser/parse_relation.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteSupport.h"
#include "utils/syscache.h"
#include "storage/smgr.h"
#include "commands/view.h"


/*
 * InsertRule -
 *	  takes the arguments and inserts them as attributes into the system
 *	  relation "pg_rewrite"
 */
static Oid
InsertRule(char *rulname,
		   int evtype,
		   Oid eventrel_oid,
		   AttrNumber evslot_index,
		   bool evinstead,
		   char *evqual,
		   char *actiontree)
{
	int			i;
	Datum		values[Natts_pg_rewrite];
	char		nulls[Natts_pg_rewrite];
	NameData	rname;
	Relation	pg_rewrite_desc;
	TupleDesc	tupDesc;
	HeapTuple	tup;
	Oid			rewriteObjectId;

	if (IsDefinedRewriteRule(rulname))
		elog(ERROR, "Attempt to insert rule '%s' failed: already exists",
			 rulname);

	/* ----------------
	 *	Set up *nulls and *values arrays
	 * ----------------
	 */
	MemSet(nulls, ' ', sizeof(nulls));

	i = 0;
	namestrcpy(&rname, rulname);
	values[i++] = NameGetDatum(&rname);
	values[i++] = CharGetDatum(evtype + '0');
	values[i++] = ObjectIdGetDatum(eventrel_oid);
	values[i++] = Int16GetDatum(evslot_index);
	values[i++] = BoolGetDatum(evinstead);
	values[i++] = DirectFunctionCall1(textin, CStringGetDatum(evqual));
	values[i++] = DirectFunctionCall1(textin, CStringGetDatum(actiontree));

	/* ----------------
	 *	create a new pg_rewrite tuple
	 * ----------------
	 */
	pg_rewrite_desc = heap_openr(RewriteRelationName, RowExclusiveLock);

	tupDesc = pg_rewrite_desc->rd_att;

	tup = heap_formtuple(tupDesc,
						 values,
						 nulls);

	heap_insert(pg_rewrite_desc, tup);

	rewriteObjectId = tup->t_data->t_oid;

	if (RelationGetForm(pg_rewrite_desc)->relhasindex)
	{
		Relation	idescs[Num_pg_rewrite_indices];

		CatalogOpenIndices(Num_pg_rewrite_indices, Name_pg_rewrite_indices,
						   idescs);
		CatalogIndexInsert(idescs, Num_pg_rewrite_indices, pg_rewrite_desc,
						   tup);
		CatalogCloseIndices(Num_pg_rewrite_indices, idescs);
	}

	heap_freetuple(tup);

	heap_close(pg_rewrite_desc, RowExclusiveLock);

	return rewriteObjectId;
}

/*
 *		for now, event_object must be a single attribute
 */
static void
ValidateRule(int event_type,
			 char *eobj_string,
			 char *eslot_string,
			 Node *event_qual,
			 List **action,
			 int is_instead,
			 Oid event_attype)
{
	if (((event_type == CMD_INSERT) || (event_type == CMD_DELETE)) &&
		eslot_string)
	{
		elog(ERROR,
		"rules not allowed for insert or delete events to an attribute");
	}

#ifdef NOT_USED

	/*
	 * on retrieve to class.attribute do instead nothing is converted to
	 * 'on retrieve to class.attribute do instead retrieve (attribute =
	 * NULL)' --- this is also a terrible hack that works well -- glass
	 */
	if (is_instead && !*action && eslot_string && event_type == CMD_SELECT)
	{
		char	   *temp_buffer = (char *) palloc(strlen(template) + 80);

		sprintf(temp_buffer, template, event_attype,
				get_typlen(event_attype), eslot_string,
				event_attype);

		*action = (List *) stringToNode(temp_buffer);

		pfree(temp_buffer);
	}
#endif
}

void
DefineQueryRewrite(RuleStmt *stmt)
{
	CmdType		event_type = stmt->event;
	Attr	   *event_obj = stmt->object;
	Node	   *event_qual = stmt->whereClause;
	bool		is_instead = stmt->instead;
	List	   *action = stmt->actions;
	Relation	event_relation;
	Oid			ev_relid;
	Oid			ruleId;
	char	   *eslot_string = NULL;
	int			event_attno;
	Oid			event_attype;
	char	   *actionP,
			   *event_qualP;
	List	   *l;
	Query	   *query;
	bool		RelisBecomingView = false;

	/*
	 * If we are installing an ON SELECT rule, we had better grab
	 * AccessExclusiveLock to ensure no SELECTs are currently running on
	 * the event relation.  For other types of rules, it might be sufficient
	 * to grab ShareLock to lock out insert/update/delete actions.  But
	 * for now, let's just grab AccessExclusiveLock all the time.
	 */
	event_relation = heap_openr(event_obj->relname, AccessExclusiveLock);
	ev_relid = RelationGetRelid(event_relation);

	/* ----------
	 * The current rewrite handler is known to work on relation level
	 * rules only. And for SELECT events, it expects one non-nothing
	 * action that is instead and returns exactly a tuple of the
	 * rewritten relation. This restricts SELECT rules to views.
	 *
	 *	   Jan
	 * ----------
	 */
	if (event_obj->attrs)
		elog(ERROR, "attribute level rules currently not supported");

	/*
	 * eslot_string = strVal(lfirst(event_obj->attrs));
	 */
	else
		eslot_string = NULL;

	/*
	 * No rule actions that modify OLD or NEW
	 */
	foreach(l, action)
	{
		query = (Query *) lfirst(l);
		if (query->resultRelation == PRS2_OLD_VARNO)
		{
			elog(ERROR, "rule actions on OLD currently not supported"
				 "\n\tuse views or triggers instead");
		}
		if (query->resultRelation == PRS2_NEW_VARNO)
		{
			elog(ERROR, "rule actions on NEW currently not supported"
				 "\n\tuse triggers instead");
		}
	}

	/*
	 * Rules ON SELECT are restricted to view definitions
	 */
	if (event_type == CMD_SELECT)
	{
		TargetEntry *tle;
		Resdom	   *resdom;
		Form_pg_attribute attr;
		char	   *attname;
		int			i;
		char		*expected_name;

		/*
		 * So there cannot be INSTEAD NOTHING, ...
		 */
		if (length(action) == 0)
		{
			elog(ERROR, "instead nothing rules on select currently not supported"
				 "\n\tuse views instead");
		}

		/*
		 * ... there cannot be multiple actions, ...
		 */
		if (length(action) > 1)
			elog(ERROR, "multiple action rules on select currently not supported");

		/*
		 * ... the one action must be a SELECT, ...
		 */
		query = (Query *) lfirst(action);
		if (!is_instead || query->commandType != CMD_SELECT)
			elog(ERROR, "only instead-select rules currently supported on select");
		if (event_qual != NULL)
			elog(ERROR, "event qualifications not supported for rules on select");

		/*
		 * ... the targetlist of the SELECT action must exactly match the
		 * event relation, ...
		 */
		if (event_relation->rd_att->natts != length(query->targetList))
			elog(ERROR, "select rules target list must match event relations structure");

		for (i = 1; i <= event_relation->rd_att->natts; i++)
		{
			tle = (TargetEntry *) nth(i - 1, query->targetList);
			resdom = tle->resdom;
			attr = event_relation->rd_att->attrs[i - 1];
			attname = NameStr(attr->attname);

			if (strcmp(resdom->resname, attname) != 0)
				elog(ERROR, "select rules target entry %d has different column name from %s", i, attname);

			if (attr->atttypid != resdom->restype)
				elog(ERROR, "select rules target entry %d has different type from attribute %s", i, attname);

			if (attr->atttypmod != resdom->restypmod)
				elog(ERROR, "select rules target entry %d has different size from attribute %s", i, attname);
		}

		/*
		 * ... there must not be another ON SELECT rule already ...
		 */
		if (event_relation->rd_rules != NULL)
		{
			for (i = 0; i < event_relation->rd_rules->numLocks; i++)
			{
				RewriteRule *rule;

				rule = event_relation->rd_rules->rules[i];
				if (rule->event == CMD_SELECT)
					elog(ERROR, "%s is already a view",
						 RelationGetRelationName(event_relation));
			}
		}

		/*
		 * LIMIT in view is not supported
		 */
		if (query->limitOffset != NULL || query->limitCount != NULL)
			elog(ERROR, "LIMIT clause not supported in views");

		/*
		 * DISTINCT on view is not supported
		 */
		if (query->distinctClause != NIL)
			elog(ERROR, "DISTINCT not supported in views");

		/*
		 * ORDER BY in view is not supported
		 */
		if (query->sortClause != NIL)
			elog(ERROR, "ORDER BY not supported in views");

		/*
		 * ... and finally the rule must be named _RETviewname.
		 */
		expected_name = MakeRetrieveViewRuleName(event_obj->relname);
		if (strcmp(expected_name, stmt->rulename) != 0)
		{
			elog(ERROR, "view rule for %s must be named %s",
				 event_obj->relname, expected_name);
		}
		pfree(expected_name);

		/*
		 * Are we converting a relation to a view?
		 *
		 * If so, check that the relation is empty because the storage
		 * for the relation is going to be deleted.
		 */
		if (event_relation->rd_rel->relkind != RELKIND_VIEW)
		{
			HeapScanDesc  scanDesc;
			HeapTuple	  tuple;

			scanDesc = heap_beginscan(event_relation, 0, SnapshotNow, 0, NULL);
			tuple = heap_getnext(scanDesc, 0);
			if (HeapTupleIsValid(tuple))
				elog(ERROR, "Relation \"%s\" is not empty. Cannot convert it to view",
					 event_obj->relname);

			/* don't need heap_freetuple because we never got a valid tuple */
			heap_endscan(scanDesc);

			RelisBecomingView = true;
		}
	}

	/*
	 * This rule is allowed - install it.
	 */
	if (eslot_string == NULL)
	{
		event_attno = -1;
		event_attype = InvalidOid;
	}
	else
	{
		event_attno = attnameAttNum(event_relation, eslot_string);
		event_attype = attnumTypeId(event_relation, event_attno);
	}

	/* fix bug about instead nothing */
	ValidateRule(event_type, event_obj->relname,
				 eslot_string, event_qual, &action,
				 is_instead, event_attype);

	/* discard rule if it's null action and not INSTEAD; it's a no-op */
	if (action != NIL || is_instead)
	{
		Relation	relationRelation;
		HeapTuple	tuple;
		Relation	idescs[Num_pg_class_indices];

		event_qualP = nodeToString(event_qual);
		actionP = nodeToString(action);

		ruleId = InsertRule(stmt->rulename,
							event_type,
							ev_relid,
							event_attno,
							is_instead,
							event_qualP,
							actionP);

		/*
		 * Set pg_class 'relhasrules' field TRUE for event relation.
		 * If appropriate, also modify the 'relkind' field to show that
		 * the relation is now a view.
		 *
		 * Important side effect: an SI notice is broadcast to force all
		 * backends (including me!) to update relcache entries with the new
		 * rule.
		 *
		 * NOTE : Used to call setRelhasrulesInRelation. The code
		 * was inlined so that two updates were not needed. mhh 31-aug-2000
		 */

		/*
		 * Find the tuple to update in pg_class, using syscache for the lookup.
		 */
		relationRelation = heap_openr(RelationRelationName, RowExclusiveLock);
		tuple = SearchSysCacheTupleCopy(RELOID,
										ObjectIdGetDatum(ev_relid),
										0, 0, 0);
		Assert(HeapTupleIsValid(tuple));

		/* Do the update */
		((Form_pg_class) GETSTRUCT(tuple))->relhasrules = true;
		if (RelisBecomingView)
			((Form_pg_class) GETSTRUCT(tuple))->relkind = RELKIND_VIEW;

		heap_update(relationRelation, &tuple->t_self, tuple, NULL);

		/* Keep the catalog indices up to date */
		CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, idescs);
		CatalogIndexInsert(idescs, Num_pg_class_indices, relationRelation, tuple);
		CatalogCloseIndices(Num_pg_class_indices, idescs);

		heap_freetuple(tuple);
		heap_close(relationRelation, RowExclusiveLock);
	}

	/*
	 * IF the relation is becoming a view, delete the storage
	 * files associated with it.  NB: we had better have AccessExclusiveLock
	 * to do this ...
	 */
	if (RelisBecomingView)
		smgrunlink(DEFAULT_SMGR, event_relation);

	/* Close rel, but keep lock till commit... */
	heap_close(event_relation, NoLock);
}
