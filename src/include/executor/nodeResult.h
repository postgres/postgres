/*-------------------------------------------------------------------------
 *
 * nodeResult.h
 *
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeResult.h,v 1.29 2010/07/12 17:01:06 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODERESULT_H
#define NODERESULT_H

#include "nodes/execnodes.h"

extern ResultState *ExecInitResult(Result *node, EState *estate, int eflags);
extern TupleTableSlot *ExecResult(ResultState *node);
extern void ExecEndResult(ResultState *node);
extern void ExecResultMarkPos(ResultState *node);
extern void ExecResultRestrPos(ResultState *node);
extern void ExecReScanResult(ResultState *node);

#endif   /* NODERESULT_H */
