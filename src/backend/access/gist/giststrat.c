/*-------------------------------------------------------------------------
 *
 * giststrat.c
 *	  strategy map data for GiSTs.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  /usr/local/devel/pglite/cvs/src/backend/access/gist/giststrat.c,v 1.4 1995/06/14 00:10:05 jolly Exp
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gist.h"
#include "access/istrat.h"

/*
 *	Note:  negate, commute, and negatecommute all assume that operators are
 *		   ordered as follows in the strategy map:
 *
 *		contains, contained-by
 *
 *	The negate, commute, and negatecommute arrays are used by the planner
 *	to plan indexed scans over data that appears in the qualificiation in
 *	a boolean negation, or whose operands appear in the wrong order.  For
 *	example, if the operator "<%" means "contains", and the user says
 *
 *		where not rel.box <% "(10,10,20,20)"::box
 *
 *	the planner can plan an index scan by noting that GiST indices have
 *	an operator in their operator class for negating <%.
 *
 *	Similarly, if the user says something like
 *
 *		where "(10,10,20,20)"::box <% rel.box
 *
 *	the planner can see that the GiST index on rel.box has an operator in
 *	its opclass for commuting <%, and plan the scan using that operator.
 *	This added complexity in the access methods makes the planner a lot easier
 *	to write.
 */

/* if a op b, what operator tells us if (not a op b)? */
static StrategyNumber GISTNegate[GISTNStrategies] = {
	InvalidStrategy,
	InvalidStrategy,
	InvalidStrategy
};

/* if a op_1 b, what is the operator op_2 such that b op_2 a? */
static StrategyNumber GISTCommute[GISTNStrategies] = {
	InvalidStrategy,
	InvalidStrategy,
	InvalidStrategy
};

/* if a op_1 b, what is the operator op_2 such that (b !op_2 a)? */
static StrategyNumber GISTNegateCommute[GISTNStrategies] = {
	InvalidStrategy,
	InvalidStrategy,
	InvalidStrategy
};

/*
 * GiSTs do not currently support TermData (see rtree/rtstrat.c for
 * discussion of
 * TermData) -- such logic must be encoded in the user's Consistent function.
 */

/*
 *	If you were sufficiently attentive to detail, you would go through
 *	the ExpressionData pain above for every one of the strategies
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

static StrategyEvaluationData GISTEvaluationData = {
	GISTNStrategies,			/* # of strategies */
	(StrategyTransformMap) GISTNegate,	/* how to do (not qual) */
	(StrategyTransformMap) GISTCommute, /* how to swap operands */
	(StrategyTransformMap) GISTNegateCommute,	/* how to do both */
	{NULL}
};

StrategyNumber
RelationGetGISTStrategy(Relation r,
						AttrNumber attnum,
						RegProcedure proc)
{
	return RelationGetStrategy(r, attnum, &GISTEvaluationData, proc);
}

#ifdef NOT_USED
bool
RelationInvokeGISTStrategy(Relation r,
						   AttrNumber attnum,
						   StrategyNumber s,
						   Datum left,
						   Datum right)
{
	return (RelationInvokeStrategy(r, &GISTEvaluationData, attnum, s,
								   left, right));
}

#endif
