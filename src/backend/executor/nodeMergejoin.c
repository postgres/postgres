/*-------------------------------------------------------------------------
 *
 * nodeMergejoin.c
 *	  routines supporting merge joins
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeMergejoin.c,v 1.82.2.1 2007/02/02 00:07:28 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecMergeJoin			mergejoin outer and inner relations.
 *		ExecInitMergeJoin		creates and initializes run time states
 *		ExecEndMergeJoin		cleans up the node.
 *
 * NOTES
 *
 *		Merge-join is done by joining the inner and outer tuples satisfying
 *		join clauses of the form ((= outerKey innerKey) ...).
 *		The join clause list is provided by the query planner and may contain
 *		more than one (= outerKey innerKey) clause (for composite sort key).
 *
 *		However, the query executor needs to know whether an outer
 *		tuple is "greater/smaller" than an inner tuple so that it can
 *		"synchronize" the two relations. For example, consider the following
 *		relations:
 *
 *				outer: (0 ^1 1 2 5 5 5 6 6 7)	current tuple: 1
 *				inner: (1 ^3 5 5 5 5 6)			current tuple: 3
 *
 *		To continue the merge-join, the executor needs to scan both inner
 *		and outer relations till the matching tuples 5. It needs to know
 *		that currently inner tuple 3 is "greater" than outer tuple 1 and
 *		therefore it should scan the outer relation first to find a
 *		matching tuple and so on.
 *
 *		Therefore, when initializing the merge-join node, we look up the
 *		associated sort operators.	We assume the planner has seen to it
 *		that the inputs are correctly sorted by these operators.  Rather
 *		than directly executing the merge join clauses, we evaluate the
 *		left and right key expressions separately and then compare the
 *		columns one at a time (see MJCompare).
 *
 *
 *		Consider the above relations and suppose that the executor has
 *		just joined the first outer "5" with the last inner "5". The
 *		next step is of course to join the second outer "5" with all
 *		the inner "5's". This requires repositioning the inner "cursor"
 *		to point at the first inner "5". This is done by "marking" the
 *		first inner 5 so we can restore the "cursor" to it before joining
 *		with the second outer 5. The access method interface provides
 *		routines to mark and restore to a tuple.
 *
 *
 *		Essential operation of the merge join algorithm is as follows:
 *
 *		Join {
 *			get initial outer and inner tuples				INITIALIZE
 *			do forever {
 *				while (outer != inner) {					SKIP_TEST
 *					if (outer < inner)
 *						advance outer						SKIPOUTER_ADVANCE
 *					else
 *						advance inner						SKIPINNER_ADVANCE
 *				}
 *				mark inner position							SKIP_TEST
 *				do forever {
 *					while (outer == inner) {
 *						join tuples							JOINTUPLES
 *						advance inner position				NEXTINNER
 *					}
 *					advance outer position					NEXTOUTER
 *					if (outer == mark)						TESTOUTER
 *						restore inner position to mark		TESTOUTER
 *					else
 *						break	// return to top of outer loop
 *				}
 *			}
 *		}
 *
 *		The merge join operation is coded in the fashion
 *		of a state machine.  At each state, we do something and then
 *		proceed to another state.  This state is stored in the node's
 *		execution state information and is preserved across calls to
 *		ExecMergeJoin. -cim 10/31/89
 */
#include "postgres.h"

#include "access/nbtree.h"
#include "catalog/pg_amop.h"
#include "executor/execdebug.h"
#include "executor/execdefs.h"
#include "executor/nodeMergejoin.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"


/*
 * Comparison strategies supported by MJCompare
 *
 * XXX eventually should extend these to support descending-order sorts.
 * There are some tricky issues however about being sure we are on the same
 * page as the underlying sort or index as to which end NULLs sort to.
 */
typedef enum
{
	MERGEFUNC_LT,				/* raw "<" operator */
	MERGEFUNC_CMP				/* -1 / 0 / 1 three-way comparator */
} MergeFunctionKind;

/* Runtime data for each mergejoin clause */
typedef struct MergeJoinClauseData
{
	/* Executable expression trees */
	ExprState  *lexpr;			/* left-hand (outer) input expression */
	ExprState  *rexpr;			/* right-hand (inner) input expression */

	/*
	 * If we have a current left or right input tuple, the values of the
	 * expressions are loaded into these fields:
	 */
	Datum		ldatum;			/* current left-hand value */
	Datum		rdatum;			/* current right-hand value */
	bool		lisnull;		/* and their isnull flags */
	bool		risnull;

	/*
	 * Remember whether mergejoin operator is strict (usually it will be).
	 * NOTE: if it's not strict, we still assume it cannot return true for one
	 * null and one non-null input.
	 */
	bool		mergestrict;

	/*
	 * The comparison strategy in use, and the lookup info to let us call the
	 * needed comparison routines.	eqfinfo is the "=" operator itself.
	 * cmpfinfo is either the btree comparator or the "<" operator.
	 */
	MergeFunctionKind cmpstrategy;
	FmgrInfo	eqfinfo;
	FmgrInfo	cmpfinfo;
} MergeJoinClauseData;


#define MarkInnerTuple(innerTupleSlot, mergestate) \
	ExecCopySlot((mergestate)->mj_MarkedTupleSlot, (innerTupleSlot))


/*
 * MJExamineQuals
 *
 * This deconstructs the list of mergejoinable expressions, which is given
 * to us by the planner in the form of a list of "leftexpr = rightexpr"
 * expression trees in the order matching the sort columns of the inputs.
 * We build an array of MergeJoinClause structs containing the information
 * we will need at runtime.  Each struct essentially tells us how to compare
 * the two expressions from the original clause.
 *
 * The best, most efficient way to compare two expressions is to use a btree
 * comparison support routine, since that requires only one function call
 * per comparison.	Hence we try to find a btree opclass that matches the
 * mergejoinable operator.	If we cannot find one, we'll have to call both
 * the "=" and (often) the "<" operator for each comparison.
 */
