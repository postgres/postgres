/*-------------------------------------------------------------------------
 *
 * nodeAgg.h
 *	  prototypes for nodeAgg.c
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeAgg.h,v 1.23 2004/12/31 22:03:29 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEAGG_H
#define NODEAGG_H

#include "fmgr.h"
#include "nodes/execnodes.h"

extern int	ExecCountSlotsAgg(Agg *node);
extern AggState *ExecInitAgg(Agg *node, EState *estate);
extern TupleTableSlot *ExecAgg(AggState *node);
extern void ExecEndAgg(AggState *node);
extern void ExecReScanAgg(AggState *node, ExprContext *exprCtxt);

extern Datum aggregate_dummy(PG_FUNCTION_ARGS);

#endif   /* NODEAGG_H */
