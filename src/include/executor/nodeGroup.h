/*-------------------------------------------------------------------------
 *
 * nodeGroup.h
 *	  prototypes for nodeGroup.c
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeGroup.h,v 1.23 2002/12/05 15:50:37 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEGROUP_H
#define NODEGROUP_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsGroup(Group *node);
extern GroupState *ExecInitGroup(Group *node, EState *estate);
extern TupleTableSlot *ExecGroup(GroupState *node);
extern void ExecEndGroup(GroupState *node);
extern void ExecReScanGroup(GroupState *node, ExprContext *exprCtxt);

extern bool execTuplesMatch(HeapTuple tuple1,
				HeapTuple tuple2,
				TupleDesc tupdesc,
				int numCols,
				AttrNumber *matchColIdx,
				FmgrInfo *eqfunctions,
				MemoryContext evalContext);
extern FmgrInfo *execTuplesMatchPrepare(TupleDesc tupdesc,
					   int numCols,
					   AttrNumber *matchColIdx);

#endif   /* NODEGROUP_H */
