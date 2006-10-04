/*-------------------------------------------------------------------------
 *
 * rewriteDefine.c
 *	  routines for defining a rewrite rule
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/rewrite/rewriteDefine.c,v 1.115 2006/10/04 00:29:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_rewrite.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "parser/parse_expr.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteManip.h"
#include "rewrite/rewriteSupport.h"
#include "storage/smgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static void checkRuleResultList(List *targetList, TupleDesc resultDesc,
					bool isSelect);
static bool setRuleCheckAsUser_walker(Node *node, Oid *context);
static void setRuleCheckAsUser_Query(Query *qry, Oid userid);


/*
 * InsertRule -
 *	  takes the arguments and inserts them as a row into the system
 *	  relation "pg_rewrite"
 */
static Oid
InsertRule(char *rulname,
		   int evtype,
		   Oid eventrel_oid,
		   AttrNumber evslot_index,
		   bool evinstead,
		   Node *event_qual,
		   List *action,
		   bool replace)
{
	char	   *evqual = nodeToString(event_qual);
	char	   *actiontree = nodeToString((Node *) action);
	int			i;
	Datum		values[Natts_pg_rewrite];
	char		nulls[Natts_pg_rewrite];
	char		replaces[Natts_pg_rewrite];
	NameData	rname;
	Relation	pg_rewrite_desc;
	HeapTuple	tup,
				oldtup;
	Oid			rewriteObjectId;
	ObjectAddress myself,
				referenced;
	bool		is_update = false;

	/*
	 * Set up *nulls and *values arrays
	 */
	MemSet(nulls, ' ', sizeof(nulls));

	i = 0;
	namestrcpy(&rname, rulname);
	values[i++] = NameGetDatum(&rname); /* rulename */
	values[i++] = ObjectIdGetDatum(eventrel_oid);		/* ev_class */
	values[i++] = Int16GetDatum(evslot_index);	/* ev_attr */
	values[i++] = CharGetDatum(evtype + '0');	/* ev_type */
	values[i++] = BoolGetDatum(evinstead);		/* is_instead */
	values[i++] = DirectFunctionCall1(textin, CStringGetDatum(evqual)); /* ev_qual */
	values[i++] = DirectFunctionCall1(textin, CStringGetDatum(actiontree));		/* ev_action */

	/*
	 * Ready to store new pg_rewrite tuple
	 */
	pg_rewrite_desc = heap_open(RewriteRelationId, RowExclusiveLock);

	/*
	 * Check to see if we are replacing an existing tuple
	 */
	oldtup = SearchSysCache(RULERELNAME,
							ObjectIdGetDatum(eventrel_oid),
							PointerGetDatum(rulname),
							0, 0);

	if (HeapTupleIsValid(oldtup))
	{
		if (!replace)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("rule \"%s\" for relation \"%s\" already exists",
							rulname, get_rel_name(eventrel_oid))));

		/*
		 * When replacing, we don't need to replace every attribute
		 */
		MemSet(replaces, ' ', sizeof(replaces));
		replaces[Anum_pg_rewrite_ev_attr - 1] = 'r';
		replaces[Anum_pg_rewrite_ev_type - 1] = 'r';
		replaces[Anum_pg_rewrite_is_instead - 1] = 'r';
		replaces[Anum_pg_rewrite_ev_qual - 1] = 'r';
		replaces[Anum_pg_rewrite_ev_action - 1] = 'r';

		tup = heap_modifytuple(oldtup, RelationGetDescr(pg_rewrite_desc),
							   values, nulls, replaces);

		simple_heap_update(pg_rewrite_desc, &tup->t_self, tup);

		ReleaseSysCache(oldtup);

		rewriteObjectId = HeapTupleGetOid(tup);
		is_update = true;
	}
	else
	{
		tup = heap_formtuple(pg_rewrite_desc->rd_att, values, nulls);

		rewriteObjectId = simple_heap_insert(pg_rewrite_desc, tup);
	}

	/* Need to update indexes in either case */
	CatalogUpdateIndexes(pg_rewrite_desc, tup);

	heap_freetuple(tup);

	/* If replacing, get rid of old dependencies and make new ones */
	if (is_update)
		deleteDependencyRecordsFor(RewriteRelationId, rewriteObjectId);

	/*
	 * Install dependency on rule's relation to ensure it will go away on
	 * relation deletion.  If the rule is ON SELECT, make the dependency
	 * implicit --- this prevents deleting a view's SELECT rule.  Other kinds
	 * of rules can be AUTO.
	 */
	myself.classId = RewriteRelationId;
	myself.objectId = rewriteObjectId;
	myself.objectSubId = 0;

	referenced.classId = RelationRelationId;
	referenced.objectId = eventrel_oid;
	referenced.objectSubId = 0;

	recordDependencyOn(&myself, &referenced,
			 (evtype == CMD_SELECT) ? DEPENDENCY_INTERNAL : DEPENDENCY_AUTO);

	/*
	 * Also install dependencies on objects referenced in action and qual.
	 */
	recordDependencyOnExpr(&myself, (Node *) action, NIL,
						   DEPENDENCY_NORMAL);

	if (event_qual != NULL)
	{
		/* Find query containing OLD/NEW rtable entries */
		Query	   *qry = (Query *) linitial(action);

		qry = getInsertSelectQuery(qry, NULL);
		recordDependencyOnExpr(&myself, event_qual, qry->rtable,
							   DEPENDENCY_NORMAL);
	}

	heap_close(pg_rewrite_desc, RowExclusiveLock);

	return rewriteObjectId;
}

