/*-------------------------------------------------------------------------
 *
 * viewUpdate.c
 *	  routines for translating a view definition into
 *	  INSERT/UPDATE/DELETE rules (i.e. updatable views).
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * ORIGINAL AUTHORS
 * 	Bernd Helmle, Jaime Casanova
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/rewrite/viewUpdate.c,v 1.1 2009/01/22 17:27:54 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/xact.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_rewrite.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_oper.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/viewUpdate.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/rel.h"

typedef TargetEntry** ViewDefColumnList;

typedef struct ViewBaseRelation
{
	List   *defs;			/* List of all base relations (root starts
							 * with only one relation because we
							 * implement only simple updatability) */
	Oid		parentRelation;	/* Oid of parent relation, 0 indicates root */
} ViewBaseRelation;

typedef struct ViewBaseRelationItem
{
	Relation		rel;		/* the Relation itself */
	Query		   *rule;		/* _RETURN rule of a view relation */
	TargetEntry	  **tentries;	/* saves order of column target list */
} ViewBaseRelationItem;

typedef struct ViewExprContext
{
	Index       newRTE;
	Index       oldRTE;
	Index       baseRTE;
	Index       subQueryLevel;
	ViewDefColumnList tentries;
} ViewExprContext;

static const char *get_auto_rule_name(CmdType type);
static Query *get_return_rule(Relation rel);
static void read_rearranged_cols(ViewBaseRelation *tree);
static bool is_select_query_updatable(const Query *query);
static Oid get_reloid_from_select(const Query *select,
								  int *rti, RangeTblEntry **rel_entry);
static void create_update_rule(Oid viewOid,
							   const Query *select,
							   const Relation baserel,
							   TargetEntry **tentries,
							   CmdType ruletype);
static void get_base_base_relations(const Query *view, Oid baserelid, List **list);
static void copyReversedTargetEntryPtr(List *targetList,
									   ViewDefColumnList targets);
static bool check_reltree(ViewBaseRelation *node);
static Query *form_update_query(const Query *select, ViewDefColumnList tentries, CmdType type);
static RangeTblEntry *get_relation_RTE(const Query *select,
									   unsigned int *offset);
static Index get_rtindex_for_rel(List *rte_list,
								 const char *relname);
static bool replace_tlist_varno_walker(Node *node,
									   ViewExprContext *ctxt);
static OpExpr *create_opexpr(Var *var_left, Var *var_right);
static void form_where_for_updrule(const Query *select, FromExpr **from,
								   const Relation baserel, Index baserti,
								   Index oldrti);
static void build_update_target_list(const Query *update, const Query *select,
									 const Relation baserel);

/*------------------------------------------------------------------------------
 * Private functions
 * -----------------------------------------------------------------------------
 */

static const char *
get_auto_rule_name(CmdType type)
{
	if (type == CMD_INSERT)
		return "_INSERT";
	if (type == CMD_UPDATE)
		return "_UPDATE";
	if (type == CMD_DELETE)
		return "_DELETE";
	return NULL;
}

/*
 * Returns the range table index for the specified relname.
 *
 * XXX This seems pretty grotty ... can't we do this in some other way?
 */
static Index
get_rtindex_for_rel(List *rte_list, const char *relname)
{
	ListCell   *cell;
	int			index = 0;

	AssertArg(relname);

	foreach(cell, rte_list)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(cell);

		index++;

		if (rte && strncmp(rte->eref->aliasname, relname, NAMEDATALEN) == 0)
			break;
	}

	Assert(index > 0);

	return (Index) index;
}

/*
 * Returns the RangeTblEntry starting at the specified offset. The
 * function can be used to iterate over the rtable list of the
 * specified select query tree.  Returns NULL if nothing is found.
 *
 * NOTE: The function only returns those RangeTblEntry that do not
 * match a *NEW* or *OLD* RangeTblEntry.
 *
 * The offset is incremented as a side effect.
 */
static RangeTblEntry *
get_relation_RTE(const Query *select, unsigned int *offset)
{
	AssertArg(offset);
	AssertArg(select);

	while (*offset <= list_length(select->rtable))
	{
		RangeTblEntry *rte = rt_fetch(*offset, select->rtable);
		(*offset)++;

		/* skip non-table RTEs */
		if (rte->rtekind != RTE_RELATION)
			continue;

		/*
		 * Skip RTEs named *NEW* and *OLD*.
		 *
		 * XXX It would be nice to be able to use something else than just
		 * the names here ... However, rtekind does not work as expected :-(
		 */
		if (strncmp(rte->eref->aliasname, "*NEW*", 6) == 0
			|| strncmp(rte->eref->aliasname, "*OLD*", 6) == 0)
			continue;

		return rte;
	}

	return NULL;
}

/*
 * Rewrite varno's and varattno for the specified Var node if it is in
 * a reversed order regarding to the underlying relation.  The lookup
 * table tentries holds all TargetEntries which are on a different
 * location in the view definition.  If var isn't on a different
 * position in the current view than on its original relation, nothing
 * is done.
 *
 * Note: This function assumes that the caller has already checked all
 * parameters for NULL.
 */
