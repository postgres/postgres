/*-------------------------------------------------------------------------
 *
 * nodeMergejoin.c
 *	  routines supporting merge joins
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeMergejoin.c,v 1.25 1999/02/28 00:36:05 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecMergeJoin			mergejoin outer and inner relations.
 *		ExecInitMergeJoin		creates and initializes run time states
 *		ExecEndMergeJoin		cleand up the node.
 *
 * NOTES
 *		Essential operation of the merge join algorithm is as follows:
 *		(** indicates the tuples satisfy the merge clause).
 *
 *		Join {												   -
 *			get initial outer and inner tuples				INITIALIZE
 *			Skip Inner										SKIPINNER
 *			mark inner position								JOINMARK
 *			do forever {									   -
 *				while (outer == inner) {					JOINTEST
 *					join tuples								JOINTUPLES
 *					advance inner position					NEXTINNER
 *				}											   -
 *				advance outer position						NEXTOUTER
 *				if (outer == mark) {						TESTOUTER
 *					restore inner position to mark			TESTOUTER
 *					continue								   -
 *				} else {									   -
 *					Skip Outer								SKIPOUTER
 *					mark inner position						JOINMARK
 *				}											   -
 *			}												   -
 *		}													   -
 *
 *		Skip Outer {										SKIPOUTER
 *			if (inner == outer) Join Tuples					JOINTUPLES
 *			while (outer < inner)							SKIPOUTER
 *				advance outer								SKIPOUTER
 *			if (outer > inner)								SKIPOUTER
 *				Skip Inner									SKIPINNER
 *		}													   -
 *
 *		Skip Inner {										SKIPINNER
 *			if (inner == outer) Join Tuples					JOINTUPLES
 *			while (outer > inner)							SKIPINNER
 *				advance inner								SKIPINNER
 *			if (outer < inner)								SKIPINNER
 *				Skip Outer									SKIPOUTER
 *		}													   -
 *
 *		The merge join operation is coded in the fashion
 *		of a state machine.  At each state, we do something and then
 *		proceed to another state.  This state is stored in the node's
 *		execution state information and is preserved across calls to
 *		ExecMergeJoin. -cim 10/31/89
 *
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/pg_operator.h"
#include "executor/executor.h"
#include "executor/execdefs.h"
#include "executor/nodeMergejoin.h"
#include "executor/execdebug.h"
#include "utils/lsyscache.h"
#include "utils/psort.h"


static bool MergeCompare(List *eqQual, List *compareQual, ExprContext *econtext);

#define MarkInnerTuple(innerTupleSlot, mergestate) \
( \
	ExecStoreTuple(heap_copytuple((innerTupleSlot)->val), \
				   (mergestate)->mj_MarkedTupleSlot, \
				   InvalidBuffer, \
				   true) \
)


/* ----------------------------------------------------------------
 *		MJFormSkipQual
 *
 *		This takes the mergeclause which is a qualification of the
 *		form ((= expr expr) (= expr expr) ...) and forms a new
 *		qualification like ((> expr expr) (> expr expr) ...) which
 *		is used by ExecMergeJoin() in order to determine if we should
 *		skip tuples.  The replacement operators are named either ">"
 *		or "<" according to the replaceopname parameter, and have the
 *		same operand data types as the "=" operators they replace.
 *		(We expect there to be such operators because the "=" operators
 *		were marked mergejoinable; however, there might be a different
 *		one needed in each qual clause.)
 * ----------------------------------------------------------------
 */
static List *
MJFormSkipQual(List *qualList, char * replaceopname)
{
	List	   *qualCopy;
	List	   *qualcdr;
	Expr	   *qual;
	Oper	   *op;
	HeapTuple	optup;
	Form_pg_operator opform;
	Oid			oprleft,
				oprright;

	/* ----------------
	 *	qualList is a list: ((op .. ..) ...)
	 *	first we make a copy of it.  copyObject() makes a deep copy
	 *	so let's use it instead of the old fashoned lispCopy()...
	 * ----------------
	 */
	qualCopy = (List *) copyObject((Node *) qualList);

	foreach(qualcdr, qualCopy)
	{
		/* ----------------
		 *	 first get the current (op .. ..) list
		 * ----------------
		 */
		qual = lfirst(qualcdr);

		/* ----------------
		 *	 now get at the op
		 * ----------------
		 */
		op = (Oper *) qual->oper;
		if (!IsA(op, Oper))
			elog(ERROR, "MJFormSkipQual: op not an Oper!");

		/* ----------------
		 *	 Get the declared left and right operand types of the operator.
		 *	 Note we do *not* use the actual operand types, since those might
		 *	 be different in scenarios with binary-compatible data types.
		 *	 There should be "<" and ">" operators matching a mergejoinable
		 *	 "=" operator's declared operand types, but we might not find them
		 *	 if we search with the actual operand types.
		 * ----------------
		 */
		optup = get_operator_tuple(op->opno);
		if (!HeapTupleIsValid(optup))		/* shouldn't happen */
			elog(ERROR, "MJFormSkipQual: operator %d not found", op->opno);
		opform = (Form_pg_operator) GETSTRUCT(optup);
		oprleft = opform->oprleft;
		oprright = opform->oprright;

		/* ----------------
		 *	 Now look up the matching "<" or ">" operator.  If there isn't one,
		 *	 whoever marked the "=" operator mergejoinable was a loser.
		 * ----------------
		 */
		optup = SearchSysCacheTuple(OPRNAME,
									PointerGetDatum(replaceopname),
									ObjectIdGetDatum(oprleft),
									ObjectIdGetDatum(oprright),
									CharGetDatum('b'));
		if (!HeapTupleIsValid(optup))
			elog(ERROR,
				 "MJFormSkipQual: mergejoin operator %d has no matching %s op",
				 op->opno, replaceopname);
		opform = (Form_pg_operator) GETSTRUCT(optup);

		/* ----------------
		 *	 And replace the data in the copied operator node.
		 * ----------------
		 */
		op->opno = optup->t_data->t_oid;
		op->opid = opform->oprcode;
		op->op_fcache = NULL;
	}

	return qualCopy;
}

