/*-------------------------------------------------------------------------
 *
 * var.c
 *	  Var node manipulation routines
 *
 * Note: for most purposes, PlaceHolderVar is considered a Var too,
 * even if its contained expression is variable-free.  Also, CurrentOfExpr
 * is treated as a Var for purposes of determining whether an expression
 * contains variables.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/var.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/placeholder.h"
#include "optimizer/prep.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"


typedef struct
{
	Relids		varnos;
	PlannerInfo *root;
	int			sublevels_up;
} pull_varnos_context;

typedef struct
{
	Bitmapset  *varattnos;
	Index		varno;
} pull_varattnos_context;

typedef struct
{
	List	   *vars;
	int			sublevels_up;
} pull_vars_context;

typedef struct
{
	int			var_location;
	int			sublevels_up;
} locate_var_of_level_context;

typedef struct
{
	List	   *varlist;
	int			flags;
} pull_var_clause_context;

typedef struct
{
	PlannerInfo *root;			/* could be NULL! */
	Query	   *query;			/* outer Query */
	int			sublevels_up;
	bool		possible_sublink;	/* could aliases include a SubLink? */
	bool		inserted_sublink;	/* have we inserted a SubLink? */
} flatten_join_alias_vars_context;

static bool pull_varnos_walker(Node *node,
							   pull_varnos_context *context);
static bool pull_varattnos_walker(Node *node, pull_varattnos_context *context);
static bool pull_vars_walker(Node *node, pull_vars_context *context);
static bool contain_var_clause_walker(Node *node, void *context);
static bool contain_vars_of_level_walker(Node *node, int *sublevels_up);
static bool locate_var_of_level_walker(Node *node,
									   locate_var_of_level_context *context);
static bool pull_var_clause_walker(Node *node,
								   pull_var_clause_context *context);
static Node *flatten_join_alias_vars_mutator(Node *node,
											 flatten_join_alias_vars_context *context);
static Node *add_nullingrels_if_needed(PlannerInfo *root, Node *newnode,
									   Var *oldvar);
static bool is_standard_join_alias_expression(Node *newnode, Var *oldvar);
static void adjust_standard_join_alias_expression(Node *newnode, Var *oldvar);
static Relids alias_relid_set(Query *query, Relids relids);


/*
 * pull_varnos
 *		Create a set of all the distinct varnos present in a parsetree.
 *		Only varnos that reference level-zero rtable entries are considered.
 *
 * The result includes outer-join relids mentioned in Var.varnullingrels and
 * PlaceHolderVar.phnullingrels fields in the parsetree.
 *
 * "root" can be passed as NULL if it is not necessary to process
 * PlaceHolderVars.
 *
 * NOTE: this is used on not-yet-planned expressions.  It may therefore find
 * bare SubLinks, and if so it needs to recurse into them to look for uplevel
 * references to the desired rtable level!	But when we find a completed
 * SubPlan, we only need to look at the parameters passed to the subplan.
 */
Relids
pull_varnos(PlannerInfo *root, Node *node)
{
	pull_varnos_context context;

	context.varnos = NULL;
	context.root = root;
	context.sublevels_up = 0;

	/*
	 * Must be prepared to start with a Query or a bare expression tree; if
	 * it's a Query, we don't want to increment sublevels_up.
	 */
	query_or_expression_tree_walker(node,
									pull_varnos_walker,
									(void *) &context,
									0);

	return context.varnos;
}

/*
 * pull_varnos_of_level
 *		Create a set of all the distinct varnos present in a parsetree.
 *		Only Vars of the specified level are considered.
 */
Relids
pull_varnos_of_level(PlannerInfo *root, Node *node, int levelsup)
{
	pull_varnos_context context;

	context.varnos = NULL;
	context.root = root;
	context.sublevels_up = levelsup;

	/*
	 * Must be prepared to start with a Query or a bare expression tree; if
	 * it's a Query, we don't want to increment sublevels_up.
	 */
	query_or_expression_tree_walker(node,
									pull_varnos_walker,
									(void *) &context,
									0);

	return context.varnos;
}

