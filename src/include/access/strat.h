/*-------------------------------------------------------------------------
 *
 * strat.h--
 *    index strategy type definitions
 *    (separated out from original istrat.h to avoid circular refs)
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: strat.h,v 1.2 1996/10/19 04:05:44 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef STRAT_H
#define STRAT_H

#include "access/skey.h" 

typedef uint16	StrategyNumber;

#define InvalidStrategy	0

typedef struct StrategyTransformMapData {
    StrategyNumber	strategy[1];	/* VARIABLE LENGTH ARRAY */
} StrategyTransformMapData;	/* VARIABLE LENGTH STRUCTURE */

typedef StrategyTransformMapData	*StrategyTransformMap;

typedef struct StrategyOperatorData {
    StrategyNumber	strategy;
    bits16		flags;		/* scan qualification flags h/skey.h */
} StrategyOperatorData;

typedef StrategyOperatorData	*StrategyOperator;

typedef struct StrategyTermData {	/* conjunctive term */
    uint16			degree;
    StrategyOperatorData	operatorData[1];	/* VARIABLE LENGTH */
} StrategyTermData;	/* VARIABLE LENGTH STRUCTURE */

typedef StrategyTermData	*StrategyTerm;

typedef struct StrategyExpressionData {	/* disjunctive normal form */
    StrategyTerm	term[1];	/* VARIABLE LENGTH ARRAY */
} StrategyExpressionData;	/* VARIABLE LENGTH STRUCTURE */

typedef StrategyExpressionData	*StrategyExpression;

typedef struct StrategyEvaluationData {
    StrategyNumber		maxStrategy;
    StrategyTransformMap	negateTransform;
    StrategyTransformMap	commuteTransform;
    StrategyTransformMap	negateCommuteTransform;
    StrategyExpression	expression[12];	/* XXX VARIABLE LENGTH */
} StrategyEvaluationData;	/* VARIABLE LENGTH STRUCTURE */

typedef StrategyEvaluationData	*StrategyEvaluation;

/*
 * StrategyTransformMapIsValid --
 *	Returns true iff strategy transformation map is valid.
 */
#define	StrategyTransformMapIsValid(transform) PointerIsValid(transform)


#ifndef	CorrectStrategies		/* XXX this should be removable */
#define AMStrategies(foo)	12
#else	/* !defined(CorrectStrategies) */
#define AMStrategies(foo)	(foo)
#endif	/* !defined(CorrectStrategies) */

typedef struct StrategyMapData {
	ScanKeyData		entry[1];	/* VARIABLE LENGTH ARRAY */
} StrategyMapData;	/* VARIABLE LENGTH STRUCTURE */

typedef StrategyMapData	*StrategyMap;

typedef struct IndexStrategyData {
	StrategyMapData	strategyMapData[1];	/* VARIABLE LENGTH ARRAY */
} IndexStrategyData;	/* VARIABLE LENGTH STRUCTURE */

typedef IndexStrategyData	*IndexStrategy;

#endif /*STRAT_H */
