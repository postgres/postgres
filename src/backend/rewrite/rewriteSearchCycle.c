/*-------------------------------------------------------------------------
 *
 * rewriteSearchCycle.c
 *		Support for rewriting SEARCH and CYCLE clauses.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/rewrite/rewriteSearchCycle.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/pg_list.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "rewrite/rewriteSearchCycle.h"
#include "utils/fmgroids.h"


/*----------
 * Rewrite a CTE with SEARCH or CYCLE clause
 *
 * Consider a CTE like
 *
 * WITH RECURSIVE ctename (col1, col2, col3) AS (
 *     query1
 *   UNION [ALL]
 *     SELECT trosl FROM ctename
 * )
 *
 * With a search clause
 *
 * SEARCH BREADTH FIRST BY col1, col2 SET sqc
 *
 * the CTE is rewritten to
 *
 * WITH RECURSIVE ctename (col1, col2, col3, sqc) AS (
 *     SELECT col1, col2, col3,               -- original WITH column list
 *            ROW(0, col1, col2)              -- initial row of search columns
 *       FROM (query1) "*TLOCRN*" (col1, col2, col3)
 *   UNION [ALL]
 *     SELECT col1, col2, col3,               -- same as above
 *            ROW(sqc.depth + 1, col1, col2)  -- count depth
 *       FROM (SELECT trosl, ctename.sqc FROM ctename) "*TROCRN*" (col1, col2, col3, sqc)
 * )
 *
 * (This isn't quite legal SQL: sqc.depth is meant to refer to the first
 * column of sqc, which has a row type, but the field names are not defined
 * here.  Representing this properly in SQL would be more complicated (and the
 * SQL standard actually does it in that more complicated way), but the
 * internal representation allows us to construct it this way.)
 *
 * With a search clause
 *
 * SEARCH DEPTH FIRST BY col1, col2 SET sqc
 *
 * the CTE is rewritten to
 *
 * WITH RECURSIVE ctename (col1, col2, col3, sqc) AS (
 *     SELECT col1, col2, col3,               -- original WITH column list
 *            ARRAY[ROW(col1, col2)]          -- initial row of search columns
 *       FROM (query1) "*TLOCRN*" (col1, col2, col3)
 *   UNION [ALL]
 *     SELECT col1, col2, col3,               -- same as above
 *            sqc || ARRAY[ROW(col1, col2)]   -- record rows seen
 *       FROM (SELECT trosl, ctename.sqc FROM ctename) "*TROCRN*" (col1, col2, col3, sqc)
 * )
 *
 * With a cycle clause
 *
 * CYCLE col1, col2 SET cmc TO 'Y' DEFAULT 'N' USING cpa
 *
 * (cmc = cycle mark column, cpa = cycle path) the CTE is rewritten to
 *
 * WITH RECURSIVE ctename (col1, col2, col3, cmc, cpa) AS (
 *     SELECT col1, col2, col3,               -- original WITH column list
 *            'N',                            -- cycle mark default
 *            ARRAY[ROW(col1, col2)]          -- initial row of cycle columns
 *       FROM (query1) "*TLOCRN*" (col1, col2, col3)
 *   UNION [ALL]
 *     SELECT col1, col2, col3,               -- same as above
 *            CASE WHEN ROW(col1, col2) = ANY (ARRAY[cpa]) THEN 'Y' ELSE 'N' END,  -- compute cycle mark column
 *            cpa || ARRAY[ROW(col1, col2)]   -- record rows seen
 *       FROM (SELECT trosl, ctename.cmc, ctename.cpa FROM ctename) "*TROCRN*" (col1, col2, col3, cmc, cpa)
 *       WHERE cmc <> 'Y'
 * )
 *
 * The expression to compute the cycle mark column in the right-hand query is
 * written as
 *
 * CASE WHEN ROW(col1, col2) IN (SELECT p.* FROM TABLE(cpa) p) THEN cmv ELSE cmd END
 *
 * in the SQL standard, but in PostgreSQL we can use the scalar-array operator
 * expression shown above.
 *
 * Also, in some of the cases where operators are shown above we actually
 * directly produce the underlying function call.
 *
 * If both a search clause and a cycle clause is specified, then the search
 * clause column is added before the cycle clause columns.
 */

/*
 * Make a RowExpr from the specified column names, which have to be among the
 * output columns of the CTE.
 */
