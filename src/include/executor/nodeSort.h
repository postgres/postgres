/*-------------------------------------------------------------------------
 *
 * nodeSort.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeSort.h,v 1.5 1997/11/26 01:13:04 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODESORT_H
#define NODESORT_H

#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"

extern TupleTableSlot *ExecSort(Sort *node);
extern bool ExecInitSort(Sort *node, EState *estate, Plan *parent);
extern int	ExecCountSlotsSort(Sort *node);
extern void ExecEndSort(Sort *node);
extern void ExecSortMarkPos(Sort *node);
extern void ExecSortRestrPos(Sort *node);

#endif							/* NODESORT_H */
