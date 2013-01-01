/*-------------------------------------------------------------------------
 *
 * nodeCtescan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeCtescan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODECTESCAN_H
#define NODECTESCAN_H

#include "nodes/execnodes.h"

extern CteScanState *ExecInitCteScan(CteScan *node, EState *estate, int eflags);
extern TupleTableSlot *ExecCteScan(CteScanState *node);
extern void ExecEndCteScan(CteScanState *node);
extern void ExecReScanCteScan(CteScanState *node);

#endif   /* NODECTESCAN_H */
