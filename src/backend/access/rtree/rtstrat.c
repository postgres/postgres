/*-------------------------------------------------------------------------
 *
 * rtstrat.c
 *	  strategy map data for rtrees.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/rtree/Attic/rtstrat.c,v 1.21 2003/08/04 02:39:57 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/istrat.h"
#include "access/rtree.h"

static StrategyNumber RelationGetRTStrategy(Relation r,
					  AttrNumber attnum, RegProcedure proc);

/*
 *	Note:  negate, commute, and negatecommute all assume that operators are
 *		   ordered as follows in the strategy map:
 *
 *		left, left-or-overlap, overlap, right-or-overlap, right, same,
 *		contains, contained-by
 *
 *	The negate, commute, and negatecommute arrays are used by the planner
 *	to plan indexed scans over data that appears in the qualificiation in
 *	a boolean negation, or whose operands appear in the wrong order.  For
 *	example, if the operator "<%" means "contains", and the user says
 *
 *		where not rel.box <% "(10,10,20,20)"::box
 *
 *	the planner can plan an index scan by noting that rtree indices have
 *	an operator in their operator class for negating <%.
 *
 *	Similarly, if the user says something like
 *
 *		where "(10,10,20,20)"::box <% rel.box
 *
 *	the planner can see that the rtree index on rel.box has an operator in
 *	its opclass for commuting <%, and plan the scan using that operator.
 *	This added complexity in the access methods makes the planner a lot easier
 *	to write.
 */

/* if a op b, what operator tells us if (not a op b)? */
static StrategyNumber RTNegate[RTNStrategies] = {
	InvalidStrategy,
	InvalidStrategy,
	InvalidStrategy,
	InvalidStrategy,
	InvalidStrategy,
	InvalidStrategy,
	InvalidStrategy,
	InvalidStrategy
};

/* if a op_1 b, what is the operator op_2 such that b op_2 a? */
static StrategyNumber RTCommute[RTNStrategies] = {
	InvalidStrategy,
	InvalidStrategy,
	InvalidStrategy,
	InvalidStrategy,
	InvalidStrategy,
	InvalidStrategy,
	InvalidStrategy,
	InvalidStrategy
};

/* if a op_1 b, what is the operator op_2 such that (b !op_2 a)? */
static StrategyNumber RTNegateCommute[RTNStrategies] = {
	InvalidStrategy,
	InvalidStrategy,
	InvalidStrategy,
	InvalidStrategy,
	InvalidStrategy,
	InvalidStrategy,
	InvalidStrategy,
	InvalidStrategy
};

/*
 *	Now do the TermData arrays.  These exist in case the user doesn't give
 *	us a full set of operators for a particular operator class.  The idea
 *	is that by making multiple comparisons using any one of the supplied
 *	operators, we can decide whether two n-dimensional polygons are equal.
 *	For example, if a contains b and b contains a, we may conclude that
 *	a and b are equal.
 *
 *	The presence of the TermData arrays in all this is a historical accident.
 *	Early in the development of the POSTGRES access methods, it was believed
 *	that writing functions was harder than writing arrays.	This is wrong;
 *	TermData is hard to understand and hard to get right.  In general, when
 *	someone populates a new operator class, they populate it completely.  If
 *	Mike Hirohama had forced Cimarron Taylor to populate the strategy map
 *	for btree int2_ops completely in 1988, you wouldn't have to deal with
 *	all this now.  Too bad for you.
 *
 *	Since you can't necessarily do this in all cases (for example, you can't
 *	do it given only "intersects" or "disjoint"), TermData arrays for some
 *	operators don't appear below.
 *
 *	Note that if you DO supply all the operators required in a given opclass
 *	by inserting them into the pg_opclass system catalog, you can get away
 *	without doing all this TermData stuff.	Since the rtree code is intended
 *	to be a reference for access method implementors, I'm doing TermData
 *	correctly here.
 *
 *	Note on style:	these are all actually of type StrategyTermData, but
 *	since those have variable-length data at the end of the struct we can't
 *	properly initialize them if we declare them to be what they are.
 */

/* if you only have "contained-by", how do you determine equality? */
static uint16 RTContainedByTermData[] = {
	2,							/* make two comparisons */
	RTContainedByStrategyNumber,	/* use "a contained-by b" */
	0x0,						/* without any magic */
	RTContainedByStrategyNumber,	/* then use contained-by, */
	SK_COMMUTE					/* swapping a and b */
};