static bool
pull_varnos_walker(Node *node, pull_varnos_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == context->sublevels_up)
		{
			context->varnos = bms_add_member(context->varnos, var->varno);
			context->varnos = bms_add_members(context->varnos,
											  var->varnullingrels);
		}
		return false;
	}
	if (IsA(node, CurrentOfExpr))
	{
		CurrentOfExpr *cexpr = (CurrentOfExpr *) node;

		if (context->sublevels_up == 0)
			context->varnos = bms_add_member(context->varnos, cexpr->cvarno);
		return false;
	}
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		/*
		 * If a PlaceHolderVar is not of the target query level, ignore it,
		 * instead recursing into its expression to see if it contains any
		 * vars that are of the target level.  We'll also do that when the
		 * caller doesn't pass a "root" pointer.  (We probably shouldn't see
		 * PlaceHolderVars at all in such cases, but if we do, this is a
		 * reasonable behavior.)
		 */
		if (phv->phlevelsup == context->sublevels_up &&
			context->root != NULL)
		{
			/*
			 * Ideally, the PHV's contribution to context->varnos is its
			 * ph_eval_at set.  However, this code can be invoked before
			 * that's been computed.  If we cannot find a PlaceHolderInfo,
			 * fall back to the conservative assumption that the PHV will be
			 * evaluated at its syntactic level (phv->phrels).
			 *
			 * Another problem is that a PlaceHolderVar can appear in quals or
			 * tlists that have been translated for use in a child appendrel.
			 * Typically such a PHV is a parameter expression sourced by some
			 * other relation, so that the translation from parent appendrel
			 * to child doesn't change its phrels, and we should still take
			 * ph_eval_at at face value.  But in corner cases, the PHV's
			 * original phrels can include the parent appendrel itself, in
			 * which case the translated PHV will have the child appendrel in
			 * phrels, and we must translate ph_eval_at to match.
			 */
			PlaceHolderInfo *phinfo = NULL;

			if (phv->phlevelsup == 0)
			{
				if (phv->phid < context->root->placeholder_array_size)
					phinfo = context->root->placeholder_array[phv->phid];
			}
			if (phinfo == NULL)
			{
				/* No PlaceHolderInfo yet, use phrels */
				context->varnos = bms_add_members(context->varnos,
												  phv->phrels);
			}
			else if (bms_equal(phv->phrels, phinfo->ph_var->phrels))
			{
				/* Normal case: use ph_eval_at */
				context->varnos = bms_add_members(context->varnos,
												  phinfo->ph_eval_at);
			}
			else
			{
				/* Translated PlaceHolderVar: translate ph_eval_at to match */
				Relids		newevalat,
							delta;

				/* remove what was removed from phv->phrels ... */
				delta = bms_difference(phinfo->ph_var->phrels, phv->phrels);
				newevalat = bms_difference(phinfo->ph_eval_at, delta);
				/* ... then if that was in fact part of ph_eval_at ... */
				if (!bms_equal(newevalat, phinfo->ph_eval_at))
				{
					/* ... add what was added */
					delta = bms_difference(phv->phrels, phinfo->ph_var->phrels);
					newevalat = bms_join(newevalat, delta);
				}
				context->varnos = bms_join(context->varnos,
										   newevalat);
			}

			/*
			 * In all three cases, include phnullingrels in the result.  We
			 * don't worry about possibly needing to translate it, because
			 * appendrels only translate varnos of baserels, not outer joins.
			 */
			context->varnos = bms_add_members(context->varnos,
											  phv->phnullingrels);
			return false;		/* don't recurse into expression */
		}
	}
	else if (IsA(node, Query))
	{
		/* Recurse into RTE subquery or not-yet-planned sublink subquery */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node, pull_varnos_walker,
								   (void *) context, 0);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, pull_varnos_walker,
								  (void *) context);
}


/*
 * pull_varattnos
 *		Find all the distinct attribute numbers present in an expression tree,
 *		and add them to the initial contents of *varattnos.
 *		Only Vars of the given varno and rtable level zero are considered.
 *
 * Attribute numbers are offset by FirstLowInvalidHeapAttributeNumber so that
 * we can include system attributes (e.g., OID) in the bitmap representation.
 *
 * Currently, this does not support unplanned subqueries; that is not needed
 * for current uses.  It will handle already-planned SubPlan nodes, though,
 * looking into only the "testexpr" and the "args" list.  (The subplan cannot
 * contain any other references to Vars of the current level.)
 */
void
pull_varattnos(Node *node, Index varno, Bitmapset **varattnos)
{
	pull_varattnos_context context;

	context.varattnos = *varattnos;
	context.varno = varno;

	(void) pull_varattnos_walker(node, &context);

	*varattnos = context.varattnos;
}

