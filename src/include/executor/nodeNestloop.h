/*-------------------------------------------------------------------------
 *
 * nodeNestloop.h
 *
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeNestloop.h,v 1.13 2000/08/13 02:50:20 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODENESTLOOP_H
#define NODENESTLOOP_H

#include "nodes/plannodes.h"

extern TupleTableSlot *ExecNestLoop(NestLoop *node);
extern bool ExecInitNestLoop(NestLoop *node, EState *estate, Plan *parent);
extern int	ExecCountSlotsNestLoop(NestLoop *node);
extern void ExecEndNestLoop(NestLoop *node);
extern void ExecReScanNestLoop(NestLoop *node, ExprContext *exprCtxt,
				   Plan *parent);

#endif	 /* NODENESTLOOP_H */