/* ----------------------------------------------------------------
 *		MergeCompare
 *
 *		Compare the keys according to 'compareQual' which is of the
 *		form: {(key1a > key2a)(key1b > key2b) ...}.
 *
 *		(actually, it could also be the form (key1a < key2a)..)
 *
 *		This is different from calling ExecQual because ExecQual returns
 *		true only if ALL the comparisions clauses are satisfied.
 *		However, there is an order of significance among the keys with
 *		the first keys being most significant. Therefore, the clauses
 *		are evaluated in order and the 'compareQual' is satisfied
 *		if (key1i > key2i) is true and (key1j = key2j) for 0 < j < i.
 *		We use the original mergeclause items to detect equality.
 * ----------------------------------------------------------------
 */
static bool
MergeCompare(List *eqQual, List *compareQual, ExprContext *econtext)
{
	List	   *clause;
	List	   *eqclause;
	Datum		const_value;
	bool		isNull;
	bool		isDone;

	/* ----------------
	 *	if we have no compare qualification, return nil
	 * ----------------
	 */
	if (compareQual == NIL)
		return false;

	/* ----------------
	 *	for each pair of clauses, test them until
	 *	our compare conditions are satisified
	 * ----------------
	 */
	eqclause = eqQual;
	foreach(clause, compareQual)
	{
		/* ----------------
		 *	 first test if our compare clause is satisified.
		 *	 if so then return true. ignore isDone, don't iterate in
		 *	 quals.
		 * ----------------
		 */
		const_value = (Datum)
			ExecEvalExpr((Node *) lfirst(clause), econtext, &isNull, &isDone);

		if (DatumGetInt32(const_value) != 0)
			return true;

		/* ----------------
		 *	 ok, the compare clause failed so we test if the keys
		 *	 are equal... if key1 != key2, we return false.
		 *	 otherwise key1 = key2 so we move on to the next pair of keys.
		 *
		 *	 ignore isDone, don't iterate in quals.
		 * ----------------
		 */
		const_value = ExecEvalExpr((Node *) lfirst(eqclause),
								   econtext,
								   &isNull,
								   &isDone);

		if (DatumGetInt32(const_value) == 0)
			return false;
		eqclause = lnext(eqclause);
	}

	/* ----------------
	 *	if we get here then it means none of our key greater-than
	 *	conditions were satisified so we return false.
	 * ----------------
	 */
	return false;
}

/* ----------------------------------------------------------------
 *		ExecMergeTupleDump
 *
 *		This function is called through the MJ_dump() macro
 *		when EXEC_MERGEJOINDEBUG is defined
 * ----------------------------------------------------------------
 */
#ifdef EXEC_MERGEJOINDEBUG
void
			ExecMergeTupleDumpInner(ExprContext *econtext);

void
ExecMergeTupleDumpInner(ExprContext *econtext)
{
	TupleTableSlot *innerSlot;

	printf("==== inner tuple ====\n");
	innerSlot = econtext->ecxt_innertuple;
	if (TupIsNull(innerSlot))
		printf("(nil)\n");
	else
		MJ_debugtup(innerSlot->val,
					innerSlot->ttc_tupleDescriptor);
}

void
			ExecMergeTupleDumpOuter(ExprContext *econtext);

void
ExecMergeTupleDumpOuter(ExprContext *econtext)
{
	TupleTableSlot *outerSlot;

	printf("==== outer tuple ====\n");
	outerSlot = econtext->ecxt_outertuple;
	if (TupIsNull(outerSlot))
		printf("(nil)\n");
	else
		MJ_debugtup(outerSlot->val,
					outerSlot->ttc_tupleDescriptor);
}

void ExecMergeTupleDumpMarked(ExprContext *econtext,
						 MergeJoinState *mergestate);

