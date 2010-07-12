/*-------------------------------------------------------------------------
 *
 * nodeAppend.h
 *
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeAppend.h,v 1.31 2010/07/12 17:01:06 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEAPPEND_H
#define NODEAPPEND_H

#include "nodes/execnodes.h"

extern AppendState *ExecInitAppend(Append *node, EState *estate, int eflags);
extern TupleTableSlot *ExecAppend(AppendState *node);
extern void ExecEndAppend(AppendState *node);
extern void ExecReScanAppend(AppendState *node);

#endif   /* NODEAPPEND_H */
