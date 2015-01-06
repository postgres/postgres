/* ------------------------------------------------------------------------
 *
 * nodeCustom.h
 *
 * prototypes for CustomScan nodes
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * ------------------------------------------------------------------------
 */
#ifndef NODECUSTOM_H
#define NODECUSTOM_H

#include "nodes/execnodes.h"

/*
 * General executor code
 */
extern CustomScanState *ExecInitCustomScan(CustomScan *custom_scan,
				   EState *estate, int eflags);
extern TupleTableSlot *ExecCustomScan(CustomScanState *node);
extern void ExecEndCustomScan(CustomScanState *node);

extern void ExecReScanCustomScan(CustomScanState *node);
extern void ExecCustomMarkPos(CustomScanState *node);
extern void ExecCustomRestrPos(CustomScanState *node);

#endif   /* NODECUSTOM_H */
