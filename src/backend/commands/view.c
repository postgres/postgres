/*-------------------------------------------------------------------------
 *
 * view.c
 *	  use rewrite rules to construct views
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/view.c,v 1.78 2003/09/25 06:57:59 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "commands/tablecmds.h"
#include "commands/view.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_relation.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteManip.h"
#include "rewrite/rewriteSupport.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"


static void checkViewTupleDesc(TupleDesc newdesc, TupleDesc olddesc);


/*---------------------------------------------------------------------
 * DefineVirtualRelation
 *
 * Create the "view" relation. `DefineRelation' does all the work,
 * we just provide the correct arguments ... at least when we're
 * creating a view.  If we're updating an existing view, we have to
 * work harder.
 *---------------------------------------------------------------------
 */
static Oid
DefineVirtualRelation(const RangeVar *relation, List *tlist, bool replace)
{
	Oid			viewOid,
				namespaceId;
	CreateStmt *createStmt = makeNode(CreateStmt);
	List	   *attrList,
			   *t;

	/*
	 * create a list of ColumnDef nodes based on the names and types of
	 * the (non-junk) targetlist items from the view's SELECT list.
	 */
	attrList = NIL;
	foreach(t, tlist)
	{
		TargetEntry *entry = lfirst(t);
		Resdom	   *res = entry->resdom;

		if (!res->resjunk)
		{
			ColumnDef  *def = makeNode(ColumnDef);
			TypeName   *typename = makeNode(TypeName);

			def->colname = pstrdup(res->resname);

			typename->typeid = res->restype;
			typename->typmod = res->restypmod;
			def->typename = typename;

			def->inhcount = 0;
			def->is_local = true;
			def->is_not_null = false;
			def->raw_default = NULL;
			def->cooked_default = NULL;
			def->constraints = NIL;
			def->support = NULL;

			attrList = lappend(attrList, def);
		}
	}

	if (attrList == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("view must have at least one column")));

	/*
	 * Check to see if we want to replace an existing view.
	 */
	namespaceId = RangeVarGetCreationNamespace(relation);
	viewOid = get_relname_relid(relation->relname, namespaceId);

	if (OidIsValid(viewOid) && replace)
	{
		Relation	rel;
		TupleDesc	descriptor;

		/*
		 * Yes.  Get exclusive lock on the existing view ...
		 */
		rel = relation_open(viewOid, AccessExclusiveLock);

		/*
		 * Make sure it *is* a view, and do permissions checks.
		 */
		if (rel->rd_rel->relkind != RELKIND_VIEW)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not a view",
							RelationGetRelationName(rel))));

		if (!pg_class_ownercheck(viewOid, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
						   RelationGetRelationName(rel));

		/*
		 * Create a tuple descriptor to compare against the existing view,
		 * and verify it matches.
		 */
		descriptor = BuildDescForRelation(attrList);
		checkViewTupleDesc(descriptor, rel->rd_att);

		/*
		 * Seems okay, so return the OID of the pre-existing view.
		 */
		relation_close(rel, NoLock);	/* keep the lock! */

		return viewOid;
	}
	else
	{
		/*
		 * now create the parameters for keys/inheritance etc. All of them
		 * are nil...
		 */
		createStmt->relation = (RangeVar *) relation;
		createStmt->tableElts = attrList;
		createStmt->inhRelations = NIL;
		createStmt->constraints = NIL;
		createStmt->hasoids = false;
		createStmt->oncommit = ONCOMMIT_NOOP;

		/*
		 * finally create the relation (this will error out if there's an
		 * existing view, so we don't need more code to complain if
		 * "replace" is false).
		 */
		return DefineRelation(createStmt, RELKIND_VIEW);
	}
}

/*
 * Verify that tupledesc associated with proposed new view definition
 * matches tupledesc of old view.  This is basically a cut-down version
 * of equalTupleDescs(), with code added to generate specific complaints.
 */
static void
checkViewTupleDesc(TupleDesc newdesc, TupleDesc olddesc)
{
	int			i;

	if (newdesc->natts != olddesc->natts)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot change number of columns in view")));
	/* we can ignore tdhasoid */

	for (i = 0; i < newdesc->natts; i++)
	{
		Form_pg_attribute newattr = newdesc->attrs[i];
		Form_pg_attribute oldattr = olddesc->attrs[i];

		/* XXX not right, but we don't support DROP COL on view anyway */
		if (newattr->attisdropped != oldattr->attisdropped)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot change number of columns in view")));

		if (strcmp(NameStr(newattr->attname), NameStr(oldattr->attname)) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot change name of view column \"%s\"",
							NameStr(oldattr->attname))));
		/* XXX would it be safe to allow atttypmod to change?  Not sure */
		if (newattr->atttypid != oldattr->atttypid ||
			newattr->atttypmod != oldattr->atttypmod)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				   errmsg("cannot change data type of view column \"%s\"",
						  NameStr(oldattr->attname))));
		/* We can ignore the remaining attributes of an attribute... */
	}

	/*
	 * We ignore the constraint fields.  The new view desc can't have any
	 * constraints, and the only ones that could be on the old view are
	 * defaults, which we are happy to leave in place.
	 */
}

