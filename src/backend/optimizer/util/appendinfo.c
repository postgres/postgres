/*-------------------------------------------------------------------------
 *
 * appendinfo.c
 *	  Routines for mapping between append parent(s) and children
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/appendinfo.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "foreign/fdwapi.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


typedef struct
{
	PlannerInfo *root;
	int			nappinfos;
	AppendRelInfo **appinfos;
} adjust_appendrel_attrs_context;

static void make_inh_translation_list(Relation oldrelation,
									  Relation newrelation,
									  Index newvarno,
									  AppendRelInfo *appinfo);
static Node *adjust_appendrel_attrs_mutator(Node *node,
											adjust_appendrel_attrs_context *context);


/*
 * make_append_rel_info
 *	  Build an AppendRelInfo for the parent-child pair
 */
AppendRelInfo *
make_append_rel_info(Relation parentrel, Relation childrel,
					 Index parentRTindex, Index childRTindex)
{
	AppendRelInfo *appinfo = makeNode(AppendRelInfo);

	appinfo->parent_relid = parentRTindex;
	appinfo->child_relid = childRTindex;
	appinfo->parent_reltype = parentrel->rd_rel->reltype;
	appinfo->child_reltype = childrel->rd_rel->reltype;
	make_inh_translation_list(parentrel, childrel, childRTindex, appinfo);
	appinfo->parent_reloid = RelationGetRelid(parentrel);

	return appinfo;
}

/*
 * make_inh_translation_list
 *	  Build the list of translations from parent Vars to child Vars for
 *	  an inheritance child, as well as a reverse-translation array.
 *
 * The reverse-translation array has an entry for each child relation
 * column, which is either the 1-based index of the corresponding parent
 * column, or 0 if there's no match (that happens for dropped child columns,
 * as well as child columns beyond those of the parent, which are allowed in
 * traditional inheritance though not partitioning).
 *
 * For paranoia's sake, we match type/collation as well as attribute name.
 */
static void
make_inh_translation_list(Relation oldrelation, Relation newrelation,
						  Index newvarno,
						  AppendRelInfo *appinfo)
{
	List	   *vars = NIL;
	AttrNumber *pcolnos;
	TupleDesc	old_tupdesc = RelationGetDescr(oldrelation);
	TupleDesc	new_tupdesc = RelationGetDescr(newrelation);
	Oid			new_relid = RelationGetRelid(newrelation);
	int			oldnatts = old_tupdesc->natts;
	int			newnatts = new_tupdesc->natts;
	int			old_attno;
	int			new_attno = 0;

	/* Initialize reverse-translation array with all entries zero */
	appinfo->num_child_cols = newnatts;
	appinfo->parent_colnos = pcolnos =
		(AttrNumber *) palloc0(newnatts * sizeof(AttrNumber));

	for (old_attno = 0; old_attno < oldnatts; old_attno++)
	{
		Form_pg_attribute att;
		char	   *attname;
		Oid			atttypid;
		int32		atttypmod;
		Oid			attcollation;

		att = TupleDescAttr(old_tupdesc, old_attno);
		if (att->attisdropped)
		{
			/* Just put NULL into this list entry */
			vars = lappend(vars, NULL);
			continue;
		}
		attname = NameStr(att->attname);
		atttypid = att->atttypid;
		atttypmod = att->atttypmod;
		attcollation = att->attcollation;

		/*
		 * When we are generating the "translation list" for the parent table
		 * of an inheritance set, no need to search for matches.
		 */
		if (oldrelation == newrelation)
		{
			vars = lappend(vars, makeVar(newvarno,
										 (AttrNumber) (old_attno + 1),
										 atttypid,
										 atttypmod,
										 attcollation,
										 0));
			pcolnos[old_attno] = old_attno + 1;
			continue;
		}

		/*
		 * Otherwise we have to search for the matching column by name.
		 * There's no guarantee it'll have the same column position, because
		 * of cases like ALTER TABLE ADD COLUMN and multiple inheritance.
		 * However, in simple cases, the relative order of columns is mostly
		 * the same in both relations, so try the column of newrelation that
		 * follows immediately after the one that we just found, and if that
		 * fails, let syscache handle it.
		 */
		if (new_attno >= newnatts ||
			(att = TupleDescAttr(new_tupdesc, new_attno))->attisdropped ||
			strcmp(attname, NameStr(att->attname)) != 0)
		{
			HeapTuple	newtup;

			newtup = SearchSysCacheAttName(new_relid, attname);
			if (!HeapTupleIsValid(newtup))
				elog(ERROR, "could not find inherited attribute \"%s\" of relation \"%s\"",
					 attname, RelationGetRelationName(newrelation));
			new_attno = ((Form_pg_attribute) GETSTRUCT(newtup))->attnum - 1;
			Assert(new_attno >= 0 && new_attno < newnatts);
			ReleaseSysCache(newtup);

			att = TupleDescAttr(new_tupdesc, new_attno);
		}

		/* Found it, check type and collation match */
		if (atttypid != att->atttypid || atttypmod != att->atttypmod)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
					 errmsg("attribute \"%s\" of relation \"%s\" does not match parent's type",
							attname, RelationGetRelationName(newrelation))));
		if (attcollation != att->attcollation)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
					 errmsg("attribute \"%s\" of relation \"%s\" does not match parent's collation",
							attname, RelationGetRelationName(newrelation))));

		vars = lappend(vars, makeVar(newvarno,
									 (AttrNumber) (new_attno + 1),
									 atttypid,
									 atttypmod,
									 attcollation,
									 0));
		pcolnos[new_attno] = old_attno + 1;
		new_attno++;
	}

	appinfo->translated_vars = vars;
}