static MergeJoinClause
MJExamineQuals(List *qualList, PlanState *parent)
{
	MergeJoinClause clauses;
	int			nClauses = list_length(qualList);
	int			iClause;
	ListCell   *l;

	clauses = (MergeJoinClause) palloc0(nClauses * sizeof(MergeJoinClauseData));

	iClause = 0;
	foreach(l, qualList)
	{
		OpExpr	   *qual = (OpExpr *) lfirst(l);
		MergeJoinClause clause = &clauses[iClause];
		Oid			ltop;
		Oid			gtop;
		RegProcedure ltproc;
		RegProcedure gtproc;
		AclResult	aclresult;
		CatCList   *catlist;
		int			i;

		if (!IsA(qual, OpExpr))
			elog(ERROR, "mergejoin clause is not an OpExpr");

		/*
		 * Prepare the input expressions for execution.
		 */
		clause->lexpr = ExecInitExpr((Expr *) linitial(qual->args), parent);
		clause->rexpr = ExecInitExpr((Expr *) lsecond(qual->args), parent);

		/*
		 * Check permission to call the mergejoinable operator. For
		 * predictability, we check this even if we end up not using it.
		 */
		aclresult = pg_proc_aclcheck(qual->opfuncid, GetUserId(), ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_PROC,
						   get_func_name(qual->opfuncid));

		/* Set up the fmgr lookup information */
		fmgr_info(qual->opfuncid, &(clause->eqfinfo));

		/* And remember strictness */
		clause->mergestrict = clause->eqfinfo.fn_strict;

		/*
		 * Lookup the comparison operators that go with the mergejoinable
		 * top-level operator.	(This will elog if the operator isn't
		 * mergejoinable, which would be the planner's mistake.)
		 */
		op_mergejoin_crossops(qual->opno,
							  &ltop,
							  &gtop,
							  &ltproc,
							  &gtproc);

		clause->cmpstrategy = MERGEFUNC_LT;

		/*
		 * Look for a btree opclass including all three operators. This is
		 * much like SelectSortFunction except we insist on matching all the
		 * operators provided, and it can be a cross-type opclass.
		 *
		 * XXX for now, insist on forward sort so that NULLs can be counted on
		 * to be high.
		 */
		catlist = SearchSysCacheList(AMOPOPID, 1,
									 ObjectIdGetDatum(qual->opno),
									 0, 0, 0);

		for (i = 0; i < catlist->n_members; i++)
		{
			HeapTuple	tuple = &catlist->members[i]->tuple;
			Form_pg_amop aform = (Form_pg_amop) GETSTRUCT(tuple);
			Oid			opcid = aform->amopclaid;

			if (aform->amopstrategy != BTEqualStrategyNumber)
				continue;
			if (!opclass_is_btree(opcid))
				continue;
			if (get_op_opclass_strategy(ltop, opcid) == BTLessStrategyNumber &&
			 get_op_opclass_strategy(gtop, opcid) == BTGreaterStrategyNumber)
			{
				clause->cmpstrategy = MERGEFUNC_CMP;
				ltproc = get_opclass_proc(opcid, aform->amopsubtype,
										  BTORDER_PROC);
				Assert(RegProcedureIsValid(ltproc));
				break;			/* done looking */
			}
		}

		ReleaseSysCacheList(catlist);

		/* Check permission to call "<" operator or cmp function */
		aclresult = pg_proc_aclcheck(ltproc, GetUserId(), ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_PROC,
						   get_func_name(ltproc));

		/* Set up the fmgr lookup information */
		fmgr_info(ltproc, &(clause->cmpfinfo));

		iClause++;
	}

	return clauses;
}

/*
 * MJEvalOuterValues
 *
 * Compute the values of the mergejoined expressions for the current
 * outer tuple.  We also detect whether it's impossible for the current
 * outer tuple to match anything --- this is true if it yields a NULL
 * input for any strict mergejoin operator.
 *
 * We evaluate the values in OuterEContext, which can be reset each
 * time we move to a new tuple.
 */
static bool
MJEvalOuterValues(MergeJoinState *mergestate)
{
	ExprContext *econtext = mergestate->mj_OuterEContext;
	bool		canmatch = true;
	int			i;
	MemoryContext oldContext;

	ResetExprContext(econtext);

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	econtext->ecxt_outertuple = mergestate->mj_OuterTupleSlot;

	for (i = 0; i < mergestate->mj_NumClauses; i++)
	{
		MergeJoinClause clause = &mergestate->mj_Clauses[i];

		clause->ldatum = ExecEvalExpr(clause->lexpr, econtext,
									  &clause->lisnull, NULL);
		if (clause->lisnull && clause->mergestrict)
			canmatch = false;
	}

	MemoryContextSwitchTo(oldContext);

	return canmatch;
}

/*
 * MJEvalInnerValues
 *
 * Same as above, but for the inner tuple.	Here, we have to be prepared
 * to load data from either the true current inner, or the marked inner,
 * so caller must tell us which slot to load from.
 */
static bool
MJEvalInnerValues(MergeJoinState *mergestate, TupleTableSlot *innerslot)
{
	ExprContext *econtext = mergestate->mj_InnerEContext;
	bool		canmatch = true;
	int			i;
	MemoryContext oldContext;

	ResetExprContext(econtext);

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	econtext->ecxt_innertuple = innerslot;

	for (i = 0; i < mergestate->mj_NumClauses; i++)
	{
		MergeJoinClause clause = &mergestate->mj_Clauses[i];

		clause->rdatum = ExecEvalExpr(clause->rexpr, econtext,
									  &clause->risnull, NULL);
		if (clause->risnull && clause->mergestrict)
			canmatch = false;
	}

	MemoryContextSwitchTo(oldContext);

	return canmatch;
}

/*
 * MJCompare
 *
 * Compare the mergejoinable values of the current two input tuples
 * and return 0 if they are equal (ie, the mergejoin equalities all
 * succeed), +1 if outer > inner, -1 if outer < inner.
 *
 * MJEvalOuterValues and MJEvalInnerValues must already have been called
 * for the current outer and inner tuples, respectively.
 */