static void
adjustVarnoIfReversedCol(Var *var,
						 Index newRTE,
						 ViewDefColumnList tentries)
{
	TargetEntry *entry = tentries[var->varattno - 1];

	/*
	 * tentries holds NULL if given var isn't on a different location
	 * in the view Only replace if column order is reversed.
	 */
	if (entry && entry->resno != var->varattno)
	{
			var->varattno = entry->resno;
			var->varoattno = entry->resno;
	}

	/* Finally, make varno point to the *NEW* range table entry. */
	var->varno = newRTE;
	var->varnoold = newRTE;
}

/*
 * Creates an equal operator expression for the specified Vars.  They
 * are assumed to be of the same type.
 */
static OpExpr *
create_opexpr(Var *var_left, Var *var_right)
{
	OpExpr	   *result;
	HeapTuple  tuple;
	Form_pg_operator operator;
	Oid        eqOid;

	AssertArg(var_left);
	AssertArg(var_right);
	Assert(var_left->vartype == var_right->vartype);

	get_sort_group_operators(var_left->vartype, false, true, false,
							 NULL, &eqOid, NULL);

	tuple = SearchSysCache(OPEROID, ObjectIdGetDatum(eqOid), 0, 0, 0);

	operator = (Form_pg_operator) GETSTRUCT(tuple);
	result = makeNode(OpExpr);

	result->opno = HeapTupleGetOid(tuple);
	result->opfuncid = operator->oprcode;
	result->opresulttype = operator->oprresult;
	result->opretset = false;

	result->args = lappend(result->args, var_left);
	result->args = lappend(result->args, var_right);

	ReleaseSysCache(tuple);

	return result;
}

/*
 * Creates an expression tree for a WHERE clause.
 *
 * If from is not NULL, assigns the root node to the specified
 * FromExpr of the target query tree.
 *
 * Note that the function appends the specified opExpr op to the
 * specified anchor (if anchor != NULL) and returns that immediately.
 * That way this function could be used to add operator nodes to an
 * existing BoolExpr tree or (if from is given), to create a new Query
 * qualification list.
 */
static Node *
build_expression_tree(FromExpr *from, Node **anchor, BoolExpr *expr, OpExpr *op)
{
	/* Already some nodes there?  */
	if (*anchor)
	{
		expr->args = lappend(expr->args, op);
		((BoolExpr *)(*anchor))->args = lappend(((BoolExpr *)(*anchor))->args,
												expr);
		*anchor = (Node *)expr;
	}
	else
	{
		/* Currently no nodes ... */
		BoolExpr *boolexpr = makeNode(BoolExpr);
		expr->args = lappend(expr->args, op);
		boolexpr->args = lappend(boolexpr->args, expr);

		*anchor = (Node *) boolexpr;

		if (from)
			from->quals = *anchor;
	}

	return *anchor;
}

/*
 * Forms the WHERE clause for DELETE/UPDATE rules targeted to the
 * specified view.
 */
static void
form_where_for_updrule(const Query *select,	/* View retrieve rule */
					   FromExpr **from,		/* FromExpr for stmt */
					   const Relation baserel,	/* base relation of view */
					   Index baserti,		/* Index of base relation RTE */
					   Index oldrti)		/* Index of *OLD* RTE */
{
	BoolExpr *expr = NULL;
	Node     *anchor = NULL;
	Form_pg_attribute *attrs;
	ListCell *cell;

	AssertArg(baserti > 0);
	AssertArg(oldrti > 0);
	AssertArg(*from);
	AssertArg(baserel);

	attrs = baserel->rd_att->attrs;

	foreach(cell, select->targetList)
	{
		TargetEntry *te = (TargetEntry *) lfirst(cell);
		Var		   *var1;
		Var		   *var2;
		OpExpr	   *op;
		BoolExpr   *null_condition;
		NullTest   *nulltest1;
		NullTest   *nulltest2;

		/* If te->expr holds no Var pointer, continue.  */
		if (!IsA(te->expr, Var))
			continue;

		null_condition = makeNode(BoolExpr);
		nulltest1        = makeNode(NullTest);
		nulltest2        = makeNode(NullTest);

		/*
		 * These are the new operands we had to check for equality.
		 *
		 * For DELETE/UPDATE rules, var1 points to the *OLD* RTE, var2
		 * references the base relation.
		 */
		var1 = copyObject((Var *) (te->expr));

		/*
		 * Look at varoattno to determine whether this attribute has a different
		 * location in the underlying base table. If that case, retrieve the
		 * attribute from the base table and assign it to var2; otherwise
		 * simply copy it to var1.
		 */
		if (var1->varoattno > 0)
		{
			var2 = makeNode(Var);

			var2->varno = baserti;
			var2->varnoold = baserti;
			var2->varattno = attrs[var1->varoattno - 1]->attnum;
			var2->vartype = attrs[var1->varoattno - 1]->atttypid;
			var2->vartypmod = attrs[var1->varoattno - 1]->atttypmod;
			var2->varlevelsup = var1->varlevelsup;
			var2->varnoold = var2->varno;
			var2->varoattno = var2->varattno;
		}
		else
		{
			var2 = copyObject(var1);
			var2->varno = baserti;
			var2->varnoold = baserti;
		}

		var1->varno = oldrti;
		var1->varnoold = oldrti;

		/*
		 * rewrite varattno of var2 to point to the right column in relation
		 * *OLD* or *NEW*
		 */
		var2->varattno = te->resorigcol;
		var2->varoattno = te->resorigcol;

		/*
		 * rewrite varattno of var1 to point to the right column in base
		 * relation
		 */
		var1->varattno = te->resno;
		var1->varoattno = te->resno;

		op = create_opexpr(var1, var2);
		expr = makeNode(BoolExpr);
		expr->boolop = OR_EXPR;
		null_condition->boolop = AND_EXPR;

		nulltest1->arg = (Expr *)var1;
		nulltest1->nulltesttype = IS_NULL;

		nulltest2->arg = (Expr *)var2;
		nulltest2->nulltesttype = IS_NULL;

		null_condition->args = lappend(null_condition->args, nulltest1);
		null_condition->args = lappend(null_condition->args, nulltest2);
		expr->args = lappend(expr->args, null_condition);

		anchor = build_expression_tree(*from, (Node **) &anchor, expr, op);
	}
}

