/*-------------------------------------------------------------------------
 *
 * rewriteDefine.c
 *	  routines for defining a rewrite rule
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/rewrite/rewriteDefine.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_rewrite.h"
#include "catalog/storage.h"
#include "commands/policy.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_utilcmd.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteManip.h"
#include "rewrite/rewriteSupport.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tqual.h"


static void checkRuleResultList(List *targetList, TupleDesc resultDesc,
					bool isSelect, bool requireColumnNameMatch);
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
		   bool evinstead,
		   Node *event_qual,
		   List *action,
		   bool replace)
{
	char	   *evqual = nodeToString(event_qual);
	char	   *actiontree = nodeToString((Node *) action);
	Datum		values[Natts_pg_rewrite];
	bool		nulls[Natts_pg_rewrite];
	bool		replaces[Natts_pg_rewrite];
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
	MemSet(nulls, false, sizeof(nulls));

	namestrcpy(&rname, rulname);
	values[Anum_pg_rewrite_rulename - 1] = NameGetDatum(&rname);
	values[Anum_pg_rewrite_ev_class - 1] = ObjectIdGetDatum(eventrel_oid);
	values[Anum_pg_rewrite_ev_type - 1] = CharGetDatum(evtype + '0');
	values[Anum_pg_rewrite_ev_enabled - 1] = CharGetDatum(RULE_FIRES_ON_ORIGIN);
	values[Anum_pg_rewrite_is_instead - 1] = BoolGetDatum(evinstead);
	values[Anum_pg_rewrite_ev_qual - 1] = CStringGetTextDatum(evqual);
	values[Anum_pg_rewrite_ev_action - 1] = CStringGetTextDatum(actiontree);

	/*
	 * Ready to store new pg_rewrite tuple
	 */
	pg_rewrite_desc = heap_open(RewriteRelationId, RowExclusiveLock);

	/*
	 * Check to see if we are replacing an existing tuple
	 */
	oldtup = SearchSysCache2(RULERELNAME,
							 ObjectIdGetDatum(eventrel_oid),
							 PointerGetDatum(rulname));

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
		MemSet(replaces, false, sizeof(replaces));
		replaces[Anum_pg_rewrite_ev_type - 1] = true;
		replaces[Anum_pg_rewrite_is_instead - 1] = true;
		replaces[Anum_pg_rewrite_ev_qual - 1] = true;
		replaces[Anum_pg_rewrite_ev_action - 1] = true;

		tup = heap_modify_tuple(oldtup, RelationGetDescr(pg_rewrite_desc),
								values, nulls, replaces);

		simple_heap_update(pg_rewrite_desc, &tup->t_self, tup);

		ReleaseSysCache(oldtup);

		rewriteObjectId = HeapTupleGetOid(tup);
		is_update = true;
	}
	else
	{
		tup = heap_form_tuple(pg_rewrite_desc->rd_att, values, nulls);

		rewriteObjectId = simple_heap_insert(pg_rewrite_desc, tup);
	}

	/* Need to update indexes in either case */
	CatalogUpdateIndexes(pg_rewrite_desc, tup);

	heap_freetuple(tup);

	/* If replacing, get rid of old dependencies and make new ones */
	if (is_update)
		deleteDependencyRecordsFor(RewriteRelationId, rewriteObjectId, false);

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

	/* Post creation hook for new rule */
	InvokeObjectPostCreateHook(RewriteRelationId, rewriteObjectId, 0);

	heap_close(pg_rewrite_desc, RowExclusiveLock);

	return rewriteObjectId;
}

/*
 * DefineRule
 *		Execute a CREATE RULE command.
 */
ObjectAddress
DefineRule(RuleStmt *stmt, const char *queryString)
{
	List	   *actions;
	Node	   *whereClause;
	Oid			relId;

	/* Parse analysis. */
	transformRuleStmt(stmt, queryString, &actions, &whereClause);

	/*
	 * Find and lock the relation.  Lock level should match
	 * DefineQueryRewrite.
	 */
	relId = RangeVarGetRelid(stmt->relation, AccessExclusiveLock, false);

	/* ... and execute */
	return DefineQueryRewrite(stmt->rulename,
							  relId,
							  whereClause,
							  stmt->event,
							  stmt->instead,
							  stmt->replace,
							  actions);
}


/*
 * DefineQueryRewrite
 *		Create a rule
 *
 * This is essentially the same as DefineRule() except that the rule's
 * action and qual have already been passed through parse analysis.
 */