static int
MJCompare(MergeJoinState *mergestate)
{
	int			result = 0;
	bool		nulleqnull = false;
	ExprContext *econtext = mergestate->js.ps.ps_ExprContext;
	int			i;
	MemoryContext oldContext;
	FunctionCallInfoData fcinfo;

	/*
	 * Call the comparison functions in short-lived context, in case they leak
	 * memory.
	 */
	ResetExprContext(econtext);

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	for (i = 0; i < mergestate->mj_NumClauses; i++)
	{
		MergeJoinClause clause = &mergestate->mj_Clauses[i];
		Datum		fresult;

		/*
		 * Deal with null inputs.  We treat NULL as sorting after non-NULL.
		 *
		 * If both inputs are NULL, and the comparison function isn't strict,
		 * then we call it and check for a true result (this allows operators
		 * that behave like IS NOT DISTINCT to be mergejoinable). If the
		 * function is strict or returns false, we temporarily pretend NULL ==
		 * NULL and contine checking remaining columns.
		 */
		if (clause->lisnull)
		{
			if (clause->risnull)
			{
				if (!clause->eqfinfo.fn_strict)
				{
					InitFunctionCallInfoData(fcinfo, &(clause->eqfinfo), 2,
											 NULL, NULL);
					fcinfo.arg[0] = clause->ldatum;
					fcinfo.arg[1] = clause->rdatum;
					fcinfo.argnull[0] = true;
					fcinfo.argnull[1] = true;
					fresult = FunctionCallInvoke(&fcinfo);
					if (!fcinfo.isnull && DatumGetBool(fresult))
					{
						/* treat nulls as really equal */
						continue;
					}
				}
				nulleqnull = true;
				continue;
			}
			/* NULL > non-NULL */
			result = 1;
			break;
		}
		if (clause->risnull)
		{
			/* non-NULL < NULL */
			result = -1;
			break;
		}

		if (clause->cmpstrategy == MERGEFUNC_LT)
		{
			InitFunctionCallInfoData(fcinfo, &(clause->eqfinfo), 2,
									 NULL, NULL);
			fcinfo.arg[0] = clause->ldatum;
			fcinfo.arg[1] = clause->rdatum;
			fcinfo.argnull[0] = false;
			fcinfo.argnull[1] = false;
			fresult = FunctionCallInvoke(&fcinfo);
			if (fcinfo.isnull)
			{
				nulleqnull = true;
				continue;
			}
			else if (DatumGetBool(fresult))
			{
				/* equal */
				continue;
			}
			InitFunctionCallInfoData(fcinfo, &(clause->cmpfinfo), 2,
									 NULL, NULL);
			fcinfo.arg[0] = clause->ldatum;
			fcinfo.arg[1] = clause->rdatum;
			fcinfo.argnull[0] = false;
			fcinfo.argnull[1] = false;
			fresult = FunctionCallInvoke(&fcinfo);
			if (fcinfo.isnull)
			{
				nulleqnull = true;
				continue;
			}
			else if (DatumGetBool(fresult))
			{
				/* less than */
				result = -1;
				break;
			}
			else
			{
				/* greater than */
				result = 1;
				break;
			}
		}
		else
			/* must be MERGEFUNC_CMP */
		{
			InitFunctionCallInfoData(fcinfo, &(clause->cmpfinfo), 2,
									 NULL, NULL);
			fcinfo.arg[0] = clause->ldatum;
			fcinfo.arg[1] = clause->rdatum;
			fcinfo.argnull[0] = false;
			fcinfo.argnull[1] = false;
			fresult = FunctionCallInvoke(&fcinfo);
			if (fcinfo.isnull)
			{
				nulleqnull = true;
				continue;
			}
			else if (DatumGetInt32(fresult) == 0)
			{
				/* equal */
				continue;
			}
			else if (DatumGetInt32(fresult) < 0)
			{
				/* less than */
				result = -1;
				break;
			}
			else
			{
				/* greater than */
				result = 1;
				break;
			}
		}
	}

	/*
	 * If we had any null comparison results or NULL-vs-NULL inputs, we do not
	 * want to report that the tuples are equal.  Instead, if result is still
	 * 0, change it to +1.	This will result in advancing the inner side of
	 * the join.
	 */
	if (nulleqnull && result == 0)
		result = 1;

	MemoryContextSwitchTo(oldContext);

	return result;
}


/*
 * Generate a fake join tuple with nulls for the inner tuple,
 * and return it if it passes the non-join quals.
 */
static TupleTableSlot *
MJFillOuter(MergeJoinState *node)
{
	ExprContext *econtext = node->js.ps.ps_ExprContext;
	List	   *otherqual = node->js.ps.qual;

	ResetExprContext(econtext);

	econtext->ecxt_outertuple = node->mj_OuterTupleSlot;
	econtext->ecxt_innertuple = node->mj_NullInnerTupleSlot;

	if (ExecQual(otherqual, econtext, false))
	{
		/*
		 * qualification succeeded.  now form the desired projection tuple and
		 * return the slot containing it.
		 */
		TupleTableSlot *result;
		ExprDoneCond isDone;

		MJ_printf("ExecMergeJoin: returning outer fill tuple\n");

		result = ExecProject(node->js.ps.ps_ProjInfo, &isDone);

		if (isDone != ExprEndResult)
		{
			node->js.ps.ps_TupFromTlist =
				(isDone == ExprMultipleResult);
			return result;
		}
	}

	return NULL;
}

/*
 * Generate a fake join tuple with nulls for the outer tuple,
 * and return it if it passes the non-join quals.
 */
static TupleTableSlot *
MJFillInner(MergeJoinState *node)
{
	ExprContext *econtext = node->js.ps.ps_ExprContext;
	List	   *otherqual = node->js.ps.qual;

	ResetExprContext(econtext);

	econtext->ecxt_outertuple = node->mj_NullOuterTupleSlot;
	econtext->ecxt_innertuple = node->mj_InnerTupleSlot;

	if (ExecQual(otherqual, econtext, false))
	{
		/*
		 * qualification succeeded.  now form the desired projection tuple and
		 * return the slot containing it.
		 */
		TupleTableSlot *result;
		ExprDoneCond isDone;

		MJ_printf("ExecMergeJoin: returning inner fill tuple\n");

		result = ExecProject(node->js.ps.ps_ProjInfo, &isDone);

		if (isDone != ExprEndResult)
		{
			node->js.ps.ps_TupFromTlist =
				(isDone == ExprMultipleResult);
			return result;
		}
	}

	return NULL;
}


/* ----------------------------------------------------------------
 *		ExecMergeTupleDump
 *
 *		This function is called through the MJ_dump() macro
 *		when EXEC_MERGEJOINDEBUG is defined
 * ----------------------------------------------------------------
 */
#ifdef EXEC_MERGEJOINDEBUG

static void
ExecMergeTupleDumpOuter(MergeJoinState *mergestate)
{
	TupleTableSlot *outerSlot = mergestate->mj_OuterTupleSlot;

	printf("==== outer tuple ====\n");
	if (TupIsNull(outerSlot))
		printf("(nil)\n");
	else
		MJ_debugtup(outerSlot);
}

static void
ExecMergeTupleDumpInner(MergeJoinState *mergestate)
{
	TupleTableSlot *innerSlot = mergestate->mj_InnerTupleSlot;

	printf("==== inner tuple ====\n");
	if (TupIsNull(innerSlot))
		printf("(nil)\n");
	else
		MJ_debugtup(innerSlot);
}