/*
 * Replaces the varnos for the specified targetlist to rtIndex
 */
static bool
replace_tlist_varno_walker(Node *node,
						   ViewExprContext *ctxt)
{
	AssertArg(ctxt);

	if (!node)
		return false;

	switch(node->type)
	{
		case T_Var:
			elog(DEBUG1, "adjusting varno old %d to new %d",
				 ((Var *)(node))->varno,
				 ctxt->newRTE);

			((Var *)(node))->varno = ctxt->newRTE;
			adjustVarnoIfReversedCol((Var *)node,
									 ctxt->newRTE,
									 ctxt->tentries);
			/* nothing more to do */
			break;

		case T_ArrayRef:
		{
			ArrayRef *array = (ArrayRef *) node;

			/*
			 * Things are getting complicated here.  We have found an
			 * array subscripting operation.  It's necessary to
			 * examine all varno's found in this operation to make
			 * sure, we're getting right.  This covers cases where a
			 * view selects a single index or complete array from a
			 * base table or view.
			 */

			/*
			 * Look at expressions that evaluate upper array
			 * indexes. Make sure all varno's are modified.  This is
			 * done by walking the expression tree recursively.
			 */
			expression_tree_walker((Node *) array->refupperindexpr,
								   replace_tlist_varno_walker,
								   (void *)ctxt);

			expression_tree_walker((Node *) array->reflowerindexpr,
								   replace_tlist_varno_walker,
								   (void *)ctxt);

			expression_tree_walker((Node *) array->refexpr,
								   replace_tlist_varno_walker,
								   (void *)ctxt);

			expression_tree_walker((Node *) array->refassgnexpr,
								   replace_tlist_varno_walker,
								   (void *)ctxt);
		}

		default:
			break;
	}

	return expression_tree_walker(node, replace_tlist_varno_walker, ctxt);
}

/*
 * Adds RTEs to form a query tree.
 *
 * select has to be a valid initialized view definition query tree
 * (the function assumes that this query has passed the
 * is_select_query_updatable() function).
 */
static Query *
form_update_query(const Query *select, ViewDefColumnList tentries, CmdType type)
{
	RangeTblEntry *rte;
	Oid			reloid;
	Query	   *newquery;

	AssertArg(select);
	AssertArg(tentries);

	newquery = makeNode(Query);
	newquery->commandType = type;

	/* copy the range table entries */
	newquery->rtable = copyObject(select->rtable);

	/* prepare other stuff */
	newquery->canSetTag = true;
	newquery->jointree = makeNode(FromExpr);

	/*
	 * Set result relation to the base relation.
	 *
	 * Since we currently only support updatable views with one
	 * underlying table, we simply extract the one relation which
	 * isn't labeled as *OLD* or *NEW*.
	 */
	reloid = get_reloid_from_select(select, &(newquery->resultRelation), &rte);
	if (!OidIsValid(reloid))
		elog(ERROR, "could not retrieve base relation OID");

	Assert(newquery->resultRelation > 0);

	/* adjust inFromCl of result range table entry */
	rte->inFromCl = false;

	/* We don't need a target list for DELETE. */
	if (type != CMD_DELETE)
	{
		ViewExprContext ctxt;
		ListCell        *cell;

		/* Copy all target entries. */
		newquery->targetList = copyObject(select->targetList);

		/*
		 * Replace all varnos to point to the *NEW* node in all targetentry
		 * expressions.
		 */

		ctxt.newRTE = PRS2_NEW_VARNO;
		ctxt.tentries = tentries;

		foreach(cell, newquery->targetList)
		{
			Node *node = (Node *) lfirst(cell);
			expression_tree_walker(node,
								   replace_tlist_varno_walker,
								   (void *) &ctxt);
		}
	}

	return newquery;
}

