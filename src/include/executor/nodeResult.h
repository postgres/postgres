/*-------------------------------------------------------------------------
 *
 * nodeResult.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeResult.h,v 1.5 1997/11/26 01:12:58 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODERESULT_H
#define NODERESULT_H

#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"

extern TupleTableSlot *ExecResult(Result *node);
extern bool ExecInitResult(Result *node, EState *estate, Plan *parent);
extern int	ExecCountSlotsResult(Result *node);
extern void ExecEndResult(Result *node);

#endif							/* NODERESULT_H */
