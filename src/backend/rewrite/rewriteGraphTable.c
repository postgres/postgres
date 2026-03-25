/*-------------------------------------------------------------------------
 *
 * rewriteGraphTable.c
 *		Support for rewriting GRAPH_TABLE clauses.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/rewrite/rewriteGraphTable.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/htup_details.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_propgraph_element.h"
#include "catalog/pg_propgraph_element_label.h"
#include "catalog/pg_propgraph_label.h"
#include "catalog/pg_propgraph_label_property.h"
#include "catalog/pg_propgraph_property.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parse_collate.h"
#include "parser/parse_func.h"
#include "parser/parse_node.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "parser/parse_graphtable.h"
#include "rewrite/rewriteGraphTable.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"


/*
 * Represents one path factor in a path.
 *
 * In a non-cyclic path, one path factor corresponds to one element pattern.
 *
 * In a cyclic path, one path factor corresponds to all the element patterns with
 * the same variable name.
 */
struct path_factor
{
	GraphElementPatternKind kind;
	const char *variable;
	Node	   *labelexpr;
	Node	   *whereClause;
	int			factorpos;		/* Position of this path factor in the list of
								 * path factors representing a given path
								 * pattern. */
	List	   *labeloids;		/* OIDs of all the labels referenced in
								 * labelexpr. */
	/* Links to adjacent vertex path factors if this is an edge path factor. */
	struct path_factor *src_pf;
	struct path_factor *dest_pf;
};

/*
 * Represents one property graph element (vertex or edge) in the path.
 *
 * Label expression in an element pattern resolves into a set of elements. We
 * create one path_element object for each of those elements.
 */
struct path_element
{
	/* Path factor from which this element is derived. */
	struct path_factor *path_factor;
	Oid			elemoid;
	Oid			reloid;
	/* Source and destination vertex elements for an edge element. */
	Oid			srcvertexid;
	Oid			destvertexid;
	/* Source and destination conditions for an edge element. */
	List	   *src_quals;
	List	   *dest_quals;
};

static Node *replace_property_refs(Oid propgraphid, Node *node, const List *mappings);
static List *build_edge_vertex_link_quals(HeapTuple edgetup, int edgerti, int refrti, Oid refid, AttrNumber catalog_key_attnum, AttrNumber catalog_ref_attnum, AttrNumber catalog_eqop_attnum);
static List *generate_queries_for_path_pattern(RangeTblEntry *rte, List *element_patterns);
static Query *generate_query_for_graph_path(RangeTblEntry *rte, List *path);
static Node *generate_setop_from_pathqueries(List *pathqueries, List **rtable, List **targetlist);
static List *generate_queries_for_path_pattern_recurse(RangeTblEntry *rte, List *pathqueries, List *cur_path, List *path_pattern_lists, int elempos);
static Query *generate_query_for_empty_path_pattern(RangeTblEntry *rte);
static Query *generate_union_from_pathqueries(List **pathqueries);
static List *get_path_elements_for_path_factor(Oid propgraphid, struct path_factor *pf);
static bool is_property_associated_with_label(Oid labeloid, Oid propoid);
static Node *get_element_property_expr(Oid elemoid, Oid propoid, int rtindex);

/*
 * Convert GRAPH_TABLE clause into a subquery using relational
 * operators.
 */
Query *
rewriteGraphTable(Query *parsetree, int rt_index)
{
	RangeTblEntry *rte;
	Query	   *graph_table_query;
	List	   *path_pattern;
	List	   *pathqueries = NIL;

	rte = rt_fetch(rt_index, parsetree->rtable);

	Assert(list_length(rte->graph_pattern->path_pattern_list) == 1);

	path_pattern = linitial(rte->graph_pattern->path_pattern_list);
	pathqueries = generate_queries_for_path_pattern(rte, path_pattern);
	graph_table_query = generate_union_from_pathqueries(&pathqueries);

	AcquireRewriteLocks(graph_table_query, true, false);

	rte->rtekind = RTE_SUBQUERY;
	rte->subquery = graph_table_query;
	rte->lateral = true;

	/*
	 * Reset no longer applicable fields, to appease
	 * WRITE_READ_PARSE_PLAN_TREES.
	 */
	rte->graph_pattern = NULL;
	rte->graph_table_columns = NIL;

	return parsetree;
}

/*
 * Generate queries representing the given path pattern applied to the given
 * property graph.
 *
 * A path pattern consists of one or more element patterns. Each of the element
 * patterns may be satisfied by multiple elements. A path satisfying the given
 * path pattern consists of one element from each element pattern.  There can be
 * as many paths as the number of combinations of the elements.  A path pattern
 * in itself is a K-partite graph where K = number of element patterns in the
 * path pattern. The possible paths are computed by performing a DFS in this
 * graph. The DFS is implemented as recursion. Each of these paths is converted
 * into a query connecting all the elements in that path. Set of these queries is
 * returned.
 *
 * Between every two vertex elements in the path there is an edge element that
 * connects them.  An edge connects two vertexes identified by the source and
 * destination keys respectively. The connection between an edge and its
 * adjacent vertex is naturally computed as an equi-join between edge and vertex
 * table on their respective keys. Hence the query representing one path
 * consists of JOINs between edge and vertex tables.
 *
 * generate_queries_for_path_pattern() starts the recursion but actual work is
 * done by generate_queries_for_path_pattern_recurse().
 * generate_query_for_graph_path() constructs a query for a given path.
 *
 * A path pattern may result into no path if any of the element pattern yields no
 * elements or edge patterns yield no edges connecting adjacent vertex patterns.
 * In such a case a dummy query which returns no result is returned
 * (generate_query_for_empty_path_pattern()).
 *
 * 'path_pattern' is given path pattern to be applied on the property graph in
 * the GRAPH_TABLE clause represented by given 'rte'.
 */
