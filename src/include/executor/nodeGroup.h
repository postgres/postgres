/*-------------------------------------------------------------------------
 *
 * nodeGroup.h--
 *	  prototypes for nodeGroup.c
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeGroup.h,v 1.5 1997/11/26 01:12:46 momjian Exp $
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

#endif							/* NODEGROUP_H */
