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
 *	  $Header: /cvsroot/pgsql/src/backend/rewrite/rewriteDefine.c,v 1.47 2000/06/28 03:31:56 tgl Exp $
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


/*
 * InsertRule -
 *	  takes the arguments and inserts them as attributes into the system
 *	  relation "pg_rewrite"
 *
 *		ARGS :	rulname			-		name of the rule
 *				evtype			-		one of RETRIEVE,REPLACE,DELETE,APPEND
 *				evobj			-		name of relation
 *				evslot			-		comma delimited list of slots
 *										if null => multi-attr rule
 *				evinstead		-		is an instead rule
 *				actiontree		-		parsetree(s) of rule action
 */
static Oid
InsertRule(char *rulname,
		   int evtype,
		   char *evobj,
		   char *evslot,
		   char *evqual,
		   bool evinstead,
		   char *actiontree)
{
	Relation	eventrel;
	Oid			eventrel_oid;
	AttrNumber	evslot_index;
	int			i;
	Datum		values[Natts_pg_rewrite];
	char		nulls[Natts_pg_rewrite];
	NameData	rname;
	Relation	pg_rewrite_desc;
	TupleDesc	tupDesc;
	HeapTuple	tup;
	Oid			rewriteObjectId;

	eventrel = heap_openr(evobj, AccessShareLock);
	eventrel_oid = RelationGetRelid(eventrel);

	/*
	 * if the slotname is null, we know that this is a multi-attr rule
	 */
	if (evslot == NULL)
		evslot_index = -1;
	else
		evslot_index = attnameAttNum(eventrel, evslot);
	heap_close(eventrel, AccessShareLock);

	if (evqual == NULL)
		evqual = "<>";

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
	values[i++] = PointerGetDatum(lztextin(evqual));
	values[i++] = PointerGetDatum(lztextin(actiontree));

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
	Relation	event_relation = NULL;
	Oid			ruleId;
	Oid			ev_relid = 0;
	char	   *eslot_string = NULL;
	int			event_attno = 0;
	Oid			event_attype = 0;
	char	   *actionP,
			   *event_qualP;
	List	   *l;
	Query	   *query;

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
	if (action != NIL)
		foreach(l, action)
	{
		query = (Query *) lfirst(l);
		if (query->resultRelation == 1)
		{
			elog(NOTICE, "rule actions on OLD currently not supported");
			elog(ERROR, " use views or triggers instead");
		}
		if (query->resultRelation == 2)
		{
			elog(NOTICE, "rule actions on NEW currently not supported");
			elog(ERROR, " use triggers instead");
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
		char		expected_name[NAMEDATALEN + 5];

		/*
		 * So there cannot be INSTEAD NOTHING, ...
		 */
		if (length(action) == 0)
		{
			elog(NOTICE, "instead nothing rules on select currently not supported");
			elog(ERROR, " use views instead");
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
		event_relation = heap_openr(event_obj->relname, AccessShareLock);

		if (event_relation->rd_att->natts != length(query->targetList))
			elog(ERROR, "select rules target list must match event relations structure");

		for (i = 1; i <= event_relation->rd_att->natts; i++)
		{
			tle = (TargetEntry *) nth(i - 1, query->targetList);
			resdom = tle->resdom;
			attr = event_relation->rd_att->attrs[i - 1];
			attname = pstrdup(NameStr(attr->attname));

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

		heap_close(event_relation, AccessShareLock);

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
		sprintf(expected_name, "_RET%s", event_obj->relname);
		if (strcmp(expected_name, stmt->rulename) != 0)
		{
			elog(ERROR, "view rule for %s must be named %s",
				 event_obj->relname, expected_name);
		}
	}

	/*
	 * This rule is allowed - install it.
	 */

	event_relation = heap_openr(event_obj->relname, AccessShareLock);
	ev_relid = RelationGetRelid(event_relation);

	if (eslot_string == NULL)
	{
		event_attno = -1;
		event_attype = -1;		/* XXX - don't care */
	}
	else
	{
		event_attno = attnameAttNum(event_relation, eslot_string);
		event_attype = attnumTypeId(event_relation, event_attno);
	}
	heap_close(event_relation, AccessShareLock);

	/* fix bug about instead nothing */
	ValidateRule(event_type, event_obj->relname,
				 eslot_string, event_qual, &action,
				 is_instead, event_attype);

	if (action == NULL)
	{
		if (!is_instead)
			return;				/* doesn't do anything */

		event_qualP = nodeToString(event_qual);

		ruleId = InsertRule(stmt->rulename,
							event_type,
							event_obj->relname,
							eslot_string,
							event_qualP,
							true,
							"<>");
		prs2_addToRelation(ev_relid, ruleId, event_type, event_attno, TRUE,
						   event_qual, NIL);

	}
	else
	{
		event_qualP = nodeToString(event_qual);
		actionP = nodeToString(action);

		ruleId = InsertRule(stmt->rulename,
							event_type,
							event_obj->relname,
							eslot_string,
							event_qualP,
							is_instead,
							actionP);

		/* what is the max size of type text? XXX -- glass */
		if (length(action) > 15)
			elog(ERROR, "max # of actions exceeded");
		prs2_addToRelation(ev_relid, ruleId, event_type, event_attno,
						   is_instead, event_qual, action);
	}
}