static bool
pull_varattnos_walker(Node *node, pull_varattnos_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varno == context->varno && var->varlevelsup == 0)
			context->varattnos =
				bms_add_member(context->varattnos,
							   var->varattno - FirstLowInvalidHeapAttributeNumber);
		return false;
	}

	/* Should not find an unplanned subquery */
	Assert(!IsA(node, Query));

	return expression_tree_walker(node, pull_varattnos_walker,
								  (void *) context);
}


/*
 * pull_vars_of_level
 *		Create a list of all Vars (and PlaceHolderVars) referencing the
 *		specified query level in the given parsetree.
 *
 * Caution: the Vars are not copied, only linked into the list.
 */
List *
pull_vars_of_level(Node *node, int levelsup)
{
	pull_vars_context context;

	context.vars = NIL;
	context.sublevels_up = levelsup;

	/*
	 * Must be prepared to start with a Query or a bare expression tree; if
	 * it's a Query, we don't want to increment sublevels_up.
	 */
	query_or_expression_tree_walker(node,
									pull_vars_walker,
									(void *) &context,
									0);

	return context.vars;
}

static bool
pull_vars_walker(Node *node, pull_vars_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == context->sublevels_up)
			context->vars = lappend(context->vars, var);
		return false;
	}
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		if (phv->phlevelsup == context->sublevels_up)
			context->vars = lappend(context->vars, phv);
		/* we don't want to look into the contained expression */
		return false;
	}
	if (IsA(node, Query))
	{
		/* Recurse into RTE subquery or not-yet-planned sublink subquery */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node, pull_vars_walker,
								   (void *) context, 0);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, pull_vars_walker,
								  (void *) context);
}


/*
 * contain_var_clause
 *	  Recursively scan a clause to discover whether it contains any Var nodes
 *	  (of the current query level).
 *
 *	  Returns true if any varnode found.
 *
 * Does not examine subqueries, therefore must only be used after reduction
 * of sublinks to subplans!
 */
bool
contain_var_clause(Node *node)
{
	return contain_var_clause_walker(node, NULL);
}

static bool
contain_var_clause_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		if (((Var *) node)->varlevelsup == 0)
			return true;		/* abort the tree traversal and return true */
		return false;
	}
	if (IsA(node, CurrentOfExpr))
		return true;
	if (IsA(node, PlaceHolderVar))
	{
		if (((PlaceHolderVar *) node)->phlevelsup == 0)
			return true;		/* abort the tree traversal and return true */
		/* else fall through to check the contained expr */
	}
	return expression_tree_walker(node, contain_var_clause_walker, context);
}


/*
 * contain_vars_of_level
 *	  Recursively scan a clause to discover whether it contains any Var nodes
 *	  of the specified query level.
 *
 *	  Returns true if any such Var found.
 *
 * Will recurse into sublinks.  Also, may be invoked directly on a Query.
 */
bool
contain_vars_of_level(Node *node, int levelsup)
{
	int			sublevels_up = levelsup;

	return query_or_expression_tree_walker(node,
										   contain_vars_of_level_walker,
										   (void *) &sublevels_up,
										   0);
}

static bool
contain_vars_of_level_walker(Node *node, int *sublevels_up)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		if (((Var *) node)->varlevelsup == *sublevels_up)
			return true;		/* abort tree traversal and return true */
		return false;
	}
	if (IsA(node, CurrentOfExpr))
	{
		if (*sublevels_up == 0)
			return true;
		return false;
	}
	if (IsA(node, PlaceHolderVar))
	{
		if (((PlaceHolderVar *) node)->phlevelsup == *sublevels_up)
			return true;		/* abort the tree traversal and return true */
		/* else fall through to check the contained expr */
	}
	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		bool		result;

		(*sublevels_up)++;
		result = query_tree_walker((Query *) node,
								   contain_vars_of_level_walker,
								   (void *) sublevels_up,
								   0);
		(*sublevels_up)--;
		return result;
	}
	return expression_tree_walker(node,
								  contain_vars_of_level_walker,
								  (void *) sublevels_up);
}


/*
 * locate_var_of_level
 *	  Find the parse location of any Var of the specified query level.
 *
 * Returns -1 if no such Var is in the querytree, or if they all have
 * unknown parse location.  (The former case is probably caller error,
 * but we don't bother to distinguish it from the latter case.)
 *
 * Will recurse into sublinks.  Also, may be invoked directly on a Query.
 *
 * Note: it might seem appropriate to merge this functionality into
 * contain_vars_of_level, but that would complicate that function's API.
 * Currently, the only uses of this function are for error reporting,
 * and so shaving cycles probably isn't very important.
 */
