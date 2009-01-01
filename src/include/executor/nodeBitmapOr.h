/*-------------------------------------------------------------------------
 *
 * nodeBitmapOr.h
 *
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeBitmapOr.h,v 1.6 2009/01/01 17:23:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEBITMAPOR_H
#define NODEBITMAPOR_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsBitmapOr(BitmapOr *node);
extern BitmapOrState *ExecInitBitmapOr(BitmapOr *node, EState *estate, int eflags);
extern Node *MultiExecBitmapOr(BitmapOrState *node);
extern void ExecEndBitmapOr(BitmapOrState *node);
extern void ExecReScanBitmapOr(BitmapOrState *node, ExprContext *exprCtxt);

#endif   /* NODEBITMAPOR_H */