static RowExpr *
make_path_rowexpr(const CommonTableExpr *cte, const List *col_list)
{
	RowExpr    *rowexpr;
	ListCell   *lc;

	rowexpr = makeNode(RowExpr);
	rowexpr->row_typeid = RECORDOID;
	rowexpr->row_format = COERCE_IMPLICIT_CAST;
	rowexpr->location = -1;

	foreach(lc, col_list)
	{
		char	   *colname = strVal(lfirst(lc));

		for (int i = 0; i < list_length(cte->ctecolnames); i++)
		{
			char	   *colname2 = strVal(list_nth(cte->ctecolnames, i));

			if (strcmp(colname, colname2) == 0)
			{
				Var		   *var;

				var = makeVar(1, i + 1,
							  list_nth_oid(cte->ctecoltypes, i),
							  list_nth_int(cte->ctecoltypmods, i),
							  list_nth_oid(cte->ctecolcollations, i),
							  0);
				rowexpr->args = lappend(rowexpr->args, var);
				rowexpr->colnames = lappend(rowexpr->colnames, makeString(colname));
				break;
			}
		}
	}

	return rowexpr;
}

/*
 * Wrap a RowExpr in an ArrayExpr, for the initial search depth first or cycle
 * row.
 */
static Expr *
make_path_initial_array(RowExpr *rowexpr)
{
	ArrayExpr  *arr;

	arr = makeNode(ArrayExpr);
	arr->array_typeid = RECORDARRAYOID;
	arr->element_typeid = RECORDOID;
	arr->location = -1;
	arr->elements = list_make1(rowexpr);

	return (Expr *) arr;
}

/*
 * Make an array catenation expression like
 *
 * cpa || ARRAY[ROW(cols)]
 *
 * where the varattno of cpa is provided as path_varattno.
 */
static Expr *
make_path_cat_expr(RowExpr *rowexpr, AttrNumber path_varattno)
{
	ArrayExpr  *arr;
	FuncExpr   *fexpr;

	arr = makeNode(ArrayExpr);
	arr->array_typeid = RECORDARRAYOID;
	arr->element_typeid = RECORDOID;
	arr->location = -1;
	arr->elements = list_make1(rowexpr);

	fexpr = makeFuncExpr(F_ARRAY_CAT, RECORDARRAYOID,
						 list_make2(makeVar(1, path_varattno, RECORDARRAYOID, -1, 0, 0),
									arr),
						 InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);

	return (Expr *) fexpr;
}

/*
 * The real work happens here.
 */