void
DefineQueryRewrite(RuleStmt *stmt)
{
	RangeVar   *event_obj = stmt->relation;
	Node	   *event_qual = stmt->whereClause;
	CmdType		event_type = stmt->event;
	bool		is_instead = stmt->instead;
	bool		replace = stmt->replace;
	List	   *action = stmt->actions;
	Relation	event_relation;
	Oid			ev_relid;
	Oid			ruleId;
	int			event_attno;
	ListCell   *l;
	Query	   *query;
	bool		RelisBecomingView = false;

	/*
	 * If we are installing an ON SELECT rule, we had better grab
	 * AccessExclusiveLock to ensure no SELECTs are currently running on the
	 * event relation.	For other types of rules, it might be sufficient to
	 * grab ShareLock to lock out insert/update/delete actions.  But for now,
	 * let's just grab AccessExclusiveLock all the time.
	 */
	event_relation = heap_openrv(event_obj, AccessExclusiveLock);
	ev_relid = RelationGetRelid(event_relation);

	/*
	 * Check user has permission to apply rules to this relation.
	 */
	if (!pg_class_ownercheck(ev_relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   RelationGetRelationName(event_relation));

	/*
	 * No rule actions that modify OLD or NEW
	 */
	foreach(l, action)
	{
		query = (Query *) lfirst(l);
		if (query->resultRelation == 0)
			continue;
		/* Don't be fooled by INSERT/SELECT */
		if (query != getInsertSelectQuery(query, NULL))
			continue;
		if (query->resultRelation == PRS2_OLD_VARNO)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("rule actions on OLD are not implemented"),
					 errhint("Use views or triggers instead.")));
		if (query->resultRelation == PRS2_NEW_VARNO)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("rule actions on NEW are not implemented"),
					 errhint("Use triggers instead.")));
	}

	if (event_type == CMD_SELECT)
	{
		/*
		 * Rules ON SELECT are restricted to view definitions
		 *
		 * So there cannot be INSTEAD NOTHING, ...
		 */
		if (list_length(action) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			   errmsg("INSTEAD NOTHING rules on SELECT are not implemented"),
					 errhint("Use views instead.")));

		/*
		 * ... there cannot be multiple actions, ...
		 */
		if (list_length(action) > 1)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("multiple actions for rules on SELECT are not implemented")));

		/*
		 * ... the one action must be a SELECT, ...
		 */
		query = (Query *) linitial(action);
		if (!is_instead ||
			query->commandType != CMD_SELECT || query->into != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("rules on SELECT must have action INSTEAD SELECT")));

		/*
		 * ... there can be no rule qual, ...
		 */
		if (event_qual != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("event qualifications are not implemented for rules on SELECT")));

		/*
		 * ... the targetlist of the SELECT action must exactly match the
		 * event relation, ...
		 */
		checkRuleResultList(query->targetList,
							RelationGetDescr(event_relation),
							true);

		/*
		 * ... there must not be another ON SELECT rule already ...
		 */
		if (!replace && event_relation->rd_rules != NULL)
		{
			int			i;

			for (i = 0; i < event_relation->rd_rules->numLocks; i++)
			{
				RewriteRule *rule;

				rule = event_relation->rd_rules->rules[i];
				if (rule->event == CMD_SELECT)
					ereport(ERROR,
						  (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						   errmsg("\"%s\" is already a view",
								  RelationGetRelationName(event_relation))));
			}
		}

		/*
		 * ... and finally the rule must be named _RETURN.
		 */
		if (strcmp(stmt->rulename, ViewSelectRuleName) != 0)
		{
			/*
			 * In versions before 7.3, the expected name was _RETviewname. For
			 * backwards compatibility with old pg_dump output, accept that
			 * and silently change it to _RETURN.  Since this is just a quick
			 * backwards-compatibility hack, limit the number of characters
			 * checked to a few less than NAMEDATALEN; this saves having to
			 * worry about where a multibyte character might have gotten
			 * truncated.
			 */
			if (strncmp(stmt->rulename, "_RET", 4) != 0 ||
				strncmp(stmt->rulename + 4, event_obj->relname,
						NAMEDATALEN - 4 - 4) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("view rule for \"%s\" must be named \"%s\"",
								event_obj->relname, ViewSelectRuleName)));
			stmt->rulename = pstrdup(ViewSelectRuleName);
		}

		/*
		 * Are we converting a relation to a view?
		 *
		 * If so, check that the relation is empty because the storage for the
		 * relation is going to be deleted.  Also insist that the rel not have
		 * any triggers, indexes, or child tables.
		 */
		if (event_relation->rd_rel->relkind != RELKIND_VIEW)
		{
			HeapScanDesc scanDesc;

			scanDesc = heap_beginscan(event_relation, SnapshotNow, 0, NULL);
			if (heap_getnext(scanDesc, ForwardScanDirection) != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("could not convert table \"%s\" to a view because it is not empty",
								event_obj->relname)));
			heap_endscan(scanDesc);

			if (event_relation->rd_rel->reltriggers != 0)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("could not convert table \"%s\" to a view because it has triggers",
								event_obj->relname),
						 errhint("In particular, the table may not be involved in any foreign key relationships.")));

			if (event_relation->rd_rel->relhasindex)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("could not convert table \"%s\" to a view because it has indexes",
								event_obj->relname)));

			if (event_relation->rd_rel->relhassubclass)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("could not convert table \"%s\" to a view because it has child tables",
								event_obj->relname)));

			RelisBecomingView = true;
		}
	}
	else
	{
		/*
		 * For non-SELECT rules, a RETURNING list can appear in at most one of
		 * the actions ... and there can't be any RETURNING list at all in a
		 * conditional or non-INSTEAD rule.  (Actually, there can be at most
		 * one RETURNING list across all rules on the same event, but it seems
		 * best to enforce that at rule expansion time.)  If there is a
		 * RETURNING list, it must match the event relation.
		 */
		bool		haveReturning = false;

		foreach(l, action)
		{
			query = (Query *) lfirst(l);

			if (!query->returningList)
				continue;
			if (haveReturning)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				  errmsg("cannot have multiple RETURNING lists in a rule")));
			haveReturning = true;
			if (event_qual != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("RETURNING lists are not supported in conditional rules")));
			if (!is_instead)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("RETURNING lists are not supported in non-INSTEAD rules")));
			checkRuleResultList(query->returningList,
								RelationGetDescr(event_relation),
								false);
		}
	}

	/*
	 * This rule is allowed - prepare to install it.
	 */
	event_attno = -1;

	/* discard rule if it's null action and not INSTEAD; it's a no-op */
	if (action != NIL || is_instead)
	{
		ruleId = InsertRule(stmt->rulename,
							event_type,
							ev_relid,
							event_attno,
							is_instead,
							event_qual,
							action,
							replace);

		/*
		 * Set pg_class 'relhasrules' field TRUE for event relation. If
		 * appropriate, also modify the 'relkind' field to show that the
		 * relation is now a view.
		 *
		 * Important side effect: an SI notice is broadcast to force all
		 * backends (including me!) to update relcache entries with the new
		 * rule.
		 */
		SetRelationRuleStatus(ev_relid, true, RelisBecomingView);
	}

	/*
	 * IF the relation is becoming a view, delete the storage files associated
	 * with it.  NB: we had better have AccessExclusiveLock to do this ...
	 *
	 * XXX what about getting rid of its TOAST table?  For now, we don't.
	 */
	if (RelisBecomingView)
	{
		RelationOpenSmgr(event_relation);
		smgrscheduleunlink(event_relation->rd_smgr, event_relation->rd_istemp);
	}

	/* Close rel, but keep lock till commit... */
	heap_close(event_relation, NoLock);
}