/*
 * adjust_appendrel_attrs
 *	  Copy the specified query or expression and translate Vars referring to a
 *	  parent rel to refer to the corresponding child rel instead.  We also
 *	  update rtindexes appearing outside Vars, such as resultRelation and
 *	  jointree relids.
 *
 * Note: this is only applied after conversion of sublinks to subplans,
 * so we don't need to cope with recursion into sub-queries.
 *
 * Note: this is not hugely different from what pullup_replace_vars() does;
 * maybe we should try to fold the two routines together.
 */
Node *
adjust_appendrel_attrs(PlannerInfo *root, Node *node, int nappinfos,
					   AppendRelInfo **appinfos)
{
	adjust_appendrel_attrs_context context;

	context.root = root;
	context.nappinfos = nappinfos;
	context.appinfos = appinfos;

	/* If there's nothing to adjust, don't call this function. */
	Assert(nappinfos >= 1 && appinfos != NULL);

	/* Should never be translating a Query tree. */
	Assert(node == NULL || !IsA(node, Query));

	return adjust_appendrel_attrs_mutator(node, &context);
}

static Node *
adjust_appendrel_attrs_mutator(Node *node,
							   adjust_appendrel_attrs_context *context)
{
	AppendRelInfo **appinfos = context->appinfos;
	int			nappinfos = context->nappinfos;
	int			cnt;

	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) copyObject(node);
		AppendRelInfo *appinfo = NULL;

		if (var->varlevelsup != 0)
			return (Node *) var;	/* no changes needed */

		/*
		 * You might think we need to adjust var->varnullingrels, but that
		 * shouldn't need any changes.  It will contain outer-join relids,
		 * while the transformation we are making affects only baserels.
		 * Below, we just propagate var->varnullingrels into the translated
		 * Var.
		 *
		 * If var->varnullingrels isn't empty, and the translation wouldn't be
		 * a Var, we have to fail.  One could imagine wrapping the translated
		 * expression in a PlaceHolderVar, but that won't work because this is
		 * typically used after freezing placeholders.  Fortunately, the case
		 * appears unreachable at the moment.  We can see nonempty
		 * var->varnullingrels here, but only in cases involving partitionwise
		 * joining, and in such cases the translations will always be Vars.
		 * (Non-Var translations occur only for appendrels made by flattening
		 * UNION ALL subqueries.)  Should we need to make this work in future,
		 * a possible fix is to mandate that prepjointree.c create PHVs for
		 * all non-Var outputs of such subqueries, and then we could look up
		 * the pre-existing PHV here.  Or perhaps just wrap the translations
		 * that way to begin with?
		 *
		 * If var->varreturningtype is not VAR_RETURNING_DEFAULT, then that
		 * also needs to be copied to the translated Var.  That too would fail
		 * if the translation wasn't a Var, but that should never happen since
		 * a non-default var->varreturningtype is only used for Vars referring
		 * to the result relation, which should never be a flattened UNION ALL
		 * subquery.
		 */

		for (cnt = 0; cnt < nappinfos; cnt++)
		{
			if (var->varno == appinfos[cnt]->parent_relid)
			{
				appinfo = appinfos[cnt];
				break;
			}
		}

		if (appinfo)
		{
			var->varno = appinfo->child_relid;
			/* it's now a generated Var, so drop any syntactic labeling */
			var->varnosyn = 0;
			var->varattnosyn = 0;
			if (var->varattno > 0)
			{
				Node	   *newnode;

				if (var->varattno > list_length(appinfo->translated_vars))
					elog(ERROR, "attribute %d of relation \"%s\" does not exist",
						 var->varattno, get_rel_name(appinfo->parent_reloid));
				newnode = copyObject(list_nth(appinfo->translated_vars,
											  var->varattno - 1));
				if (newnode == NULL)
					elog(ERROR, "attribute %d of relation \"%s\" does not exist",
						 var->varattno, get_rel_name(appinfo->parent_reloid));
				if (IsA(newnode, Var))
				{
					((Var *) newnode)->varreturningtype = var->varreturningtype;
					((Var *) newnode)->varnullingrels = var->varnullingrels;
				}
				else
				{
					if (var->varreturningtype != VAR_RETURNING_DEFAULT)
						elog(ERROR, "failed to apply returningtype to a non-Var");
					if (var->varnullingrels != NULL)
						elog(ERROR, "failed to apply nullingrels to a non-Var");
				}
				return newnode;
			}
			else if (var->varattno == 0)
			{
				/*
				 * Whole-row Var: if we are dealing with named rowtypes, we
				 * can use a whole-row Var for the child table plus a coercion
				 * step to convert the tuple layout to the parent's rowtype.
				 * Otherwise we have to generate a RowExpr.
				 */
				if (OidIsValid(appinfo->child_reltype))
				{
					Assert(var->vartype == appinfo->parent_reltype);
					if (appinfo->parent_reltype != appinfo->child_reltype)
					{
						ConvertRowtypeExpr *r = makeNode(ConvertRowtypeExpr);

						r->arg = (Expr *) var;
						r->resulttype = appinfo->parent_reltype;
						r->convertformat = COERCE_IMPLICIT_CAST;
						r->location = -1;
						/* Make sure the Var node has the right type ID, too */
						var->vartype = appinfo->child_reltype;
						return (Node *) r;
					}
				}
				else
				{
					/*
					 * Build a RowExpr containing the translated variables.
					 *
					 * In practice var->vartype will always be RECORDOID here,
					 * so we need to come up with some suitable column names.
					 * We use the parent RTE's column names.
					 *
					 * Note: we can't get here for inheritance cases, so there
					 * is no need to worry that translated_vars might contain
					 * some dummy NULLs.
					 */
					RowExpr    *rowexpr;
					List	   *fields;
					RangeTblEntry *rte;

					rte = rt_fetch(appinfo->parent_relid,
								   context->root->parse->rtable);
					fields = copyObject(appinfo->translated_vars);
					rowexpr = makeNode(RowExpr);
					rowexpr->args = fields;
					rowexpr->row_typeid = var->vartype;
					rowexpr->row_format = COERCE_IMPLICIT_CAST;
					rowexpr->colnames = copyObject(rte->eref->colnames);
					rowexpr->location = -1;

					if (var->varreturningtype != VAR_RETURNING_DEFAULT)
						elog(ERROR, "failed to apply returningtype to a non-Var");
					if (var->varnullingrels != NULL)
						elog(ERROR, "failed to apply nullingrels to a non-Var");

					return (Node *) rowexpr;
				}
			}
			/* system attributes don't need any other translation */
		}
		else if (var->varno == ROWID_VAR)
		{
			/*
			 * If it's a ROWID_VAR placeholder, see if we've reached a leaf
			 * target rel, for which we can translate the Var to a specific
			 * instantiation.  We should never be asked to translate to a set
			 * of relids containing more than one leaf target rel, so the
			 * answer will be unique.  If we're still considering non-leaf
			 * inheritance levels, return the ROWID_VAR Var as-is.
			 */
			Relids		leaf_result_relids = context->root->leaf_result_relids;
			Index		leaf_relid = 0;

			for (cnt = 0; cnt < nappinfos; cnt++)
			{
				if (bms_is_member(appinfos[cnt]->child_relid,
								  leaf_result_relids))
				{
					if (leaf_relid)
						elog(ERROR, "cannot translate to multiple leaf relids");
					leaf_relid = appinfos[cnt]->child_relid;
				}
			}

			if (leaf_relid)
			{
				RowIdentityVarInfo *ridinfo = (RowIdentityVarInfo *)
					list_nth(context->root->row_identity_vars, var->varattno - 1);

				if (bms_is_member(leaf_relid, ridinfo->rowidrels))
				{
					/* Substitute the Var given in the RowIdentityVarInfo */
					var = copyObject(ridinfo->rowidvar);
					/* ... but use the correct relid */
					var->varno = leaf_relid;
					/* identity vars shouldn't have nulling rels */
					Assert(var->varnullingrels == NULL);
					/* varnosyn in the RowIdentityVarInfo is probably wrong */
					var->varnosyn = 0;
					var->varattnosyn = 0;
				}
				else
				{
					/*
					 * This leaf rel can't return the desired value, so
					 * substitute a NULL of the correct type.
					 */
					return (Node *) makeNullConst(var->vartype,
												  var->vartypmod,
												  var->varcollid);
				}
			}
		}
		return (Node *) var;
	}
	if (IsA(node, CurrentOfExpr))
	{
		CurrentOfExpr *cexpr = (CurrentOfExpr *) copyObject(node);

		for (cnt = 0; cnt < nappinfos; cnt++)
		{
			AppendRelInfo *appinfo = appinfos[cnt];

			if (cexpr->cvarno == appinfo->parent_relid)
			{
				cexpr->cvarno = appinfo->child_relid;
				break;
			}
		}
		return (Node *) cexpr;
	}
	if (IsA(node, PlaceHolderVar))
	{
		/* Copy the PlaceHolderVar node with correct mutation of subnodes */
		PlaceHolderVar *phv;

		phv = (PlaceHolderVar *) expression_tree_mutator(node,
														 adjust_appendrel_attrs_mutator,
														 context);
		/* now fix PlaceHolderVar's relid sets */
		if (phv->phlevelsup == 0)
		{
			phv->phrels = adjust_child_relids(phv->phrels,
											  nappinfos, appinfos);
			/* as above, we needn't touch phnullingrels */
		}
		return (Node *) phv;
	}
	/* Shouldn't need to handle planner auxiliary nodes here */
	Assert(!IsA(node, SpecialJoinInfo));
	Assert(!IsA(node, AppendRelInfo));
	Assert(!IsA(node, PlaceHolderInfo));
	Assert(!IsA(node, MinMaxAggInfo));

	/*
	 * We have to process RestrictInfo nodes specially.  (Note: although
	 * set_append_rel_pathlist will hide RestrictInfos in the parent's
	 * baserestrictinfo list from us, it doesn't hide those in joininfo.)
	 */
	if (IsA(node, RestrictInfo))
	{
		RestrictInfo *oldinfo = (RestrictInfo *) node;
		RestrictInfo *newinfo = makeNode(RestrictInfo);

		/* Copy all flat-copiable fields, notably including rinfo_serial */
		memcpy(newinfo, oldinfo, sizeof(RestrictInfo));

		/* Recursively fix the clause itself */
		newinfo->clause = (Expr *)
			adjust_appendrel_attrs_mutator((Node *) oldinfo->clause, context);

		/* and the modified version, if an OR clause */
		newinfo->orclause = (Expr *)
			adjust_appendrel_attrs_mutator((Node *) oldinfo->orclause, context);

		/* adjust relid sets too */
		newinfo->clause_relids = adjust_child_relids(oldinfo->clause_relids,
													 context->nappinfos,
													 context->appinfos);
		newinfo->required_relids = adjust_child_relids(oldinfo->required_relids,
													   context->nappinfos,
													   context->appinfos);
		newinfo->outer_relids = adjust_child_relids(oldinfo->outer_relids,
													context->nappinfos,
													context->appinfos);
		newinfo->left_relids = adjust_child_relids(oldinfo->left_relids,
												   context->nappinfos,
												   context->appinfos);
		newinfo->right_relids = adjust_child_relids(oldinfo->right_relids,
													context->nappinfos,
													context->appinfos);

		/*
		 * Reset cached derivative fields, since these might need to have
		 * different values when considering the child relation.  Note we
		 * don't reset left_ec/right_ec: each child variable is implicitly
		 * equivalent to its parent, so still a member of the same EC if any.
		 */
		newinfo->eval_cost.startup = -1;
		newinfo->norm_selec = -1;
		newinfo->outer_selec = -1;
		newinfo->left_em = NULL;
		newinfo->right_em = NULL;
		newinfo->scansel_cache = NIL;
		newinfo->left_bucketsize = -1;
		newinfo->right_bucketsize = -1;
		newinfo->left_mcvfreq = -1;
		newinfo->right_mcvfreq = -1;

		return (Node *) newinfo;
	}

	/*
	 * NOTE: we do not need to recurse into sublinks, because they should
	 * already have been converted to subplans before we see them.
	 */
	Assert(!IsA(node, SubLink));
	Assert(!IsA(node, Query));
	/* We should never see these Query substructures, either. */
	Assert(!IsA(node, RangeTblRef));
	Assert(!IsA(node, JoinExpr));

	return expression_tree_mutator(node, adjust_appendrel_attrs_mutator, context);
}