ObjectAddress
DefineQueryRewrite(char *rulename,
				   Oid event_relid,
				   Node *event_qual,
				   CmdType event_type,
				   bool is_instead,
				   bool replace,
				   List *action)
{
	Relation	event_relation;
	ListCell   *l;
	Query	   *query;
	bool		RelisBecomingView = false;
	Oid			ruleId = InvalidOid;
	ObjectAddress address;

	/*
	 * If we are installing an ON SELECT rule, we had better grab
	 * AccessExclusiveLock to ensure no SELECTs are currently running on the
	 * event relation. For other types of rules, it would be sufficient to
	 * grab ShareRowExclusiveLock to lock out insert/update/delete actions and
	 * to ensure that we lock out current CREATE RULE statements; but because
	 * of race conditions in access to catalog entries, we can't do that yet.
	 *
	 * Note that this lock level should match the one used in DefineRule.
	 */
	event_relation = heap_open(event_relid, AccessExclusiveLock);

	/*
	 * Verify relation is of a type that rules can sensibly be applied to.
	 * Internal callers can target materialized views, but transformRuleStmt()
	 * blocks them for users.  Don't mention them in the error message.
	 */
	if (event_relation->rd_rel->relkind != RELKIND_RELATION &&
		event_relation->rd_rel->relkind != RELKIND_MATVIEW &&
		event_relation->rd_rel->relkind != RELKIND_VIEW)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table or view",
						RelationGetRelationName(event_relation))));

	if (!allowSystemTableMods && IsSystemRelation(event_relation))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(event_relation))));

	/*
	 * Check user has permission to apply rules to this relation.
	 */
	if (!pg_class_ownercheck(event_relid, GetUserId()))
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
			query->commandType != CMD_SELECT ||
			query->utilityStmt != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("rules on SELECT must have action INSTEAD SELECT")));

		/*
		 * ... it cannot contain data-modifying WITH ...
		 */
		if (query->hasModifyingCTE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("rules on SELECT must not contain data-modifying statements in WITH")));

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
							true,
							event_relation->rd_rel->relkind !=
							RELKIND_MATVIEW);

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
		if (strcmp(rulename, ViewSelectRuleName) != 0)
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
			if (strncmp(rulename, "_RET", 4) != 0 ||
				strncmp(rulename + 4, RelationGetRelationName(event_relation),
						NAMEDATALEN - 4 - 4) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("view rule for \"%s\" must be named \"%s\"",
								RelationGetRelationName(event_relation),
								ViewSelectRuleName)));
			rulename = pstrdup(ViewSelectRuleName);
		}

		/*
		 * Are we converting a relation to a view?
		 *
		 * If so, check that the relation is empty because the storage for the
		 * relation is going to be deleted.  Also insist that the rel not have
		 * any triggers, indexes, child tables, policies, or RLS enabled.
		 * (Note: these tests are too strict, because they will reject
		 * relations that once had such but don't anymore.  But we don't
		 * really care, because this whole business of converting relations to
		 * views is just a kluge to allow dump/reload of views that
		 * participate in circular dependencies.)
		 */
		if (event_relation->rd_rel->relkind != RELKIND_VIEW &&
			event_relation->rd_rel->relkind != RELKIND_MATVIEW)
		{
			HeapScanDesc scanDesc;
			Snapshot	snapshot;

			snapshot = RegisterSnapshot(GetLatestSnapshot());
			scanDesc = heap_beginscan(event_relation, snapshot, 0, NULL);
			if (heap_getnext(scanDesc, ForwardScanDirection) != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("could not convert table \"%s\" to a view because it is not empty",
								RelationGetRelationName(event_relation))));
			heap_endscan(scanDesc);
			UnregisterSnapshot(snapshot);

			if (event_relation->rd_rel->relhastriggers)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("could not convert table \"%s\" to a view because it has triggers",
								RelationGetRelationName(event_relation)),
						 errhint("In particular, the table cannot be involved in any foreign key relationships.")));

			if (event_relation->rd_rel->relhasindex)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("could not convert table \"%s\" to a view because it has indexes",
								RelationGetRelationName(event_relation))));

			if (event_relation->rd_rel->relhassubclass)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("could not convert table \"%s\" to a view because it has child tables",
								RelationGetRelationName(event_relation))));

			if (event_relation->rd_rel->relrowsecurity)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("could not convert table \"%s\" to a view because it has row security enabled",
								RelationGetRelationName(event_relation))));

			if (relation_has_policies(event_relation))
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("could not convert table \"%s\" to a view because it has row security policies",
								RelationGetRelationName(event_relation))));

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
								false, false);
		}
	}

	/*
	 * This rule is allowed - prepare to install it.
	 */

	/* discard rule if it's null action and not INSTEAD; it's a no-op */
	if (action != NIL || is_instead)
	{
		ruleId = InsertRule(rulename,
							event_type,
							event_relid,
							is_instead,
							event_qual,
							action,
							replace);

		/*
		 * Set pg_class 'relhasrules' field TRUE for event relation.
		 *
		 * Important side effect: an SI notice is broadcast to force all
		 * backends (including me!) to update relcache entries with the new
		 * rule.
		 */
		SetRelationRuleStatus(event_relid, true);
	}

	/* ---------------------------------------------------------------------
	 * If the relation is becoming a view:
	 * - delete the associated storage files
	 * - get rid of any system attributes in pg_attribute; a view shouldn't
	 *	 have any of those
	 * - remove the toast table; there is no need for it anymore, and its
	 *	 presence would make vacuum slightly more complicated
	 * - set relkind to RELKIND_VIEW, and adjust other pg_class fields
	 *	 to be appropriate for a view
	 *
	 * NB: we had better have AccessExclusiveLock to do this ...
	 * ---------------------------------------------------------------------
	 */
	if (RelisBecomingView)
	{
		Relation	relationRelation;
		Oid			toastrelid;
		HeapTuple	classTup;
		Form_pg_class classForm;

		relationRelation = heap_open(RelationRelationId, RowExclusiveLock);
		toastrelid = event_relation->rd_rel->reltoastrelid;

		/* drop storage while table still looks like a table  */
		RelationDropStorage(event_relation);
		DeleteSystemAttributeTuples(event_relid);

		/*
		 * Drop the toast table if any.  (This won't take care of updating the
		 * toast fields in the relation's own pg_class entry; we handle that
		 * below.)
		 */
		if (OidIsValid(toastrelid))
		{
			ObjectAddress toastobject;

			/*
			 * Delete the dependency of the toast relation on the main
			 * relation so we can drop the former without dropping the latter.
			 */
			deleteDependencyRecordsFor(RelationRelationId, toastrelid,
									   false);

			/* Make deletion of dependency record visible */
			CommandCounterIncrement();

			/* Now drop toast table, including its index */
			toastobject.classId = RelationRelationId;
			toastobject.objectId = toastrelid;
			toastobject.objectSubId = 0;
			performDeletion(&toastobject, DROP_RESTRICT,
							PERFORM_DELETION_INTERNAL);
		}

		/*
		 * SetRelationRuleStatus may have updated the pg_class row, so we must
		 * advance the command counter before trying to update it again.
		 */
		CommandCounterIncrement();

		/*
		 * Fix pg_class entry to look like a normal view's, including setting
		 * the correct relkind and removal of reltoastrelid of the toast table
		 * we potentially removed above.
		 */
		classTup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(event_relid));
		if (!HeapTupleIsValid(classTup))
			elog(ERROR, "cache lookup failed for relation %u", event_relid);
		classForm = (Form_pg_class) GETSTRUCT(classTup);

		classForm->reltablespace = InvalidOid;
		classForm->relpages = 0;
		classForm->reltuples = 0;
		classForm->relallvisible = 0;
		classForm->reltoastrelid = InvalidOid;
		classForm->relhasindex = false;
		classForm->relkind = RELKIND_VIEW;
		classForm->relhasoids = false;
		classForm->relhaspkey = false;
		classForm->relfrozenxid = InvalidTransactionId;
		classForm->relminmxid = InvalidMultiXactId;
		classForm->relreplident = REPLICA_IDENTITY_NOTHING;

		simple_heap_update(relationRelation, &classTup->t_self, classTup);
		CatalogUpdateIndexes(relationRelation, classTup);

		heap_freetuple(classTup);
		heap_close(relationRelation, RowExclusiveLock);
	}

	ObjectAddressSet(address, RewriteRelationId, ruleId);

	/* Close rel, but keep lock till commit... */
	heap_close(event_relation, NoLock);

	return address;
}

