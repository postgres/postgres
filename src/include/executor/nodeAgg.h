/*-------------------------------------------------------------------------
 *
 * nodeAgg.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeAgg.h,v 1.6 1997/11/26 01:12:40 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEAGG_H
#define NODEAGG_H

#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"

extern TupleTableSlot *ExecAgg(Agg *node);
extern bool ExecInitAgg(Agg *node, EState *estate, Plan *parent);
extern int	ExecCountSlotsAgg(Agg *node);
extern void ExecEndAgg(Agg *node);

#endif							/* NODEAGG_H */
