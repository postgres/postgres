/*-------------------------------------------------------------------------
 *
 * nbtstrat.c
 *	  Strategy map entries for the btree indexed access method
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/nbtree/Attic/nbtstrat.c,v 1.18 2003/08/04 02:39:57 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/istrat.h"
#include "access/nbtree.h"

/*
 * Note:
 *		StrategyNegate, StrategyCommute, and StrategyNegateCommute
 *		assume <, <=, ==, >=, > ordering.
 */
static StrategyNumber BTNegate[BTMaxStrategyNumber] = {
	BTGreaterEqualStrategyNumber,
	BTGreaterStrategyNumber,
	InvalidStrategy,
	BTLessStrategyNumber,
	BTLessEqualStrategyNumber
};

static StrategyNumber BTCommute[BTMaxStrategyNumber] = {
	BTGreaterStrategyNumber,
	BTGreaterEqualStrategyNumber,
	InvalidStrategy,
	BTLessEqualStrategyNumber,
	BTLessStrategyNumber
};

static StrategyNumber BTNegateCommute[BTMaxStrategyNumber] = {
	BTLessEqualStrategyNumber,
	BTLessStrategyNumber,
	InvalidStrategy,
	BTGreaterStrategyNumber,
	BTGreaterEqualStrategyNumber
};

static uint16 BTLessTermData[] = {		/* XXX type clash */
	2,
	BTLessStrategyNumber,
	SK_NEGATE,
	BTLessStrategyNumber,
	SK_NEGATE | SK_COMMUTE
};

static uint16 BTLessEqualTermData[] = { /* XXX type clash */
	2,
	BTLessEqualStrategyNumber,
	0x0,
	BTLessEqualStrategyNumber,
	SK_COMMUTE
};

static uint16 BTGreaterEqualTermData[] = {		/* XXX type clash */
	2,
	BTGreaterEqualStrategyNumber,
	0x0,
	BTGreaterEqualStrategyNumber,
	SK_COMMUTE
};

static uint16 BTGreaterTermData[] = {	/* XXX type clash */
	2,
	BTGreaterStrategyNumber,
	SK_NEGATE,
	BTGreaterStrategyNumber,
	SK_NEGATE | SK_COMMUTE
};

static StrategyTerm BTEqualExpressionData[] = {
	(StrategyTerm) BTLessTermData,		/* XXX */
	(StrategyTerm) BTLessEqualTermData, /* XXX */
	(StrategyTerm) BTGreaterEqualTermData,		/* XXX */
	(StrategyTerm) BTGreaterTermData,	/* XXX */
	NULL
};

static StrategyExpression BTEvaluationExpressions[BTMaxStrategyNumber] = {
	NULL,
	NULL,
	(StrategyExpression) BTEqualExpressionData,
	NULL,
	NULL
};

static StrategyEvaluationData BTEvaluationData = {
	BTMaxStrategyNumber,
	(StrategyTransformMap) BTNegate,
	(StrategyTransformMap) BTCommute,
	(StrategyTransformMap) BTNegateCommute,
	BTEvaluationExpressions
};

/* ----------------------------------------------------------------
 *		RelationGetBTStrategy
 * ----------------------------------------------------------------
 */

StrategyNumber
_bt_getstrat(Relation rel,
			 AttrNumber attno,
			 RegProcedure proc)
{
	StrategyNumber strat;

	strat = RelationGetStrategy(rel, attno, &BTEvaluationData, proc);

	Assert(StrategyNumberIsValid(strat));

	return strat;
}

#ifdef NOT_USED

bool
_bt_invokestrat(Relation rel,
				AttrNumber attno,
				StrategyNumber strat,
				Datum left,
				Datum right)
{
	return (RelationInvokeStrategy(rel, &BTEvaluationData, attno, strat,
								   left, right));
}

#endif