/*
 * adjust_appendrel_attrs_multilevel
 *	  Apply Var translations from an appendrel parent down to a child.
 *
 * Replace Vars in the "node" expression that reference "parentrel" with
 * the appropriate Vars for "childrel".  childrel can be more than one
 * inheritance level removed from parentrel.
 */
Node *
adjust_appendrel_attrs_multilevel(PlannerInfo *root, Node *node,
								  RelOptInfo *childrel,
								  RelOptInfo *parentrel)
{
	AppendRelInfo **appinfos;
	int			nappinfos;

	/* Recurse if immediate parent is not the top parent. */
	if (childrel->parent != parentrel)
	{
		if (childrel->parent)
			node = adjust_appendrel_attrs_multilevel(root, node,
													 childrel->parent,
													 parentrel);
		else
			elog(ERROR, "childrel is not a child of parentrel");
	}

	/* Now translate for this child. */
	appinfos = find_appinfos_by_relids(root, childrel->relids, &nappinfos);

	node = adjust_appendrel_attrs(root, node, nappinfos, appinfos);

	pfree(appinfos);

	return node;
}

/*
 * Substitute child relids for parent relids in a Relid set.  The array of
 * appinfos specifies the substitutions to be performed.
 */
Relids
adjust_child_relids(Relids relids, int nappinfos, AppendRelInfo **appinfos)
{
	Bitmapset  *result = NULL;
	int			cnt;

	for (cnt = 0; cnt < nappinfos; cnt++)
	{
		AppendRelInfo *appinfo = appinfos[cnt];

		/* Remove parent, add child */
		if (bms_is_member(appinfo->parent_relid, relids))
		{
			/* Make a copy if we are changing the set. */
			if (!result)
				result = bms_copy(relids);

			result = bms_del_member(result, appinfo->parent_relid);
			result = bms_add_member(result, appinfo->child_relid);
		}
	}

	/* If we made any changes, return the modified copy. */
	if (result)
		return result;

	/* Otherwise, return the original set without modification. */
	return relids;
}