/*
 * checkRuleResultList
 *		Verify that targetList produces output compatible with a tupledesc
 *
 * The targetList might be either a SELECT targetlist, or a RETURNING list;
 * isSelect tells which.  This is used for choosing error messages.
 *
 * A SELECT targetlist may optionally require that column names match.
 */
static void
checkRuleResultList(List *targetList, TupleDesc resultDesc, bool isSelect,
					bool requireColumnNameMatch)
{
	ListCell   *tllist;
	int			i;

	/* Only a SELECT may require a column name match. */
	Assert(isSelect || !requireColumnNameMatch);

	i = 0;
	foreach(tllist, targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(tllist);
		Oid			tletypid;
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
		 * Disallow dropped columns in the relation.  This is not really
		 * expected to happen when creating an ON SELECT rule.  It'd be
		 * possible if someone tried to convert a relation with dropped
		 * columns to a view, but the only case we care about supporting
		 * table-to-view conversion for is pg_dump, and pg_dump won't do that.
		 *
		 * Unfortunately, the situation is also possible when adding a rule
		 * with RETURNING to a regular table, and rejecting that case is
		 * altogether more annoying.  In principle we could support it by
		 * modifying the targetlist to include dummy NULL columns
		 * corresponding to the dropped columns in the tupdesc.  However,
		 * places like ruleutils.c would have to be fixed to not process such
		 * entries, and that would take an uncertain and possibly rather large
		 * amount of work.  (Note we could not dodge that by marking the dummy
		 * columns resjunk, since it's precisely the non-resjunk tlist columns
		 * that are expected to correspond to table columns.)
		 */
		if (attr->attisdropped)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 isSelect ?
					 errmsg("cannot convert relation containing dropped columns to view") :
					 errmsg("cannot create a RETURNING list for a relation containing dropped columns")));

		/* Check name match if required; no need for two error texts here */
		if (requireColumnNameMatch && strcmp(tle->resname, attname) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("SELECT rule's target entry %d has different column name from column \"%s\"",
							i, attname),
					 errdetail("SELECT target entry is named \"%s\".",
							   tle->resname)));

		/* Check type match. */
		tletypid = exprType((Node *) tle->expr);
		if (attr->atttypid != tletypid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 isSelect ?
					 errmsg("SELECT rule's target entry %d has different type from column \"%s\"",
							i, attname) :
					 errmsg("RETURNING list's entry %d has different type from column \"%s\"",
							i, attname),
					 isSelect ?
					 errdetail("SELECT target entry has type %s, but column has type %s.",
							   format_type_be(tletypid),
							   format_type_be(attr->atttypid)) :
					 errdetail("RETURNING list entry has type %s, but column has type %s.",
							   format_type_be(tletypid),
							   format_type_be(attr->atttypid))));

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
							i, attname),
					 isSelect ?
					 errdetail("SELECT target entry has type %s, but column has type %s.",
							   format_type_with_typemod(tletypid, tletypmod),
							   format_type_with_typemod(attr->atttypid,
														attr->atttypmod)) :
					 errdetail("RETURNING list entry has type %s, but column has type %s.",
							   format_type_with_typemod(tletypid, tletypmod),
							   format_type_with_typemod(attr->atttypid,
														attr->atttypmod))));
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
 * Note: for a view (ON SELECT rule), the checkAsUser field of the OLD
 * RTE entry will be overridden when the view rule is expanded, and the
 * checkAsUser field of the NEW entry is irrelevant because that entry's
 * requiredPerms bits will always be zero.  However, for other types of rules
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

	/* Recurse into subquery-in-WITH */
	foreach(l, qry->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(l);

		setRuleCheckAsUser_Query((Query *) cte->ctequery, userid);
	}

	/* If there are sublinks, search for them and process their RTEs */
	if (qry->hasSubLinks)
		query_tree_walker(qry, setRuleCheckAsUser_walker, (void *) &userid,
						  QTW_IGNORE_RC_SUBQUERIES);
}


