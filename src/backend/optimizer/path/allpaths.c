/*-------------------------------------------------------------------------
 *
 * allpaths.c
 *	  Routines to find possible search paths for processing a query
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/allpaths.c,v 1.67 2000/11/12 00:36:58 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "optimizer/cost.h"
#include "optimizer/geqo.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "parser/parsetree.h"


bool		enable_geqo = true;
int			geqo_rels = DEFAULT_GEQO_RELS;


static void set_base_rel_pathlists(Query *root);
static void set_plain_rel_pathlist(Query *root, RelOptInfo *rel,
								   RangeTblEntry *rte);
static void set_inherited_rel_pathlist(Query *root, RelOptInfo *rel,
									   RangeTblEntry *rte,
									   List *inheritlist);
static RelOptInfo *make_one_rel_by_joins(Query *root, int levels_needed,
										 List *initial_rels);

#ifdef OPTIMIZER_DEBUG
static void debug_print_rel(Query *root, RelOptInfo *rel);

#endif


/*
 * make_one_rel
 *	  Finds all possible access paths for executing a query, returning a
 *	  single rel that represents the join of all base rels in the query.
 */
RelOptInfo *
make_one_rel(Query *root)
{
	RelOptInfo *rel;

	/*
	 * Generate access paths for the base rels.
	 */
	set_base_rel_pathlists(root);

	/*
	 * Generate access paths for the entire join tree.
	 */
	Assert(root->jointree != NULL && IsA(root->jointree, FromExpr));

	rel = make_fromexpr_rel(root, root->jointree);

	/*
	 * The result should join all the query's rels.
	 */
	Assert(length(rel->relids) == length(root->base_rel_list));

	return rel;
}

/*
 * set_base_rel_pathlists
 *	  Finds all paths available for scanning each base-relation entry.
 *	  Sequential scan and any available indices are considered.
 *	  Each useful path is attached to its relation's 'pathlist' field.
 */
static void
set_base_rel_pathlists(Query *root)
{
	List	   *rellist;

	foreach(rellist, root->base_rel_list)
	{
		RelOptInfo *rel = (RelOptInfo *) lfirst(rellist);
		Index		rti;
		RangeTblEntry *rte;
		List	   *inheritlist;

		Assert(length(rel->relids) == 1); /* better be base rel */
		rti = lfirsti(rel->relids);
		rte = rt_fetch(rti, root->rtable);

		if (rel->issubquery)
		{
			/* Subquery --- generate a separate plan for it */

			/*
			 * XXX for now, we just apply any restrict clauses that came
			 * from the outer query as qpquals of the SubqueryScan node.
			 * Later, think about pushing them down into the subquery itself.
			 */

			/* Generate the plan for the subquery */
			rel->subplan = subquery_planner(rte->subquery,
											-1.0 /* default case */ );

			/* Copy number of output rows from subplan */
			rel->tuples = rel->subplan->plan_rows;

			/* Mark rel with estimated output rows, width, etc */
			set_baserel_size_estimates(root, rel);

			/* Generate appropriate path */
			add_path(rel, create_subqueryscan_path(rel));

			/* Select cheapest path (pretty easy in this case...) */
			set_cheapest(rel);
		}
		else if ((inheritlist = expand_inherted_rtentry(root, rti)) != NIL)
		{
			/* Relation is root of an inheritance tree, process specially */
			set_inherited_rel_pathlist(root, rel, rte, inheritlist);
		}
		else
		{
			/* Plain relation */
			set_plain_rel_pathlist(root, rel, rte);
		}
	}
}

/*
 * set_plain_rel_pathlist
 *	  Build access paths for a plain relation (no subquery, no inheritance)
 */