/*
 * Substitute child's relids for parent's relids in a Relid set.
 * The childrel can be multiple inheritance levels below the parent.
 */
Relids
adjust_child_relids_multilevel(PlannerInfo *root, Relids relids,
							   RelOptInfo *childrel,
							   RelOptInfo *parentrel)
{
	AppendRelInfo **appinfos;
	int			nappinfos;

	/*
	 * If the given relids set doesn't contain any of the parent relids, it
	 * will remain unchanged.
	 */
	if (!bms_overlap(relids, parentrel->relids))
		return relids;

	/* Recurse if immediate parent is not the top parent. */
	if (childrel->parent != parentrel)
	{
		if (childrel->parent)
			relids = adjust_child_relids_multilevel(root, relids,
													childrel->parent,
													parentrel);
		else
			elog(ERROR, "childrel is not a child of parentrel");
	}

	/* Now translate for this child. */
	appinfos = find_appinfos_by_relids(root, childrel->relids, &nappinfos);

	relids = adjust_child_relids(relids, nappinfos, appinfos);

	pfree(appinfos);

	return relids;
}

/*
 * adjust_inherited_attnums
 *	  Translate an integer list of attribute numbers from parent to child.
 */
List *
adjust_inherited_attnums(List *attnums, AppendRelInfo *context)
{
	List	   *result = NIL;
	ListCell   *lc;

	/* This should only happen for an inheritance case, not UNION ALL */
	Assert(OidIsValid(context->parent_reloid));

	/* Look up each attribute in the AppendRelInfo's translated_vars list */
	foreach(lc, attnums)
	{
		AttrNumber	parentattno = lfirst_int(lc);
		Var		   *childvar;

		/* Look up the translation of this column: it must be a Var */
		if (parentattno <= 0 ||
			parentattno > list_length(context->translated_vars))
			elog(ERROR, "attribute %d of relation \"%s\" does not exist",
				 parentattno, get_rel_name(context->parent_reloid));
		childvar = (Var *) list_nth(context->translated_vars, parentattno - 1);
		if (childvar == NULL || !IsA(childvar, Var))
			elog(ERROR, "attribute %d of relation \"%s\" does not exist",
				 parentattno, get_rel_name(context->parent_reloid));

		result = lappend_int(result, childvar->varattno);
	}
	return result;
}

