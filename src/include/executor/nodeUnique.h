/*-------------------------------------------------------------------------
 *
 * nodeUnique.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeUnique.h,v 1.5 1997/11/26 01:13:06 momjian Exp $
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

#endif							/* NODEUNIQUE_H */