static void
set_plain_rel_pathlist(Query *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	List	   *indices = find_secondary_indexes(rte->relid);

	/* Mark rel with estimated output rows, width, etc */
	set_baserel_size_estimates(root, rel);

	/*
	 * Generate paths and add them to the rel's pathlist.
	 *
	 * Note: add_path() will discard any paths that are dominated by
	 * another available path, keeping only those paths that are
	 * superior along at least one dimension of cost or sortedness.
	 */

	/* Consider sequential scan */
	add_path(rel, create_seqscan_path(rel));

	/* Consider TID scans */
	create_tidscan_paths(root, rel);

	/* Consider index paths for both simple and OR index clauses */
	create_index_paths(root, rel, indices,
					   rel->baserestrictinfo,
					   rel->joininfo);

	/*
	 * Note: create_or_index_paths depends on create_index_paths to
	 * have marked OR restriction clauses with relevant indices; this
	 * is why it doesn't need to be given the list of indices.
	 */
	create_or_index_paths(root, rel, rel->baserestrictinfo);

	/* Now find the cheapest of the paths for this rel */
	set_cheapest(rel);
}

/*
 * set_inherited_rel_pathlist
 *	  Build access paths for a inheritance tree rooted at rel
 *
 * inheritlist is a list of RT indexes of all tables in the inheritance tree,
 * including the parent itself.  Note we will not come here unless there's
 * at least one child in addition to the parent.
 */
static void
set_inherited_rel_pathlist(Query *root, RelOptInfo *rel, RangeTblEntry *rte,
						   List *inheritlist)
{
	int			parentRTindex = lfirsti(rel->relids);
	Oid			parentOID = rte->relid;
	List	   *subpaths = NIL;
	List	   *il;

	/*
	 * XXX for now, can't handle inherited expansion of FOR UPDATE;
	 * can we do better?
	 */
	if (intMember(parentRTindex, root->rowMarks))
		elog(ERROR, "SELECT FOR UPDATE is not supported for inherit queries");

	/*
	 * Recompute size estimates for whole inheritance tree
	 */
	rel->rows = 0;
	rel->width = 0;

	/*
	 * Generate access paths for each table in the tree (parent AND children),
	 * and pick the cheapest path for each table.
	 */
	foreach(il, inheritlist)
	{
		int		childRTindex = lfirsti(il);
		RangeTblEntry *childrte;
		Oid		childOID;
		RelOptInfo *childrel;

		childrte = rt_fetch(childRTindex, root->rtable);
		childOID = childrte->relid;

		/*
		 * Make a RelOptInfo for the child so we can do planning.  Do NOT
		 * attach the RelOptInfo to the query's base_rel_list, however.
		 *
		 * NOTE: when childRTindex == parentRTindex, we create a second
		 * RelOptInfo for the same relation.  This RelOptInfo will represent
		 * the parent table alone, whereas the original RelOptInfo represents
		 * the union of the inheritance tree members.
		 */
		childrel = make_base_rel(root, childRTindex);

		/*
		 * Copy the parent's targetlist and restriction quals to the child,
		 * with attribute-number adjustment if needed.  We don't bother
		 * to copy the join quals, since we can't do any joining here.
		 */
		childrel->targetlist = (List *)
			adjust_inherited_attrs((Node *) rel->targetlist,
								   parentRTindex,
								   parentOID,
								   childRTindex,
								   childOID);
		childrel->baserestrictinfo = (List *)
			adjust_inherited_attrs((Node *) rel->baserestrictinfo,
								   parentRTindex,
								   parentOID,
								   childRTindex,
								   childOID);
		childrel->baserestrictcost = rel->baserestrictcost;

		/*
		 * Now compute child access paths, and save the cheapest.
		 */
		set_plain_rel_pathlist(root, childrel, childrte);

		subpaths = lappend(subpaths, childrel->cheapest_total_path);

		/* Also update total size estimates */
		rel->rows += childrel->rows;
		if (childrel->width > rel->width)
			rel->width = childrel->width;
	}

	/*
	 * Finally, build Append path and install it as the only access
	 * path for the parent rel.
	 */
	add_path(rel, (Path *) create_append_path(rel, subpaths));

	/* Select cheapest path (pretty easy in this case...) */
	set_cheapest(rel);
}