/*
 * Rewrite a TargetEntry, based on the given arguments to match
 * the new Query tree of the new DELETE/UPDATE/INSERT rule and/or
 * its underlying base relation.
 *
 * form_te_for_update() needs to carefully reassign Varno's of
 * all Var expressions assigned to the given TargetEntry and to
 * adjust all type info values and attribute index locations so
 * that the rewritten TargetEntry corresponds to the correct
 * column in the underlying base relation.
 *
 * Someone should consider that columns could be in reversed
 * order in a view definition, so we need to take care to
 * "restore" the correct order of all columns in the target list
 * of the new view update rules.
 *
 * There's also some additional overhead if we have an array field
 * involved.  In this case we have to loop recursively through the
 * array expressions to get all target entries right.
 */
static void
form_te_for_update(int2 attnum, Form_pg_attribute attrs, Oid baserelid,
				   Expr *expr, TargetEntry *te_update)
{
	/*
	 * First, try if this is an array subscripting operation. If true, dive
	 * recursively into the subscripting tree examining all varnos.
	 */

	if (IsA(expr, ArrayRef))
	{
		ArrayRef *array = (ArrayRef *) expr;

		if (array->refassgnexpr != NULL)
			form_te_for_update(attnum, attrs, baserelid, array->refassgnexpr,
							   te_update);

		if (array->refupperindexpr != NIL)
		{
			ListCell *cell;

			foreach(cell, array->refupperindexpr)
				form_te_for_update(attnum, attrs, baserelid, (Expr *) lfirst(cell), te_update);
		}

		if (array->reflowerindexpr != NIL)
		{
			ListCell *cell;

			foreach(cell, array->reflowerindexpr)
				form_te_for_update(attnum, attrs, baserelid, (Expr *) lfirst(cell), te_update);
		}

		if (array->refexpr != NULL)
			form_te_for_update(attnum, attrs, baserelid, array->refexpr,
							   te_update);
	}
	else if (IsA(expr, Var))
	{
		/*
		 * Base case of recursion: actually build the TargetEntry.
		 */
		Var *upd_var = (Var *) (te_update->expr);

		upd_var->varattno = te_update->resno;
		upd_var->varoattno = te_update->resno;

		upd_var->vartype = attrs->atttypid;
		upd_var->vartypmod = attrs->atttypmod;

		upd_var->varnoold = upd_var->varno;

		te_update->resno = attnum;
		te_update->resname = pstrdup(get_attname(baserelid, attnum));
		te_update->ressortgroupref = 0;
		te_update->resorigcol = 0;
		te_update->resorigtbl = 0;
		te_update->resjunk = false;
	}
}

/*
 * Create the returning list for the given query tree.  This allows
 * using RETURING in view update actions.  Note that the function
 * creates the returning list from the target list of the given query
 * tree if src is set to NULL.  This requires to call
 * build_update_target_list() on that query tree before.  If src !=
 * NULL, the target list is created from this query tree instead.
 */
static void
create_rule_returning_list(Query *query, const Query *src, Index newRTE,
						   ViewDefColumnList tentries)
{
	ViewExprContext ctxt;
	ListCell        *cell;

	ctxt.newRTE = newRTE;
	ctxt.tentries = tentries;

	/* determine target list source */
	if (src)
		query->returningList = copyObject(src->targetList);
	else
		query->returningList = copyObject(query->targetList);

	foreach(cell, query->returningList)
		expression_tree_walker((Node *) lfirst(cell),
							   replace_tlist_varno_walker,
							   (void *) &ctxt);
}

/*
 * Build the target list for a view UPDATE rule.
 *
 * Note: The function assumes a Query tree specified by update, which
 * was copied by form_update_query(). We need the original Query tree
 * to adjust the properties of each member of the TargetList of the
 * new query tree.
 */
static void
build_update_target_list(const Query *update, const Query *select,
						 Relation baserel)
{
	ListCell	   *cell1;
	ListCell	   *cell2;

	/*
	 * This Assert() is appropriate, since we rely on a query tree
	 * created by from_query(), which copies the target list from the
	 * original query tree specified by the argument select, which
	 * holds the current view definition.  So both target lists have
	 * to be equal in length.
	 */
	Assert(list_length(update->targetList) == list_length(select->targetList));

	/*
	 * Copy the target list of the view definition to the
	 * returningList.  This is required to support RETURNING clauses
	 * in view update actions.
	 */
	forboth(cell1, select->targetList, cell2, update->targetList)
	{
		TargetEntry *entry = (TargetEntry *) lfirst(cell1);
		TargetEntry *upd_entry = (TargetEntry *) lfirst(cell2);
		int			attindex;
		Form_pg_attribute attr;

		if (entry->resorigcol > 0)
			/*
			 * This column seems to have a different order than in the base
			 * table.  We get the attribute from the base relation referenced
			 * by rel and create a new resdom. This new result domain is then
			 * assigned instead of the old one.
			 */
			attindex = entry->resorigcol;
		else
			attindex = entry->resno;

		attr = baserel->rd_att->attrs[attindex - 1];

		form_te_for_update(attindex, attr, baserel->rd_id, upd_entry->expr,
						   upd_entry);
	}
}

/*
 * Examines the columns by the current view and initializes the lookup
 * table for all rearranged columns in base relations.  The function
 * requires a relation tree initialized by get_base_base_relations().
 */
