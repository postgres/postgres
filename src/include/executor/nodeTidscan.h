/*-------------------------------------------------------------------------
 *
 * nodeTidscan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeTidscan.h,v 1.3 2000/04/12 17:16:33 momjian Exp $
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
extern int	ExecCountSlotsTidScan(TidScan *node);
extern void ExecTidReScan(TidScan *node, ExprContext *exprCtxt, Plan *parent);

#endif	 /* NODETIDSCAN_H */
