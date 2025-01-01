/*-------------------------------------------------------------------------
 *
 * nodeValuesscan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
extern void ExecReScanValuesScan(ValuesScanState *node);

#endif							/* NODEVALUESSCAN_H */
