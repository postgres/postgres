/*-------------------------------------------------------------------------
 *
 * nodeSubqueryscan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeSubqueryscan.h,v 1.7 2002/12/05 15:50:38 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODESUBQUERYSCAN_H
#define NODESUBQUERYSCAN_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsSubqueryScan(SubqueryScan *node);
extern SubqueryScanState *ExecInitSubqueryScan(SubqueryScan *node, EState *estate);
extern TupleTableSlot *ExecSubqueryScan(SubqueryScanState *node);
extern void ExecEndSubqueryScan(SubqueryScanState *node);
extern void ExecSubqueryReScan(SubqueryScanState *node, ExprContext *exprCtxt);

#endif   /* NODESUBQUERYSCAN_H */