/*
 * make_fromexpr_rel
 *	  Build access paths for a FromExpr jointree node.
 */
RelOptInfo *
make_fromexpr_rel(Query *root, FromExpr *from)
{
	int			levels_needed;
	List	   *initial_rels = NIL;
	List	   *jt;

	/*
	 * Count the number of child jointree nodes.  This is the depth
	 * of the dynamic-programming algorithm we must employ to consider
	 * all ways of joining the child nodes.
	 */
	levels_needed = length(from->fromlist);

	if (levels_needed <= 0)
		return NULL;			/* nothing to do? */

	/*
	 * Construct a list of rels corresponding to the child jointree nodes.
	 * This may contain both base rels and rels constructed according to
	 * explicit JOIN directives.
	 */
	foreach(jt, from->fromlist)
	{
		Node	   *jtnode = (Node *) lfirst(jt);

		initial_rels = lappend(initial_rels,
							   make_jointree_rel(root, jtnode));
	}

	if (levels_needed == 1)
	{
		/*
		 * Single jointree node, so we're done.
		 */
		return (RelOptInfo *) lfirst(initial_rels);
	}
	else
	{
		/*
		 * Consider the different orders in which we could join the rels,
		 * using either GEQO or regular optimizer.
		 */
		if (enable_geqo && levels_needed >= geqo_rels)
			return geqo(root, levels_needed, initial_rels);
		else
			return make_one_rel_by_joins(root, levels_needed, initial_rels);
	}
}

/*
 * make_one_rel_by_joins
 *	  Find all possible joinpaths for a query by successively finding ways
 *	  to join component relations into join relations.
 *
 * 'levels_needed' is the number of iterations needed, ie, the number of
 *		independent jointree items in the query.  This is > 1.
 *
 * 'initial_rels' is a list of RelOptInfo nodes for each independent
 *		jointree item.  These are the components to be joined together.
 *
 * Returns the final level of join relations, i.e., the relation that is
 * the result of joining all the original relations together.
 */
static RelOptInfo *
make_one_rel_by_joins(Query *root, int levels_needed, List *initial_rels)
{
	List	  **joinitems;
	int			lev;
	RelOptInfo *rel;

	/*
	 * We employ a simple "dynamic programming" algorithm: we first find
	 * all ways to build joins of two jointree items, then all ways to
	 * build joins of three items (from two-item joins and single items),
	 * then four-item joins, and so on until we have considered all ways
	 * to join all the items into one rel.
	 *
	 * joinitems[j] is a list of all the j-item rels.  Initially we set
	 * joinitems[1] to represent all the single-jointree-item relations.
	 */
	joinitems = (List **) palloc((levels_needed+1) * sizeof(List *));
	MemSet(joinitems, 0, (levels_needed+1) * sizeof(List *));

	joinitems[1] = initial_rels;

	for (lev = 2; lev <= levels_needed; lev++)
	{
		List	   *x;

		/*
		 * Determine all possible pairs of relations to be joined at this
		 * level, and build paths for making each one from every available
		 * pair of lower-level relations.
		 */
		joinitems[lev] = make_rels_by_joins(root, lev, joinitems);

		/*
		 * Do cleanup work on each just-processed rel.
		 */
		foreach(x, joinitems[lev])
		{
			rel = (RelOptInfo *) lfirst(x);

#ifdef NOT_USED

			/*
			 * * for each expensive predicate in each path in each
			 * distinct rel, * consider doing pullup  -- JMH
			 */
			if (XfuncMode != XFUNC_NOPULL && XfuncMode != XFUNC_OFF)
				xfunc_trypullup(rel);
#endif

			/* Find and save the cheapest paths for this rel */
			set_cheapest(rel);

#ifdef OPTIMIZER_DEBUG
			debug_print_rel(root, rel);
#endif
		}
	}

	/*
	 * We should have a single rel at the final level.
	 */
	Assert(length(joinitems[levels_needed]) == 1);

	rel = (RelOptInfo *) lfirst(joinitems[levels_needed]);

	return rel;
}

