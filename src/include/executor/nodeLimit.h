/*-------------------------------------------------------------------------
 *
 * nodeLimit.h
 *
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeLimit.h,v 1.1 2000/10/26 21:38:03 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODELIMIT_H
#define NODELIMIT_H

#include "nodes/plannodes.h"

extern TupleTableSlot *ExecLimit(Limit *node);
extern bool ExecInitLimit(Limit *node, EState *estate, Plan *parent);
extern int	ExecCountSlotsLimit(Limit *node);
extern void ExecEndLimit(Limit *node);
extern void ExecReScanLimit(Limit *node, ExprContext *exprCtxt, Plan *parent);

#endif	 /* NODELIMIT_H */
