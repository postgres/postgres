/*-------------------------------------------------------------------------
 *
 * nodeSubqueryscan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeSubqueryscan.h,v 1.1 2000/09/29 18:21:38 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODESUBQUERYSCAN_H
#define NODESUBQUERYSCAN_H

#include "nodes/plannodes.h"

extern TupleTableSlot *ExecSubqueryScan(SubqueryScan *node);
extern void ExecEndSubqueryScan(SubqueryScan *node);
extern bool ExecInitSubqueryScan(SubqueryScan *node, EState *estate, Plan *parent);
extern int	ExecCountSlotsSubqueryScan(SubqueryScan *node);
extern void ExecSubqueryReScan(SubqueryScan *node, ExprContext *exprCtxt, Plan *parent);

#endif	 /* NODESUBQUERYSCAN_H */