static void
read_rearranged_cols(ViewBaseRelation *tree)
{
	AssertArg(tree);

	if (tree->defs)
	{
		int		num_items = list_length(tree->defs);
		int		i;

		/*
		 * Traverse the relation tree and look on all base relations
		 * for reversed column order in their target lists.  We have
		 * to perform a look-ahead-read on the tree, because we need
		 * to know how much columns the next base relation has, to
		 * allocate enough memory in tentries.
		 *
		 * Note that if we only have one base relation (a "real"
		 * table, not a view) exists, we have nothing to do, because
		 * this base relation cannot have a reversed column order
		 * caused by a view definition query.
		 */
		for (i = num_items - 1; i > 0; i--)
		{
			ViewBaseRelationItem *item_current;
			ViewBaseRelationItem *item_next;
			ViewBaseRelation *current;
			ViewBaseRelation *next;

			current = (ViewBaseRelation *) list_nth(tree->defs, i);

			/*
			 * We look ahead for the next base relation. We can do
			 * this here safely, because the loop condition terminates
			 * before reaching the list head.
			 */
			next = (ViewBaseRelation *) list_nth(tree->defs, i - 1);

			/*
			 * Note that the code currently requires a simply updatable
			 * relation tree.  This means we handle one base relation
			 * per loop, only.
			 */
			Assert(next);
			Assert(current);
			Assert(list_length(next->defs) == 1);
			Assert(list_length(current->defs) == 1);

			item_current = (ViewBaseRelationItem *) list_nth(current->defs, 0);
			item_next = (ViewBaseRelationItem *) list_nth(next->defs, 0);

			/* allocate tentries buffer */
			item_current->tentries = (ViewDefColumnList) palloc(sizeof(TargetEntry *) * RelationGetNumberOfAttributes(item_next->rel));

			copyReversedTargetEntryPtr(item_current->rule->targetList,
									   item_current->tentries);
		}
	}
}

/*------------------------------------------------------------------------------
 * Retrieves all relations from the view that can be considered a "base
 * relation".  The function returns a list that holds lists of all relation
 * OIDs found for the view. The list is filled top down, that means the head of
 * the list holds the relations for the "highest" view in the tree.
 *
 * Consider this view definition tree where each node is a relation the above
 * node is based on:
 *
 *                         1
 *                        / \
 *                       2   3
 *                      / \   \
 *                     4   5   6
 *                        /
 *                       7
 *
 * The function will then return a list with the following layout:
 *
 * Listindex          Node(s)
 * --------------------------
 * 1                  7
 * 2                  4 5 6
 * 3                  2 3
 *
 * As you can see in the table, all relations that are "children" of the
 * given root relation (the view relation itself) are saved in the
 * tree, except the root node itself.
 *------------------------------------------------------------------------------
 */
static void
get_base_base_relations(const Query *view, Oid baserelid, List **list)
{
	RangeTblEntry  *entry;
	unsigned int	offset = 1;
	ViewBaseRelation *childRel;

	if (!view)
		return;

	childRel = (ViewBaseRelation *) palloc(sizeof(ViewBaseRelation));
	childRel->defs = NIL;
	childRel->parentRelation = baserelid;

	/* Get all OIDs from the RTE list of view. */
	while ((entry = get_relation_RTE(view, &offset)) != NULL)
	{
		Relation	rel;
		ViewBaseRelationItem *item;

		/*
		 * Is this really a view or relation?
		 */
		rel = relation_open(entry->relid, AccessShareLock);

		if (rel->rd_rel->relkind != RELKIND_RELATION &&
			rel->rd_rel->relkind != RELKIND_VIEW)
		{
			/* don't need this one */
			relation_close(rel, AccessShareLock);
			continue;
		}

		item = (ViewBaseRelationItem *) palloc0(sizeof(ViewBaseRelationItem));
		item->rel = rel;
		item->rule = NULL;

		if (rel->rd_rel->relkind == RELKIND_VIEW)
			/*
			 * Get the rule _RETURN expression tree for the specified
			 * relation OID.  We need this to recurse into the view
			 * base relation tree.
			 */
			item->rule = get_return_rule(rel);

		elog(DEBUG1, "extracted relation %s for relation tree",
			 RelationGetRelationName(rel));
		childRel->defs = lappend(childRel->defs, item);

		/* recurse to any other child relations */
		if (item->rule)
			get_base_base_relations(item->rule, RelationGetRelid(rel), list);

	}

	if (childRel->defs)
		*list = lappend(*list, childRel);
}

static void
copyReversedTargetEntryPtr(List *targetList, ViewDefColumnList targets)
{
	ListCell *cell;

	AssertArg(targets);
	AssertArg(targetList);

	/* NOTE: We only reassign pointers. */
	foreach(cell, targetList)
	{
		Node *node = (Node *) lfirst(cell);

		if (IsA(node, TargetEntry))
		{
			/*
			 * Look at the resdom's resorigcol to determine whether
			 * this is a reversed column (meaning, it has a different
			 * column number than the underlying base table).
			 */
			TargetEntry *entry = (TargetEntry *) node;

			if (!IsA(entry->expr, Var))
				/* nothing to do here */
				continue;

			if (entry->resorigcol > 0 && entry->resno != entry->resorigcol)
			{
				/*
				 * Save this TargetEntry to the appropiate place in
				 * the lookup table.  Do it only if not already
				 * occupied (this could happen if the column is
				 * specified more than one time in the view
				 * definition).
				 */
				if (targets[entry->resorigcol - 1] == NULL)
					targets[entry->resorigcol - 1] = entry;
			}
		}
	}
}

