/*-------------------------------------------------------------------------
 *
 * nodeTee.h--
 *	  support functions for a Tee executor node
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeTee.h,v 1.6 1998/09/01 04:36:09 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef NODETEE_H
#define NODETEE_H

#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"

extern TupleTableSlot *ExecTee(Tee *node, Plan *parent);
extern bool ExecInitTee(Tee *node, EState *estate, Plan *parent);
extern void ExecTeeReScan(Tee *node, ExprContext *exprCtxt, Plan *parent);
extern void ExecEndTee(Tee *node, Plan *parent);
extern int	ExecCountSlotsTee(Tee *node);

#endif	 /* NODETEE_H */
