/*-------------------------------------------------------------------------
 *
 * btstrat.c--
 *	  Srategy map entries for the btree indexed access method
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/hash/Attic/hashstrat.c,v 1.11 1997/09/08 02:20:21 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <access/hash.h>
#include <access/istrat.h>

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

	return (strat);
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
