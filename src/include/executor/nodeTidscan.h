/*-------------------------------------------------------------------------
 *
 * nodeTidscan.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeTidscan.h,v 1.1 1999/11/24 16:52:49 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODETIDSCAN_H
#define NODETIDSCAN_H

#include "nodes/plannodes.h"

extern TupleTableSlot *ExecTidScan(TidScan *node);
extern void ExecTidReScan(TidScan *node, ExprContext *exprCtxt, Plan *parent);
extern void ExecEndTidScan(TidScan *node);
extern void ExecTidMarkPos(TidScan *node);
extern void ExecTidRestrPos(TidScan *node);
extern bool ExecInitTidScan(TidScan *node, EState *estate, Plan *parent);
extern int  ExecCountSlotsTidScan(TidScan *node);
extern void ExecTidReScan(TidScan *node, ExprContext *exprCtxt, Plan *parent);

#endif	 /* NODETIDSCAN_H */