int
locate_var_of_level(Node *node, int levelsup)
{
	locate_var_of_level_context context;

	context.var_location = -1;	/* in case we find nothing */
	context.sublevels_up = levelsup;

	(void) query_or_expression_tree_walker(node,
										   locate_var_of_level_walker,
										   (void *) &context,
										   0);

	return context.var_location;
}

static bool
locate_var_of_level_walker(Node *node,
						   locate_var_of_level_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == context->sublevels_up &&
			var->location >= 0)
		{
			context->var_location = var->location;
			return true;		/* abort tree traversal and return true */
		}
		return false;
	}
	if (IsA(node, CurrentOfExpr))
	{
		/* since CurrentOfExpr doesn't carry location, nothing we can do */
		return false;
	}
	/* No extra code needed for PlaceHolderVar; just look in contained expr */
	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node,
								   locate_var_of_level_walker,
								   (void *) context,
								   0);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node,
								  locate_var_of_level_walker,
								  (void *) context);
}


/*
 * pull_var_clause
 *	  Recursively pulls all Var nodes from an expression clause.
 *
 *	  Aggrefs are handled according to these bits in 'flags':
 *		PVC_INCLUDE_AGGREGATES		include Aggrefs in output list
 *		PVC_RECURSE_AGGREGATES		recurse into Aggref arguments
 *		neither flag				throw error if Aggref found
 *	  Vars within an Aggref's expression are included in the result only
 *	  when PVC_RECURSE_AGGREGATES is specified.
 *
 *	  WindowFuncs are handled according to these bits in 'flags':
 *		PVC_INCLUDE_WINDOWFUNCS		include WindowFuncs in output list
 *		PVC_RECURSE_WINDOWFUNCS		recurse into WindowFunc arguments
 *		neither flag				throw error if WindowFunc found
 *	  Vars within a WindowFunc's expression are included in the result only
 *	  when PVC_RECURSE_WINDOWFUNCS is specified.
 *
 *	  PlaceHolderVars are handled according to these bits in 'flags':
 *		PVC_INCLUDE_PLACEHOLDERS	include PlaceHolderVars in output list
 *		PVC_RECURSE_PLACEHOLDERS	recurse into PlaceHolderVar arguments
 *		neither flag				throw error if PlaceHolderVar found
 *	  Vars within a PHV's expression are included in the result only
 *	  when PVC_RECURSE_PLACEHOLDERS is specified.
 *
 *	  GroupingFuncs are treated exactly like Aggrefs, and so do not need
 *	  their own flag bits.
 *
 *	  CurrentOfExpr nodes are ignored in all cases.
 *
 *	  Upper-level vars (with varlevelsup > 0) should not be seen here,
 *	  likewise for upper-level Aggrefs and PlaceHolderVars.
 *
 *	  Returns list of nodes found.  Note the nodes themselves are not
 *	  copied, only referenced.
 *
 * Does not examine subqueries, therefore must only be used after reduction
 * of sublinks to subplans!
 */
List *
pull_var_clause(Node *node, int flags)
{
	pull_var_clause_context context;

	/* Assert that caller has not specified inconsistent flags */
	Assert((flags & (PVC_INCLUDE_AGGREGATES | PVC_RECURSE_AGGREGATES))
		   != (PVC_INCLUDE_AGGREGATES | PVC_RECURSE_AGGREGATES));
	Assert((flags & (PVC_INCLUDE_WINDOWFUNCS | PVC_RECURSE_WINDOWFUNCS))
		   != (PVC_INCLUDE_WINDOWFUNCS | PVC_RECURSE_WINDOWFUNCS));
	Assert((flags & (PVC_INCLUDE_PLACEHOLDERS | PVC_RECURSE_PLACEHOLDERS))
		   != (PVC_INCLUDE_PLACEHOLDERS | PVC_RECURSE_PLACEHOLDERS));

	context.varlist = NIL;
	context.flags = flags;

	pull_var_clause_walker(node, &context);
	return context.varlist;
}