/*
 * adjust_inherited_attnums_multilevel
 *	  As above, but traverse multiple inheritance levels as needed.
 */
List *
adjust_inherited_attnums_multilevel(PlannerInfo *root, List *attnums,
									Index child_relid, Index top_parent_relid)
{
	AppendRelInfo *appinfo = root->append_rel_array[child_relid];

	if (!appinfo)
		elog(ERROR, "child rel %d not found in append_rel_array", child_relid);

	/* Recurse if immediate parent is not the top parent. */
	if (appinfo->parent_relid != top_parent_relid)
		attnums = adjust_inherited_attnums_multilevel(root, attnums,
													  appinfo->parent_relid,
													  top_parent_relid);

	/* Now translate for this child */
	return adjust_inherited_attnums(attnums, appinfo);
}

/*
 * get_translated_update_targetlist
 *	  Get the processed_tlist of an UPDATE query, translated as needed to
 *	  match a child target relation.
 *
 * Optionally also return the list of target column numbers translated
 * to this target relation.  (The resnos in processed_tlist MUST NOT be
 * relied on for this purpose.)
 */
void
get_translated_update_targetlist(PlannerInfo *root, Index relid,
								 List **processed_tlist, List **update_colnos)
{
	/* This is pretty meaningless for commands other than UPDATE. */
	Assert(root->parse->commandType == CMD_UPDATE);
	if (relid == root->parse->resultRelation)
	{
		/*
		 * Non-inheritance case, so it's easy.  The caller might be expecting
		 * a tree it can scribble on, though, so copy.
		 */
		*processed_tlist = copyObject(root->processed_tlist);
		if (update_colnos)
			*update_colnos = copyObject(root->update_colnos);
	}
	else
	{
		Assert(bms_is_member(relid, root->all_result_relids));
		*processed_tlist = (List *)
			adjust_appendrel_attrs_multilevel(root,
											  (Node *) root->processed_tlist,
											  find_base_rel(root, relid),
											  find_base_rel(root, root->parse->resultRelation));
		if (update_colnos)
			*update_colnos =
				adjust_inherited_attnums_multilevel(root, root->update_colnos,
													relid,
													root->parse->resultRelation);
	}
}

