/*-------------------------------------------------------------------------
 *
 * nodeUnique.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeUnique.h,v 1.7 1998/09/01 04:36:11 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEUNIQUE_H
#define NODEUNIQUE_H

#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"

extern TupleTableSlot *ExecUnique(Unique *node);
extern bool ExecInitUnique(Unique *node, EState *estate, Plan *parent);
extern int	ExecCountSlotsUnique(Unique *node);
extern void ExecEndUnique(Unique *node);
extern void ExecReScanUnique(Unique *node, ExprContext *exprCtxt, Plan *parent);

#endif	 /* NODEUNIQUE_H */
