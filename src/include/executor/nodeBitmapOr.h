/*-------------------------------------------------------------------------
 *
 * nodeBitmapOr.h
 *
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeBitmapOr.h,v 1.7 2009/09/27 21:10:53 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEBITMAPOR_H
#define NODEBITMAPOR_H

#include "nodes/execnodes.h"

extern BitmapOrState *ExecInitBitmapOr(BitmapOr *node, EState *estate, int eflags);
extern Node *MultiExecBitmapOr(BitmapOrState *node);
extern void ExecEndBitmapOr(BitmapOrState *node);
extern void ExecReScanBitmapOr(BitmapOrState *node, ExprContext *exprCtxt);

#endif   /* NODEBITMAPOR_H */
