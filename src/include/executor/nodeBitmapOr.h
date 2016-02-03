/*-------------------------------------------------------------------------
 *
 * nodeBitmapOr.h
 *
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeBitmapOr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEBITMAPOR_H
#define NODEBITMAPOR_H

#include "nodes/execnodes.h"

extern BitmapOrState *ExecInitBitmapOr(BitmapOr *node, EState *estate, int eflags);
extern Node *MultiExecBitmapOr(BitmapOrState *node);
extern void ExecEndBitmapOr(BitmapOrState *node);
extern void ExecReScanBitmapOr(BitmapOrState *node);

#endif   /* NODEBITMAPOR_H */
