/*-------------------------------------------------------------------------
 *
 * nodeTidrangescan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeTidrangescan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODETIDRANGESCAN_H
#define NODETIDRANGESCAN_H

#include "nodes/execnodes.h"

extern TidRangeScanState *ExecInitTidRangeScan(TidRangeScan *node,
											   EState *estate, int eflags);
extern void ExecEndTidRangeScan(TidRangeScanState *node);
extern void ExecReScanTidRangeScan(TidRangeScanState *node);

#endif							/* NODETIDRANGESCAN_H */
