/*-------------------------------------------------------------------------
 *
 * nodeGroup.h--
 *	  prototypes for nodeGroup.c
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeGroup.h,v 1.6 1998/07/16 01:49:19 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEGROUP_H
#define NODEGROUP_H

#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"

extern TupleTableSlot *ExecGroup(Group *node);
extern bool ExecInitGroup(Group *node, EState *estate, Plan *parent);
extern int	ExecCountSlotsGroup(Group *node);
extern void ExecEndGroup(Group *node);
extern void ExecReScanGroup(Group *node, ExprContext *exprCtxt, Plan *parent);

#endif							/* NODEGROUP_H */