static List *
generate_queries_for_path_pattern(RangeTblEntry *rte, List *path_pattern)
{
	List	   *pathqueries = NIL;
	List	   *path_elem_lists = NIL;
	int			factorpos = 0;
	List	   *path_factors = NIL;
	struct path_factor *prev_pf = NULL;

	Assert(list_length(path_pattern) > 0);

	/*
	 * Create a list of path factors representing the given path pattern
	 * linking edge path factors to their adjacent vertex path factors.
	 *
	 * While doing that merge element patterns with the same variable name
	 * into a single path_factor.
	 */
	foreach_node(GraphElementPattern, gep, path_pattern)
	{
		struct path_factor *pf = NULL;

		/*
		 * Unsupported conditions should have been caught by the parser
		 * itself. We have corresponding Asserts here to document the
		 * assumptions in this code.
		 */
		Assert(gep->kind == VERTEX_PATTERN || IS_EDGE_PATTERN(gep->kind));
		Assert(!gep->quantifier);

		foreach_ptr(struct path_factor, other, path_factors)
		{
			if (gep->variable && other->variable &&
				strcmp(gep->variable, other->variable) == 0)
			{
				if (other->kind != gep->kind)
					ereport(ERROR,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("element patterns with same variable name \"%s\" but different element pattern types",
									gep->variable)));

				/*
				 * If both the element patterns have label expressions, they
				 * need to be conjuncted, which is not supported right now.
				 *
				 * However, an empty label expression means all labels.
				 * Conjunction of any label expression with all labels is the
				 * expression itself. Hence if only one of the two element
				 * patterns has a label expression use that expression.
				 */
				if (!other->labelexpr)
					other->labelexpr = gep->labelexpr;
				else if (gep->labelexpr && !equal(other->labelexpr, gep->labelexpr))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("element patterns with same variable name \"%s\" but different label expressions are not supported",
									gep->variable)));

				/*
				 * If two element patterns have the same variable name, they
				 * represent the same set of graph elements and hence are
				 * constrained by conditions from both the element patterns.
				 */
				if (!other->whereClause)
					other->whereClause = gep->whereClause;
				else if (gep->whereClause)
					other->whereClause = (Node *) makeBoolExpr(AND_EXPR,
															   list_make2(other->whereClause, gep->whereClause),
															   -1);
				pf = other;
				break;
			}
		}

		if (!pf)
		{
			pf = palloc0_object(struct path_factor);
			pf->factorpos = factorpos++;
			pf->kind = gep->kind;
			pf->labelexpr = gep->labelexpr;
			pf->variable = gep->variable;
			pf->whereClause = gep->whereClause;

			path_factors = lappend(path_factors, pf);
		}

		/*
		 * Setup links to the previous path factor in the path.
		 *
		 * If the previous path factor represents an edge, this path factor
		 * represents an adjacent vertex; the source vertex for an edge
		 * pointing left or the destination vertex for an edge pointing right.
		 * If this path factor represents an edge, the previous path factor
		 * represents an adjacent vertex; source vertex for an edge pointing
		 * right or the destination vertex for an edge pointing left.
		 *
		 * Edge pointing in any direction is treated similar to that pointing
		 * in right direction here.  When constructing a query in
		 * generate_query_for_graph_path(), we will try links in both the
		 * directions.
		 *
		 * If multiple edge patterns share the same variable name, they
		 * constrain the adjacent vertex patterns since an edge can connect
		 * only one pair of vertexes. These adjacent vertex patterns need to
		 * be merged even though they have different variables. Such element
		 * patterns form a walk of graph where vertex and edges are repeated.
		 * For example, in (a)-[b]->(c)<-[b]-(d), (a) and (d) represent the
		 * same vertex element. This is slightly harder to implement and
		 * probably less useful. Hence not supported for now.
		 */
		if (prev_pf)
		{
			if (prev_pf->kind == EDGE_PATTERN_RIGHT || prev_pf->kind == EDGE_PATTERN_ANY)
			{
				Assert(!IS_EDGE_PATTERN(pf->kind));
				if (prev_pf->dest_pf && prev_pf->dest_pf != pf)
					ereport(ERROR,
							errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							errmsg("an edge cannot connect more than two vertexes even in a cyclic pattern"));
				prev_pf->dest_pf = pf;
			}
			else if (prev_pf->kind == EDGE_PATTERN_LEFT)
			{
				Assert(!IS_EDGE_PATTERN(pf->kind));
				if (prev_pf->src_pf && prev_pf->src_pf != pf)
					ereport(ERROR,
							errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							errmsg("an edge cannot connect more than two vertexes even in a cyclic pattern"));
				prev_pf->src_pf = pf;
			}

			if (pf->kind == EDGE_PATTERN_RIGHT || pf->kind == EDGE_PATTERN_ANY)
			{
				Assert(!IS_EDGE_PATTERN(prev_pf->kind));
				if (pf->src_pf && pf->src_pf != prev_pf)
					ereport(ERROR,
							errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							errmsg("an edge cannot connect more than two vertexes even in a cyclic pattern"));
				pf->src_pf = prev_pf;
			}
			else if (pf->kind == EDGE_PATTERN_LEFT)
			{
				Assert(!IS_EDGE_PATTERN(prev_pf->kind));
				if (pf->dest_pf && pf->dest_pf != prev_pf)
					ereport(ERROR,
							errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							errmsg("an edge cannot connect more than two vertexes even in a cyclic pattern"));
				pf->dest_pf = prev_pf;
			}
		}

		prev_pf = pf;
	}

	/*
	 * Collect list of elements for each path factor. Do this after all the
	 * edge links are setup correctly.
	 */
	foreach_ptr(struct path_factor, pf, path_factors)
		path_elem_lists = lappend(path_elem_lists,
								  get_path_elements_for_path_factor(rte->relid, pf));

	pathqueries = generate_queries_for_path_pattern_recurse(rte, pathqueries,
															NIL, path_elem_lists, 0);
	if (!pathqueries)
		pathqueries = list_make1(generate_query_for_empty_path_pattern(rte));

	return pathqueries;
}