/*
 * Change the firing semantics of an existing rule.
 */
void
EnableDisableRule(Relation rel, const char *rulename,
				  char fires_when)
{
	Relation	pg_rewrite_desc;
	Oid			owningRel = RelationGetRelid(rel);
	Oid			eventRelationOid;
	HeapTuple	ruletup;
	bool		changed = false;

	/*
	 * Find the rule tuple to change.
	 */
	pg_rewrite_desc = heap_open(RewriteRelationId, RowExclusiveLock);
	ruletup = SearchSysCacheCopy2(RULERELNAME,
								  ObjectIdGetDatum(owningRel),
								  PointerGetDatum(rulename));
	if (!HeapTupleIsValid(ruletup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("rule \"%s\" for relation \"%s\" does not exist",
						rulename, get_rel_name(owningRel))));

	/*
	 * Verify that the user has appropriate permissions.
	 */
	eventRelationOid = ((Form_pg_rewrite) GETSTRUCT(ruletup))->ev_class;
	Assert(eventRelationOid == owningRel);
	if (!pg_class_ownercheck(eventRelationOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   get_rel_name(eventRelationOid));

	/*
	 * Change ev_enabled if it is different from the desired new state.
	 */
	if (DatumGetChar(((Form_pg_rewrite) GETSTRUCT(ruletup))->ev_enabled) !=
		fires_when)
	{
		((Form_pg_rewrite) GETSTRUCT(ruletup))->ev_enabled =
			CharGetDatum(fires_when);
		simple_heap_update(pg_rewrite_desc, &ruletup->t_self, ruletup);

		/* keep system catalog indexes current */
		CatalogUpdateIndexes(pg_rewrite_desc, ruletup);

		changed = true;
	}

	InvokeObjectPostAlterHook(RewriteRelationId,
							  HeapTupleGetOid(ruletup), 0);

	heap_freetuple(ruletup);
	heap_close(pg_rewrite_desc, RowExclusiveLock);

	/*
	 * If we changed anything, broadcast a SI inval message to force each
	 * backend (including our own!) to rebuild relation's relcache entry.
	 * Otherwise they will fail to apply the change promptly.
	 */
	if (changed)
		CacheInvalidateRelcache(rel);
}


/*
 * Perform permissions and integrity checks before acquiring a relation lock.
 */
static void
RangeVarCallbackForRenameRule(const RangeVar *rv, Oid relid, Oid oldrelid,
							  void *arg)
{
	HeapTuple	tuple;
	Form_pg_class form;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		return;					/* concurrently dropped */
	form = (Form_pg_class) GETSTRUCT(tuple);

	/* only tables and views can have rules */
	if (form->relkind != RELKIND_RELATION && form->relkind != RELKIND_VIEW)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table or view", rv->relname)));

	if (!allowSystemTableMods && IsSystemClass(relid, form))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						rv->relname)));

	/* you must own the table to rename one of its rules */
	if (!pg_class_ownercheck(relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS, rv->relname);

	ReleaseSysCache(tuple);
}