/*
 * find_appinfos_by_relids
 * 		Find AppendRelInfo structures for base relations listed in relids.
 *
 * The relids argument is typically a join relation's relids, which can
 * include outer-join RT indexes in addition to baserels.  We silently
 * ignore the outer joins.
 *
 * The AppendRelInfos are returned in an array, which can be pfree'd by the
 * caller. *nappinfos is set to the number of entries in the array.
 */
AppendRelInfo **
find_appinfos_by_relids(PlannerInfo *root, Relids relids, int *nappinfos)
{
	AppendRelInfo **appinfos;
	int			cnt = 0;
	int			i;

	/* Allocate an array that's certainly big enough */
	appinfos = (AppendRelInfo **)
		palloc(sizeof(AppendRelInfo *) * bms_num_members(relids));

	i = -1;
	while ((i = bms_next_member(relids, i)) >= 0)
	{
		AppendRelInfo *appinfo = root->append_rel_array[i];

		if (!appinfo)
		{
			/* Probably i is an OJ index, but let's check */
			if (find_base_rel_ignore_join(root, i) == NULL)
				continue;
			/* It's a base rel, but we lack an append_rel_array entry */
			elog(ERROR, "child rel %d not found in append_rel_array", i);
		}

		appinfos[cnt++] = appinfo;
	}
	*nappinfos = cnt;
	return appinfos;
}


/*****************************************************************************
 *
 *		ROW-IDENTITY VARIABLE MANAGEMENT
 *
 * This code lacks a good home, perhaps.  We choose to keep it here because
 * adjust_appendrel_attrs_mutator() is its principal co-conspirator.  That
 * function does most of what is needed to expand ROWID_VAR Vars into the
 * right things.
 *
 *****************************************************************************/

/*
 * add_row_identity_var
 *	  Register a row-identity column to be used in UPDATE/DELETE/MERGE.
 *
 * The Var must be equal(), aside from varno, to any other row-identity
 * column with the same rowid_name.  Thus, for example, "wholerow"
 * row identities had better use vartype == RECORDOID.
 *
 * rtindex is currently redundant with rowid_var->varno, but we specify
 * it as a separate parameter in case this is ever generalized to support
 * non-Var expressions.  (We could reasonably handle expressions over
 * Vars of the specified rtindex, but for now that seems unnecessary.)
 */