/*
 * Recursive workhorse function of generate_queries_for_path_pattern().
 *
 * `elempos` is the position of the next element being added in the path being
 * built.
 */
static List *
generate_queries_for_path_pattern_recurse(RangeTblEntry *rte, List *pathqueries, List *cur_path, List *path_elem_lists, int elempos)
{
	List	   *path_elems = list_nth_node(List, path_elem_lists, elempos);

	foreach_ptr(struct path_element, pe, path_elems)
	{
		/* Update current path being built with current element. */
		cur_path = lappend(cur_path, pe);

		/*
		 * If this is the last element in the path, generate query for the
		 * completed path. Else recurse processing the next element.
		 */
		if (list_length(path_elem_lists) == list_length(cur_path))
		{
			Query	   *pathquery = generate_query_for_graph_path(rte, cur_path);

			Assert(elempos == list_length(path_elem_lists) - 1);
			if (pathquery)
				pathqueries = lappend(pathqueries, pathquery);
		}
		else
			pathqueries = generate_queries_for_path_pattern_recurse(rte, pathqueries,
																	cur_path,
																	path_elem_lists,
																	elempos + 1);
		/* Make way for the next element at the same position. */
		cur_path = list_delete_last(cur_path);
	}

	return pathqueries;
}

/*
 * Construct a query representing given graph path.
 *
 * The query contains:
 *
 * 1. targetlist corresponding to the COLUMNS clause of GRAPH_TABLE clause
 *
 * 2. quals corresponding to the WHERE clause of individual elements, WHERE
 * clause in GRAPH_TABLE clause and quals representing edge-vertex links.
 *
 * 3. fromlist containing all elements in the path
 *
 * The collations of property expressions are obtained from the catalog. The
 * collations of expressions in COLUMNS and WHERE clauses are assigned before
 * rewriting the graph table.  The collations of the edge-vertex link quals are
 * assigned when crafting those quals. Thus everything in the query that requires
 * collation assignment has been taken care of already. No separate collation
 * assignment is required in this function.
 *
 * More details in the prologue of generate_queries_for_path_pattern().
 */
static Query *
generate_query_for_graph_path(RangeTblEntry *rte, List *graph_path)
{
	Query	   *path_query = makeNode(Query);
	List	   *fromlist = NIL;
	List	   *qual_exprs = NIL;
	List	   *vars;

	path_query->commandType = CMD_SELECT;

	foreach_ptr(struct path_element, pe, graph_path)
	{
		struct path_factor *pf = pe->path_factor;
		RangeTblRef *rtr;
		Relation	rel;
		ParseNamespaceItem *pni;

		Assert(pf->kind == VERTEX_PATTERN || IS_EDGE_PATTERN(pf->kind));

		/* Add conditions representing edge connections. */
		if (IS_EDGE_PATTERN(pf->kind))
		{
			struct path_element *src_pe;
			struct path_element *dest_pe;
			Expr	   *edge_qual = NULL;

			Assert(pf->src_pf && pf->dest_pf);
			src_pe = list_nth(graph_path, pf->src_pf->factorpos);
			dest_pe = list_nth(graph_path, pf->dest_pf->factorpos);

			/* Make sure that the links of adjacent vertices are correct. */
			Assert(pf->src_pf == src_pe->path_factor &&
				   pf->dest_pf == dest_pe->path_factor);

			if (src_pe->elemoid == pe->srcvertexid &&
				dest_pe->elemoid == pe->destvertexid)
				edge_qual = makeBoolExpr(AND_EXPR,
										 list_concat(copyObject(pe->src_quals),
													 copyObject(pe->dest_quals)),
										 -1);

			/*
			 * An edge pattern in any direction matches edges in both
			 * directions, try swapping source and destination. When the
			 * source and destination is the same vertex table, quals
			 * corresponding to either direction may get satisfied. Hence OR
			 * the quals corresponding to both the directions.
			 */
			if (pf->kind == EDGE_PATTERN_ANY &&
				dest_pe->elemoid == pe->srcvertexid &&
				src_pe->elemoid == pe->destvertexid)
			{
				List	   *src_quals = copyObject(pe->dest_quals);
				List	   *dest_quals = copyObject(pe->src_quals);
				Expr	   *rev_edge_qual;

				/* Swap the source and destination varnos in the quals. */
				ChangeVarNodes((Node *) dest_quals, pe->path_factor->src_pf->factorpos + 1,
							   pe->path_factor->dest_pf->factorpos + 1, 0);
				ChangeVarNodes((Node *) src_quals, pe->path_factor->dest_pf->factorpos + 1,
							   pe->path_factor->src_pf->factorpos + 1, 0);

				rev_edge_qual = makeBoolExpr(AND_EXPR, list_concat(src_quals, dest_quals), -1);
				if (edge_qual)
					edge_qual = makeBoolExpr(OR_EXPR, list_make2(edge_qual, rev_edge_qual), -1);
				else
					edge_qual = rev_edge_qual;
			}

			/*
			 * If the given edge element does not connect the adjacent vertex
			 * elements in this path, the path is broken. Abandon this path as
			 * it won't return any rows.
			 */
			if (edge_qual == NULL)
				return NULL;

			qual_exprs = lappend(qual_exprs, edge_qual);
		}
		else
			Assert(!pe->src_quals && !pe->dest_quals);

		/*
		 * Create RangeTblEntry for this element table.
		 *
		 * SQL/PGQ standard (Ref. Section 11.19, Access rule 2 and General
		 * rule 4) does not specify whose access privileges to use when
		 * accessing the element tables: property graph owner's or current
		 * user's. It is safer to use current user's privileges so as not to
		 * make property graphs as a hole for unpriviledged data access. This
		 * is inline with the views being security_invoker by default.
		 */
		rel = table_open(pe->reloid, AccessShareLock);
		pni = addRangeTableEntryForRelation(make_parsestate(NULL), rel, AccessShareLock,
											NULL, true, false);
		table_close(rel, NoLock);
		path_query->rtable = lappend(path_query->rtable, pni->p_rte);
		path_query->rteperminfos = lappend(path_query->rteperminfos, pni->p_perminfo);
		pni->p_rte->perminfoindex = list_length(path_query->rteperminfos);
		rtr = makeNode(RangeTblRef);
		rtr->rtindex = list_length(path_query->rtable);
		fromlist = lappend(fromlist, rtr);

		/*
		 * Make sure that the assumption mentioned in create_pe_for_element()
		 * holds true; that the elements' RangeTblEntrys are added in the
		 * order in which their respective path factors appear in the list of
		 * path factors representing the path pattern.
		 */
		Assert(pf->factorpos + 1 == rtr->rtindex);

		if (pf->whereClause)
		{
			Node	   *tr;

			tr = replace_property_refs(rte->relid, pf->whereClause, list_make1(pe));

			qual_exprs = lappend(qual_exprs, tr);
		}
	}

	if (rte->graph_pattern->whereClause)
	{
		Node	   *path_quals = replace_property_refs(rte->relid,
													   (Node *) rte->graph_pattern->whereClause,
													   graph_path);

		qual_exprs = lappend(qual_exprs, path_quals);
	}

	path_query->jointree = makeFromExpr(fromlist,
										qual_exprs ? (Node *) makeBoolExpr(AND_EXPR, qual_exprs, -1) : NULL);

	/* Construct query targetlist from COLUMNS specification of GRAPH_TABLE. */
	path_query->targetList = castNode(List,
									  replace_property_refs(rte->relid,
															(Node *) rte->graph_table_columns,
															graph_path));

	/*
	 * Mark the columns being accessed in the path query as requiring SELECT
	 * privilege. Any lateral columns should have been handled when the
	 * corresponding ColumnRefs were transformed. Ignore those here.
	 */
	vars = pull_vars_of_level((Node *) list_make2(qual_exprs, path_query->targetList), 0);
	foreach_node(Var, var, vars)
	{
		RTEPermissionInfo *perminfo = getRTEPermissionInfo(path_query->rteperminfos,
														   rt_fetch(var->varno, path_query->rtable));

		/* Must offset the attnum to fit in a bitmapset */
		perminfo->selectedCols = bms_add_member(perminfo->selectedCols,
												var->varattno - FirstLowInvalidHeapAttributeNumber);
	}

	return path_query;
}