static void
ExecMergeTupleDumpMarked(MergeJoinState *mergestate)
{
	TupleTableSlot *markedSlot = mergestate->mj_MarkedTupleSlot;

	printf("==== marked tuple ====\n");
	if (TupIsNull(markedSlot))
		printf("(nil)\n");
	else
		MJ_debugtup(markedSlot);
}

static void
ExecMergeTupleDump(MergeJoinState *mergestate)
{
	printf("******** ExecMergeTupleDump ********\n");

	ExecMergeTupleDumpOuter(mergestate);
	ExecMergeTupleDumpInner(mergestate);
	ExecMergeTupleDumpMarked(mergestate);

	printf("******** \n");
}
#endif

/* ----------------------------------------------------------------
 *		ExecMergeJoin
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecMergeJoin(MergeJoinState *node)
{
	EState	   *estate;
	List	   *joinqual;
	List	   *otherqual;
	bool		qualResult;
	int			compareResult;
	PlanState  *innerPlan;
	TupleTableSlot *innerTupleSlot;
	PlanState  *outerPlan;
	TupleTableSlot *outerTupleSlot;
	ExprContext *econtext;
	bool		doFillOuter;
	bool		doFillInner;

	/*
	 * get information from node
	 */
	estate = node->js.ps.state;
	innerPlan = innerPlanState(node);
	outerPlan = outerPlanState(node);
	econtext = node->js.ps.ps_ExprContext;
	joinqual = node->js.joinqual;
	otherqual = node->js.ps.qual;
	doFillOuter = node->mj_FillOuter;
	doFillInner = node->mj_FillInner;

	/*
	 * Check to see if we're still projecting out tuples from a previous join
	 * tuple (because there is a function-returning-set in the projection
	 * expressions).  If so, try to project another one.
	 */
	if (node->js.ps.ps_TupFromTlist)
	{
		TupleTableSlot *result;
		ExprDoneCond isDone;

		result = ExecProject(node->js.ps.ps_ProjInfo, &isDone);
		if (isDone == ExprMultipleResult)
			return result;
		/* Done with that source tuple... */
		node->js.ps.ps_TupFromTlist = false;
	}

	/*
	 * Reset per-tuple memory context to free any expression evaluation
	 * storage allocated in the previous tuple cycle.  Note this can't happen
	 * until we're done projecting out tuples from a join tuple.
	 */
	ResetExprContext(econtext);

	/*
	 * ok, everything is setup.. let's go to work
	 */
	for (;;)
	{
		MJ_dump(node);

		/*
		 * get the current state of the join and do things accordingly.
		 */
		switch (node->mj_JoinState)
		{
				/*
				 * EXEC_MJ_INITIALIZE_OUTER means that this is the first time
				 * ExecMergeJoin() has been called and so we have to fetch the
				 * first matchable tuple for both outer and inner subplans. We
				 * do the outer side in INITIALIZE_OUTER state, then advance
				 * to INITIALIZE_INNER state for the inner subplan.
				 */
			case EXEC_MJ_INITIALIZE_OUTER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_INITIALIZE_OUTER\n");

				outerTupleSlot = ExecProcNode(outerPlan);
				node->mj_OuterTupleSlot = outerTupleSlot;
				if (TupIsNull(outerTupleSlot))
				{
					MJ_printf("ExecMergeJoin: nothing in outer subplan\n");
					if (doFillInner)
					{
						/*
						 * Need to emit right-join tuples for remaining inner
						 * tuples.	We set MatchedInner = true to force the
						 * ENDOUTER state to advance inner.
						 */
						node->mj_JoinState = EXEC_MJ_ENDOUTER;
						node->mj_MatchedInner = true;
						break;
					}
					/* Otherwise we're done. */
					return NULL;
				}

				/* Compute join values and check for unmatchability */
				if (MJEvalOuterValues(node))
				{
					/* OK to go get the first inner tuple */
					node->mj_JoinState = EXEC_MJ_INITIALIZE_INNER;
				}
				else
				{
					/* Stay in same state to fetch next outer tuple */
					if (doFillOuter)
					{
						/*
						 * Generate a fake join tuple with nulls for the inner
						 * tuple, and return it if it passes the non-join
						 * quals.
						 */
						TupleTableSlot *result;

						result = MJFillOuter(node);
						if (result)
							return result;
					}
				}
				break;

			case EXEC_MJ_INITIALIZE_INNER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_INITIALIZE_INNER\n");

				innerTupleSlot = ExecProcNode(innerPlan);
				node->mj_InnerTupleSlot = innerTupleSlot;
				if (TupIsNull(innerTupleSlot))
				{
					MJ_printf("ExecMergeJoin: nothing in inner subplan\n");
					if (doFillOuter)
					{
						/*
						 * Need to emit left-join tuples for all outer tuples,
						 * including the one we just fetched.  We set
						 * MatchedOuter = false to force the ENDINNER state to
						 * emit first tuple before advancing outer.
						 */
						node->mj_JoinState = EXEC_MJ_ENDINNER;
						node->mj_MatchedOuter = false;
						break;
					}
					/* Otherwise we're done. */
					return NULL;
				}

				/* Compute join values and check for unmatchability */
				if (MJEvalInnerValues(node, innerTupleSlot))
				{
					/*
					 * OK, we have the initial tuples.	Begin by skipping
					 * non-matching tuples.
					 */
					node->mj_JoinState = EXEC_MJ_SKIP_TEST;
				}
				else
				{
					/* Stay in same state to fetch next inner tuple */
					if (doFillInner)
					{
						/*
						 * Generate a fake join tuple with nulls for the outer
						 * tuple, and return it if it passes the non-join
						 * quals.
						 */
						TupleTableSlot *result;

						result = MJFillInner(node);
						if (result)
							return result;
					}
				}
				break;

				/*
				 * EXEC_MJ_JOINTUPLES means we have two tuples which satisfied
				 * the merge clause so we join them and then proceed to get
				 * the next inner tuple (EXEC_MJ_NEXTINNER).
				 */
			case EXEC_MJ_JOINTUPLES:
				MJ_printf("ExecMergeJoin: EXEC_MJ_JOINTUPLES\n");

				/*
				 * Set the next state machine state.  The right things will
				 * happen whether we return this join tuple or just fall
				 * through to continue the state machine execution.
				 */
				node->mj_JoinState = EXEC_MJ_NEXTINNER;

				/*
				 * Check the extra qual conditions to see if we actually want
				 * to return this join tuple.  If not, can proceed with merge.
				 * We must distinguish the additional joinquals (which must
				 * pass to consider the tuples "matched" for outer-join logic)
				 * from the otherquals (which must pass before we actually
				 * return the tuple).
				 *
				 * We don't bother with a ResetExprContext here, on the
				 * assumption that we just did one while checking the merge
				 * qual.  One per tuple should be sufficient.  We do have to
				 * set up the econtext links to the tuples for ExecQual to
				 * use.
				 */
				outerTupleSlot = node->mj_OuterTupleSlot;
				econtext->ecxt_outertuple = outerTupleSlot;
				innerTupleSlot = node->mj_InnerTupleSlot;
				econtext->ecxt_innertuple = innerTupleSlot;

				if (node->js.jointype == JOIN_IN &&
					node->mj_MatchedOuter)
					qualResult = false;
				else
				{
					qualResult = (joinqual == NIL ||
								  ExecQual(joinqual, econtext, false));
					MJ_DEBUG_QUAL(joinqual, qualResult);
				}

				if (qualResult)
				{
					node->mj_MatchedOuter = true;
					node->mj_MatchedInner = true;

					qualResult = (otherqual == NIL ||
								  ExecQual(otherqual, econtext, false));
					MJ_DEBUG_QUAL(otherqual, qualResult);

					if (qualResult)
					{
						/*
						 * qualification succeeded.  now form the desired
						 * projection tuple and return the slot containing it.
						 */
						TupleTableSlot *result;
						ExprDoneCond isDone;

						MJ_printf("ExecMergeJoin: returning tuple\n");

						result = ExecProject(node->js.ps.ps_ProjInfo,
											 &isDone);

						if (isDone != ExprEndResult)
						{
							node->js.ps.ps_TupFromTlist =
								(isDone == ExprMultipleResult);
							return result;
						}
					}
				}
				break;

				/*
				 * EXEC_MJ_NEXTINNER means advance the inner scan to the next
				 * tuple. If the tuple is not nil, we then proceed to test it
				 * against the join qualification.
				 *
				 * Before advancing, we check to see if we must emit an
				 * outer-join fill tuple for this inner tuple.
				 */
			case EXEC_MJ_NEXTINNER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_NEXTINNER\n");

				if (doFillInner && !node->mj_MatchedInner)
				{
					/*
					 * Generate a fake join tuple with nulls for the outer
					 * tuple, and return it if it passes the non-join quals.
					 */
					TupleTableSlot *result;

					node->mj_MatchedInner = true;		/* do it only once */

					result = MJFillInner(node);
					if (result)
						return result;
				}

				/*
				 * now we get the next inner tuple, if any.  If there's none,
				 * advance to next outer tuple (which may be able to join to
				 * previously marked tuples).
				 */
				innerTupleSlot = ExecProcNode(innerPlan);
				node->mj_InnerTupleSlot = innerTupleSlot;
				MJ_DEBUG_PROC_NODE(innerTupleSlot);
				node->mj_MatchedInner = false;

				if (TupIsNull(innerTupleSlot))
				{
					node->mj_JoinState = EXEC_MJ_NEXTOUTER;
					break;
				}

				/*
				 * Load up the new inner tuple's comparison values.  If we see
				 * that it contains a NULL and hence can't match any outer
				 * tuple, we can skip the comparison and assume the new tuple
				 * is greater than current outer.
				 */
				if (!MJEvalInnerValues(node, innerTupleSlot))
				{
					node->mj_JoinState = EXEC_MJ_NEXTOUTER;
					break;
				}

				/*
				 * Test the new inner tuple to see if it matches outer.
				 *
				 * If they do match, then we join them and move on to the next
				 * inner tuple (EXEC_MJ_JOINTUPLES).
				 *
				 * If they do not match then advance to next outer tuple.
				 */
				compareResult = MJCompare(node);
				MJ_DEBUG_COMPARE(compareResult);

				if (compareResult == 0)
					node->mj_JoinState = EXEC_MJ_JOINTUPLES;
				else
				{
					Assert(compareResult < 0);
					node->mj_JoinState = EXEC_MJ_NEXTOUTER;
				}
				break;

				/*-------------------------------------------
				 * EXEC_MJ_NEXTOUTER means
				 *
				 *				outer inner
				 * outer tuple -  5		5  - marked tuple
				 *				  5		5
				 *				  6		6  - inner tuple
				 *				  7		7
				 *
				 * we know we just bumped into the
				 * first inner tuple > current outer tuple (or possibly
				 * the end of the inner stream)
				 * so get a new outer tuple and then
				 * proceed to test it against the marked tuple
				 * (EXEC_MJ_TESTOUTER)
				 *
				 * Before advancing, we check to see if we must emit an
				 * outer-join fill tuple for this outer tuple.
				 *------------------------------------------------
				 */
			case EXEC_MJ_NEXTOUTER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_NEXTOUTER\n");

				if (doFillOuter && !node->mj_MatchedOuter)
				{
					/*
					 * Generate a fake join tuple with nulls for the inner
					 * tuple, and return it if it passes the non-join quals.
					 */
					TupleTableSlot *result;

					node->mj_MatchedOuter = true;		/* do it only once */

					result = MJFillOuter(node);
					if (result)
						return result;
				}

				/*
				 * now we get the next outer tuple, if any
				 */
				outerTupleSlot = ExecProcNode(outerPlan);
				node->mj_OuterTupleSlot = outerTupleSlot;
				MJ_DEBUG_PROC_NODE(outerTupleSlot);
				node->mj_MatchedOuter = false;

				/*
				 * if the outer tuple is null then we are done with the join,
				 * unless we have inner tuples we need to null-fill.
				 */
				if (TupIsNull(outerTupleSlot))
				{
					MJ_printf("ExecMergeJoin: end of outer subplan\n");
					innerTupleSlot = node->mj_InnerTupleSlot;
					if (doFillInner && !TupIsNull(innerTupleSlot))
					{
						/*
						 * Need to emit right-join tuples for remaining inner
						 * tuples.
						 */
						node->mj_JoinState = EXEC_MJ_ENDOUTER;
						break;
					}
					/* Otherwise we're done. */
					return NULL;
				}

				/* Compute join values and check for unmatchability */
				if (MJEvalOuterValues(node))
				{
					/* Go test the new tuple against the marked tuple */
					node->mj_JoinState = EXEC_MJ_TESTOUTER;
				}
				else
				{
					/* Can't match, so fetch next outer tuple */
					node->mj_JoinState = EXEC_MJ_NEXTOUTER;
				}
				break;

				/*--------------------------------------------------------
				 * EXEC_MJ_TESTOUTER If the new outer tuple and the marked
				 * tuple satisfy the merge clause then we know we have
				 * duplicates in the outer scan so we have to restore the
				 * inner scan to the marked tuple and proceed to join the
				 * new outer tuple with the inner tuples.
				 *
				 * This is the case when
				 *						  outer inner
				 *							4	  5  - marked tuple
				 *			 outer tuple -	5	  5
				 *		 new outer tuple -	5	  5
				 *							6	  8  - inner tuple
				 *							7	 12
				 *
				 *				new outer tuple == marked tuple
				 *
				 * If the outer tuple fails the test, then we are done
				 * with the marked tuples, and we have to look for a
				 * match to the current inner tuple.  So we will
				 * proceed to skip outer tuples until outer >= inner
				 * (EXEC_MJ_SKIP_TEST).
				 *
				 *		This is the case when
				 *
				 *						  outer inner
				 *							5	  5  - marked tuple
				 *			 outer tuple -	5	  5
				 *		 new outer tuple -	6	  8  - inner tuple
				 *							7	 12
				 *
				 *				new outer tuple > marked tuple
				 *
				 *---------------------------------------------------------
				 */
			case EXEC_MJ_TESTOUTER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_TESTOUTER\n");

				/*
				 * Here we must compare the outer tuple with the marked inner
				 * tuple.  (We can ignore the result of MJEvalInnerValues,
				 * since the marked inner tuple is certainly matchable.)
				 */
				innerTupleSlot = node->mj_MarkedTupleSlot;
				(void) MJEvalInnerValues(node, innerTupleSlot);

				compareResult = MJCompare(node);
				MJ_DEBUG_COMPARE(compareResult);

				if (compareResult == 0)
				{
					/*
					 * the merge clause matched so now we restore the inner
					 * scan position to the first mark, and go join that tuple
					 * (and any following ones) to the new outer.
					 *
					 * NOTE: we do not need to worry about the MatchedInner
					 * state for the rescanned inner tuples.  We know all of
					 * them will match this new outer tuple and therefore
					 * won't be emitted as fill tuples.  This works *only*
					 * because we require the extra joinquals to be nil when
					 * doing a right or full join --- otherwise some of the
					 * rescanned tuples might fail the extra joinquals.
					 */
					ExecRestrPos(innerPlan);

					/*
					 * ExecRestrPos probably should give us back a new Slot,
					 * but since it doesn't, use the marked slot.  (The
					 * previously returned mj_InnerTupleSlot cannot be assumed
					 * to hold the required tuple.)
					 */
					node->mj_InnerTupleSlot = innerTupleSlot;
					/* we need not do MJEvalInnerValues again */

					node->mj_JoinState = EXEC_MJ_JOINTUPLES;
				}
				else
				{
					/* ----------------
					 *	if the new outer tuple didn't match the marked inner
					 *	tuple then we have a case like:
					 *
					 *			 outer inner
					 *			   4	 4	- marked tuple
					 * new outer - 5	 4
					 *			   6	 5	- inner tuple
					 *			   7
					 *
					 *	which means that all subsequent outer tuples will be
					 *	larger than our marked inner tuples.  So we need not
					 *	revisit any of the marked tuples but can proceed to
					 *	look for a match to the current inner.	If there's
					 *	no more inners, we are done.
					 * ----------------
					 */
					Assert(compareResult > 0);
					innerTupleSlot = node->mj_InnerTupleSlot;
					if (TupIsNull(innerTupleSlot))
					{
						if (doFillOuter)
						{
							/*
							 * Need to emit left-join tuples for remaining
							 * outer tuples.
							 */
							node->mj_JoinState = EXEC_MJ_ENDINNER;
							break;
						}
						/* Otherwise we're done. */
						return NULL;
					}

					/* reload comparison data for current inner */
					if (MJEvalInnerValues(node, innerTupleSlot))
					{
						/* proceed to compare it to the current outer */
						node->mj_JoinState = EXEC_MJ_SKIP_TEST;
					}
					else
					{
						/*
						 * current inner can't possibly match any outer;
						 * better to advance the inner scan than the outer.
						 */
						node->mj_JoinState = EXEC_MJ_SKIPINNER_ADVANCE;
					}
				}
				break;

				/*----------------------------------------------------------
				 * EXEC_MJ_SKIP means compare tuples and if they do not
				 * match, skip whichever is lesser.
				 *
				 * For example:
				 *
				 *				outer inner
				 *				  5		5
				 *				  5		5
				 * outer tuple -  6		8  - inner tuple
				 *				  7    12
				 *				  8    14
				 *
				 * we have to advance the outer scan
				 * until we find the outer 8.
				 *
				 * On the other hand:
				 *
				 *				outer inner
				 *				  5		5
				 *				  5		5
				 * outer tuple - 12		8  - inner tuple
				 *				 14    10
				 *				 17    12
				 *
				 * we have to advance the inner scan
				 * until we find the inner 12.
				 *----------------------------------------------------------
				 */
			case EXEC_MJ_SKIP_TEST:
				MJ_printf("ExecMergeJoin: EXEC_MJ_SKIP_TEST\n");

				/*
				 * before we advance, make sure the current tuples do not
				 * satisfy the mergeclauses.  If they do, then we update the
				 * marked tuple position and go join them.
				 */
				compareResult = MJCompare(node);
				MJ_DEBUG_COMPARE(compareResult);

				if (compareResult == 0)
				{
					ExecMarkPos(innerPlan);

					MarkInnerTuple(node->mj_InnerTupleSlot, node);

					node->mj_JoinState = EXEC_MJ_JOINTUPLES;
				}
				else if (compareResult < 0)
					node->mj_JoinState = EXEC_MJ_SKIPOUTER_ADVANCE;
				else
					/* compareResult > 0 */
					node->mj_JoinState = EXEC_MJ_SKIPINNER_ADVANCE;
				break;

				/*
				 * Before advancing, we check to see if we must emit an
				 * outer-join fill tuple for this outer tuple.
				 */
			case EXEC_MJ_SKIPOUTER_ADVANCE:
				MJ_printf("ExecMergeJoin: EXEC_MJ_SKIPOUTER_ADVANCE\n");

				if (doFillOuter && !node->mj_MatchedOuter)
				{
					/*
					 * Generate a fake join tuple with nulls for the inner
					 * tuple, and return it if it passes the non-join quals.
					 */
					TupleTableSlot *result;

					node->mj_MatchedOuter = true;		/* do it only once */

					result = MJFillOuter(node);
					if (result)
						return result;
				}

				/*
				 * now we get the next outer tuple, if any
				 */
				outerTupleSlot = ExecProcNode(outerPlan);
				node->mj_OuterTupleSlot = outerTupleSlot;
				MJ_DEBUG_PROC_NODE(outerTupleSlot);
				node->mj_MatchedOuter = false;

				/*
				 * if the outer tuple is null then we are done with the join,
				 * unless we have inner tuples we need to null-fill.
				 */
				if (TupIsNull(outerTupleSlot))
				{
					MJ_printf("ExecMergeJoin: end of outer subplan\n");
					innerTupleSlot = node->mj_InnerTupleSlot;
					if (doFillInner && !TupIsNull(innerTupleSlot))
					{
						/*
						 * Need to emit right-join tuples for remaining inner
						 * tuples.
						 */
						node->mj_JoinState = EXEC_MJ_ENDOUTER;
						break;
					}
					/* Otherwise we're done. */
					return NULL;
				}

				/* Compute join values and check for unmatchability */
				if (MJEvalOuterValues(node))
				{
					/* Go test the new tuple against the current inner */
					node->mj_JoinState = EXEC_MJ_SKIP_TEST;
				}
				else
				{
					/* Can't match, so fetch next outer tuple */
					node->mj_JoinState = EXEC_MJ_SKIPOUTER_ADVANCE;
				}
				break;

				/*
				 * Before advancing, we check to see if we must emit an
				 * outer-join fill tuple for this inner tuple.
				 */
			case EXEC_MJ_SKIPINNER_ADVANCE:
				MJ_printf("ExecMergeJoin: EXEC_MJ_SKIPINNER_ADVANCE\n");

				if (doFillInner && !node->mj_MatchedInner)
				{
					/*
					 * Generate a fake join tuple with nulls for the outer
					 * tuple, and return it if it passes the non-join quals.
					 */
					TupleTableSlot *result;

					node->mj_MatchedInner = true;		/* do it only once */

					result = MJFillInner(node);
					if (result)
						return result;
				}

				/*
				 * now we get the next inner tuple, if any
				 */
				innerTupleSlot = ExecProcNode(innerPlan);
				node->mj_InnerTupleSlot = innerTupleSlot;
				MJ_DEBUG_PROC_NODE(innerTupleSlot);
				node->mj_MatchedInner = false;

				/*
				 * if the inner tuple is null then we are done with the join,
				 * unless we have outer tuples we need to null-fill.
				 */
				if (TupIsNull(innerTupleSlot))
				{
					MJ_printf("ExecMergeJoin: end of inner subplan\n");
					outerTupleSlot = node->mj_OuterTupleSlot;
					if (doFillOuter && !TupIsNull(outerTupleSlot))
					{
						/*
						 * Need to emit left-join tuples for remaining outer
						 * tuples.
						 */
						node->mj_JoinState = EXEC_MJ_ENDINNER;
						break;
					}
					/* Otherwise we're done. */
					return NULL;
				}

				/* Compute join values and check for unmatchability */
				if (MJEvalInnerValues(node, innerTupleSlot))
				{
					/* proceed to compare it to the current outer */
					node->mj_JoinState = EXEC_MJ_SKIP_TEST;
				}
				else
				{
					/*
					 * current inner can't possibly match any outer; better to
					 * advance the inner scan than the outer.
					 */
					node->mj_JoinState = EXEC_MJ_SKIPINNER_ADVANCE;
				}
				break;

				/*
				 * EXEC_MJ_ENDOUTER means we have run out of outer tuples, but
				 * are doing a right/full join and therefore must null-fill
				 * any remaing unmatched inner tuples.
				 */
			case EXEC_MJ_ENDOUTER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_ENDOUTER\n");

				Assert(doFillInner);

				if (!node->mj_MatchedInner)
				{
					/*
					 * Generate a fake join tuple with nulls for the outer
					 * tuple, and return it if it passes the non-join quals.
					 */
					TupleTableSlot *result;

					node->mj_MatchedInner = true;		/* do it only once */

					result = MJFillInner(node);
					if (result)
						return result;
				}

				/*
				 * now we get the next inner tuple, if any
				 */
				innerTupleSlot = ExecProcNode(innerPlan);
				node->mj_InnerTupleSlot = innerTupleSlot;
				MJ_DEBUG_PROC_NODE(innerTupleSlot);
				node->mj_MatchedInner = false;

				if (TupIsNull(innerTupleSlot))
				{
					MJ_printf("ExecMergeJoin: end of inner subplan\n");
					return NULL;
				}

				/* Else remain in ENDOUTER state and process next tuple. */
				break;

				/*
				 * EXEC_MJ_ENDINNER means we have run out of inner tuples, but
				 * are doing a left/full join and therefore must null- fill
				 * any remaing unmatched outer tuples.
				 */
			case EXEC_MJ_ENDINNER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_ENDINNER\n");

				Assert(doFillOuter);

				if (!node->mj_MatchedOuter)
				{
					/*
					 * Generate a fake join tuple with nulls for the inner
					 * tuple, and return it if it passes the non-join quals.
					 */
					TupleTableSlot *result;

					node->mj_MatchedOuter = true;		/* do it only once */

					result = MJFillOuter(node);
					if (result)
						return result;
				}

				/*
				 * now we get the next outer tuple, if any
				 */
				outerTupleSlot = ExecProcNode(outerPlan);
				node->mj_OuterTupleSlot = outerTupleSlot;
				MJ_DEBUG_PROC_NODE(outerTupleSlot);
				node->mj_MatchedOuter = false;

				if (TupIsNull(outerTupleSlot))
				{
					MJ_printf("ExecMergeJoin: end of outer subplan\n");
					return NULL;
				}

				/* Else remain in ENDINNER state and process next tuple. */
				break;

				/*
				 * broken state value?
				 */
			default:
				elog(ERROR, "unrecognized mergejoin state: %d",
					 (int) node->mj_JoinState);
		}
	}
}

