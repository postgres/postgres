/*-------------------------------------------------------------------------
 *
 * nodeValuesscan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeValuesscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEVALUESSCAN_H
#define NODEVALUESSCAN_H

#include "nodes/execnodes.h"

extern ValuesScanState *ExecInitValuesScan(ValuesScan *node, EState *estate, int eflags);
extern TupleTableSlot *ExecValuesScan(ValuesScanState *node);
extern void ExecEndValuesScan(ValuesScanState *node);
extern void ExecValuesMarkPos(ValuesScanState *node);
extern void ExecValuesRestrPos(ValuesScanState *node);
extern void ExecReScanValuesScan(ValuesScanState *node);

#endif   /* NODEVALUESSCAN_H */