/*
 * Construct a query which would not return any rows.
 *
 * More details in the prologue of generate_queries_for_path_pattern().
 */
static Query *
generate_query_for_empty_path_pattern(RangeTblEntry *rte)
{
	Query	   *query = makeNode(Query);

	query->commandType = CMD_SELECT;
	query->rtable = NIL;
	query->rteperminfos = NIL;
	query->jointree = makeFromExpr(NIL, (Node *) makeBoolConst(false, false));

	/*
	 * Even though no rows are returned, the result still projects the same
	 * columns as projected by GRAPH_TABLE clause. Do this by constructing a
	 * target list full of NULL values.
	 */
	foreach_node(TargetEntry, te, rte->graph_table_columns)
	{
		Node	   *nte = (Node *) te->expr;

		te->expr = (Expr *) makeNullConst(exprType(nte), exprTypmod(nte), exprCollation(nte));
		query->targetList = lappend(query->targetList, te);
	}

	return query;
}

/*
 * Construct a query which is UNION of given path queries.
 *
 * The UNION query derives collations of its targetlist entries from the
 * corresponding targetlist entries of the path queries. The targetlists of path
 * queries being UNION'ed already have collations assigned.  No separate
 * collation assignment required in this function.
 *
 * The function destroys given pathqueries list while constructing
 * SetOperationStmt recursively. Hence the function always returns with
 * `pathqueries` set to NIL.
 */
static Query *
generate_union_from_pathqueries(List **pathqueries)
{
	List	   *rtable = NIL;
	Query	   *sampleQuery = linitial_node(Query, *pathqueries);
	SetOperationStmt *sostmt;
	Query	   *union_query;
	int			resno;
	ListCell   *lctl,
			   *lct,
			   *lcm,
			   *lcc;

	Assert(list_length(*pathqueries) > 0);

	/* If there's only one pathquery, no need to construct a UNION query. */
	if (list_length(*pathqueries) == 1)
	{
		*pathqueries = NIL;
		return sampleQuery;
	}

	sostmt = castNode(SetOperationStmt,
					  generate_setop_from_pathqueries(*pathqueries, &rtable, NULL));

	/* Encapsulate the set operation statement into a Query. */
	union_query = makeNode(Query);
	union_query->commandType = CMD_SELECT;
	union_query->rtable = rtable;
	union_query->setOperations = (Node *) sostmt;
	union_query->rteperminfos = NIL;
	union_query->jointree = makeFromExpr(NIL, NULL);

	/*
	 * Generate dummy targetlist for outer query using column names from one
	 * of the queries and common datatypes/collations of topmost set
	 * operation.  It shouldn't matter which query. Also it shouldn't matter
	 * which RT index is used as varno in the target list entries, as long as
	 * it corresponds to a real RT entry; else funny things may happen when
	 * the tree is mashed by rule rewriting. So we use 1 since there's always
	 * one RT entry at least.
	 */
	Assert(rt_fetch(1, rtable));
	union_query->targetList = NULL;
	resno = 1;
	forfour(lct, sostmt->colTypes,
			lcm, sostmt->colTypmods,
			lcc, sostmt->colCollations,
			lctl, sampleQuery->targetList)
	{
		Oid			colType = lfirst_oid(lct);
		int32		colTypmod = lfirst_int(lcm);
		Oid			colCollation = lfirst_oid(lcc);
		TargetEntry *sample_tle = (TargetEntry *) lfirst(lctl);
		char	   *colName;
		TargetEntry *tle;
		Var		   *var;

		Assert(!sample_tle->resjunk);
		colName = pstrdup(sample_tle->resname);
		var = makeVar(1, sample_tle->resno, colType, colTypmod, colCollation, 0);
		var->location = exprLocation((Node *) sample_tle->expr);
		tle = makeTargetEntry((Expr *) var, (AttrNumber) resno++, colName, false);
		union_query->targetList = lappend(union_query->targetList, tle);
	}

	*pathqueries = NIL;
	return union_query;
}