/*
 * Transforms the specified view definition into an INSERT, UPDATE, or
 * DELETE rule.
 *
 * Note: The function assumes that the specified query tree has passed the
 * is_select_query_updatable() function.
 */
static void
create_update_rule(Oid viewOid, const Query *select, const Relation baserel,
				   ViewDefColumnList tentries,
				   CmdType ruletype)
{
	Query	   *newquery;
	Oid			baserelid;
	Index		baserti;
	RangeTblEntry *rte;

	AssertArg(tentries);
	AssertArg(baserel);
	AssertArg(select);
	AssertArg(ruletype == CMD_INSERT || ruletype == CMD_UPDATE || ruletype == CMD_DELETE);

	newquery = form_update_query(select, tentries, ruletype);

	/*
	 * form_update_query() has prepared the jointree of the new UPDATE/DELETE rule.
	 *
	 * Now, our UPDATE rule needs range table references for the *NEW*
	 * and base relation RTEs.  A DELETE rule needs range table
	 * references for the *OLD* and base relation RTEs.
	 */

	baserelid = get_reloid_from_select(select, NULL, &rte);
	if (!OidIsValid(baserelid))
		elog(ERROR, "could not get the base relation from the view definition");

	baserti = get_rtindex_for_rel(newquery->rtable,
								  rte->eref->aliasname);
	Assert(baserti > 0);

	rte = rt_fetch(baserti, newquery->rtable);

	if (ruletype != CMD_INSERT)
	{
		RangeTblRef *oldref;
		RangeTblRef *baseref;

		oldref = makeNode(RangeTblRef);
		oldref->rtindex = PRS2_OLD_VARNO;

		baseref = makeNode(RangeTblRef);
		baseref->rtindex = baserti;

		newquery->jointree->fromlist = list_make2(baseref, oldref);

		/* Create the WHERE condition qualification for the rule action. */
		form_where_for_updrule(select, &(newquery->jointree),
							   baserel, baserti, PRS2_OLD_VARNO);
	}

	if (ruletype != CMD_DELETE)
		/*
		 * We must reorder the columns in the targetlist to match the
		 * underlying table.  We do this after calling
		 * form_where_for_updrule() because build_update_target_list()
		 * relies on the original resdoms in the update tree.
		 */
		build_update_target_list(newquery, select, baserel);

	/*
	 * Create the returning list now that build_update_target_list()
	 * has done the leg work.
	 */
	if (ruletype == CMD_DELETE)
		create_rule_returning_list(newquery, select, PRS2_OLD_VARNO, tentries);
	else
		create_rule_returning_list(newquery, NULL, PRS2_NEW_VARNO, tentries);

	/* Set ACL bit */
	if (ruletype == CMD_INSERT)
		rte->requiredPerms |= ACL_INSERT;
	else if (ruletype == CMD_UPDATE)
		rte->requiredPerms |= ACL_UPDATE;
	else if (ruletype == CMD_DELETE)
		rte->requiredPerms |= ACL_DELETE;

	/* Create system rule */
	DefineQueryRewrite(pstrdup(get_auto_rule_name(ruletype)),
					   viewOid, /* event_relid */
					   NULL, /* WHERE clause */
					   ruletype,
					   true, /* is_instead */
					   true, /* is_auto */
					   false, /* replace */
					   list_make1(newquery) /* action */);
}

/*
 * Checks the specified Query for updatability.  Currently, "simply
 * updatable" rules are implemented.
 */