/* ----------------------------------------------------------------
 *		ExecInitMergeJoin
 * ----------------------------------------------------------------
 */
MergeJoinState *
ExecInitMergeJoin(MergeJoin *node, EState *estate, int eflags)
{
	MergeJoinState *mergestate;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	MJ1_printf("ExecInitMergeJoin: %s\n",
			   "initializing node");

	/*
	 * create state structure
	 */
	mergestate = makeNode(MergeJoinState);
	mergestate->js.ps.plan = (Plan *) node;
	mergestate->js.ps.state = estate;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &mergestate->js.ps);

	/*
	 * we need two additional econtexts in which we can compute the join
	 * expressions from the left and right input tuples.  The node's regular
	 * econtext won't do because it gets reset too often.
	 */
	mergestate->mj_OuterEContext = CreateExprContext(estate);
	mergestate->mj_InnerEContext = CreateExprContext(estate);

	/*
	 * initialize child expressions
	 */
	mergestate->js.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->join.plan.targetlist,
					 (PlanState *) mergestate);
	mergestate->js.ps.qual = (List *)
		ExecInitExpr((Expr *) node->join.plan.qual,
					 (PlanState *) mergestate);
	mergestate->js.jointype = node->join.jointype;
	mergestate->js.joinqual = (List *)
		ExecInitExpr((Expr *) node->join.joinqual,
					 (PlanState *) mergestate);
	/* mergeclauses are handled below */

	/*
	 * initialize child nodes
	 *
	 * inner child must support MARK/RESTORE.
	 */
	outerPlanState(mergestate) = ExecInitNode(outerPlan(node), estate, eflags);
	innerPlanState(mergestate) = ExecInitNode(innerPlan(node), estate,
											  eflags | EXEC_FLAG_MARK);

