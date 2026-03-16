/*-------------------------------------------------------------------------
 *
 * parse_graphtable.c
 *	  parsing of GRAPH_TABLE
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_graphtable.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_propgraph_label.h"
#include "catalog/pg_propgraph_property.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_graphtable.h"
#include "parser/parse_node.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/syscache.h"


/*
 * Return human-readable name of the type of graph element pattern in
 * GRAPH_TABLE clause, usually for error message purpose.
 */
static const char *
get_gep_kind_name(GraphElementPatternKind gepkind)
{
	switch (gepkind)
	{
		case VERTEX_PATTERN:
			return "vertex";
		case EDGE_PATTERN_LEFT:
			return "edge pointing left";
		case EDGE_PATTERN_RIGHT:
			return "edge pointing right";
		case EDGE_PATTERN_ANY:
			return "edge pointing any direction";
		case PAREN_EXPR:
			return "nested path pattern";
	}

	/*
	 * When a GraphElementPattern is constructed by the parser, it will set a
	 * value from the GraphElementPatternKind enum. But we may get here if the
	 * GraphElementPatternKind value stored in a catalog is corrupted.
	 */
	return "unknown";
}

/*
 * Transform a property reference.
 *
 * A property reference is parsed as a ColumnRef of the form:
 * <variable>.<property>. If <variable> is one of the variables bound to an
 * element pattern in the graph pattern and <property> can be resolved as a
 * property of the property graph, then we return a GraphPropertyRef node
 * representing the property reference. If the <variable> exists in the graph
 * pattern but <property> does not exist in the property graph, we raise an
 * error. However, if <variable> does not exist in the graph pattern, we return
 * NULL to let the caller handle it as some other kind of ColumnRef. The
 * variables bound to the element patterns in the graph pattern are expected to
 * be collected in the GraphTableParseState.
 */
Node *
transformGraphTablePropertyRef(ParseState *pstate, ColumnRef *cref)
{
	GraphTableParseState *gpstate = pstate->p_graph_table_pstate;

	if (!gpstate)
		return NULL;

	if (list_length(cref->fields) == 2)
	{
		Node	   *field1 = linitial(cref->fields);
		Node	   *field2 = lsecond(cref->fields);
		char	   *elvarname;
		char	   *propname;

		if (IsA(field1, A_Star) || IsA(field2, A_Star))
		{
			if (pstate->p_expr_kind == EXPR_KIND_SELECT_TARGET)
				ereport(ERROR,
						errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("\"*\" is not supported here"),
						parser_errposition(pstate, cref->location));
			else
				ereport(ERROR,
						errcode(ERRCODE_SYNTAX_ERROR),
						errmsg("\"*\" not allowed here"),
						parser_errposition(pstate, cref->location));
		}

		elvarname = strVal(field1);
		propname = strVal(field2);

		if (list_member(gpstate->variables, field1))
		{
			GraphPropertyRef *gpr = makeNode(GraphPropertyRef);
			HeapTuple	pgptup;
			Form_pg_propgraph_property pgpform;

			pgptup = SearchSysCache2(PROPGRAPHPROPNAME, ObjectIdGetDatum(gpstate->graphid), CStringGetDatum(propname));
			if (!HeapTupleIsValid(pgptup))
				ereport(ERROR,
						errcode(ERRCODE_SYNTAX_ERROR),
						errmsg("property \"%s\" does not exist", propname));
			pgpform = (Form_pg_propgraph_property) GETSTRUCT(pgptup);

			gpr->location = cref->location;
			gpr->elvarname = elvarname;
			gpr->propid = pgpform->oid;
			gpr->typeId = pgpform->pgptypid;
			gpr->typmod = pgpform->pgptypmod;
			gpr->collation = pgpform->pgpcollation;

			ReleaseSysCache(pgptup);

			return (Node *) gpr;
		}
	}

	return NULL;
}

/*
 * Transform a label expression.
 *
 * A label expression is parsed as either a ColumnRef with a single field or a
 * label expression like label disjunction. The single field in the ColumnRef is
 * treated as a label name and transformed to a GraphLabelRef node. The label
 * expression is recursively transformed into an expression tree containg
 * GraphLabelRef nodes corresponding to the names of the labels appearing in the
 * expression. If any label name cannot be resolved to a label in the property
 * graph, an error is raised.
 */