/*****************************************************************************
 *
 *****************************************************************************/

#ifdef OPTIMIZER_DEBUG

static void
print_joinclauses(Query *root, List *clauses)
{
	List	   *l;
	extern void print_expr(Node *expr, List *rtable);	/* in print.c */

	foreach(l, clauses)
	{
		RestrictInfo *c = lfirst(l);

		print_expr((Node *) c->clause, root->rtable);
		if (lnext(l))
			printf(" ");
	}
}

static void
print_path(Query *root, Path *path, int indent)
{
	char	   *ptype = NULL;
	JoinPath   *jp;
	bool		join = false;
	int			i;

	for (i = 0; i < indent; i++)
		printf("\t");

	switch (nodeTag(path))
	{
		case T_Path:
			ptype = "SeqScan";
			join = false;
			break;
		case T_IndexPath:
			ptype = "IdxScan";
			join = false;
			break;
		case T_NestPath:
			ptype = "Nestloop";
			join = true;
			break;
		case T_MergePath:
			ptype = "MergeJoin";
			join = true;
			break;
		case T_HashPath:
			ptype = "HashJoin";
			join = true;
			break;
		default:
			break;
	}
	if (join)
	{
		jp = (JoinPath *) path;

		printf("%s rows=%.0f cost=%.2f..%.2f\n",
			   ptype, path->parent->rows,
			   path->startup_cost, path->total_cost);

		if (path->pathkeys)
		{
			for (i = 0; i < indent; i++)
				printf("\t");
			printf("  pathkeys=");
			print_pathkeys(path->pathkeys, root->rtable);
		}

		switch (nodeTag(path))
		{
			case T_MergePath:
			case T_HashPath:
				for (i = 0; i < indent; i++)
					printf("\t");
				printf("  clauses=(");
				print_joinclauses(root, jp->joinrestrictinfo);
				printf(")\n");

				if (nodeTag(path) == T_MergePath)
				{
					MergePath  *mp = (MergePath *) path;

					if (mp->outersortkeys || mp->innersortkeys)
					{
						for (i = 0; i < indent; i++)
							printf("\t");
						printf("  sortouter=%d sortinner=%d\n",
							   ((mp->outersortkeys) ? 1 : 0),
							   ((mp->innersortkeys) ? 1 : 0));
					}
				}
				break;
			default:
				break;
		}
		print_path(root, jp->outerjoinpath, indent + 1);
		print_path(root, jp->innerjoinpath, indent + 1);
	}
	else
	{
		int			relid = lfirsti(path->parent->relids);

		printf("%s(%d) rows=%.0f cost=%.2f..%.2f\n",
			   ptype, relid, path->parent->rows,
			   path->startup_cost, path->total_cost);

		if (path->pathkeys)
		{
			for (i = 0; i < indent; i++)
				printf("\t");
			printf("  pathkeys=");
			print_pathkeys(path->pathkeys, root->rtable);
		}
	}
}

static void
debug_print_rel(Query *root, RelOptInfo *rel)
{
	List	   *l;

	printf("(");
	foreach(l, rel->relids)
		printf("%d ", lfirsti(l));
	printf("): rows=%.0f width=%d\n", rel->rows, rel->width);

	printf("\tpath list:\n");
	foreach(l, rel->pathlist)
		print_path(root, lfirst(l), 1);
	printf("\tcheapest startup path:\n");
	print_path(root, rel->cheapest_startup_path, 1);
	printf("\tcheapest total path:\n");
	print_path(root, rel->cheapest_total_path, 1);
}

#endif	 /* OPTIMIZER_DEBUG */