void
add_row_identity_var(PlannerInfo *root, Var *orig_var,
					 Index rtindex, const char *rowid_name)
{
	TargetEntry *tle;
	Var		   *rowid_var;
	RowIdentityVarInfo *ridinfo;
	ListCell   *lc;

	/* For now, the argument must be just a Var of the given rtindex */
	Assert(IsA(orig_var, Var));
	Assert(orig_var->varno == rtindex);
	Assert(orig_var->varlevelsup == 0);
	Assert(orig_var->varnullingrels == NULL);

	/*
	 * If we're doing non-inherited UPDATE/DELETE/MERGE, there's little need
	 * for ROWID_VAR shenanigans.  Just shove the presented Var into the
	 * processed_tlist, and we're done.
	 */
	if (rtindex == root->parse->resultRelation)
	{
		tle = makeTargetEntry((Expr *) orig_var,
							  list_length(root->processed_tlist) + 1,
							  pstrdup(rowid_name),
							  true);
		root->processed_tlist = lappend(root->processed_tlist, tle);
		return;
	}

	/*
	 * Otherwise, rtindex should reference a leaf target relation that's being
	 * added to the query during expand_inherited_rtentry().
	 */
	Assert(bms_is_member(rtindex, root->leaf_result_relids));
	Assert(root->append_rel_array[rtindex] != NULL);

	/*
	 * We have to find a matching RowIdentityVarInfo, or make one if there is
	 * none.  To allow using equal() to match the vars, change the varno to
	 * ROWID_VAR, leaving all else alone.
	 */
	rowid_var = copyObject(orig_var);
	/* This could eventually become ChangeVarNodes() */
	rowid_var->varno = ROWID_VAR;

	/* Look for an existing row-id column of the same name */
	foreach(lc, root->row_identity_vars)
	{
		ridinfo = (RowIdentityVarInfo *) lfirst(lc);
		if (strcmp(rowid_name, ridinfo->rowidname) != 0)
			continue;
		if (equal(rowid_var, ridinfo->rowidvar))
		{
			/* Found a match; we need only record that rtindex needs it too */
			ridinfo->rowidrels = bms_add_member(ridinfo->rowidrels, rtindex);
			return;
		}
		else
		{
			/* Ooops, can't handle this */
			elog(ERROR, "conflicting uses of row-identity name \"%s\"",
				 rowid_name);
		}
	}

	/* No request yet, so add a new RowIdentityVarInfo */
	ridinfo = makeNode(RowIdentityVarInfo);
	ridinfo->rowidvar = copyObject(rowid_var);
	/* for the moment, estimate width using just the datatype info */
	ridinfo->rowidwidth = get_typavgwidth(exprType((Node *) rowid_var),
										  exprTypmod((Node *) rowid_var));
	ridinfo->rowidname = pstrdup(rowid_name);
	ridinfo->rowidrels = bms_make_singleton(rtindex);

	root->row_identity_vars = lappend(root->row_identity_vars, ridinfo);

	/* Change rowid_var into a reference to this row_identity_vars entry */
	rowid_var->varattno = list_length(root->row_identity_vars);

	/* Push the ROWID_VAR reference variable into processed_tlist */
	tle = makeTargetEntry((Expr *) rowid_var,
						  list_length(root->processed_tlist) + 1,
						  pstrdup(rowid_name),
						  true);
	root->processed_tlist = lappend(root->processed_tlist, tle);
}

/*
 * add_row_identity_columns
 *
 * This function adds the row identity columns needed by the core code.
 * FDWs might call add_row_identity_var() for themselves to add nonstandard
 * columns.  (Duplicate requests are fine.)
 */
