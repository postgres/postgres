/*-------------------------------------------------------------------------
 *
 * strat.h
 *	  index strategy type definitions
 *	  (separated out from original istrat.h to avoid circular refs)
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: strat.h,v 1.27 2003/08/04 02:40:10 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef STRAT_H
#define STRAT_H

#include "access/skey.h"


typedef uint16 StrategyNumber;

#define InvalidStrategy 0

typedef struct StrategyTransformMapData
{
	StrategyNumber strategy[1]; /* VARIABLE LENGTH ARRAY */
} StrategyTransformMapData;		/* VARIABLE LENGTH STRUCTURE */

typedef StrategyTransformMapData *StrategyTransformMap;

typedef struct StrategyOperatorData
{
	StrategyNumber strategy;
	bits16		flags;			/* scan qualification flags, see skey.h */
} StrategyOperatorData;

typedef StrategyOperatorData *StrategyOperator;

typedef struct StrategyTermData
{								/* conjunctive term */
	uint16		degree;
	StrategyOperatorData operatorData[1];		/* VARIABLE LENGTH ARRAY */
} StrategyTermData;				/* VARIABLE LENGTH STRUCTURE */

typedef StrategyTermData *StrategyTerm;

typedef struct StrategyExpressionData
{								/* disjunctive normal form */
	StrategyTerm term[1];		/* VARIABLE LENGTH ARRAY */
} StrategyExpressionData;		/* VARIABLE LENGTH STRUCTURE */

typedef StrategyExpressionData *StrategyExpression;

typedef struct StrategyEvaluationData
{
	StrategyNumber maxStrategy;
	/* each of these must point to an array of maxStrategy elements: */
	StrategyTransformMap negateTransform;
	StrategyTransformMap commuteTransform;
	StrategyTransformMap negateCommuteTransform;
	StrategyExpression *expression;
} StrategyEvaluationData;

typedef StrategyEvaluationData *StrategyEvaluation;

/*
 * StrategyTransformMapIsValid
 *		Returns true iff strategy transformation map is valid.
 */
#define StrategyTransformMapIsValid(transform) PointerIsValid(transform)


#define AMStrategies(foo)		(foo)

typedef struct StrategyMapData
{
	ScanKeyData entry[1];		/* VARIABLE LENGTH ARRAY */
} StrategyMapData;				/* VARIABLE LENGTH STRUCTURE */

typedef StrategyMapData *StrategyMap;

typedef struct IndexStrategyData
{
	StrategyMapData strategyMapData[1]; /* VARIABLE LENGTH ARRAY */
} IndexStrategyData;			/* VARIABLE LENGTH STRUCTURE */

typedef IndexStrategyData *IndexStrategy;

#endif   /* STRAT_H */