static bool
pull_var_clause_walker(Node *node, pull_var_clause_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		if (((Var *) node)->varlevelsup != 0)
			elog(ERROR, "Upper-level Var found where not expected");
		context->varlist = lappend(context->varlist, node);
		return false;
	}
	else if (IsA(node, Aggref))
	{
		if (((Aggref *) node)->agglevelsup != 0)
			elog(ERROR, "Upper-level Aggref found where not expected");
		if (context->flags & PVC_INCLUDE_AGGREGATES)
		{
			context->varlist = lappend(context->varlist, node);
			/* we do NOT descend into the contained expression */
			return false;
		}
		else if (context->flags & PVC_RECURSE_AGGREGATES)
		{
			/* fall through to recurse into the aggregate's arguments */
		}
		else
			elog(ERROR, "Aggref found where not expected");
	}
	else if (IsA(node, GroupingFunc))
	{
		if (((GroupingFunc *) node)->agglevelsup != 0)
			elog(ERROR, "Upper-level GROUPING found where not expected");
		if (context->flags & PVC_INCLUDE_AGGREGATES)
		{
			context->varlist = lappend(context->varlist, node);
			/* we do NOT descend into the contained expression */
			return false;
		}
		else if (context->flags & PVC_RECURSE_AGGREGATES)
		{
			/* fall through to recurse into the GroupingFunc's arguments */
		}
		else
			elog(ERROR, "GROUPING found where not expected");
	}
	else if (IsA(node, WindowFunc))
	{
		/* WindowFuncs have no levelsup field to check ... */
		if (context->flags & PVC_INCLUDE_WINDOWFUNCS)
		{
			context->varlist = lappend(context->varlist, node);
			/* we do NOT descend into the contained expressions */
			return false;
		}
		else if (context->flags & PVC_RECURSE_WINDOWFUNCS)
		{
			/* fall through to recurse into the windowfunc's arguments */
		}
		else
			elog(ERROR, "WindowFunc found where not expected");
	}
	else if (IsA(node, PlaceHolderVar))
	{
		if (((PlaceHolderVar *) node)->phlevelsup != 0)
			elog(ERROR, "Upper-level PlaceHolderVar found where not expected");
		if (context->flags & PVC_INCLUDE_PLACEHOLDERS)
		{
			context->varlist = lappend(context->varlist, node);
			/* we do NOT descend into the contained expression */
			return false;
		}
		else if (context->flags & PVC_RECURSE_PLACEHOLDERS)
		{
			/* fall through to recurse into the placeholder's expression */
		}
		else
			elog(ERROR, "PlaceHolderVar found where not expected");
	}
	return expression_tree_walker(node, pull_var_clause_walker,
								  (void *) context);
}


/*
 * flatten_join_alias_vars
 *	  Replace Vars that reference JOIN outputs with references to the original
 *	  relation variables instead.  This allows quals involving such vars to be
 *	  pushed down.  Whole-row Vars that reference JOIN relations are expanded
 *	  into RowExpr constructs that name the individual output Vars.  This
 *	  is necessary since we will not scan the JOIN as a base relation, which
 *	  is the only way that the executor can directly handle whole-row Vars.
 *
 * This also adjusts relid sets found in some expression node types to
 * substitute the contained base+OJ rels for any join relid.
 *
 * If a JOIN contains sub-selects that have been flattened, its join alias
 * entries might now be arbitrary expressions, not just Vars.  This affects
 * this function in two important ways.  First, we might find ourselves
 * inserting SubLink expressions into subqueries, and we must make sure that
 * their Query.hasSubLinks fields get set to true if so.  If there are any
 * SubLinks in the join alias lists, the outer Query should already have
 * hasSubLinks = true, so this is only relevant to un-flattened subqueries.
 * Second, we have to preserve any varnullingrels info attached to the
 * alias Vars we're replacing.  If the replacement expression is a Var or
 * PlaceHolderVar or constructed from those, we can just add the
 * varnullingrels bits to the existing nullingrels field(s); otherwise
 * we have to add a PlaceHolderVar wrapper.
 *
 * NOTE: this is also used by the parser, to expand join alias Vars before
 * checking GROUP BY validity.  For that use-case, root will be NULL, which
 * is why we have to pass the Query separately.  We need the root itself only
 * for making PlaceHolderVars.  We can avoid making PlaceHolderVars in the
 * parser's usage because it won't be dealing with arbitrary expressions:
 * so long as adjust_standard_join_alias_expression can handle everything
 * the parser would make as a join alias expression, we're OK.
 */
