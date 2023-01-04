/*-------------------------------------------------------------------------
 *
 * nodeNamedtuplestorescan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeNamedtuplestorescan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODENAMEDTUPLESTORESCAN_H
#define NODENAMEDTUPLESTORESCAN_H

#include "nodes/execnodes.h"

extern NamedTuplestoreScanState *ExecInitNamedTuplestoreScan(NamedTuplestoreScan *node, EState *estate, int eflags);
extern void ExecEndNamedTuplestoreScan(NamedTuplestoreScanState *node);
extern void ExecReScanNamedTuplestoreScan(NamedTuplestoreScanState *node);

#endif							/* NODENAMEDTUPLESTORESCAN_H */
