/*-------------------------------------------------------------------------
 *
 * btstrat.c--
 *    Srategy map entries for the btree indexed access method
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/access/hash/Attic/hashstrat.c,v 1.1.1.1 1996/07/09 06:21:10 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/bufpage.h"

#include "utils/elog.h"
#include "utils/rel.h"
#include "utils/excid.h"

#include "access/genam.h"
#include "access/hash.h"

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

