/*
 * Construct a query which is UNION of all the given path queries.
 *
 * The function destroys given pathqueries list while constructing
 * SetOperationStmt recursively.
 */
static Node *
generate_setop_from_pathqueries(List *pathqueries, List **rtable, List **targetlist)
{
	SetOperationStmt *sostmt;
	Query	   *lquery;
	Node	   *rarg;
	RangeTblRef *lrtr = makeNode(RangeTblRef);
	List	   *rtargetlist;
	ParseNamespaceItem *pni;

	/* Recursion termination condition. */
	if (list_length(pathqueries) == 0)
	{
		*targetlist = NIL;
		return NULL;
	}

	lquery = linitial_node(Query, pathqueries);

	pni = addRangeTableEntryForSubquery(make_parsestate(NULL), lquery, NULL,
										false, false);
	*rtable = lappend(*rtable, pni->p_rte);
	lrtr->rtindex = list_length(*rtable);
	rarg = generate_setop_from_pathqueries(list_delete_first(pathqueries), rtable, &rtargetlist);
	if (rarg == NULL)
	{
		/*
		 * No further path queries in the list. Convert the last query into a
		 * RangeTblRef as expected by SetOperationStmt. Extract a list of the
		 * non-junk TLEs for upper-level processing.
		 */
		if (targetlist)
		{
			*targetlist = NIL;
			foreach_node(TargetEntry, tle, lquery->targetList)
			{
				if (!tle->resjunk)
					*targetlist = lappend(*targetlist, tle);
			}
		}
		return (Node *) lrtr;
	}

	sostmt = makeNode(SetOperationStmt);
	sostmt->op = SETOP_UNION;
	sostmt->all = true;
	sostmt->larg = (Node *) lrtr;
	sostmt->rarg = rarg;
	constructSetOpTargetlist(NULL, sostmt, lquery->targetList, rtargetlist, targetlist, "UNION", false);

	return (Node *) sostmt;
}

/*
 * Construct a path_element object for the graph element given by `elemoid`
 * satisfied by the path factor `pf`.
 *
 * If the type of graph element does not fit the element pattern kind, the
 * function returns NULL.
 */
static struct path_element *
create_pe_for_element(struct path_factor *pf, Oid elemoid)
{
	HeapTuple	eletup = SearchSysCache1(PROPGRAPHELOID, ObjectIdGetDatum(elemoid));
	Form_pg_propgraph_element pgeform;
	struct path_element *pe;

	if (!eletup)
		elog(ERROR, "cache lookup failed for property graph element %u", elemoid);
	pgeform = ((Form_pg_propgraph_element) GETSTRUCT(eletup));

	if ((pgeform->pgekind == PGEKIND_VERTEX && pf->kind != VERTEX_PATTERN) ||
		(pgeform->pgekind == PGEKIND_EDGE && !IS_EDGE_PATTERN(pf->kind)))
	{
		ReleaseSysCache(eletup);
		return NULL;
	}

	pe = palloc0_object(struct path_element);
	pe->path_factor = pf;
	pe->elemoid = elemoid;
	pe->reloid = pgeform->pgerelid;

	/*
	 * When a path is converted into a query
	 * (generate_query_for_graph_path()), a RangeTblEntry will be created for
	 * every element in the path.  Fixing rtindexes of RangeTblEntrys here
	 * makes it possible to craft elements' qual expressions only once while
	 * we have access to the catalog entry. Otherwise they need to be crafted
	 * as many times as the number of paths a given element appears in,
	 * fetching catalog entry again each time.  Hence we simply assume
	 * RangeTblEntrys will be created in the same order in which the
	 * corresponding path factors appear in the list of path factors
	 * representing a path pattern. That way their rtindexes will be same as
	 * path_factor::factorpos + 1.
	 */
	if (IS_EDGE_PATTERN(pf->kind))
	{
		pe->srcvertexid = pgeform->pgesrcvertexid;
		pe->destvertexid = pgeform->pgedestvertexid;
		Assert(pf->src_pf && pf->dest_pf);

		pe->src_quals = build_edge_vertex_link_quals(eletup, pf->factorpos + 1, pf->src_pf->factorpos + 1,
													 pe->srcvertexid,
													 Anum_pg_propgraph_element_pgesrckey,
													 Anum_pg_propgraph_element_pgesrcref,
													 Anum_pg_propgraph_element_pgesrceqop);
		pe->dest_quals = build_edge_vertex_link_quals(eletup, pf->factorpos + 1, pf->dest_pf->factorpos + 1,
													  pe->destvertexid,
													  Anum_pg_propgraph_element_pgedestkey,
													  Anum_pg_propgraph_element_pgedestref,
													  Anum_pg_propgraph_element_pgedesteqop);
	}

	ReleaseSysCache(eletup);

	return pe;
}

