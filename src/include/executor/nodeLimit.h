/*-------------------------------------------------------------------------
 *
 * nodeLimit.h
 *
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeLimit.h,v 1.5 2001/11/05 17:46:33 momjian Exp $
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

#endif   /* NODELIMIT_H */