CommonTableExpr *
rewriteSearchAndCycle(CommonTableExpr *cte)
{
	Query	   *ctequery;
	SetOperationStmt *sos;
	int			rti1,
				rti2;
	RangeTblEntry *rte1,
			   *rte2,
			   *newrte;
	Query	   *newq1,
			   *newq2;
	Query	   *newsubquery;
	RangeTblRef *rtr;
	Oid			search_seq_type = InvalidOid;
	AttrNumber	sqc_attno = InvalidAttrNumber;
	AttrNumber	cmc_attno = InvalidAttrNumber;
	AttrNumber	cpa_attno = InvalidAttrNumber;
	TargetEntry *tle;
	RowExpr    *cycle_col_rowexpr = NULL;
	RowExpr    *search_col_rowexpr = NULL;
	List	   *ewcl;
	int			cte_rtindex = -1;

	Assert(cte->search_clause || cte->cycle_clause);

	cte = copyObject(cte);

	ctequery = castNode(Query, cte->ctequery);

	/*
	 * The top level of the CTE's query should be a UNION.  Find the two
	 * subqueries.
	 */
	Assert(ctequery->setOperations);
	sos = castNode(SetOperationStmt, ctequery->setOperations);
	Assert(sos->op == SETOP_UNION);

	rti1 = castNode(RangeTblRef, sos->larg)->rtindex;
	rti2 = castNode(RangeTblRef, sos->rarg)->rtindex;

	rte1 = rt_fetch(rti1, ctequery->rtable);
	rte2 = rt_fetch(rti2, ctequery->rtable);

	Assert(rte1->rtekind == RTE_SUBQUERY);
	Assert(rte2->rtekind == RTE_SUBQUERY);

	/*
	 * We'll need this a few times later.
	 */
	if (cte->search_clause)
	{
		if (cte->search_clause->search_breadth_first)
			search_seq_type = RECORDOID;
		else
			search_seq_type = RECORDARRAYOID;
	}

	/*
	 * Attribute numbers of the added columns in the CTE's column list
	 */
	if (cte->search_clause)
		sqc_attno = list_length(cte->ctecolnames) + 1;
	if (cte->cycle_clause)
	{
		cmc_attno = list_length(cte->ctecolnames) + 1;
		cpa_attno = list_length(cte->ctecolnames) + 2;
		if (cte->search_clause)
		{
			cmc_attno++;
			cpa_attno++;
		}
	}

	/*
	 * Make new left subquery
	 */
	newq1 = makeNode(Query);
	newq1->commandType = CMD_SELECT;
	newq1->canSetTag = true;

	newrte = makeNode(RangeTblEntry);
	newrte->rtekind = RTE_SUBQUERY;
	newrte->alias = makeAlias("*TLOCRN*", cte->ctecolnames);
	newrte->eref = newrte->alias;
	newsubquery = copyObject(rte1->subquery);
	IncrementVarSublevelsUp((Node *) newsubquery, 1, 1);
	newrte->subquery = newsubquery;
	newrte->inFromCl = true;
	newq1->rtable = list_make1(newrte);

	rtr = makeNode(RangeTblRef);
	rtr->rtindex = 1;
	newq1->jointree = makeFromExpr(list_make1(rtr), NULL);

	/*
	 * Make target list
	 */
	for (int i = 0; i < list_length(cte->ctecolnames); i++)
	{
		Var		   *var;

		var = makeVar(1, i + 1,
					  list_nth_oid(cte->ctecoltypes, i),
					  list_nth_int(cte->ctecoltypmods, i),
					  list_nth_oid(cte->ctecolcollations, i),
					  0);
		tle = makeTargetEntry((Expr *) var, i + 1, strVal(list_nth(cte->ctecolnames, i)), false);
		tle->resorigtbl = list_nth_node(TargetEntry, rte1->subquery->targetList, i)->resorigtbl;
		tle->resorigcol = list_nth_node(TargetEntry, rte1->subquery->targetList, i)->resorigcol;
		newq1->targetList = lappend(newq1->targetList, tle);
	}

	if (cte->search_clause)
	{
		Expr	   *texpr;

		search_col_rowexpr = make_path_rowexpr(cte, cte->search_clause->search_col_list);
		if (cte->search_clause->search_breadth_first)
		{
			search_col_rowexpr->args = lcons(makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
													   Int64GetDatum(0), false, FLOAT8PASSBYVAL),
											 search_col_rowexpr->args);
			search_col_rowexpr->colnames = lcons(makeString("*DEPTH*"), search_col_rowexpr->colnames);
			texpr = (Expr *) search_col_rowexpr;
		}
		else
			texpr = make_path_initial_array(search_col_rowexpr);
		tle = makeTargetEntry(texpr,
							  list_length(newq1->targetList) + 1,
							  cte->search_clause->search_seq_column,
							  false);
		newq1->targetList = lappend(newq1->targetList, tle);
	}
	if (cte->cycle_clause)
	{
		tle = makeTargetEntry((Expr *) cte->cycle_clause->cycle_mark_default,
							  list_length(newq1->targetList) + 1,
							  cte->cycle_clause->cycle_mark_column,
							  false);
		newq1->targetList = lappend(newq1->targetList, tle);
		cycle_col_rowexpr = make_path_rowexpr(cte, cte->cycle_clause->cycle_col_list);
		tle = makeTargetEntry(make_path_initial_array(cycle_col_rowexpr),
							  list_length(newq1->targetList) + 1,
							  cte->cycle_clause->cycle_path_column,
							  false);
		newq1->targetList = lappend(newq1->targetList, tle);
	}

	rte1->subquery = newq1;

	if (cte->search_clause)
	{
		rte1->eref->colnames = lappend(rte1->eref->colnames, makeString(cte->search_clause->search_seq_column));
	}
	if (cte->cycle_clause)
	{
		rte1->eref->colnames = lappend(rte1->eref->colnames, makeString(cte->cycle_clause->cycle_mark_column));
		rte1->eref->colnames = lappend(rte1->eref->colnames, makeString(cte->cycle_clause->cycle_path_column));
	}

	/*
	 * Make new right subquery
	 */
	newq2 = makeNode(Query);
	newq2->commandType = CMD_SELECT;
	newq2->canSetTag = true;

	newrte = makeNode(RangeTblEntry);
	newrte->rtekind = RTE_SUBQUERY;
	ewcl = copyObject(cte->ctecolnames);
	if (cte->search_clause)
	{
		ewcl = lappend(ewcl, makeString(cte->search_clause->search_seq_column));
	}
	if (cte->cycle_clause)
	{
		ewcl = lappend(ewcl, makeString(cte->cycle_clause->cycle_mark_column));
		ewcl = lappend(ewcl, makeString(cte->cycle_clause->cycle_path_column));
	}
	newrte->alias = makeAlias("*TROCRN*", ewcl);
	newrte->eref = newrte->alias;

	/*
	 * Find the reference to the recursive CTE in the right UNION subquery's
	 * range table.  We expect it to be two levels up from the UNION subquery
	 * (and must check that to avoid being fooled by sub-WITHs with the same
	 * CTE name).  There will not be more than one such reference, because the
	 * parser would have rejected that (see checkWellFormedRecursion() in
	 * parse_cte.c).  However, the parser doesn't insist that the reference
	 * appear in the UNION subquery's topmost range table, so we might fail to
	 * find it at all.  That's an unimplemented case for the moment.
	 */
	for (int rti = 1; rti <= list_length(rte2->subquery->rtable); rti++)
	{
		RangeTblEntry *e = rt_fetch(rti, rte2->subquery->rtable);

		if (e->rtekind == RTE_CTE &&
			strcmp(cte->ctename, e->ctename) == 0 &&
			e->ctelevelsup == 2)
		{
			cte_rtindex = rti;
			break;
		}
	}
	if (cte_rtindex <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("with a SEARCH or CYCLE clause, the recursive reference to WITH query \"%s\" must be at the top level of its right-hand SELECT",
						cte->ctename)));

	newsubquery = copyObject(rte2->subquery);
	IncrementVarSublevelsUp((Node *) newsubquery, 1, 1);

	/*
	 * Add extra columns to target list of subquery of right subquery
	 */
	if (cte->search_clause)
	{
		Var		   *var;

		/* ctename.sqc */
		var = makeVar(cte_rtindex, sqc_attno,
					  search_seq_type, -1, InvalidOid, 0);
		tle = makeTargetEntry((Expr *) var,
							  list_length(newsubquery->targetList) + 1,
							  cte->search_clause->search_seq_column,
							  false);
		newsubquery->targetList = lappend(newsubquery->targetList, tle);
	}
	if (cte->cycle_clause)
	{
		Var		   *var;

		/* ctename.cmc */
		var = makeVar(cte_rtindex, cmc_attno,
					  cte->cycle_clause->cycle_mark_type,
					  cte->cycle_clause->cycle_mark_typmod,
					  cte->cycle_clause->cycle_mark_collation, 0);
		tle = makeTargetEntry((Expr *) var,
							  list_length(newsubquery->targetList) + 1,
							  cte->cycle_clause->cycle_mark_column,
							  false);
		newsubquery->targetList = lappend(newsubquery->targetList, tle);

		/* ctename.cpa */
		var = makeVar(cte_rtindex, cpa_attno,
					  RECORDARRAYOID, -1, InvalidOid, 0);
		tle = makeTargetEntry((Expr *) var,
							  list_length(newsubquery->targetList) + 1,
							  cte->cycle_clause->cycle_path_column,
							  false);
		newsubquery->targetList = lappend(newsubquery->targetList, tle);
	}

	newrte->subquery = newsubquery;
	newrte->inFromCl = true;
	newq2->rtable = list_make1(newrte);

	rtr = makeNode(RangeTblRef);
	rtr->rtindex = 1;

	if (cte->cycle_clause)
	{
		Expr	   *expr;

		/*
		 * Add cmc <> cmv condition
		 */
		expr = make_opclause(cte->cycle_clause->cycle_mark_neop, BOOLOID, false,
							 (Expr *) makeVar(1, cmc_attno,
											  cte->cycle_clause->cycle_mark_type,
											  cte->cycle_clause->cycle_mark_typmod,
											  cte->cycle_clause->cycle_mark_collation, 0),
							 (Expr *) cte->cycle_clause->cycle_mark_value,
							 InvalidOid,
							 cte->cycle_clause->cycle_mark_collation);

		newq2->jointree = makeFromExpr(list_make1(rtr), (Node *) expr);
	}
	else
		newq2->jointree = makeFromExpr(list_make1(rtr), NULL);

	/*
	 * Make target list
	 */
	for (int i = 0; i < list_length(cte->ctecolnames); i++)
	{
		Var		   *var;

		var = makeVar(1, i + 1,
					  list_nth_oid(cte->ctecoltypes, i),
					  list_nth_int(cte->ctecoltypmods, i),
					  list_nth_oid(cte->ctecolcollations, i),
					  0);
		tle = makeTargetEntry((Expr *) var, i + 1, strVal(list_nth(cte->ctecolnames, i)), false);
		tle->resorigtbl = list_nth_node(TargetEntry, rte2->subquery->targetList, i)->resorigtbl;
		tle->resorigcol = list_nth_node(TargetEntry, rte2->subquery->targetList, i)->resorigcol;
		newq2->targetList = lappend(newq2->targetList, tle);
	}

	if (cte->search_clause)
	{
		Expr	   *texpr;

		if (cte->search_clause->search_breadth_first)
		{
			FieldSelect *fs;
			FuncExpr   *fexpr;

			/*
			 * ROW(sqc.depth + 1, cols)
			 */

			search_col_rowexpr = copyObject(search_col_rowexpr);

			fs = makeNode(FieldSelect);
			fs->arg = (Expr *) makeVar(1, sqc_attno, RECORDOID, -1, 0, 0);
			fs->fieldnum = 1;
			fs->resulttype = INT8OID;
			fs->resulttypmod = -1;

			fexpr = makeFuncExpr(F_INT8INC, INT8OID, list_make1(fs), InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);

			lfirst(list_head(search_col_rowexpr->args)) = fexpr;

			texpr = (Expr *) search_col_rowexpr;
		}
		else
		{
			/*
			 * sqc || ARRAY[ROW(cols)]
			 */
			texpr = make_path_cat_expr(search_col_rowexpr, sqc_attno);
		}
		tle = makeTargetEntry(texpr,
							  list_length(newq2->targetList) + 1,
							  cte->search_clause->search_seq_column,
							  false);
		newq2->targetList = lappend(newq2->targetList, tle);
	}

	if (cte->cycle_clause)
	{
		ScalarArrayOpExpr *saoe;
		CaseExpr   *caseexpr;
		CaseWhen   *casewhen;

		/*
		 * CASE WHEN ROW(cols) = ANY (ARRAY[cpa]) THEN cmv ELSE cmd END
		 */

		saoe = makeNode(ScalarArrayOpExpr);
		saoe->location = -1;
		saoe->opno = RECORD_EQ_OP;
		saoe->useOr = true;
		saoe->args = list_make2(cycle_col_rowexpr,
								makeVar(1, cpa_attno, RECORDARRAYOID, -1, 0, 0));

		caseexpr = makeNode(CaseExpr);
		caseexpr->location = -1;
		caseexpr->casetype = cte->cycle_clause->cycle_mark_type;
		caseexpr->casecollid = cte->cycle_clause->cycle_mark_collation;
		casewhen = makeNode(CaseWhen);
		casewhen->location = -1;
		casewhen->expr = (Expr *) saoe;
		casewhen->result = (Expr *) cte->cycle_clause->cycle_mark_value;
		caseexpr->args = list_make1(casewhen);
		caseexpr->defresult = (Expr *) cte->cycle_clause->cycle_mark_default;

		tle = makeTargetEntry((Expr *) caseexpr,
							  list_length(newq2->targetList) + 1,
							  cte->cycle_clause->cycle_mark_column,
							  false);
		newq2->targetList = lappend(newq2->targetList, tle);

		/*
		 * cpa || ARRAY[ROW(cols)]
		 */
		tle = makeTargetEntry(make_path_cat_expr(cycle_col_rowexpr, cpa_attno),
							  list_length(newq2->targetList) + 1,
							  cte->cycle_clause->cycle_path_column,
							  false);
		newq2->targetList = lappend(newq2->targetList, tle);
	}

	rte2->subquery = newq2;

	if (cte->search_clause)
	{
		rte2->eref->colnames = lappend(rte2->eref->colnames, makeString(cte->search_clause->search_seq_column));
	}
	if (cte->cycle_clause)
	{
		rte2->eref->colnames = lappend(rte2->eref->colnames, makeString(cte->cycle_clause->cycle_mark_column));
		rte2->eref->colnames = lappend(rte2->eref->colnames, makeString(cte->cycle_clause->cycle_path_column));
	}

	/*
	 * Add the additional columns to the SetOperationStmt
	 */
	if (cte->search_clause)
	{
		sos->colTypes = lappend_oid(sos->colTypes, search_seq_type);
		sos->colTypmods = lappend_int(sos->colTypmods, -1);
		sos->colCollations = lappend_oid(sos->colCollations, InvalidOid);
		if (!sos->all)
			sos->groupClauses = lappend(sos->groupClauses,
										makeSortGroupClauseForSetOp(search_seq_type, true));
	}
	if (cte->cycle_clause)
	{
		sos->colTypes = lappend_oid(sos->colTypes, cte->cycle_clause->cycle_mark_type);
		sos->colTypmods = lappend_int(sos->colTypmods, cte->cycle_clause->cycle_mark_typmod);
		sos->colCollations = lappend_oid(sos->colCollations, cte->cycle_clause->cycle_mark_collation);
		if (!sos->all)
			sos->groupClauses = lappend(sos->groupClauses,
										makeSortGroupClauseForSetOp(cte->cycle_clause->cycle_mark_type, true));

		sos->colTypes = lappend_oid(sos->colTypes, RECORDARRAYOID);
		sos->colTypmods = lappend_int(sos->colTypmods, -1);
		sos->colCollations = lappend_oid(sos->colCollations, InvalidOid);
		if (!sos->all)
			sos->groupClauses = lappend(sos->groupClauses,
										makeSortGroupClauseForSetOp(RECORDARRAYOID, true));
	}

	/*
	 * Add the additional columns to the CTE query's target list
	 */
	if (cte->search_clause)
	{
		ctequery->targetList = lappend(ctequery->targetList,
									   makeTargetEntry((Expr *) makeVar(1, sqc_attno,
																		search_seq_type, -1, InvalidOid, 0),
													   list_length(ctequery->targetList) + 1,
													   cte->search_clause->search_seq_column,
													   false));
	}
	if (cte->cycle_clause)
	{
		ctequery->targetList = lappend(ctequery->targetList,
									   makeTargetEntry((Expr *) makeVar(1, cmc_attno,
																		cte->cycle_clause->cycle_mark_type,
																		cte->cycle_clause->cycle_mark_typmod,
																		cte->cycle_clause->cycle_mark_collation, 0),
													   list_length(ctequery->targetList) + 1,
													   cte->cycle_clause->cycle_mark_column,
													   false));
		ctequery->targetList = lappend(ctequery->targetList,
									   makeTargetEntry((Expr *) makeVar(1, cpa_attno,
																		RECORDARRAYOID, -1, InvalidOid, 0),
													   list_length(ctequery->targetList) + 1,
													   cte->cycle_clause->cycle_path_column,
													   false));
	}

	/*
	 * Add the additional columns to the CTE's output columns
	 */
	cte->ctecolnames = ewcl;
	if (cte->search_clause)
	{
		cte->ctecoltypes = lappend_oid(cte->ctecoltypes, search_seq_type);
		cte->ctecoltypmods = lappend_int(cte->ctecoltypmods, -1);
		cte->ctecolcollations = lappend_oid(cte->ctecolcollations, InvalidOid);
	}
	if (cte->cycle_clause)
	{
		cte->ctecoltypes = lappend_oid(cte->ctecoltypes, cte->cycle_clause->cycle_mark_type);
		cte->ctecoltypmods = lappend_int(cte->ctecoltypmods, cte->cycle_clause->cycle_mark_typmod);
		cte->ctecolcollations = lappend_oid(cte->ctecolcollations, cte->cycle_clause->cycle_mark_collation);

		cte->ctecoltypes = lappend_oid(cte->ctecoltypes, RECORDARRAYOID);
		cte->ctecoltypmods = lappend_int(cte->ctecoltypmods, -1);
		cte->ctecolcollations = lappend_oid(cte->ctecolcollations, InvalidOid);
	}

	return cte;
}