/*
 * checkRuleResultList
 *		Verify that targetList produces output compatible with a tupledesc
 *
 * The targetList might be either a SELECT targetlist, or a RETURNING list;
 * isSelect tells which.  (This is mostly used for choosing error messages,
 * but also we don't enforce column name matching for RETURNING.)
 */
static void
checkRuleResultList(List *targetList, TupleDesc resultDesc, bool isSelect)
{
	ListCell   *tllist;
	int			i;

	i = 0;
	foreach(tllist, targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(tllist);
		int32		tletypmod;
		Form_pg_attribute attr;
		char	   *attname;

		/* resjunk entries may be ignored */
		if (tle->resjunk)
			continue;
		i++;
		if (i > resultDesc->natts)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 isSelect ?
				   errmsg("SELECT rule's target list has too many entries") :
					 errmsg("RETURNING list has too many entries")));

		attr = resultDesc->attrs[i - 1];
		attname = NameStr(attr->attname);

		/*
		 * Disallow dropped columns in the relation.  This won't happen in the
		 * cases we actually care about (namely creating a view via CREATE
		 * TABLE then CREATE RULE, or adding a RETURNING rule to a view).
		 * Trying to cope with it is much more trouble than it's worth,
		 * because we'd have to modify the rule to insert dummy NULLs at the
		 * right positions.
		 */
		if (attr->attisdropped)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot convert relation containing dropped columns to view")));

		if (isSelect && strcmp(tle->resname, attname) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("SELECT rule's target entry %d has different column name from \"%s\"", i, attname)));

		if (attr->atttypid != exprType((Node *) tle->expr))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 isSelect ?
					 errmsg("SELECT rule's target entry %d has different type from column \"%s\"",
							i, attname) :
					 errmsg("RETURNING list's entry %d has different type from column \"%s\"",
							i, attname)));

		/*
		 * Allow typmods to be different only if one of them is -1, ie,
		 * "unspecified".  This is necessary for cases like "numeric", where
		 * the table will have a filled-in default length but the select
		 * rule's expression will probably have typmod = -1.
		 */
		tletypmod = exprTypmod((Node *) tle->expr);
		if (attr->atttypmod != tletypmod &&
			attr->atttypmod != -1 && tletypmod != -1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 isSelect ?
					 errmsg("SELECT rule's target entry %d has different size from column \"%s\"",
							i, attname) :
					 errmsg("RETURNING list's entry %d has different size from column \"%s\"",
							i, attname)));
	}

	if (i != resultDesc->natts)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 isSelect ?
				 errmsg("SELECT rule's target list has too few entries") :
				 errmsg("RETURNING list has too few entries")));
}