#define MERGEJOIN_NSLOTS 4

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &mergestate->js.ps);

	mergestate->mj_MarkedTupleSlot = ExecInitExtraTupleSlot(estate);
	ExecSetSlotDescriptor(mergestate->mj_MarkedTupleSlot,
						  ExecGetResultType(innerPlanState(mergestate)));

	switch (node->join.jointype)
	{
		case JOIN_INNER:
		case JOIN_IN:
			mergestate->mj_FillOuter = false;
			mergestate->mj_FillInner = false;
			break;
		case JOIN_LEFT:
			mergestate->mj_FillOuter = true;
			mergestate->mj_FillInner = false;
			mergestate->mj_NullInnerTupleSlot =
				ExecInitNullTupleSlot(estate,
							  ExecGetResultType(innerPlanState(mergestate)));
			break;
		case JOIN_RIGHT:
			mergestate->mj_FillOuter = false;
			mergestate->mj_FillInner = true;
			mergestate->mj_NullOuterTupleSlot =
				ExecInitNullTupleSlot(estate,
							  ExecGetResultType(outerPlanState(mergestate)));

			/*
			 * Can't handle right or full join with non-nil extra joinclauses.
			 * This should have been caught by planner.
			 */
			if (node->join.joinqual != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("RIGHT JOIN is only supported with merge-joinable join conditions")));
			break;
		case JOIN_FULL:
			mergestate->mj_FillOuter = true;
			mergestate->mj_FillInner = true;
			mergestate->mj_NullOuterTupleSlot =
				ExecInitNullTupleSlot(estate,
							  ExecGetResultType(outerPlanState(mergestate)));
			mergestate->mj_NullInnerTupleSlot =
				ExecInitNullTupleSlot(estate,
							  ExecGetResultType(innerPlanState(mergestate)));

			/*
			 * Can't handle right or full join with non-nil extra joinclauses.
			 */
			if (node->join.joinqual != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("FULL JOIN is only supported with merge-joinable join conditions")));
			break;
		default:
			elog(ERROR, "unrecognized join type: %d",
				 (int) node->join.jointype);
	}

	/*
	 * initialize tuple type and projection info
	 */
	ExecAssignResultTypeFromTL(&mergestate->js.ps);
	ExecAssignProjectionInfo(&mergestate->js.ps, NULL);

	/*
	 * preprocess the merge clauses
	 */
	mergestate->mj_NumClauses = list_length(node->mergeclauses);
	mergestate->mj_Clauses = MJExamineQuals(node->mergeclauses,
											(PlanState *) mergestate);

	/*
	 * initialize join state
	 */
	mergestate->mj_JoinState = EXEC_MJ_INITIALIZE_OUTER;
	mergestate->js.ps.ps_TupFromTlist = false;
	mergestate->mj_MatchedOuter = false;
	mergestate->mj_MatchedInner = false;
	mergestate->mj_OuterTupleSlot = NULL;
	mergestate->mj_InnerTupleSlot = NULL;

	/*
	 * initialization successful
	 */
	MJ1_printf("ExecInitMergeJoin: %s\n",
			   "node initialized");

	return mergestate;
}