void
ExecMergeTupleDumpMarked(ExprContext *econtext,
						 MergeJoinState *mergestate)
{
	TupleTableSlot *markedSlot;

	printf("==== marked tuple ====\n");
	markedSlot = mergestate->mj_MarkedTupleSlot;

	if (TupIsNull(markedSlot))
		printf("(nil)\n");
	else
		MJ_debugtup(markedSlot->val,
					markedSlot->ttc_tupleDescriptor);
}

void
			ExecMergeTupleDump(ExprContext *econtext, MergeJoinState *mergestate);

void
ExecMergeTupleDump(ExprContext *econtext, MergeJoinState *mergestate)
{
	printf("******** ExecMergeTupleDump ********\n");

	ExecMergeTupleDumpInner(econtext);
	ExecMergeTupleDumpOuter(econtext);
	ExecMergeTupleDumpMarked(econtext, mergestate);

	printf("******** \n");
}

#endif

/* ----------------------------------------------------------------
 *		ExecMergeJoin
 *
 * old comments
 *		Details of the merge-join routines:
 *
 *		(1) ">" and "<" operators
 *
 *		Merge-join is done by joining the inner and outer tuples satisfying
 *		the join clauses of the form ((= outerKey innerKey) ...).
 *		The join clauses is provided by the query planner and may contain
 *		more than one (= outerKey innerKey) clauses (for composite key).
 *
 *		However, the query executor needs to know whether an outer
 *		tuple is "greater/smaller" than an inner tuple so that it can
 *		"synchronize" the two relations. For e.g., consider the following
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
 *		Therefore, when initializing the merge-join node, the executor
 *		creates the "greater/smaller" clause by substituting the "="
 *		operator in the join clauses with the corresponding ">" operator.
 *		The opposite "smaller/greater" clause is formed by substituting "<".
 *
 *		Note: prior to v6.5, the relational clauses were formed using the
 *		sort op used to sort the inner relation, which of course would fail
 *		if the outer and inner keys were of different data types.
 *		In the current code, we instead assume that operators named "<" and ">"
 *		will do the right thing.  This should be true since the mergejoin "="
 *		operator's pg_operator entry will have told the planner to sort by
 *		"<" for each of the left and right sides.
 *
 *		(2) repositioning inner "cursor"
 *
 *		Consider the above relations and suppose that the executor has
 *		just joined the first outer "5" with the last inner "5". The
 *		next step is of course to join the second outer "5" with all
 *		the inner "5's". This requires repositioning the inner "cursor"
 *		to point at the first inner "5". This is done by "marking" the
 *		first inner 5 and restore the "cursor" to it before joining
 *		with the second outer 5. The access method interface provides
 *		routines to mark and restore to a tuple.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecMergeJoin(MergeJoin *node)
{
	EState	   *estate;
	MergeJoinState *mergestate;
	ScanDirection direction;
	List	   *innerSkipQual;
	List	   *outerSkipQual;
	List	   *mergeclauses;
	List	   *qual;
	bool		qualResult;
	bool		compareResult;

	Plan	   *innerPlan;
	TupleTableSlot *innerTupleSlot;

	Plan	   *outerPlan;
	TupleTableSlot *outerTupleSlot;

	ExprContext *econtext;

#ifdef ENABLE_OUTER_JOINS

	/*
	 * These should be set from the expression context! - thomas
	 * 1999-02-20
	 */
	static bool isLeftJoin = true;
	static bool isRightJoin = false;

