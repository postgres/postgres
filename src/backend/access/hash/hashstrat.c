/*-------------------------------------------------------------------------
 *
 * btstrat.c
 *	  Srategy map entries for the btree indexed access method
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/hash/Attic/hashstrat.c,v 1.16 2000/01/26 05:55:55 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"


/*
 *	only one valid strategy for hash tables: equality.
 */

#ifdef NOT_USED
static StrategyNumber HTNegate[1] = {
	InvalidStrategy
};

static StrategyNumber HTCommute[1] = {
	HTEqualStrategyNumber
};

static StrategyNumber HTNegateCommute[1] = {
	InvalidStrategy
};

static StrategyEvaluationData HTEvaluationData = {
	/* XXX static for simplicity */

	HTMaxStrategyNumber,
	(StrategyTransformMap) HTNegate,
	(StrategyTransformMap) HTCommute,
	(StrategyTransformMap) HTNegateCommute,
	{NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}
};

#endif

/* ----------------------------------------------------------------
 *		RelationGetHashStrategy
 * ----------------------------------------------------------------
 */

#ifdef NOT_USED
static StrategyNumber
_hash_getstrat(Relation rel,
			   AttrNumber attno,
			   RegProcedure proc)
{
	StrategyNumber strat;

	strat = RelationGetStrategy(rel, attno, &HTEvaluationData, proc);

	Assert(StrategyNumberIsValid(strat));

	return strat;
}

#endif

#ifdef NOT_USED
static bool
_hash_invokestrat(Relation rel,
				  AttrNumber attno,
				  StrategyNumber strat,
				  Datum left,
				  Datum right)
{
	return (RelationInvokeStrategy(rel, &HTEvaluationData, attno, strat,
								   left, right));
}

#endif