static Node *
transformLabelExpr(GraphTableParseState *gpstate, Node *labelexpr)
{
	Node	   *result;

	if (labelexpr == NULL)
		return NULL;

	check_stack_depth();

	switch (nodeTag(labelexpr))
	{
		case T_ColumnRef:
			{
				ColumnRef  *cref = (ColumnRef *) labelexpr;
				const char *labelname;
				Oid			labelid;
				GraphLabelRef *lref;

				Assert(list_length(cref->fields) == 1);
				labelname = strVal(linitial(cref->fields));

				labelid = GetSysCacheOid2(PROPGRAPHLABELNAME, Anum_pg_propgraph_label_oid, ObjectIdGetDatum(gpstate->graphid), CStringGetDatum(labelname));
				if (!labelid)
					ereport(ERROR,
							errcode(ERRCODE_UNDEFINED_OBJECT),
							errmsg("label \"%s\" does not exist in property graph \"%s\"", labelname, get_rel_name(gpstate->graphid)));

				lref = makeNode(GraphLabelRef);
				lref->labelid = labelid;
				lref->location = cref->location;

				result = (Node *) lref;
				break;
			}

		case T_BoolExpr:
			{
				BoolExpr   *be = (BoolExpr *) labelexpr;
				ListCell   *lc;
				List	   *args = NIL;

				foreach(lc, be->args)
				{
					Node	   *arg = (Node *) lfirst(lc);

					arg = transformLabelExpr(gpstate, arg);
					args = lappend(args, arg);
				}

				result = (Node *) makeBoolExpr(be->boolop, args, be->location);
				break;
			}

		default:
			/* should not reach here */
			elog(ERROR, "unsupported label expression node: %d", (int) nodeTag(labelexpr));
			result = NULL;		/* keep compiler quiet */
			break;
	}

	return result;
}

/*
 * Transform a GraphElementPattern.
 *
 * Transform the label expression and the where clause in the element pattern
 * given by GraphElementPattern. The variable name in the GraphElementPattern is
 * added to the list of variables in the GraphTableParseState which is used to
 * resolve property references in this element pattern or elsewhere in the
 * GRAPH_TABLE.
 */
static Node *
transformGraphElementPattern(ParseState *pstate, GraphElementPattern *gep)
{
	GraphTableParseState *gpstate = pstate->p_graph_table_pstate;

	if (gep->kind != VERTEX_PATTERN && !IS_EDGE_PATTERN(gep->kind))
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("unsupported element pattern kind: \"%s\"", get_gep_kind_name(gep->kind)));

	if (gep->quantifier)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("element pattern quantifier is not supported")));

	if (gep->variable)
		gpstate->variables = lappend(gpstate->variables, makeString(pstrdup(gep->variable)));

	gep->labelexpr = transformLabelExpr(gpstate, gep->labelexpr);

	gep->whereClause = transformExpr(pstate, gep->whereClause, EXPR_KIND_WHERE);
	assign_expr_collations(pstate, gep->whereClause);

	return (Node *) gep;
}

/*
 * Transform a path term (list of GraphElementPattern's).
 */
static Node *
transformPathTerm(ParseState *pstate, List *path_term)
{
	List	   *result = NIL;

	foreach_node(GraphElementPattern, gep, path_term)
		result = lappend(result,
						 transformGraphElementPattern(pstate, gep));

	return (Node *) result;
}

/*
 * Transform a path pattern list (list of path terms).
 */
static Node *
transformPathPatternList(ParseState *pstate, List *path_pattern)
{
	List	   *result = NIL;

	/* Grammar doesn't allow empty path pattern list */
	Assert(list_length(path_pattern) > 0);

	/*
	 * We do not support multiple path patterns in one GRAPH_TABLE clause
	 * right now. But we may do so in future.
	 */
	if (list_length(path_pattern) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("multiple path patterns in one GRAPH_TABLE clause not supported")));

	foreach_node(List, path_term, path_pattern)
		result = lappend(result, transformPathTerm(pstate, path_term));

	return (Node *) result;
}

/*
 * Transform a GraphPattern.
 *
 * A GraphPattern consists of a list of one or more path patterns and an
 * optional where clause. Transform them. We use the previously constructure
 * list of variables in the GraphTableParseState to resolve property references
 * in the WHERE clause.
 */
Node *
transformGraphPattern(ParseState *pstate, GraphPattern *graph_pattern)
{
	List	   *path_pattern_list = castNode(List,
											 transformPathPatternList(pstate, graph_pattern->path_pattern_list));

	graph_pattern->path_pattern_list = path_pattern_list;
	graph_pattern->whereClause = transformExpr(pstate, graph_pattern->whereClause, EXPR_KIND_WHERE);
	assign_expr_collations(pstate, graph_pattern->whereClause);

	return (Node *) graph_pattern;
}
