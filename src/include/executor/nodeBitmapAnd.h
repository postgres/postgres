/*-------------------------------------------------------------------------
 *
 * nodeBitmapAnd.h
 *
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeBitmapAnd.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEBITMAPAND_H
#define NODEBITMAPAND_H

#include "nodes/execnodes.h"

extern BitmapAndState *ExecInitBitmapAnd(BitmapAnd *node, EState *estate, int eflags);
extern Node *MultiExecBitmapAnd(BitmapAndState *node);
extern void ExecEndBitmapAnd(BitmapAndState *node);
extern void ExecReScanBitmapAnd(BitmapAndState *node);

#endif   /* NODEBITMAPAND_H */