Node *
flatten_join_alias_vars(PlannerInfo *root, Query *query, Node *node)
{
	flatten_join_alias_vars_context context;

	/*
	 * We do not expect this to be applied to the whole Query, only to
	 * expressions or LATERAL subqueries.  Hence, if the top node is a Query,
	 * it's okay to immediately increment sublevels_up.
	 */
	Assert(node != (Node *) query);

	context.root = root;
	context.query = query;
	context.sublevels_up = 0;
	/* flag whether join aliases could possibly contain SubLinks */
	context.possible_sublink = query->hasSubLinks;
	/* if hasSubLinks is already true, no need to work hard */
	context.inserted_sublink = query->hasSubLinks;

	return flatten_join_alias_vars_mutator(node, &context);
}

static Node *
flatten_join_alias_vars_mutator(Node *node,
								flatten_join_alias_vars_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		RangeTblEntry *rte;
		Node	   *newvar;

		/* No change unless Var belongs to a JOIN of the target level */
		if (var->varlevelsup != context->sublevels_up)
			return node;		/* no need to copy, really */
		rte = rt_fetch(var->varno, context->query->rtable);
		if (rte->rtekind != RTE_JOIN)
			return node;
		if (var->varattno == InvalidAttrNumber)
		{
			/* Must expand whole-row reference */
			RowExpr    *rowexpr;
			List	   *fields = NIL;
			List	   *colnames = NIL;
			ListCell   *lv;
			ListCell   *ln;

			Assert(list_length(rte->joinaliasvars) == list_length(rte->eref->colnames));
			forboth(lv, rte->joinaliasvars, ln, rte->eref->colnames)
			{
				newvar = (Node *) lfirst(lv);
				/* Ignore dropped columns */
				if (newvar == NULL)
					continue;
				newvar = copyObject(newvar);

				/*
				 * If we are expanding an alias carried down from an upper
				 * query, must adjust its varlevelsup fields.
				 */
				if (context->sublevels_up != 0)
					IncrementVarSublevelsUp(newvar, context->sublevels_up, 0);
				/* Preserve original Var's location, if possible */
				if (IsA(newvar, Var))
					((Var *) newvar)->location = var->location;
				/* Recurse in case join input is itself a join */
				/* (also takes care of setting inserted_sublink if needed) */
				newvar = flatten_join_alias_vars_mutator(newvar, context);
				fields = lappend(fields, newvar);
				/* We need the names of non-dropped columns, too */
				colnames = lappend(colnames, copyObject((Node *) lfirst(ln)));
			}
			rowexpr = makeNode(RowExpr);
			rowexpr->args = fields;
			rowexpr->row_typeid = var->vartype;
			rowexpr->row_format = COERCE_IMPLICIT_CAST;
			/* vartype will always be RECORDOID, so we always need colnames */
			rowexpr->colnames = colnames;
			rowexpr->location = var->location;

			/* Lastly, add any varnullingrels to the replacement expression */
			return add_nullingrels_if_needed(context->root, (Node *) rowexpr,
											 var);
		}

		/* Expand join alias reference */
		Assert(var->varattno > 0);
		newvar = (Node *) list_nth(rte->joinaliasvars, var->varattno - 1);
		Assert(newvar != NULL);
		newvar = copyObject(newvar);

		/*
		 * If we are expanding an alias carried down from an upper query, must
		 * adjust its varlevelsup fields.
		 */
		if (context->sublevels_up != 0)
			IncrementVarSublevelsUp(newvar, context->sublevels_up, 0);

		/* Preserve original Var's location, if possible */
		if (IsA(newvar, Var))
			((Var *) newvar)->location = var->location;

		/* Recurse in case join input is itself a join */
		newvar = flatten_join_alias_vars_mutator(newvar, context);

		/* Detect if we are adding a sublink to query */
		if (context->possible_sublink && !context->inserted_sublink)
			context->inserted_sublink = checkExprHasSubLink(newvar);

		/* Lastly, add any varnullingrels to the replacement expression */
		return add_nullingrels_if_needed(context->root, newvar, var);
	}
	if (IsA(node, PlaceHolderVar))
	{
		/* Copy the PlaceHolderVar node with correct mutation of subnodes */
		PlaceHolderVar *phv;

		phv = (PlaceHolderVar *) expression_tree_mutator(node,
														 flatten_join_alias_vars_mutator,
														 (void *) context);
		/* now fix PlaceHolderVar's relid sets */
		if (phv->phlevelsup == context->sublevels_up)
		{
			phv->phrels = alias_relid_set(context->query,
										  phv->phrels);
			/* we *don't* change phnullingrels */
		}
		return (Node *) phv;
	}

	if (IsA(node, Query))
	{
		/* Recurse into RTE subquery or not-yet-planned sublink subquery */
		Query	   *newnode;
		bool		save_inserted_sublink;

		context->sublevels_up++;
		save_inserted_sublink = context->inserted_sublink;
		context->inserted_sublink = ((Query *) node)->hasSubLinks;
		newnode = query_tree_mutator((Query *) node,
									 flatten_join_alias_vars_mutator,
									 (void *) context,
									 QTW_IGNORE_JOINALIASES);
		newnode->hasSubLinks |= context->inserted_sublink;
		context->inserted_sublink = save_inserted_sublink;
		context->sublevels_up--;
		return (Node *) newnode;
	}
	/* Already-planned tree not supported */
	Assert(!IsA(node, SubPlan));
	/* Shouldn't need to handle these planner auxiliary nodes here */
	Assert(!IsA(node, SpecialJoinInfo));
	Assert(!IsA(node, PlaceHolderInfo));
	Assert(!IsA(node, MinMaxAggInfo));

	return expression_tree_mutator(node, flatten_join_alias_vars_mutator,
								   (void *) context);
}