#endif

	/* ----------------
	 *	get information from node
	 * ----------------
	 */
	mergestate = node->mergestate;
	estate = node->join.state;
	direction = estate->es_direction;
	innerPlan = innerPlan((Plan *) node);
	outerPlan = outerPlan((Plan *) node);
	econtext = mergestate->jstate.cs_ExprContext;
	mergeclauses = node->mergeclauses;
	qual = node->join.qual;

	if (ScanDirectionIsForward(direction))
	{
		outerSkipQual = mergestate->mj_OuterSkipQual;
		innerSkipQual = mergestate->mj_InnerSkipQual;
	}
	else
	{
		outerSkipQual = mergestate->mj_InnerSkipQual;
		innerSkipQual = mergestate->mj_OuterSkipQual;
	}

	/* ----------------
	 *	ok, everything is setup.. let's go to work
	 * ----------------
	 */
	if (mergestate->jstate.cs_TupFromTlist)
	{
		TupleTableSlot *result;
		ProjectionInfo *projInfo;
		bool		isDone;

		projInfo = mergestate->jstate.cs_ProjInfo;
		result = ExecProject(projInfo, &isDone);
		if (!isDone)
			return result;
	}
	for (;;)
	{
		/* ----------------
		 *	get the current state of the join and do things accordingly.
		 *	Note: The join states are highlighted with 32-* comments for
		 *		  improved readability.
		 * ----------------
		 */
		MJ_dump(econtext, mergestate);

		switch (mergestate->mj_JoinState)
		{

				/*
				 * EXEC_MJ_INITIALIZE means that this is the first time
				 * ExecMergeJoin() has been called and so we have to
				 * initialize the inner, outer and marked tuples as well
				 * as various stuff in the expression context.
				 */
			case EXEC_MJ_INITIALIZE:
				MJ_printf("ExecMergeJoin: EXEC_MJ_INITIALIZE\n");

				/*
				 * Note: at this point, if either of our inner or outer
				 * tuples are nil, then the join ends immediately because
				 * we know one of the subplans is empty.
				 */
				innerTupleSlot = ExecProcNode(innerPlan, (Plan *) node);
				if (TupIsNull(innerTupleSlot))
				{
					MJ_printf("ExecMergeJoin: **** inner tuple is nil ****\n");
					return NULL;
				}

				outerTupleSlot = ExecProcNode(outerPlan, (Plan *) node);
				if (TupIsNull(outerTupleSlot))
				{
					MJ_printf("ExecMergeJoin: **** outer tuple is nil ****\n");
					return NULL;
				}

				/* ----------------
				 *	 store the inner and outer tuple in the merge state
				 * ----------------
				 */
				econtext->ecxt_innertuple = innerTupleSlot;
				econtext->ecxt_outertuple = outerTupleSlot;

				mergestate->mj_MarkedTupleSlot->ttc_tupleDescriptor =
					innerTupleSlot->ttc_tupleDescriptor;

				/* ----------------
				 *	initialize merge join state to skip inner tuples.
				 * ----------------
				 */
				mergestate->mj_JoinState = EXEC_MJ_SKIPINNER;
				break;

				/*
				 * EXEC_MJ_JOINMARK means we have just found a new outer
				 * tuple and a possible matching inner tuple. This is the
				 * case after the INITIALIZE, SKIPOUTER or SKIPINNER
				 * states.
				 */
			case EXEC_MJ_JOINMARK:
				MJ_printf("ExecMergeJoin: EXEC_MJ_JOINMARK\n");
				ExecMarkPos(innerPlan);

				MarkInnerTuple(econtext->ecxt_innertuple, mergestate);

				mergestate->mj_JoinState = EXEC_MJ_JOINTEST;
				break;

				/*
				 * EXEC_MJ_JOINTEST means we have two tuples which might
				 * satisfy the merge clause, so we test them.
				 *
				 * If they do satisfy, then we join them and move on to the
				 * next inner tuple (EXEC_MJ_JOINTUPLES).
				 *
				 * If they do not satisfy then advance to next outer tuple.
				 */
			case EXEC_MJ_JOINTEST:
				MJ_printf("ExecMergeJoin: EXEC_MJ_JOINTEST\n");

				qualResult = ExecQual((List *) mergeclauses, econtext);
				MJ_DEBUG_QUAL(mergeclauses, qualResult);

				if (qualResult)
					mergestate->mj_JoinState = EXEC_MJ_JOINTUPLES;
				else
					mergestate->mj_JoinState = EXEC_MJ_NEXTOUTER;
				break;

				/*
				 * EXEC_MJ_JOINTUPLES means we have two tuples which
				 * satisified the merge clause so we join them and then
				 * proceed to get the next inner tuple (EXEC_NEXT_INNER).
				 */
			case EXEC_MJ_JOINTUPLES:
				MJ_printf("ExecMergeJoin: EXEC_MJ_JOINTUPLES\n");
				mergestate->mj_JoinState = EXEC_MJ_NEXTINNER;

				qualResult = ExecQual((List *) qual, econtext);
				MJ_DEBUG_QUAL(qual, qualResult);

				if (qualResult)
				{
					/* ----------------
					 *	qualification succeeded.  now form the desired
					 *	projection tuple and return the slot containing it.
					 * ----------------
					 */
					ProjectionInfo *projInfo;
					TupleTableSlot *result;
					bool		isDone;

					MJ_printf("ExecMergeJoin: **** returning tuple ****\n");

					projInfo = mergestate->jstate.cs_ProjInfo;

					result = ExecProject(projInfo, &isDone);
					mergestate->jstate.cs_TupFromTlist = !isDone;
					return result;
				}
				break;

				/*
				 * EXEC_MJ_NEXTINNER means advance the inner scan to the
				 * next tuple. If the tuple is not nil, we then proceed to
				 * test it against the join qualification.
				 */
			case EXEC_MJ_NEXTINNER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_NEXTINNER\n");

				/* ----------------
				 *	now we get the next inner tuple, if any
				 * ----------------
				 */
				innerTupleSlot = ExecProcNode(innerPlan, (Plan *) node);
				MJ_DEBUG_PROC_NODE(innerTupleSlot);
				econtext->ecxt_innertuple = innerTupleSlot;

				if (TupIsNull(innerTupleSlot))
					mergestate->mj_JoinState = EXEC_MJ_NEXTOUTER;
				else
					mergestate->mj_JoinState = EXEC_MJ_JOINTEST;
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
				 * first inner tuple > current outer tuple
				 * so get a new outer tuple and then
				 * proceed to test it against the marked tuple
				 * (EXEC_MJ_TESTOUTER)
				 *------------------------------------------------
				 */
			case EXEC_MJ_NEXTOUTER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_NEXTOUTER\n");

				outerTupleSlot = ExecProcNode(outerPlan, (Plan *) node);
				MJ_DEBUG_PROC_NODE(outerTupleSlot);
				econtext->ecxt_outertuple = outerTupleSlot;

				/* ----------------
				 *	if the outer tuple is null then we know
				 *	we are done with the join
				 * ----------------
				 */
				if (TupIsNull(outerTupleSlot))
				{
					MJ_printf("ExecMergeJoin: **** outer tuple is nil ****\n");
					return NULL;
				}

				mergestate->mj_JoinState = EXEC_MJ_TESTOUTER;
				break;

				/*--------------------------------------------------------
				 * EXEC_MJ_TESTOUTER If the new outer tuple and the marked
				 * tuple satisfy the merge clause then we know we have
				 * duplicates in the outer scan so we have to restore the
				 * inner scan to the marked tuple and proceed to join the
				 * new outer tuples with the inner tuples (EXEC_MJ_JOINTEST)
				 *
				 * This is the case when
				 *						  outer inner
				 *							4	  5  - marked tuple
				 *			 outer tuple -	5	  5
				 *		 new outer tuple -	5	  5
				 *							6	  8  - inner tuple
				 *							7	 12
				 *
				 *				new outer tuple = marked tuple
				 *
				 *		If the outer tuple fails the test, then we know we have
				 *		to proceed to skip outer tuples until outer >= inner
				 *		(EXEC_MJ_SKIPOUTER).
				 *
				 *		This is the case when
				 *
				 *						  outer inner
				 *							5	  5  - marked tuple
				 *			 outer tuple -	5	  5
				 *		 new outer tuple -	6	  8  - inner tuple
				 *							7	 12
				 *
				 *
				 *		 new outer tuple > marked tuple
				 *
				 *---------------------------------------------------------
				 */
			case EXEC_MJ_TESTOUTER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_TESTOUTER\n");

				/* ----------------
				 *	here we compare the outer tuple with the marked inner tuple
				 *	by using the marked tuple in place of the inner tuple.
				 * ----------------
				 */
				innerTupleSlot = econtext->ecxt_innertuple;
				econtext->ecxt_innertuple = mergestate->mj_MarkedTupleSlot;

				qualResult = ExecQual((List *) mergeclauses, econtext);
				MJ_DEBUG_QUAL(mergeclauses, qualResult);

				if (qualResult)
				{

					/*
					 * the merge clause matched so now we juggle the slots
					 * back the way they were and proceed to JOINTEST.
					 *
					 * I can't understand why we have to go to JOINTEST and
					 * compare outer tuple with the same inner one again
					 * -> go to JOINTUPLES...	 - vadim 02/27/98
					 */

					ExecRestrPos(innerPlan);
#if 0
					mergestate->mj_JoinState = EXEC_MJ_JOINTEST;
#endif
					mergestate->mj_JoinState = EXEC_MJ_JOINTUPLES;

				}
				else
				{
					econtext->ecxt_innertuple = innerTupleSlot;
					/* ----------------
					 *	if the inner tuple was nil and the new outer
					 *	tuple didn't match the marked outer tuple then
					 *	we may have the case:
					 *
					 *			 outer inner
					 *			   4	 4	- marked tuple
					 * new outer - 5	 4
					 *			   6	nil - inner tuple
					 *			   7
					 *
					 *	which means that all subsequent outer tuples will be
					 *	larger than our inner tuples.
					 * ----------------
					 */
					if (TupIsNull(innerTupleSlot))
					{
#ifdef ENABLE_OUTER_JOINS
						if (isLeftJoin)
						{
							/* continue on to null fill outer tuples */
							mergestate->mj_JoinState = EXEC_MJ_FILLOUTER;
							break;
						}
#endif
						MJ_printf("ExecMergeJoin: **** weird case 1 ****\n");
						return NULL;
					}

					/* continue on to skip outer tuples */
					mergestate->mj_JoinState = EXEC_MJ_SKIPOUTER;
				}
				break;

				/*----------------------------------------------------------
				 * EXEC_MJ_SKIPOUTER means skip over tuples in the outer plan
				 * until we find an outer tuple > current inner tuple.
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
				 *----------------------------------------------------------
				 */
			case EXEC_MJ_SKIPOUTER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_SKIPOUTER\n");
				/* ----------------
				 *	before we advance, make sure the current tuples
				 *	do not satisfy the mergeclauses.  If they do, then
				 *	we update the marked tuple and go join them.
				 * ----------------
				 */
				qualResult = ExecQual((List *) mergeclauses, econtext);
				MJ_DEBUG_QUAL(mergeclauses, qualResult);

				if (qualResult)
				{
					ExecMarkPos(innerPlan);

					MarkInnerTuple(econtext->ecxt_innertuple, mergestate);

					mergestate->mj_JoinState = EXEC_MJ_JOINTUPLES;
					break;
				}

				/* ----------------
				 *	ok, now test the skip qualification
				 * ----------------
				 */
				compareResult = MergeCompare(mergeclauses,
											 outerSkipQual,
											 econtext);

				MJ_DEBUG_MERGE_COMPARE(outerSkipQual, compareResult);

				/* ----------------
				 *	compareResult is true as long as we should
				 *	continue skipping tuples.
				 * ----------------
				 */
				if (compareResult)
				{
#ifdef ENABLE_OUTER_JOINS
					/* ----------------
					 *	if this is a left or full outer join, then fill
					 * ----------------
					 */
					if (isLeftJoin)
					{
						mergestate->mj_JoinState = EXEC_MJ_FILLOUTER;
						break;
					}
#endif

					outerTupleSlot = ExecProcNode(outerPlan, (Plan *) node);
					MJ_DEBUG_PROC_NODE(outerTupleSlot);
					econtext->ecxt_outertuple = outerTupleSlot;

					/* ----------------
					 *	if the outer tuple is null then we know
					 *	we are done with the join
					 * ----------------
					 */
					if (TupIsNull(outerTupleSlot))
					{
						MJ_printf("ExecMergeJoin: **** outerTuple is nil ****\n");
						return NULL;
					}
					/* ----------------
					 *	otherwise test the new tuple against the skip qual.
					 *	(we remain in the EXEC_MJ_SKIPOUTER state)
					 * ----------------
					 */
					break;
				}

				/* ----------------
				 *	now check the inner skip qual to see if we
				 *	should now skip inner tuples... if we fail the
				 *	inner skip qual, then we know we have a new pair
				 *	of matching tuples.
				 * ----------------
				 */
				compareResult = MergeCompare(mergeclauses,
											 innerSkipQual,
											 econtext);

				MJ_DEBUG_MERGE_COMPARE(innerSkipQual, compareResult);

				if (compareResult)
					mergestate->mj_JoinState = EXEC_MJ_SKIPINNER;
				else
					mergestate->mj_JoinState = EXEC_MJ_JOINMARK;
				break;

				/*-----------------------------------------------------------
				 * EXEC_MJ_SKIPINNER means skip over tuples in the inner plan
				 * until we find an inner tuple > current outer tuple.
				 *
				 * For example:
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
				 *
				 *-------------------------------------------------------
				 */
			case EXEC_MJ_SKIPINNER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_SKIPINNER\n");
				/* ----------------
				 *	before we advance, make sure the current tuples
				 *	do not satisfy the mergeclauses.  If they do, then
				 *	we update the marked tuple and go join them.
				 * ----------------
				 */
				qualResult = ExecQual((List *) mergeclauses, econtext);
				MJ_DEBUG_QUAL(mergeclauses, qualResult);

				if (qualResult)
				{
					ExecMarkPos(innerPlan);

					MarkInnerTuple(econtext->ecxt_innertuple, mergestate);

					mergestate->mj_JoinState = EXEC_MJ_JOINTUPLES;
					break;
				}

				/* ----------------
				 *	ok, now test the skip qualification
				 * ----------------
				 */
				compareResult = MergeCompare(mergeclauses,
											 innerSkipQual,
											 econtext);

				MJ_DEBUG_MERGE_COMPARE(innerSkipQual, compareResult);

				/* ----------------
				 *	compareResult is true as long as we should
				 *	continue skipping tuples.
				 * ----------------
				 */
				if (compareResult)
				{
#ifdef ENABLE_OUTER_JOINS
					/* ----------------
					 *	if this is a right or full outer join, then fill
					 * ----------------
					 */
					if (isRightJoin)
					{
						mergestate->mj_JoinState = EXEC_MJ_FILLINNER;
						break;
					}
#endif

					/* ----------------
					 *	now try and get a new inner tuple
					 * ----------------
					 */
					innerTupleSlot = ExecProcNode(innerPlan, (Plan *) node);
					MJ_DEBUG_PROC_NODE(innerTupleSlot);
					econtext->ecxt_innertuple = innerTupleSlot;

					/* ----------------
					 *	if the inner tuple is null then we know
					 *	we have to restore the inner scan
					 *	and advance to the next outer tuple
					 * ----------------
					 */
					if (TupIsNull(innerTupleSlot))
					{
						/* ----------------
						 *	this is an interesting case.. all our
						 *	inner tuples are smaller then our outer
						 *	tuples so we never found an inner tuple
						 *	to mark.
						 *
						 *			  outer inner
						 *	 outer tuple -	5	  4
						 *					5	  4
						 *					6	 nil  - inner tuple
						 *					7
						 *
						 *	This means the join should end.
						 * ----------------
						 */
						MJ_printf("ExecMergeJoin: **** weird case 2 ****\n");
						return NULL;
					}

					/* ----------------
					 *	otherwise test the new tuple against the skip qual.
					 *	(we remain in the EXEC_MJ_SKIPINNER state)
					 * ----------------
					 */
					break;
				}

				/* ----------------
				 *	compare finally failed and we have stopped skipping
				 *	inner tuples so now check the outer skip qual
				 *	to see if we should now skip outer tuples...
				 * ----------------
				 */
				compareResult = MergeCompare(mergeclauses,
											 outerSkipQual,
											 econtext);

				MJ_DEBUG_MERGE_COMPARE(outerSkipQual, compareResult);

				if (compareResult)
					mergestate->mj_JoinState = EXEC_MJ_SKIPOUTER;
				else
					mergestate->mj_JoinState = EXEC_MJ_JOINMARK;

				break;

#ifdef ENABLE_OUTER_JOINS

				/*
				 * EXEC_MJ_FILLINNER means we have an unmatched inner
				 * tuple which must be null-expanded into the projection
				 * tuple. get the next inner tuple and reset markers
				 * (EXEC_MJ_JOINMARK).
				 */
			case EXEC_MJ_FILLINNER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_FILLINNER\n");
				mergestate->mj_JoinState = EXEC_MJ_JOINMARK;

				/* ----------------
				 *	project the inner tuple into the result
				 * ----------------
				 */
				MJ_printf("ExecMergeJoin: project inner tuple into the result (not yet implemented)\n");

				/* ----------------
				 *	now skip this inner tuple
				 * ----------------
				 */
				innerTupleSlot = ExecProcNode(innerPlan, (Plan *) node);
				MJ_DEBUG_PROC_NODE(innerTupleSlot);
				econtext->ecxt_innertuple = innerTupleSlot;

				/* ----------------
				 *	if the inner tuple is null then we know
				 *	we have to restore the inner scan
				 *	and advance to the next outer tuple
				 * ----------------
				 */
				if (TupIsNull(innerTupleSlot))
				{
					if (isLeftJoin && !TupIsNull(outerTupleSlot))
					{
						mergestate->mj_JoinState = EXEC_MJ_FILLOUTER;
						MJ_printf("ExecMergeJoin: try to complete outer fill\n");
						break;
					}

					MJ_printf("ExecMergeJoin: **** weird case 2 ****\n");
					return NULL;
				}

				/* ----------------
				 *	otherwise test the new tuple against the skip qual.
				 *	(we move to the EXEC_MJ_JOINMARK state)
				 * ----------------
				 */
				break;

				/*
				 * EXEC_MJ_FILLOUTER means we have an unmatched outer
				 * tuple which must be null-expanded into the projection
				 * tuple. get the next outer tuple and reset markers
				 * (EXEC_MJ_JOINMARK).
				 */
			case EXEC_MJ_FILLOUTER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_FILLOUTER\n");
				mergestate->mj_JoinState = EXEC_MJ_JOINMARK;

				/* ----------------
				 *	project the outer tuple into the result
				 * ----------------
				 */
				MJ_printf("ExecMergeJoin: project outer tuple into the result (not yet implemented)\n");

				/* ----------------
				 *	now skip this outer tuple
				 * ----------------
				 */
				outerTupleSlot = ExecProcNode(outerPlan, (Plan *) node);
				MJ_DEBUG_PROC_NODE(outerTupleSlot);
				econtext->ecxt_outertuple = outerTupleSlot;

				/* ----------------
				 *	if the outer tuple is null then we know
				 *	we are done with the left half of the join
				 * ----------------
				 */
				if (TupIsNull(outerTupleSlot))
				{
					if (isRightJoin && !TupIsNull(innerTupleSlot))
					{
						mergestate->mj_JoinState = EXEC_MJ_FILLINNER;
						MJ_printf("ExecMergeJoin: try to complete inner fill\n");
						break;
					}

					MJ_printf("ExecMergeJoin: **** outerTuple is nil ****\n");
					return NULL;
				}

				/* ----------------
				 *	otherwise test the new tuple against the skip qual.
				 *	(we move to the EXEC_MJ_JOINMARK state)
				 * ----------------
				 */
				break;
#endif

				/*
				 * if we get here it means our code is fouled up and so we
				 * just end the join prematurely.
				 */
			default:
				elog(NOTICE, "ExecMergeJoin: invalid join state. aborting");
				return NULL;
		}
	}
}

