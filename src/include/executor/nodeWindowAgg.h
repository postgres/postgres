/*-------------------------------------------------------------------------
 *
 * nodeWindowAgg.h
 *	  prototypes for nodeWindowAgg.c
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeWindowAgg.h,v 1.5 2010/07/12 17:01:06 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEWINDOWAGG_H
#define NODEWINDOWAGG_H

#include "nodes/execnodes.h"

extern WindowAggState *ExecInitWindowAgg(WindowAgg *node, EState *estate, int eflags);
extern TupleTableSlot *ExecWindowAgg(WindowAggState *node);
extern void ExecEndWindowAgg(WindowAggState *node);
extern void ExecReScanWindowAgg(WindowAggState *node);

#endif   /* NODEWINDOWAGG_H */
