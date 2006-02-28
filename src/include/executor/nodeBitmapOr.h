/*-------------------------------------------------------------------------
 *
 * nodeBitmapOr.h
 *
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeBitmapOr.h,v 1.2 2006/02/28 04:10:28 tgl Exp $
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