/* ----------------------------------------------------------------
 *		ExecInitMergeJoin
 *
 * old comments
 *		Creates the run-time state information for the node and
 *		sets the relation id to contain relevant decriptors.
 * ----------------------------------------------------------------
 */
bool
ExecInitMergeJoin(MergeJoin *node, EState *estate, Plan *parent)
{
	MergeJoinState *mergestate;
	List	   *joinclauses;
	TupleTableSlot *mjSlot;

	MJ1_printf("ExecInitMergeJoin: %s\n",
			   "initializing node");

	/* ----------------
	 *	assign the node's execution state and
	 *	get the range table and direction from it
	 * ----------------
	 */
	node->join.state = estate;

	/* ----------------
	 *	create new merge state for node
	 * ----------------
	 */
	mergestate = makeNode(MergeJoinState);
	mergestate->mj_OuterSkipQual = NIL;
	mergestate->mj_InnerSkipQual = NIL;
	mergestate->mj_JoinState = 0;
	mergestate->mj_MarkedTupleSlot = NULL;
	node->mergestate = mergestate;

	/* ----------------
	 *	Miscellaneous initialization
	 *
	 *		 +	assign node's base_id
	 *		 +	assign debugging hooks and
	 *		 +	create expression context for node
	 * ----------------
	 */
	ExecAssignNodeBaseInfo(estate, &mergestate->jstate, parent);
	ExecAssignExprContext(estate, &mergestate->jstate);

#define MERGEJOIN_NSLOTS 2
	/* ----------------
	 *	tuple table initialization
	 * ----------------
	 */
	ExecInitResultTupleSlot(estate, &mergestate->jstate);
	mjSlot = (TupleTableSlot *) palloc(sizeof(TupleTableSlot));
	mjSlot->val = NULL;
	mjSlot->ttc_shouldFree = true;
	mjSlot->ttc_tupleDescriptor = NULL;
	mjSlot->ttc_whichplan = -1;
	mjSlot->ttc_descIsNew = true;
	mergestate->mj_MarkedTupleSlot = mjSlot;

	/* ----------------
	 *	form merge skip qualifications
	 * ----------------
	 */
	joinclauses = node->mergeclauses;
	mergestate->mj_OuterSkipQual = MJFormSkipQual(joinclauses, "<");
	mergestate->mj_InnerSkipQual = MJFormSkipQual(joinclauses, ">");

	MJ_printf("\nExecInitMergeJoin: OuterSkipQual is ");
	MJ_nodeDisplay(mergestate->mj_OuterSkipQual);
	MJ_printf("\nExecInitMergeJoin: InnerSkipQual is ");
	MJ_nodeDisplay(mergestate->mj_InnerSkipQual);
	MJ_printf("\n");

	/* ----------------
	 *	initialize join state
	 * ----------------
	 */
	mergestate->mj_JoinState = EXEC_MJ_INITIALIZE;

	/* ----------------
	 *	initialize subplans
	 * ----------------
	 */
	ExecInitNode(outerPlan((Plan *) node), estate, (Plan *) node);
	ExecInitNode(innerPlan((Plan *) node), estate, (Plan *) node);

	/* ----------------
	 *	initialize tuple type and projection info
	 * ----------------
	 */
	ExecAssignResultTypeFromTL((Plan *) node, &mergestate->jstate);
	ExecAssignProjectionInfo((Plan *) node, &mergestate->jstate);

	mergestate->jstate.cs_TupFromTlist = false;
	/* ----------------
	 *	initialization successful
	 * ----------------
	 */
	MJ1_printf("ExecInitMergeJoin: %s\n",
			   "node initialized");

	return TRUE;
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
ExecEndMergeJoin(MergeJoin *node)
{
	MergeJoinState *mergestate;

	MJ1_printf("ExecEndMergeJoin: %s\n",
			   "ending node processing");

	/* ----------------
	 *	get state information from the node
	 * ----------------
	 */
	mergestate = node->mergestate;

	/* ----------------
	 *	Free the projection info and the scan attribute info
	 *
	 *	Note: we don't ExecFreeResultType(mergestate)
	 *		  because the rule manager depends on the tupType
	 *		  returned by ExecMain().  So for now, this
	 *		  is freed at end-transaction time.  -cim 6/2/91
	 * ----------------
	 */
	ExecFreeProjectionInfo(&mergestate->jstate);

	/* ----------------
	 *	shut down the subplans
	 * ----------------
	 */
	ExecEndNode((Plan *) innerPlan((Plan *) node), (Plan *) node);
	ExecEndNode((Plan *) outerPlan((Plan *) node), (Plan *) node);

	/* ----------------
	 *	clean out the tuple table so that we don't try and
	 *	pfree the marked tuples..  see HACK ALERT at the top of
	 *	this file.
	 * ----------------
	 */
	ExecClearTuple(mergestate->jstate.cs_ResultTupleSlot);
	ExecClearTuple(mergestate->mj_MarkedTupleSlot);
	pfree(mergestate->mj_MarkedTupleSlot);
	mergestate->mj_MarkedTupleSlot = NULL;

	MJ1_printf("ExecEndMergeJoin: %s\n",
			   "node processing ended");
}

void
ExecReScanMergeJoin(MergeJoin *node, ExprContext *exprCtxt, Plan *parent)
{
	MergeJoinState *mergestate = node->mergestate;
	TupleTableSlot *mjSlot = mergestate->mj_MarkedTupleSlot;

	ExecClearTuple(mjSlot);
	mjSlot->val = NULL;
	mjSlot->ttc_shouldFree = true;
	mjSlot->ttc_tupleDescriptor = NULL;
	mjSlot->ttc_whichplan = -1;
	mjSlot->ttc_descIsNew = true;

	mergestate->mj_JoinState = EXEC_MJ_INITIALIZE;

	/*
	 * if chgParam of subnodes is not null then plans will be re-scanned
	 * by first ExecProcNode.
	 */
	if (((Plan *) node)->lefttree->chgParam == NULL)
		ExecReScan(((Plan *) node)->lefttree, exprCtxt, (Plan *) node);
	if (((Plan *) node)->righttree->chgParam == NULL)
		ExecReScan(((Plan *) node)->righttree, exprCtxt, (Plan *) node);

}
