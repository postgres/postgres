/*-------------------------------------------------------------------------
 *
 * nodeSubqueryscan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeSubqueryscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODESUBQUERYSCAN_H
#define NODESUBQUERYSCAN_H

#include "nodes/execnodes.h"

extern SubqueryScanState *ExecInitSubqueryScan(SubqueryScan *node, EState *estate, int eflags);
extern void ExecEndSubqueryScan(SubqueryScanState *node);
extern void ExecReScanSubqueryScan(SubqueryScanState *node);

#endif							/* NODESUBQUERYSCAN_H */
