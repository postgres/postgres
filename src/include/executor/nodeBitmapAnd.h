/*-------------------------------------------------------------------------
 *
 * nodeBitmapAnd.h
 *
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeBitmapAnd.h,v 1.6 2009/01/01 17:23:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEBITMAPAND_H
#define NODEBITMAPAND_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsBitmapAnd(BitmapAnd *node);
extern BitmapAndState *ExecInitBitmapAnd(BitmapAnd *node, EState *estate, int eflags);
extern Node *MultiExecBitmapAnd(BitmapAndState *node);
extern void ExecEndBitmapAnd(BitmapAndState *node);
extern void ExecReScanBitmapAnd(BitmapAndState *node, ExprContext *exprCtxt);

#endif   /* NODEBITMAPAND_H */
