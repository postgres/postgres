/*-------------------------------------------------------------------------
 *
 * nodeAgg.h
 *
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeAgg.h,v 1.12 2001/01/24 19:43:23 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEAGG_H
#define NODEAGG_H

#include "nodes/plannodes.h"

extern TupleTableSlot *ExecAgg(Agg *node);
extern bool ExecInitAgg(Agg *node, EState *estate, Plan *parent);
extern int	ExecCountSlotsAgg(Agg *node);
extern void ExecEndAgg(Agg *node);
extern void ExecReScanAgg(Agg *node, ExprContext *exprCtxt, Plan *parent);

#endif	 /* NODEAGG_H */