/* if you only have "contains", how do you determine equality? */
static uint16 RTContainsTermData[] = {
	2,							/* make two comparisons */
	RTContainsStrategyNumber,	/* use "a contains b" */
	0x0,						/* without any magic */
	RTContainsStrategyNumber,	/* then use contains again, */
	SK_COMMUTE					/* swapping a and b */
};

/* now put all that together in one place for the planner */
static StrategyTerm RTEqualExpressionData[] = {
	(StrategyTerm) RTContainedByTermData,
	(StrategyTerm) RTContainsTermData,
	NULL
};

/*
 *	If you were sufficiently attentive to detail, you would go through
 *	the ExpressionData pain above for every one of the seven strategies
 *	we defined.  I am not.	Now we declare the StrategyEvaluationData
 *	structure that gets shipped around to help the planner and the access
 *	method decide what sort of scan it should do, based on (a) what the
 *	user asked for, (b) what operators are defined for a particular opclass,
 *	and (c) the reams of information we supplied above.
 *
 *	The idea of all of this initialized data is to make life easier on the
 *	user when he defines a new operator class to use this access method.
 *	By filling in all the data, we let him get away with leaving holes in his
 *	operator class, and still let him use the index.  The added complexity
 *	in the access methods just isn't worth the trouble, though.
 */

static StrategyExpression RTEvaluationExpressions[RTNStrategies] = {
	NULL,						/* express left */
	NULL,						/* express overleft */
	NULL,						/* express overlap */
	NULL,						/* express overright */
	NULL,						/* express right */
	(StrategyExpression) RTEqualExpressionData, /* express same */
	NULL,						/* express contains */
	NULL						/* express contained-by */
};

static StrategyEvaluationData RTEvaluationData = {
	RTNStrategies,				/* # of strategies */
	(StrategyTransformMap) RTNegate,	/* how to do (not qual) */
	(StrategyTransformMap) RTCommute,	/* how to swap operands */
	(StrategyTransformMap) RTNegateCommute,		/* how to do both */
	RTEvaluationExpressions
};

/*
 *	Okay, now something peculiar to rtrees that doesn't apply to most other
 *	indexing structures:  When we're searching a tree for a given value, we
 *	can't do the same sorts of comparisons on internal node entries as we
 *	do at leaves.  The reason is that if we're looking for (say) all boxes
 *	that are the same as (0,0,10,10), then we need to find all leaf pages
 *	that overlap that region.  So internally we search for overlap, and at
 *	the leaf we search for equality.
 *
 *	This array maps leaf search operators to the internal search operators.
 *	We assume the normal ordering on operators:
 *
 *		left, left-or-overlap, overlap, right-or-overlap, right, same,
 *		contains, contained-by
 */
static StrategyNumber RTOperMap[RTNStrategies] = {
	RTOverLeftStrategyNumber,
	RTOverLeftStrategyNumber,
	RTOverlapStrategyNumber,
	RTOverRightStrategyNumber,
	RTOverRightStrategyNumber,
	RTContainsStrategyNumber,
	RTContainsStrategyNumber,
	RTOverlapStrategyNumber
};

static StrategyNumber
RelationGetRTStrategy(Relation r,
					  AttrNumber attnum,
					  RegProcedure proc)
{
	return RelationGetStrategy(r, attnum, &RTEvaluationData, proc);
}

#ifdef NOT_USED
bool
RelationInvokeRTStrategy(Relation r,
						 AttrNumber attnum,
						 StrategyNumber s,
						 Datum left,
						 Datum right)
{
	return (RelationInvokeStrategy(r, &RTEvaluationData, attnum, s,
								   left, right));
}
#endif

RegProcedure
RTMapOperator(Relation r,
			  AttrNumber attnum,
			  RegProcedure proc)
{
	StrategyNumber procstrat;
	StrategyMap strategyMap;

	procstrat = RelationGetRTStrategy(r, attnum, proc);
	strategyMap = IndexStrategyGetStrategyMap(RelationGetIndexStrategy(r),
											  RTNStrategies,
											  attnum);

	return strategyMap->entry[RTOperMap[procstrat - 1] - 1].sk_procedure;
}