/*
 * setRuleCheckAsUser
 *		Recursively scan a query or expression tree and set the checkAsUser
 *		field to the given userid in all rtable entries.
 *
 * Note: for a view (ON SELECT rule), the checkAsUser field of the *OLD*
 * RTE entry will be overridden when the view rule is expanded, and the
 * checkAsUser field of the *NEW* entry is irrelevant because that entry's
 * requiredPerms bits will always be zero.	However, for other types of rules
 * it's important to set these fields to match the rule owner.  So we just set
 * them always.
 */
void
setRuleCheckAsUser(Node *node, Oid userid)
{
	(void) setRuleCheckAsUser_walker(node, &userid);
}

static bool
setRuleCheckAsUser_walker(Node *node, Oid *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Query))
	{
		setRuleCheckAsUser_Query((Query *) node, *context);
		return false;
	}
	return expression_tree_walker(node, setRuleCheckAsUser_walker,
								  (void *) context);
}

static void
setRuleCheckAsUser_Query(Query *qry, Oid userid)
{
	ListCell   *l;

	/* Set all the RTEs in this query node */
	foreach(l, qry->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);

		if (rte->rtekind == RTE_SUBQUERY)
		{
			/* Recurse into subquery in FROM */
			setRuleCheckAsUser_Query(rte->subquery, userid);
		}
		else
			rte->checkAsUser = userid;
	}

	/* If there are sublinks, search for them and process their RTEs */
	/* ignore subqueries in rtable because we already processed them */
	if (qry->hasSubLinks)
		query_tree_walker(qry, setRuleCheckAsUser_walker, (void *) &userid,
						  QTW_IGNORE_RT_SUBQUERIES);
}


/*
 * Rename an existing rewrite rule.
 *
 * This is unused code at the moment.
 */
#ifdef NOT_USED
void
RenameRewriteRule(Oid owningRel, const char *oldName,
				  const char *newName)
{
	Relation	pg_rewrite_desc;
	HeapTuple	ruletup;

	pg_rewrite_desc = heap_open(RewriteRelationId, RowExclusiveLock);

	ruletup = SearchSysCacheCopy(RULERELNAME,
								 ObjectIdGetDatum(owningRel),
								 PointerGetDatum(oldName),
								 0, 0);
	if (!HeapTupleIsValid(ruletup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("rule \"%s\" for relation \"%s\" does not exist",
						oldName, get_rel_name(owningRel))));

	/* should not already exist */
	if (IsDefinedRewriteRule(owningRel, newName))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("rule \"%s\" for relation \"%s\" already exists",
						newName, get_rel_name(owningRel))));

	namestrcpy(&(((Form_pg_rewrite) GETSTRUCT(ruletup))->rulename), newName);

	simple_heap_update(pg_rewrite_desc, &ruletup->t_self, ruletup);

	/* keep system catalog indexes current */
	CatalogUpdateIndexes(pg_rewrite_desc, ruletup);

	heap_freetuple(ruletup);
	heap_close(pg_rewrite_desc, RowExclusiveLock);
}

#endif
