/*-------------------------------------------------------------------------
 *
 * istrat.h--
 *    POSTGRES index strategy definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: istrat.h,v 1.2 1996/10/31 09:46:41 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	ISTRAT_H
#define ISTRAT_H

#include "access/attnum.h"
#include "access/skey.h"
#include "access/strat.h"
#include "utils/rel.h"		/* for Relation */

/*
 * StrategyNumberIsValid --
 *	True iff the strategy number is valid.
 */
#define StrategyNumberIsValid(strategyNumber) \
    ((bool) ((strategyNumber) != InvalidStrategy))

/*
 * StrategyNumberIsInBounds --
 *	True iff strategy number is within given bounds.
 *
 * Note:
 *	Assumes StrategyNumber is an unsigned type.
 *	Assumes the bounded interval to be (0,max].
 */
#define StrategyNumberIsInBounds(strategyNumber, maxStrategyNumber) \
    ((bool)(InvalidStrategy < (strategyNumber) && \
	    (strategyNumber) <= (maxStrategyNumber)))

/*
 * StrategyMapIsValid --
 *	True iff the index strategy mapping is valid.
 */
#define	StrategyMapIsValid(map) PointerIsValid(map)

/*
 * IndexStrategyIsValid --
 *	True iff the index strategy is valid.
 */
#define	IndexStrategyIsValid(s)	PointerIsValid(s)

extern ScanKey StrategyMapGetScanKeyEntry(StrategyMap map,
					  StrategyNumber strategyNumber);
extern StrategyMap IndexStrategyGetStrategyMap(IndexStrategy indexStrategy,
	StrategyNumber maxStrategyNum, AttrNumber attrNum);

extern Size
AttributeNumberGetIndexStrategySize(AttrNumber maxAttributeNumber,
				    StrategyNumber maxStrategyNumber);
extern bool StrategyOperatorIsValid(StrategyOperator operator,
				    StrategyNumber maxStrategy);
extern bool StrategyTermIsValid(StrategyTerm term,
				StrategyNumber maxStrategy);
extern bool StrategyExpressionIsValid(StrategyExpression expression,
				      StrategyNumber maxStrategy);
extern bool StrategyEvaluationIsValid(StrategyEvaluation evaluation);
extern StrategyNumber RelationGetStrategy(Relation relation,
	AttrNumber attributeNumber, StrategyEvaluation evaluation,
	RegProcedure procedure);
extern bool RelationInvokeStrategy(Relation relation,
	StrategyEvaluation evaluation, AttrNumber attributeNumber,
	StrategyNumber strategy, Datum left, Datum right);
extern void IndexSupportInitialize(IndexStrategy indexStrategy,
	RegProcedure *indexSupport, Oid indexObjectId,
	Oid accessMethodObjectId, StrategyNumber maxStrategyNumber,
	StrategyNumber maxSupportNumber, AttrNumber maxAttributeNumber);


#endif	/* ISTRAT_H */
