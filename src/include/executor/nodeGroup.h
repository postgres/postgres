/*-------------------------------------------------------------------------
 *
 * nodeGroup.h
 *	  prototypes for nodeGroup.c
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeGroup.h,v 1.15 2000/01/27 18:11:41 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEGROUP_H
#define NODEGROUP_H

#include "nodes/plannodes.h"

extern TupleTableSlot *ExecGroup(Group *node);
extern bool ExecInitGroup(Group *node, EState *estate, Plan *parent);
extern int	ExecCountSlotsGroup(Group *node);
extern void ExecEndGroup(Group *node);
extern void ExecReScanGroup(Group *node, ExprContext *exprCtxt, Plan *parent);

extern bool execTuplesMatch(HeapTuple tuple1,
							HeapTuple tuple2,
							TupleDesc tupdesc,
							int numCols,
							AttrNumber *matchColIdx,
							FmgrInfo *eqfunctions);
extern FmgrInfo *execTuplesMatchPrepare(TupleDesc tupdesc,
										int numCols,
										AttrNumber *matchColIdx);

#endif	 /* NODEGROUP_H */