/*
 * Returns the list of OIDs of graph labels which the given label expression
 * resolves to in the given property graph.
 */
static List *
get_labels_for_expr(Oid propgraphid, Node *labelexpr)
{
	List	   *label_oids;

	if (!labelexpr)
	{
		Relation	rel;
		SysScanDesc scan;
		ScanKeyData key[1];
		HeapTuple	tup;

		/*
		 * According to section 9.2 "Contextual inference of a set of labels"
		 * subclause 2.a.ii of SQL/PGQ standard, element pattern which does
		 * not have a label expression is considered to have label expression
		 * equivalent to '%|!%' which is set of all labels.
		 */
		label_oids = NIL;
		rel = table_open(PropgraphLabelRelationId, AccessShareLock);
		ScanKeyInit(&key[0],
					Anum_pg_propgraph_label_pglpgid,
					BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(propgraphid));
		scan = systable_beginscan(rel, PropgraphLabelGraphNameIndexId,
								  true, NULL, 1, key);
		while (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			Form_pg_propgraph_label label = (Form_pg_propgraph_label) GETSTRUCT(tup);

			label_oids = lappend_oid(label_oids, label->oid);
		}
		systable_endscan(scan);
		table_close(rel, AccessShareLock);
	}
	else if (IsA(labelexpr, GraphLabelRef))
	{
		GraphLabelRef *glr = castNode(GraphLabelRef, labelexpr);

		label_oids = list_make1_oid(glr->labelid);
	}
	else if (IsA(labelexpr, BoolExpr))
	{
		BoolExpr   *be = castNode(BoolExpr, labelexpr);
		List	   *label_exprs = be->args;

		label_oids = NIL;
		foreach_node(GraphLabelRef, glr, label_exprs)
			label_oids = lappend_oid(label_oids, glr->labelid);
	}
	else
	{
		/*
		 * should not reach here since gram.y will not generate a label
		 * expression with other node types.
		 */
		elog(ERROR, "unsupported label expression node: %d", (int) nodeTag(labelexpr));
	}

	return label_oids;
}

/*
 * Return a list of all the graph elements that satisfy the graph element pattern
 * represented by the given path_factor `pf`.
 *
 * First we find all the graph labels that satisfy the label expression in path
 * factor. Each label is associated with one or more graph elements.  A union of
 * all such elements satisfies the element pattern. We create one path_element
 * object representing every element whose graph element kind qualifies the
 * element pattern kind. A list of all such path_element objects is returned.
 *
 * Note that we need to report an error for an explicitly specified label which
 * is not associated with any graph element of the required kind. So we have to
 * treat each label separately. Without that requirement we could have collected
 * all the unique elements first and then created path_element objects for them
 * to simplify the code.
 */
static List *
get_path_elements_for_path_factor(Oid propgraphid, struct path_factor *pf)
{
	List	   *label_oids = get_labels_for_expr(propgraphid, pf->labelexpr);
	List	   *elem_oids_seen = NIL;
	List	   *pf_elem_oids = NIL;
	List	   *path_elements = NIL;
	List	   *unresolved_labels = NIL;
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData key[1];
	HeapTuple	tup;

	/*
	 * A property graph element can be either a vertex or an edge. Other types
	 * of path factors like nested path pattern need to be handled separately
	 * when supported.
	 */
	Assert(pf->kind == VERTEX_PATTERN || IS_EDGE_PATTERN(pf->kind));

	rel = table_open(PropgraphElementLabelRelationId, AccessShareLock);
	foreach_oid(labeloid, label_oids)
	{
		bool		found = false;

		ScanKeyInit(&key[0],
					Anum_pg_propgraph_element_label_pgellabelid,
					BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(labeloid));
		scan = systable_beginscan(rel, PropgraphElementLabelLabelIndexId, true,
								  NULL, 1, key);
		while (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			Form_pg_propgraph_element_label label_elem = (Form_pg_propgraph_element_label) GETSTRUCT(tup);
			Oid			elem_oid = label_elem->pgelelid;

			if (!list_member_oid(elem_oids_seen, elem_oid))
			{
				/*
				 * Create path_element object if the new element qualifies the
				 * element pattern kind.
				 */
				struct path_element *pe = create_pe_for_element(pf, elem_oid);

				if (pe)
				{
					path_elements = lappend(path_elements, pe);

					/* Remember qualified elements. */
					pf_elem_oids = lappend_oid(pf_elem_oids, elem_oid);
					found = true;
				}

				/*
				 * Remember qualified and unqualified elements processed so
				 * far to avoid processing already processed elements again.
				 */
				elem_oids_seen = lappend_oid(elem_oids_seen, label_elem->pgelelid);
			}
			else if (list_member_oid(pf_elem_oids, elem_oid))
			{
				/*
				 * The graph element is known to qualify the given element
				 * pattern. Flag that the current label has at least one
				 * qualified element associated with it.
				 */
				found = true;
			}
		}

		if (!found)
		{
			/*
			 * We did not find any qualified element associated with this
			 * label. The label or its properties can not be associated with
			 * the given element pattern. Throw an error if the label was
			 * explicitly specified in the element pattern. Otherwise remember
			 * it for later use.
			 */
			if (!pf->labelexpr)
				unresolved_labels = lappend_oid(unresolved_labels, labeloid);
			else
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("no property graph element of type \"%s\" has label \"%s\" associated with it in property graph \"%s\"",
								pf->kind == VERTEX_PATTERN ? "vertex" : "edge",
								get_propgraph_label_name(labeloid),
								get_rel_name(propgraphid))));
		}

		systable_endscan(scan);
	}
	table_close(rel, AccessShareLock);

	/*
	 * Remove the labels which were not explicitly mentioned in the label
	 * expression but do not have any qualified elements associated with them.
	 * Properties associated with such labels may not be referenced. See
	 * replace_property_refs_mutator() for more details.
	 */
	pf->labeloids = list_difference_oid(label_oids, unresolved_labels);

	return path_elements;
}