/*
 * Add oldvar's varnullingrels, if any, to a flattened join alias expression.
 * The newnode has been copied, so we can modify it freely.
 */
static Node *
add_nullingrels_if_needed(PlannerInfo *root, Node *newnode, Var *oldvar)
{
	if (oldvar->varnullingrels == NULL)
		return newnode;			/* nothing to do */
	/* If possible, do it by adding to existing nullingrel fields */
	if (is_standard_join_alias_expression(newnode, oldvar))
		adjust_standard_join_alias_expression(newnode, oldvar);
	else if (root)
	{
		/*
		 * We can insert a PlaceHolderVar to carry the nullingrels.  However,
		 * deciding where to evaluate the PHV is slightly tricky.  We first
		 * try to evaluate it at the natural semantic level of the new
		 * expression; but if that expression is variable-free, fall back to
		 * evaluating it at the join that the oldvar is an alias Var for.
		 */
		PlaceHolderVar *newphv;
		Index		levelsup = oldvar->varlevelsup;
		Relids		phrels = pull_varnos_of_level(root, newnode, levelsup);

		if (bms_is_empty(phrels))	/* variable-free? */
		{
			if (levelsup != 0)	/* this won't work otherwise */
				elog(ERROR, "unsupported join alias expression");
			phrels = get_relids_for_join(root->parse, oldvar->varno);
			/* If it's an outer join, eval below not above the join */
			phrels = bms_del_member(phrels, oldvar->varno);
			Assert(!bms_is_empty(phrels));
		}
		newphv = make_placeholder_expr(root, (Expr *) newnode, phrels);
		/* newphv has zero phlevelsup and NULL phnullingrels; fix it */
		newphv->phlevelsup = levelsup;
		newphv->phnullingrels = bms_copy(oldvar->varnullingrels);
		newnode = (Node *) newphv;
	}
	else
	{
		/* ooops, we're missing support for something the parser can make */
		elog(ERROR, "unsupported join alias expression");
	}
	return newnode;
}

/*
 * Check to see if we can insert nullingrels into this join alias expression
 * without use of a separate PlaceHolderVar.
 *
 * This will handle Vars, PlaceHolderVars, and implicit-coercion and COALESCE
 * expressions built from those.  This coverage needs to handle anything
 * that the parser would put into joinaliasvars.
 */