static RuleStmt *
FormViewRetrieveRule(const RangeVar *view, Query *viewParse, bool replace)
{
	RuleStmt   *rule;

	/*
	 * Create a RuleStmt that corresponds to the suitable rewrite rule
	 * args for DefineQueryRewrite();
	 */
	rule = makeNode(RuleStmt);
	rule->relation = copyObject((RangeVar *) view);
	rule->rulename = pstrdup(ViewSelectRuleName);
	rule->whereClause = NULL;
	rule->event = CMD_SELECT;
	rule->instead = true;
	rule->actions = makeList1(viewParse);
	rule->replace = replace;

	return rule;
}

static void
DefineViewRules(const RangeVar *view, Query *viewParse, bool replace)
{
	RuleStmt   *retrieve_rule;

#ifdef NOTYET
	RuleStmt   *replace_rule;
	RuleStmt   *append_rule;
	RuleStmt   *delete_rule;
#endif

	retrieve_rule = FormViewRetrieveRule(view, viewParse, replace);

#ifdef NOTYET
	replace_rule = FormViewReplaceRule(view, viewParse);
	append_rule = FormViewAppendRule(view, viewParse);
	delete_rule = FormViewDeleteRule(view, viewParse);
#endif

	DefineQueryRewrite(retrieve_rule);

#ifdef NOTYET
	DefineQueryRewrite(replace_rule);
	DefineQueryRewrite(append_rule);
	DefineQueryRewrite(delete_rule);
#endif

}

/*---------------------------------------------------------------
 * UpdateRangeTableOfViewParse
 *
 * Update the range table of the given parsetree.
 * This update consists of adding two new entries IN THE BEGINNING
 * of the range table (otherwise the rule system will die a slow,
 * horrible and painful death, and we do not want that now, do we?)
 * one for the OLD relation and one for the NEW one (both of
 * them refer in fact to the "view" relation).
 *
 * Of course we must also increase the 'varnos' of all the Var nodes
 * by 2...
 *
 * These extra RT entries are not actually used in the query,
 * except for run-time permission checking.
 *---------------------------------------------------------------
 */
static Query *
UpdateRangeTableOfViewParse(Oid viewOid, Query *viewParse)
{
	List	   *new_rt;
	RangeTblEntry *rt_entry1,
			   *rt_entry2;

	/*
	 * Make a copy of the given parsetree.	It's not so much that we don't
	 * want to scribble on our input, it's that the parser has a bad habit
	 * of outputting multiple links to the same subtree for constructs
	 * like BETWEEN, and we mustn't have OffsetVarNodes increment the
	 * varno of a Var node twice.  copyObject will expand any
	 * multiply-referenced subtree into multiple copies.
	 */
	viewParse = (Query *) copyObject(viewParse);

	/*
	 * Create the 2 new range table entries and form the new range
	 * table... OLD first, then NEW....
	 */
	rt_entry1 = addRangeTableEntryForRelation(NULL, viewOid,
											  makeAlias("*OLD*", NIL),
											  false, false);
	rt_entry2 = addRangeTableEntryForRelation(NULL, viewOid,
											  makeAlias("*NEW*", NIL),
											  false, false);
	/* Must override addRangeTableEntry's default access-check flags */
	rt_entry1->checkForRead = false;
	rt_entry2->checkForRead = false;

	new_rt = lcons(rt_entry1, lcons(rt_entry2, viewParse->rtable));

	viewParse->rtable = new_rt;

	/*
	 * Now offset all var nodes by 2, and jointree RT indexes too.
	 */
	OffsetVarNodes((Node *) viewParse, 2, 0);

	return viewParse;
}

/*-------------------------------------------------------------------
 * DefineView
 *
 *		- takes a "viewname", "parsetree" pair and then
 *		1)		construct the "virtual" relation
 *		2)		commit the command but NOT the transaction,
 *				so that the relation exists
 *				before the rules are defined.
 *		2)		define the "n" rules specified in the PRS2 paper
 *				over the "virtual" relation
 *-------------------------------------------------------------------
 */
void
DefineView(const RangeVar *view, Query *viewParse, bool replace)
{
	Oid			viewOid;

	/*
	 * Create the view relation
	 *
	 * NOTE: if it already exists and replace is false, the xact will be
	 * aborted.
	 */

	viewOid = DefineVirtualRelation(view, viewParse->targetList, replace);

	/*
	 * The relation we have just created is not visible to any other
	 * commands running with the same transaction & command id. So,
	 * increment the command id counter (but do NOT pfree any memory!!!!)
	 */
	CommandCounterIncrement();

	/*
	 * The range table of 'viewParse' does not contain entries for the
	 * "OLD" and "NEW" relations. So... add them!
	 */
	viewParse = UpdateRangeTableOfViewParse(viewOid, viewParse);

	/*
	 * Now create the rules associated with the view.
	 */
	DefineViewRules(view, viewParse, replace);
}

/*
 * RemoveView
 *
 * Remove a view given its name
 *
 * We just have to drop the relation; the associated rules will be
 * cleaned up automatically.
 */
void
RemoveView(const RangeVar *view, DropBehavior behavior)
{
	Oid			viewOid;
	ObjectAddress object;

	viewOid = RangeVarGetRelid(view, false);

	object.classId = RelOid_pg_class;
	object.objectId = viewOid;
	object.objectSubId = 0;

	performDeletion(&object, behavior);
}