static bool
is_select_query_updatable(const Query *query)
{
	ListCell *cell;
	List     *seen_attnos;

	AssertArg(query);
	AssertArg(query->commandType == CMD_SELECT);

	/*
	 * check for unsupported clauses in the view definition
	 */

	if (query->hasAggs)
	{
		elog(DEBUG1, "view is not updatable because it uses an aggregate function");
		return false;
	}

	if (query->hasWindowFuncs)
	{
		elog(DEBUG1, "view is not updatable because it uses a window function");
		return false;
	}

	if (query->hasRecursive)
	{
		elog(DEBUG1, "view is not updatable because it contains a WITH RECURSIVE clause");
		return false;
	}

	if (query->cteList)
	{
		elog(DEBUG1, "view is not updatable because it contains a WITH clause");
		return false;
	}

	if (list_length(query->groupClause) >= 1)
	{
		elog(DEBUG1, "view is not updatable because it contains a GROUP BY clause");
		return false;
	}

	if (query->havingQual)
	{
		elog(DEBUG1, "view is not updatable because it contains a HAVING clause");
		return false;
	}

	if (list_length(query->distinctClause) >= 1)
	{
		elog(DEBUG1, "view is not updatable because it contains a DISTINCT clause");
		return false;
	}

	if (query->limitOffset)
	{
		elog(DEBUG1, "view is not updatable because it contains an OFFSET clause");
		return false;
	}

	if (query->limitCount)
	{
		elog(DEBUG1, "view is not updatable because it contains a LIMIT clause");
		return false;
	}

	if (query->setOperations)
	{
		elog(DEBUG1, "view is not updatable because it contains UNION or INTERSECT or EXCEPT");
		return false;
	}

	/*
	 * Test for number of involved relations.  Since we assume to
	 * operate on a view definition SELECT query tree, we must count 3
	 * rtable entries.  Otherwise this seems not to be a view based on
	 * a single relation.
	 */
	if (list_length(query->rtable) > 3)
	{
		elog(DEBUG1, "view is not updatable because it has more than one underlying table");
		return false;
	}

	/* Any rtable entries involved?  */
	if (list_length(query->rtable) < 3)
	{
		elog(DEBUG1, "view is not updatable because it has no underlying tables");
		return false;
	}

	/*
	 * Walk down the target list and look for nodes that aren't Vars.
	 * "Simply updatable" doesn't allow functions, host variables, or
	 * constant expressions in the target list.
	 *
	 * Also, check if any of the target list entries are indexed array
	 * expressions, which aren't supported.
	 */
	seen_attnos = NIL;

	foreach(cell, query->targetList)
	{
		Node *node = (Node *) lfirst(cell);

		if (IsA(node, TargetEntry))
		{
			TargetEntry	   *te = (TargetEntry *) node;

			/*
			 * TODO -- it would be nice to support Const nodes here as well
			 * (but apparently it isn't in the standard)
			 */
			if (!IsA(te->expr, Var) && !IsA(te->expr, ArrayRef))
			{
				elog(DEBUG1, "view is not updatable because select list contains a derived column");
				return false;
			}

			/* This is currently only partially implemented, but can be fixed. */
			if (IsA(te->expr, ArrayRef))
			{
				elog(DEBUG1, "view is not updatable because select list contains an array element reference");
				return false;
			}

			if (IsA(te->expr, Var))
			{
				Var *var = (Var *) te->expr;

				/* System columns aren't updatable. */
				if (var->varattno < 0)
				{
					elog(DEBUG1, "view is not updatable because select list references a system column");
					return false;
				}

				if (list_member_int(seen_attnos, var->varattno))
				{
					elog(DEBUG1, "view is not updatable because select list references the same column more than once");
					return false;
				}
				else
					seen_attnos = lappend_int(seen_attnos, var->varattno);
			}
		}
	}

	/*
	 * Finally, check that all RTEs are acceptable.  This rejects
	 * table functions, which cannot ever be updatable, and also WITH
	 * clauses.
	 */
	foreach(cell, query->rtable)
	{
		RangeTblEntry *entry = (RangeTblEntry *) lfirst(cell);

		if (entry->rtekind != RTE_RELATION)
		{
			elog(DEBUG1, "view is not updatable because correlation \"%s\" is not a table",
				 entry->eref->aliasname);
			return false;
		}
	}

	return true;
}

/*
 * Traverse the specified relation tree.  The function stops at the
 * base relations at the leafs of the tree. If any of the relations
 * has more than one base relation, it is considered a not simply
 * updatable view and false is returned.
 */
static bool
check_reltree(ViewBaseRelation *node)
{
	ListCell *cell;

	AssertArg(node);

	foreach(cell, node->defs)
	{
		/* Walk down the tree */
		ViewBaseRelation *relations = (ViewBaseRelation *) lfirst(cell);

		if (list_length(relations->defs) > 1)
		{
			elog(DEBUG1, "possible JOIN/UNION in view definition: %d", list_length(relations->defs));
			return false;
		}
		else if (list_length(relations->defs) == 1) {
			ViewBaseRelationItem *item = (ViewBaseRelationItem *) linitial(relations->defs);

			/* if the relation found is a view, check its updatability */
			if (item->rel->rd_rel->relkind == RELKIND_VIEW && !is_select_query_updatable(item->rule))
			{
				elog(DEBUG1, "base view \"%s\" is not updatable",
					 RelationGetRelationName(item->rel));
				return false;
			}
		}
	}

	return true;
}

/*
 * Given a SELECT query tree, return the OID of the first RTE_RELATION range
 * table entry found that is not *NEW* nor *OLD*.
 *
 * Also sets the RangeTblEntry pointer into rel_entry, and the range
 * table index into rti, unless they are NULL.
 *
 * This function assumes that the specified query tree was checked by a
 * previous call to the is_select_query_updatable() function.
 */