static bool
is_standard_join_alias_expression(Node *newnode, Var *oldvar)
{
	if (newnode == NULL)
		return false;
	if (IsA(newnode, Var) &&
		((Var *) newnode)->varlevelsup == oldvar->varlevelsup)
		return true;
	else if (IsA(newnode, PlaceHolderVar) &&
			 ((PlaceHolderVar *) newnode)->phlevelsup == oldvar->varlevelsup)
		return true;
	else if (IsA(newnode, FuncExpr))
	{
		FuncExpr   *fexpr = (FuncExpr *) newnode;

		/*
		 * We need to assume that the function wouldn't produce non-NULL from
		 * NULL, which is reasonable for implicit coercions but otherwise not
		 * so much.  (Looking at its strictness is likely overkill, and anyway
		 * it would cause us to fail if someone forgot to mark an implicit
		 * coercion as strict.)
		 */
		if (fexpr->funcformat != COERCE_IMPLICIT_CAST ||
			fexpr->args == NIL)
			return false;

		/*
		 * Examine only the first argument --- coercions might have additional
		 * arguments that are constants.
		 */
		return is_standard_join_alias_expression(linitial(fexpr->args), oldvar);
	}
	else if (IsA(newnode, RelabelType))
	{
		RelabelType *relabel = (RelabelType *) newnode;

		/* This definitely won't produce non-NULL from NULL */
		return is_standard_join_alias_expression((Node *) relabel->arg, oldvar);
	}
	else if (IsA(newnode, CoerceViaIO))
	{
		CoerceViaIO *iocoerce = (CoerceViaIO *) newnode;

		/* This definitely won't produce non-NULL from NULL */
		return is_standard_join_alias_expression((Node *) iocoerce->arg, oldvar);
	}
	else if (IsA(newnode, ArrayCoerceExpr))
	{
		ArrayCoerceExpr *acoerce = (ArrayCoerceExpr *) newnode;

		/* This definitely won't produce non-NULL from NULL (at array level) */
		return is_standard_join_alias_expression((Node *) acoerce->arg, oldvar);
	}
	else if (IsA(newnode, CoalesceExpr))
	{
		CoalesceExpr *cexpr = (CoalesceExpr *) newnode;
		ListCell   *lc;

		Assert(cexpr->args != NIL);
		foreach(lc, cexpr->args)
		{
			if (!is_standard_join_alias_expression(lfirst(lc), oldvar))
				return false;
		}
		return true;
	}
	else
		return false;
}

/*
 * Insert nullingrels into an expression accepted by
 * is_standard_join_alias_expression.
 */
static void
adjust_standard_join_alias_expression(Node *newnode, Var *oldvar)
{
	if (IsA(newnode, Var) &&
		((Var *) newnode)->varlevelsup == oldvar->varlevelsup)
	{
		Var		   *newvar = (Var *) newnode;

		newvar->varnullingrels = bms_add_members(newvar->varnullingrels,
												 oldvar->varnullingrels);
	}
	else if (IsA(newnode, PlaceHolderVar) &&
			 ((PlaceHolderVar *) newnode)->phlevelsup == oldvar->varlevelsup)
	{
		PlaceHolderVar *newphv = (PlaceHolderVar *) newnode;

		newphv->phnullingrels = bms_add_members(newphv->phnullingrels,
												oldvar->varnullingrels);
	}
	else if (IsA(newnode, FuncExpr))
	{
		FuncExpr   *fexpr = (FuncExpr *) newnode;

		adjust_standard_join_alias_expression(linitial(fexpr->args), oldvar);
	}
	else if (IsA(newnode, RelabelType))
	{
		RelabelType *relabel = (RelabelType *) newnode;

		adjust_standard_join_alias_expression((Node *) relabel->arg, oldvar);
	}
	else if (IsA(newnode, CoerceViaIO))
	{
		CoerceViaIO *iocoerce = (CoerceViaIO *) newnode;

		adjust_standard_join_alias_expression((Node *) iocoerce->arg, oldvar);
	}
	else if (IsA(newnode, ArrayCoerceExpr))
	{
		ArrayCoerceExpr *acoerce = (ArrayCoerceExpr *) newnode;

		adjust_standard_join_alias_expression((Node *) acoerce->arg, oldvar);
	}
	else if (IsA(newnode, CoalesceExpr))
	{
		CoalesceExpr *cexpr = (CoalesceExpr *) newnode;
		ListCell   *lc;

		Assert(cexpr->args != NIL);
		foreach(lc, cexpr->args)
		{
			adjust_standard_join_alias_expression(lfirst(lc), oldvar);
		}
	}
	else
		Assert(false);
}

/*
 * alias_relid_set: in a set of RT indexes, replace joins by their
 * underlying base+OJ relids
 */
static Relids
alias_relid_set(Query *query, Relids relids)
{
	Relids		result = NULL;
	int			rtindex;

	rtindex = -1;
	while ((rtindex = bms_next_member(relids, rtindex)) >= 0)
	{
		RangeTblEntry *rte = rt_fetch(rtindex, query->rtable);

		if (rte->rtekind == RTE_JOIN)
			result = bms_join(result, get_relids_for_join(query, rtindex));
		else
			result = bms_add_member(result, rtindex);
	}
	return result;
}