/*
 * Rename an existing rewrite rule.
 */
ObjectAddress
RenameRewriteRule(RangeVar *relation, const char *oldName,
				  const char *newName)
{
	Oid			relid;
	Relation	targetrel;
	Relation	pg_rewrite_desc;
	HeapTuple	ruletup;
	Form_pg_rewrite ruleform;
	Oid			ruleOid;
	ObjectAddress address;

	/*
	 * Look up name, check permissions, and acquire lock (which we will NOT
	 * release until end of transaction).
	 */
	relid = RangeVarGetRelidExtended(relation, AccessExclusiveLock,
									 false, false,
									 RangeVarCallbackForRenameRule,
									 NULL);

	/* Have lock already, so just need to build relcache entry. */
	targetrel = relation_open(relid, NoLock);

	/* Prepare to modify pg_rewrite */
	pg_rewrite_desc = heap_open(RewriteRelationId, RowExclusiveLock);

	/* Fetch the rule's entry (it had better exist) */
	ruletup = SearchSysCacheCopy2(RULERELNAME,
								  ObjectIdGetDatum(relid),
								  PointerGetDatum(oldName));
	if (!HeapTupleIsValid(ruletup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("rule \"%s\" for relation \"%s\" does not exist",
						oldName, RelationGetRelationName(targetrel))));
	ruleform = (Form_pg_rewrite) GETSTRUCT(ruletup);
	ruleOid = HeapTupleGetOid(ruletup);

	/* rule with the new name should not already exist */
	if (IsDefinedRewriteRule(relid, newName))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("rule \"%s\" for relation \"%s\" already exists",
						newName, RelationGetRelationName(targetrel))));

	/*
	 * We disallow renaming ON SELECT rules, because they should always be
	 * named "_RETURN".
	 */
	if (ruleform->ev_type == CMD_SELECT + '0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("renaming an ON SELECT rule is not allowed")));

	/* OK, do the update */
	namestrcpy(&(ruleform->rulename), newName);

	simple_heap_update(pg_rewrite_desc, &ruletup->t_self, ruletup);

	/* keep system catalog indexes current */
	CatalogUpdateIndexes(pg_rewrite_desc, ruletup);

	heap_freetuple(ruletup);
	heap_close(pg_rewrite_desc, RowExclusiveLock);

	/*
	 * Invalidate relation's relcache entry so that other backends (and this
	 * one too!) are sent SI message to make them rebuild relcache entries.
	 * (Ideally this should happen automatically...)
	 */
	CacheInvalidateRelcache(targetrel);

	ObjectAddressSet(address, RewriteRelationId, ruleOid);

	/*
	 * Close rel, but keep exclusive lock!
	 */
	relation_close(targetrel, NoLock);

	return address;
}