void
add_row_identity_columns(PlannerInfo *root, Index rtindex,
						 RangeTblEntry *target_rte,
						 Relation target_relation)
{
	CmdType		commandType = root->parse->commandType;
	char		relkind = target_relation->rd_rel->relkind;
	Var		   *var;

	Assert(commandType == CMD_UPDATE || commandType == CMD_DELETE || commandType == CMD_MERGE);

	if (relkind == RELKIND_RELATION ||
		relkind == RELKIND_MATVIEW ||
		relkind == RELKIND_PARTITIONED_TABLE)
	{
		/*
		 * Emit CTID so that executor can find the row to merge, update or
		 * delete.
		 */
		var = makeVar(rtindex,
					  SelfItemPointerAttributeNumber,
					  TIDOID,
					  -1,
					  InvalidOid,
					  0);
		add_row_identity_var(root, var, rtindex, "ctid");
	}
	else if (relkind == RELKIND_FOREIGN_TABLE)
	{
		/*
		 * Let the foreign table's FDW add whatever junk TLEs it wants.
		 */
		FdwRoutine *fdwroutine;

		fdwroutine = GetFdwRoutineForRelation(target_relation, false);

		if (fdwroutine->AddForeignUpdateTargets != NULL)
			fdwroutine->AddForeignUpdateTargets(root, rtindex,
												target_rte, target_relation);

		/*
		 * For UPDATE, we need to make the FDW fetch unchanged columns by
		 * asking it to fetch a whole-row Var.  That's because the top-level
		 * targetlist only contains entries for changed columns, but
		 * ExecUpdate will need to build the complete new tuple.  (Actually,
		 * we only really need this in UPDATEs that are not pushed to the
		 * remote side, but it's hard to tell if that will be the case at the
		 * point when this function is called.)
		 *
		 * We will also need the whole row if there are any row triggers, so
		 * that the executor will have the "old" row to pass to the trigger.
		 * Alas, this misses system columns.
		 */
		if (commandType == CMD_UPDATE ||
			(target_relation->trigdesc &&
			 (target_relation->trigdesc->trig_delete_after_row ||
			  target_relation->trigdesc->trig_delete_before_row)))
		{
			var = makeVar(rtindex,
						  InvalidAttrNumber,
						  RECORDOID,
						  -1,
						  InvalidOid,
						  0);
			add_row_identity_var(root, var, rtindex, "wholerow");
		}
	}
}

/*
 * distribute_row_identity_vars
 *
 * After we have finished identifying all the row identity columns
 * needed by an inherited UPDATE/DELETE/MERGE query, make sure that
 * these columns will be generated by all the target relations.
 *
 * This is more or less like what build_base_rel_tlists() does,
 * except that it would not understand what to do with ROWID_VAR Vars.
 * Since that function runs before inheritance relations are expanded,
 * it will never see any such Vars anyway.
 */
void
distribute_row_identity_vars(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	int			result_relation = parse->resultRelation;
	RangeTblEntry *target_rte;
	RelOptInfo *target_rel;
	ListCell   *lc;

	/*
	 * There's nothing to do if this isn't an inherited UPDATE/DELETE/MERGE.
	 */
	if (parse->commandType != CMD_UPDATE && parse->commandType != CMD_DELETE &&
		parse->commandType != CMD_MERGE)
	{
		Assert(root->row_identity_vars == NIL);
		return;
	}
	target_rte = rt_fetch(result_relation, parse->rtable);
	if (!target_rte->inh)
	{
		Assert(root->row_identity_vars == NIL);
		return;
	}

	/*
	 * Ordinarily, we expect that leaf result relation(s) will have added some
	 * ROWID_VAR Vars to the query.  However, it's possible that constraint
	 * exclusion suppressed every leaf relation.  The executor will get upset
	 * if the plan has no row identity columns at all, even though it will
	 * certainly process no rows.  Handle this edge case by re-opening the top
	 * result relation and adding the row identity columns it would have used,
	 * as preprocess_targetlist() would have done if it weren't marked "inh".
	 * Then re-run build_base_rel_tlists() to ensure that the added columns
	 * get propagated to the relation's reltarget.  (This is a bit ugly, but
	 * it seems better to confine the ugliness and extra cycles to this
	 * unusual corner case.)
	 */
	if (root->row_identity_vars == NIL)
	{
		Relation	target_relation;

		target_relation = table_open(target_rte->relid, NoLock);
		add_row_identity_columns(root, result_relation,
								 target_rte, target_relation);
		table_close(target_relation, NoLock);
		build_base_rel_tlists(root, root->processed_tlist);
		/* There are no ROWID_VAR Vars in this case, so we're done. */
		return;
	}

	/*
	 * Dig through the processed_tlist to find the ROWID_VAR reference Vars,
	 * and forcibly copy them into the reltarget list of the topmost target
	 * relation.  That's sufficient because they'll be copied to the
	 * individual leaf target rels (with appropriate translation) later,
	 * during appendrel expansion --- see set_append_rel_size().
	 */
	target_rel = find_base_rel(root, result_relation);

	foreach(lc, root->processed_tlist)
	{
		TargetEntry *tle = lfirst(lc);
		Var		   *var = (Var *) tle->expr;

		if (var && IsA(var, Var) && var->varno == ROWID_VAR)
		{
			target_rel->reltarget->exprs =
				lappend(target_rel->reltarget->exprs, copyObject(var));
			/* reltarget cost and width will be computed later */
		}
	}
}