/*
 * Mutating property references into table variables
 */

struct replace_property_refs_context
{
	Oid			propgraphid;
	const List *mappings;
};

static Node *
replace_property_refs_mutator(Node *node, struct replace_property_refs_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		Var		   *newvar = copyObject(var);

		/*
		 * If it's already a Var, then it was a lateral reference.  Since we
		 * are in a subquery after the rewrite, we have to increase the level
		 * by one.
		 */
		newvar->varlevelsup++;

		return (Node *) newvar;
	}
	else if (IsA(node, GraphPropertyRef))
	{
		GraphPropertyRef *gpr = (GraphPropertyRef *) node;
		Node	   *n = NULL;
		struct path_element *found_mapping = NULL;
		struct path_factor *mapping_factor = NULL;
		List	   *unrelated_labels = NIL;

		foreach_ptr(struct path_element, m, context->mappings)
		{
			if (m->path_factor->variable && strcmp(gpr->elvarname, m->path_factor->variable) == 0)
			{
				found_mapping = m;
				break;
			}
		}

		/*
		 * transformGraphTablePropertyRef() would not create a
		 * GraphPropertyRef for a variable which is not present in the graph
		 * path pattern.
		 */
		Assert(found_mapping);

		mapping_factor = found_mapping->path_factor;

		/*
		 * Find property definition for given element through any of the
		 * associated labels qualifying the given element pattern.
		 */
		foreach_oid(labeloid, mapping_factor->labeloids)
		{
			Oid			elem_labelid = GetSysCacheOid2(PROPGRAPHELEMENTLABELELEMENTLABEL,
													   Anum_pg_propgraph_element_label_oid,
													   ObjectIdGetDatum(found_mapping->elemoid),
													   ObjectIdGetDatum(labeloid));

			if (OidIsValid(elem_labelid))
			{
				HeapTuple	tup = SearchSysCache2(PROPGRAPHLABELPROP, ObjectIdGetDatum(elem_labelid),
												  ObjectIdGetDatum(gpr->propid));

				if (!tup)
				{
					/*
					 * The label is associated with the given element but it
					 * is not associated with the required property. Check
					 * next label.
					 */
					continue;
				}

				n = stringToNode(TextDatumGetCString(SysCacheGetAttrNotNull(PROPGRAPHLABELPROP,
																			tup, Anum_pg_propgraph_label_property_plpexpr)));
				ChangeVarNodes(n, 1, mapping_factor->factorpos + 1, 0);

				ReleaseSysCache(tup);
			}
			else
			{
				/*
				 * Label is not associated with the element but it may be
				 * associated with the property through some other element.
				 * Save it for later use.
				 */
				unrelated_labels = lappend_oid(unrelated_labels, labeloid);
			}
		}

		/* See if we can resolve the property in some other way. */
		if (!n)
		{
			bool		prop_associated = false;

			foreach_oid(loid, unrelated_labels)
			{
				if (is_property_associated_with_label(loid, gpr->propid))
				{
					prop_associated = true;
					break;
				}
			}

			if (prop_associated)
			{
				/*
				 * The property is associated with at least one of the labels
				 * that satisfy given element pattern. If it's associated with
				 * the given element (through some other label), use
				 * corresponding value expression. Otherwise NULL. Ref.
				 * SQL/PGQ standard section 6.5 Property Reference, General
				 * Rule 2.b.
				 */
				n = get_element_property_expr(found_mapping->elemoid, gpr->propid,
											  mapping_factor->factorpos + 1);

				if (!n)
					n = (Node *) makeNullConst(gpr->typeId, gpr->typmod, gpr->collation);
			}

		}

		if (!n)
			ereport(ERROR,
					errcode(ERRCODE_UNDEFINED_OBJECT),
					errmsg("property \"%s\" for element variable \"%s\" not found",
						   get_propgraph_property_name(gpr->propid), mapping_factor->variable));

		return n;
	}

	return expression_tree_mutator(node, replace_property_refs_mutator, context);
}

static Node *
replace_property_refs(Oid propgraphid, Node *node, const List *mappings)
{
	struct replace_property_refs_context context;

	context.mappings = mappings;
	context.propgraphid = propgraphid;

	return expression_tree_mutator(node, replace_property_refs_mutator, &context);
}

/*
 * Build join qualification expressions between edge and vertex tables.
 */
