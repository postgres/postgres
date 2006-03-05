/*-------------------------------------------------------------------------
 *
 * nodeSubqueryscan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeSubqueryscan.h,v 1.13 2006/03/05 15:58:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODESUBQUERYSCAN_H
#define NODESUBQUERYSCAN_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsSubqueryScan(SubqueryScan *node);
extern SubqueryScanState *ExecInitSubqueryScan(SubqueryScan *node, EState *estate, int eflags);
extern TupleTableSlot *ExecSubqueryScan(SubqueryScanState *node);
extern void ExecEndSubqueryScan(SubqueryScanState *node);
extern void ExecSubqueryReScan(SubqueryScanState *node, ExprContext *exprCtxt);

#endif   /* NODESUBQUERYSCAN_H */