int
ExecCountSlotsMergeJoin(MergeJoin *node)
{
	return ExecCountSlotsNode(outerPlan((Plan *) node)) +
		ExecCountSlotsNode(innerPlan((Plan *) node)) +
		MERGEJOIN_NSLOTS;
}

/* ----------------------------------------------------------------
 *		ExecEndMergeJoin
 *
 * old comments
 *		frees storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndMergeJoin(MergeJoinState *node)
{
	MJ1_printf("ExecEndMergeJoin: %s\n",
			   "ending node processing");

	/*
	 * Free the exprcontext
	 */
	ExecFreeExprContext(&node->js.ps);

	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(node->js.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->mj_MarkedTupleSlot);

	/*
	 * shut down the subplans
	 */
	ExecEndNode(innerPlanState(node));
	ExecEndNode(outerPlanState(node));

	MJ1_printf("ExecEndMergeJoin: %s\n",
			   "node processing ended");
}

void
ExecReScanMergeJoin(MergeJoinState *node, ExprContext *exprCtxt)
{
	ExecClearTuple(node->mj_MarkedTupleSlot);

	node->mj_JoinState = EXEC_MJ_INITIALIZE_OUTER;
	node->js.ps.ps_TupFromTlist = false;
	node->mj_MatchedOuter = false;
	node->mj_MatchedInner = false;
	node->mj_OuterTupleSlot = NULL;
	node->mj_InnerTupleSlot = NULL;

	/*
	 * if chgParam of subnodes is not null then plans will be re-scanned by
	 * first ExecProcNode.
	 */
	if (((PlanState *) node)->lefttree->chgParam == NULL)
		ExecReScan(((PlanState *) node)->lefttree, exprCtxt);
	if (((PlanState *) node)->righttree->chgParam == NULL)
		ExecReScan(((PlanState *) node)->righttree, exprCtxt);

}
