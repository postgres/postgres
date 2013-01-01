/*-------------------------------------------------------------------------
 *
 * nodeForeignscan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeForeignscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEFOREIGNSCAN_H
#define NODEFOREIGNSCAN_H

#include "nodes/execnodes.h"

extern ForeignScanState *ExecInitForeignScan(ForeignScan *node, EState *estate, int eflags);
extern TupleTableSlot *ExecForeignScan(ForeignScanState *node);
extern void ExecEndForeignScan(ForeignScanState *node);
extern void ExecReScanForeignScan(ForeignScanState *node);

#endif   /* NODEFOREIGNSCAN_H */
