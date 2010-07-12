/*-------------------------------------------------------------------------
 *
 * nodeTidscan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeTidscan.h,v 1.23 2010/07/12 17:01:06 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODETIDSCAN_H
#define NODETIDSCAN_H

#include "nodes/execnodes.h"

extern TidScanState *ExecInitTidScan(TidScan *node, EState *estate, int eflags);
extern TupleTableSlot *ExecTidScan(TidScanState *node);
extern void ExecEndTidScan(TidScanState *node);
extern void ExecTidMarkPos(TidScanState *node);
extern void ExecTidRestrPos(TidScanState *node);
extern void ExecReScanTidScan(TidScanState *node);

#endif   /* NODETIDSCAN_H */
