/*-------------------------------------------------------------------------
 *
 * btstrat.c--
 *    Srategy map entries for the btree indexed access method
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/access/hash/Attic/hashstrat.c,v 1.7 1996/11/05 09:40:24 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>
 
#include <access/hash.h>
#include <access/istrat.h>

/* 
 *  only one valid strategy for hash tables: equality. 
 */

static StrategyNumber	HTNegate[1] = {
    InvalidStrategy
};

static StrategyNumber	HTCommute[1] = {
    HTEqualStrategyNumber
};

static StrategyNumber	HTNegateCommute[1] = {
    InvalidStrategy
};

static StrategyEvaluationData	HTEvaluationData = {
    /* XXX static for simplicity */

    HTMaxStrategyNumber,
    (StrategyTransformMap)HTNegate,
    (StrategyTransformMap)HTCommute,
    (StrategyTransformMap)HTNegateCommute,
    {NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL}
};

/* ----------------------------------------------------------------
 *	RelationGetHashStrategy
 * ----------------------------------------------------------------
 */

StrategyNumber
_hash_getstrat(Relation rel,
	       AttrNumber attno,
	       RegProcedure proc)
{
    StrategyNumber	strat;

    strat = RelationGetStrategy(rel, attno, &HTEvaluationData, proc);

    Assert(StrategyNumberIsValid(strat));

    return (strat);
}

bool
_hash_invokestrat(Relation rel,
		  AttrNumber attno,
		  StrategyNumber strat,
		  Datum left,
		  Datum right)
{
    return (RelationInvokeStrategy(rel, &HTEvaluationData, attno, strat, 
				   left, right));
}

























