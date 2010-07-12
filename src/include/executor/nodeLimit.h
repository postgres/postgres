/*-------------------------------------------------------------------------
 *
 * nodeLimit.h
 *
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeLimit.h,v 1.19 2010/07/12 17:01:06 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODELIMIT_H
#define NODELIMIT_H

#include "nodes/execnodes.h"

extern LimitState *ExecInitLimit(Limit *node, EState *estate, int eflags);
extern TupleTableSlot *ExecLimit(LimitState *node);
extern void ExecEndLimit(LimitState *node);
extern void ExecReScanLimit(LimitState *node);

#endif   /* NODELIMIT_H */
