/*-------------------------------------------------------------------------
 *
 * nodeGroup.h
 *	  prototypes for nodeGroup.c
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeGroup.h,v 1.33 2009/01/01 17:23:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEGROUP_H
#define NODEGROUP_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsGroup(Group *node);
extern GroupState *ExecInitGroup(Group *node, EState *estate, int eflags);
extern TupleTableSlot *ExecGroup(GroupState *node);
extern void ExecEndGroup(GroupState *node);
extern void ExecReScanGroup(GroupState *node, ExprContext *exprCtxt);

#endif   /* NODEGROUP_H */