static List *
build_edge_vertex_link_quals(HeapTuple edgetup, int edgerti, int refrti, Oid refid, AttrNumber catalog_key_attnum, AttrNumber catalog_ref_attnum, AttrNumber catalog_eqop_attnum)
{
	List	   *quals = NIL;
	Form_pg_propgraph_element pgeform;
	Datum		datum;
	Datum	   *d1,
			   *d2,
			   *d3;
	int			n1,
				n2,
				n3;
	ParseState *pstate = make_parsestate(NULL);
	Oid			refrelid = GetSysCacheOid1(PROPGRAPHELOID, Anum_pg_propgraph_element_pgerelid, ObjectIdGetDatum(refid));

	pgeform = (Form_pg_propgraph_element) GETSTRUCT(edgetup);

	datum = SysCacheGetAttrNotNull(PROPGRAPHELOID, edgetup, catalog_key_attnum);
	deconstruct_array_builtin(DatumGetArrayTypeP(datum), INT2OID, &d1, NULL, &n1);

	datum = SysCacheGetAttrNotNull(PROPGRAPHELOID, edgetup, catalog_ref_attnum);
	deconstruct_array_builtin(DatumGetArrayTypeP(datum), INT2OID, &d2, NULL, &n2);

	datum = SysCacheGetAttrNotNull(PROPGRAPHELOID, edgetup, catalog_eqop_attnum);
	deconstruct_array_builtin(DatumGetArrayTypeP(datum), OIDOID, &d3, NULL, &n3);

	if (n1 != n2)
		elog(ERROR, "array size key (%d) vs ref (%d) mismatch for element ID %u", catalog_key_attnum, catalog_ref_attnum, pgeform->oid);
	if (n1 != n3)
		elog(ERROR, "array size key (%d) vs operator (%d) mismatch for element ID %u", catalog_key_attnum, catalog_eqop_attnum, pgeform->oid);

	for (int i = 0; i < n1; i++)
	{
		AttrNumber	keyattn = DatumGetInt16(d1[i]);
		AttrNumber	refattn = DatumGetInt16(d2[i]);
		Oid			eqop = DatumGetObjectId(d3[i]);
		Var		   *keyvar;
		Var		   *refvar;
		Oid			atttypid;
		int32		atttypmod;
		Oid			attcoll;
		HeapTuple	tup;
		Form_pg_operator opform;
		List	   *args;
		Oid			actual_arg_types[2];
		Oid			declared_arg_types[2];
		OpExpr	   *linkqual;

		get_atttypetypmodcoll(pgeform->pgerelid, keyattn, &atttypid, &atttypmod, &attcoll);
		keyvar = makeVar(edgerti, keyattn, atttypid, atttypmod, attcoll, 0);
		get_atttypetypmodcoll(refrelid, refattn, &atttypid, &atttypmod, &attcoll);
		refvar = makeVar(refrti, refattn, atttypid, atttypmod, attcoll, 0);

		tup = SearchSysCache1(OPEROID, ObjectIdGetDatum(eqop));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for operator %u", eqop);
		opform = (Form_pg_operator) GETSTRUCT(tup);
		/* An equality operator is a binary operator returning boolean result. */
		Assert(opform->oprkind == 'b'
			   && RegProcedureIsValid(opform->oprcode)
			   && opform->oprresult == BOOLOID
			   && !get_func_retset(opform->oprcode));

		/*
		 * Prepare operands and cast them to the types required by the
		 * equality operator. Similar to PK/FK quals, referenced vertex key is
		 * used as left operand and referencing edge key is used as right
		 * operand.
		 */
		args = list_make2(refvar, keyvar);
		actual_arg_types[0] = exprType((Node *) refvar);
		actual_arg_types[1] = exprType((Node *) keyvar);
		declared_arg_types[0] = opform->oprleft;
		declared_arg_types[1] = opform->oprright;
		make_fn_arguments(pstate, args, actual_arg_types, declared_arg_types);

		linkqual = makeNode(OpExpr);
		linkqual->opno = opform->oid;
		linkqual->opfuncid = opform->oprcode;
		linkqual->opresulttype = opform->oprresult;
		linkqual->opretset = false;
		/* opcollid and inputcollid will be set by parse_collate.c */
		linkqual->args = args;
		linkqual->location = -1;

		ReleaseSysCache(tup);
		quals = lappend(quals, linkqual);
	}

	assign_expr_collations(pstate, (Node *) quals);

	return quals;
}

/*
 * Check if the given property is associated with the given label.
 *
 * A label projects the same set of properties through every element it is
 * associated with. Find any of the elements and return true if that element is
 * associated with the given property. False otherwise.
 */
static bool
is_property_associated_with_label(Oid labeloid, Oid propoid)
{
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData key[1];
	HeapTuple	tup;
	bool		associated = false;

	rel = table_open(PropgraphElementLabelRelationId, RowShareLock);
	ScanKeyInit(&key[0],
				Anum_pg_propgraph_element_label_pgellabelid,
				BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(labeloid));
	scan = systable_beginscan(rel, PropgraphElementLabelLabelIndexId,
							  true, NULL, 1, key);

	if (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_propgraph_element_label ele_label = (Form_pg_propgraph_element_label) GETSTRUCT(tup);

		associated = SearchSysCacheExists2(PROPGRAPHLABELPROP,
										   ObjectIdGetDatum(ele_label->oid), ObjectIdGetDatum(propoid));
	}
	systable_endscan(scan);
	table_close(rel, RowShareLock);

	return associated;
}

/*
 * If given element has the given property associated with it, through any of
 * the associated labels, return value expression of the property. Otherwise
 * NULL.
 */
static Node *
get_element_property_expr(Oid elemoid, Oid propoid, int rtindex)
{
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData key[1];
	HeapTuple	labeltup;
	Node	   *n = NULL;

	rel = table_open(PropgraphElementLabelRelationId, RowShareLock);
	ScanKeyInit(&key[0],
				Anum_pg_propgraph_element_label_pgelelid,
				BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(elemoid));
	scan = systable_beginscan(rel, PropgraphElementLabelElementLabelIndexId,
							  true, NULL, 1, key);

	while (HeapTupleIsValid(labeltup = systable_getnext(scan)))
	{
		Form_pg_propgraph_element_label ele_label = (Form_pg_propgraph_element_label) GETSTRUCT(labeltup);

		HeapTuple	proptup = SearchSysCache2(PROPGRAPHLABELPROP,
											  ObjectIdGetDatum(ele_label->oid), ObjectIdGetDatum(propoid));

		if (!proptup)
			continue;
		n = stringToNode(TextDatumGetCString(SysCacheGetAttrNotNull(PROPGRAPHLABELPROP,
																	proptup, Anum_pg_propgraph_label_property_plpexpr)));
		ChangeVarNodes(n, 1, rtindex, 0);

		ReleaseSysCache(proptup);
		break;
	}
	systable_endscan(scan);
	table_close(rel, RowShareLock);

	return n;
}
