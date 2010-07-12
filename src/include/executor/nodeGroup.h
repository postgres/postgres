/*-------------------------------------------------------------------------
 *
 * nodeGroup.h
 *	  prototypes for nodeGroup.c
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeGroup.h,v 1.36 2010/07/12 17:01:06 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEGROUP_H
#define NODEGROUP_H

#include "nodes/execnodes.h"

extern GroupState *ExecInitGroup(Group *node, EState *estate, int eflags);
extern TupleTableSlot *ExecGroup(GroupState *node);
extern void ExecEndGroup(GroupState *node);
extern void ExecReScanGroup(GroupState *node);

#endif   /* NODEGROUP_H */
