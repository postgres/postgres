/*-------------------------------------------------------------------------
 *
 * nodeTidscan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeTidscan.h,v 1.21 2009/09/27 21:10:53 tgl Exp $
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
extern void ExecTidReScan(TidScanState *node, ExprContext *exprCtxt);

#endif   /* NODETIDSCAN_H */