static Oid
get_reloid_from_select(const Query *select, int *rti, RangeTblEntry **rel_entry)
{
	ListCell   *cell;
	Oid			result = InvalidOid;
	int			index;

	/* Check specified query tree. Return immediately on error. */
	if (select == NULL || select->commandType != CMD_SELECT)
		return InvalidOid;

	/*
	 * We loop through the RTEs to get information about all involved
	 * relations.  We return the first OID we find in the list that is not
	 * *NEW* nor *OLD*.
	 */
	index = 0;
	foreach(cell, select->rtable)
	{
		RangeTblEntry *entry = (RangeTblEntry *) lfirst(cell);

		index++;

		if (entry == NULL)
			elog(ERROR, "null RTE pointer in get_reloid_from_select");

		elog(DEBUG1, "extracted range table entry for %u", entry->relid);

		/* Return the first RELATION rte we find */
		if (entry->rtekind == RTE_RELATION)
		{
			/*
			 * XXX This is ugly.  The parser prepends two RTEs with rtekind
			 * RTE_RELATION named *NEW* and *OLD*.  We have to exclude them by
			 * name!  It would be much better if it used RTE_SPECIAL
			 * instead, but other parts of the system stop working if one
			 * just changes it naively.
			 */
			if (strncmp(entry->eref->aliasname, "*NEW*", 6) == 0
				|| strncmp(entry->eref->aliasname, "*OLD*", 6) == 0)
				continue;

			result = entry->relid;
			if (rti != NULL)
				*rti = index;
			if (rel_entry != NULL)
				*rel_entry = entry;
			break;
		}
	}

	return result;
}

/*
 * get_return_rule: returns the _RETURN rule of a view as a Query node.
 */
static Query *
get_return_rule(Relation rel)
{
	Query  *query = NULL;
	int		i;

	AssertArg(rel->rd_rel->relkind == RELKIND_VIEW);

	for (i = 0; i < rel->rd_rules->numLocks; i++)
	{
		RewriteRule *rule = rel->rd_rules->rules[i];

		if (rule->event == CMD_SELECT)
		{
			/* A _RETURN rule should have only one action */
			if (list_length(rule->actions) != 1)
				elog(ERROR, "invalid _RETURN rule action specification");

			query = linitial(rule->actions);
			break;
		}
	}

	return query;
}

/*------------------------------------------------------------------------------
 * Public functions
 *------------------------------------------------------------------------------
 */

/*
 * CreateViewUpdateRules
 *
 * This is the main entry point to creating an updatable view's rules.  Given a
 * rule definition, examine it, and create the rules if appropiate, or return
 * doing nothing if not.
 */
void
CreateViewUpdateRules(Oid viewOid, const Query *viewDef)
{
	Relation	baserel;
	Form_pg_attribute *attrs;
	ViewDefColumnList tentries;
	Oid			baserelid;
	MemoryContext	cxt;
	MemoryContext	oldcxt;
	ViewBaseRelation *tree;
	ListCell   *cell;

	/*
	 * The routines in this file leak memory like crazy, so make sure we
	 * allocate it all in an appropiate context.
	 */
	cxt = AllocSetContextCreate(TopTransactionContext,
								"UpdateRulesContext",
								ALLOCSET_DEFAULT_MINSIZE,
								ALLOCSET_DEFAULT_INITSIZE,
								ALLOCSET_DEFAULT_MAXSIZE);
	oldcxt = MemoryContextSwitchTo(cxt);

	/*
	 * Create the lookup table for the view definition target columns. We save
	 * the RESDOMS in that manner to look quickly for reversed column orders.
	 */

	baserelid = get_reloid_from_select(viewDef, NULL, NULL);

	/* Get relation tree */
	tree = (ViewBaseRelation *) palloc(sizeof(ViewBaseRelation));

	tree->parentRelation = InvalidOid;
	tree->defs = NIL;
	get_base_base_relations(viewDef, baserelid, &(tree->defs));

	/* Check the query tree for updatability */
	if (!check_reltree(tree) || !is_select_query_updatable(viewDef))
	{
		elog(DEBUG1, "view is not updatable");
		goto finish;
	}

	baserel = heap_open(baserelid, AccessShareLock);
	attrs = baserel->rd_att->attrs;

	/*
	 * Copy TargetEntries to match the slot numbers in the target list with
	 * their original column attribute number. Note that only pointers are
	 * copied and they are valid only as long as the specified SELECT query
	 * stays valid!
	 */
	tentries = (ViewDefColumnList)
		palloc0(baserel->rd_rel->relnatts * sizeof(TargetEntry *));

	copyReversedTargetEntryPtr(viewDef->targetList, tentries);

	/*
	 * Now do the same for the base relation tree. read_rearranged_cols
	 * traverses the relation tree and performs a copyReversedTargetEntry()
	 * call to each base relation.
	 */
	read_rearranged_cols(tree);

	create_update_rule(viewOid, viewDef, baserel, tentries, CMD_INSERT);
	create_update_rule(viewOid, viewDef, baserel, tentries, CMD_DELETE);
	create_update_rule(viewOid, viewDef, baserel, tentries, CMD_UPDATE);

	ereport(NOTICE, (errmsg("CREATE VIEW has created automatic view update rules")));

	/* free remaining stuff */
	heap_close(baserel, NoLock);

finish:
	/* get_base_base_relations leaves some open relations */
	foreach(cell, tree->defs)
	{
		ListCell   *cell2;
		ViewBaseRelation *vbr = (ViewBaseRelation *) lfirst(cell);

		foreach(cell2, vbr->defs)
		{
			ViewBaseRelationItem *vbri = (ViewBaseRelationItem *) lfirst(cell2);

			relation_close(vbri->rel, NoLock);
		}
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);
}
