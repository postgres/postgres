/*-------------------------------------------------------------------------
 *
 * btstrat.c--
 *    Srategy map entries for the btree indexed access method
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/access/nbtree/Attic/nbtstrat.c,v 1.4 1996/11/05 10:35:37 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <access/istrat.h>
#include <storage/bufpage.h>
#include <access/nbtree.h>

/*
 * Note:
 *	StrategyNegate, StrategyCommute, and StrategyNegateCommute
 *	assume <, <=, ==, >=, > ordering.
 */
static StrategyNumber	BTNegate[5] = {
    BTGreaterEqualStrategyNumber,
    BTGreaterStrategyNumber,
    InvalidStrategy,
    BTLessStrategyNumber,
    BTLessEqualStrategyNumber
};

static StrategyNumber	BTCommute[5] = {
    BTGreaterStrategyNumber,
    BTGreaterEqualStrategyNumber,
    InvalidStrategy,
    BTLessEqualStrategyNumber,
    BTLessStrategyNumber
};

static StrategyNumber	BTNegateCommute[5] = {
    BTLessEqualStrategyNumber,
    BTLessStrategyNumber,
    InvalidStrategy,
    BTGreaterStrategyNumber,
    BTGreaterEqualStrategyNumber
};

static uint16	BTLessTermData[] = {		/* XXX type clash */
    2,
    BTLessStrategyNumber,
    SK_NEGATE,
    BTLessStrategyNumber,
    SK_NEGATE | SK_COMMUTE
};

static uint16	BTLessEqualTermData[] = {	/* XXX type clash */
    2,
    BTLessEqualStrategyNumber,
    0x0,
    BTLessEqualStrategyNumber,
    SK_COMMUTE
};

static uint16	BTGreaterEqualTermData[] = {	/* XXX type clash */
    2,
    BTGreaterEqualStrategyNumber,
    0x0,
    BTGreaterEqualStrategyNumber,
    SK_COMMUTE
    };

static uint16	BTGreaterTermData[] = {		/* XXX type clash */
    2,
    BTGreaterStrategyNumber,
    SK_NEGATE,
    BTGreaterStrategyNumber,
    SK_NEGATE | SK_COMMUTE
};

static StrategyTerm	BTEqualExpressionData[] = {
    (StrategyTerm)BTLessTermData,		/* XXX */
    (StrategyTerm)BTLessEqualTermData,		/* XXX */
    (StrategyTerm)BTGreaterEqualTermData,	/* XXX */
    (StrategyTerm)BTGreaterTermData,		/* XXX */
    NULL
};

static StrategyEvaluationData	BTEvaluationData = {
    /* XXX static for simplicity */
    
    BTMaxStrategyNumber,
    (StrategyTransformMap)BTNegate,	/* XXX */
    (StrategyTransformMap)BTCommute,	/* XXX */
    (StrategyTransformMap)BTNegateCommute,	/* XXX */

    { NULL, NULL, (StrategyExpression)BTEqualExpressionData, NULL, NULL,
      NULL,NULL,NULL,NULL,NULL,NULL,NULL}
};

/* ----------------------------------------------------------------
 *	RelationGetBTStrategy
 * ----------------------------------------------------------------
 */

StrategyNumber
_bt_getstrat(Relation rel,
	     AttrNumber attno,
	     RegProcedure proc)
{
    StrategyNumber	strat;
    
    strat = RelationGetStrategy(rel, attno, &BTEvaluationData, proc);
    
    Assert(StrategyNumberIsValid(strat));
    
    return (strat);
}

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
