/*-------------------------------------------------------------------------
 *
 * hashstrat.c
 *	  Srategy map entries for the hash indexed access method
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/hash/Attic/hashstrat.c,v 1.18 2001/05/30 19:53:40 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"


/*
 *	only one valid strategy for hash tables: equality.
 */

#ifdef NOT_USED

static StrategyNumber HTNegate[HTMaxStrategyNumber] = {
	InvalidStrategy
};

static StrategyNumber HTCommute[HTMaxStrategyNumber] = {
	HTEqualStrategyNumber
};

static StrategyNumber HTNegateCommute[HTMaxStrategyNumber] = {
	InvalidStrategy
};

static StrategyExpression HTEvaluationExpressions[HTMaxStrategyNumber] = {
	NULL
};

static StrategyEvaluationData HTEvaluationData = {
	HTMaxStrategyNumber,
	(StrategyTransformMap) HTNegate,
	(StrategyTransformMap) HTCommute,
	(StrategyTransformMap) HTNegateCommute,
	HTEvaluationExpressions
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
