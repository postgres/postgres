/*-------------------------------------------------------------------------
 *
 * nodeSetOp.h
 *
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeSetOp.h,v 1.19 2010/07/12 17:01:06 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODESETOP_H
#define NODESETOP_H

#include "nodes/execnodes.h"

extern SetOpState *ExecInitSetOp(SetOp *node, EState *estate, int eflags);
extern TupleTableSlot *ExecSetOp(SetOpState *node);
extern void ExecEndSetOp(SetOpState *node);
extern void ExecReScanSetOp(SetOpState *node);

#endif   /* NODESETOP_H */
