/*-------------------------------------------------------------------------
 *
 * nodeMaterial.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeMaterial.h,v 1.6 1998/02/13 03:43:37 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEMATERIAL_H
#define NODEMATERIAL_H

#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"

extern TupleTableSlot *ExecMaterial(Material *node);
extern bool ExecInitMaterial(Material *node, EState *estate, Plan *parent);
extern int	ExecCountSlotsMaterial(Material *node);
extern void ExecEndMaterial(Material *node);
extern void ExecMaterialReScan(Material *node, ExprContext *exprCtxt, Plan *parent);
#if 0
extern List ExecMaterialMarkPos(Material *node);
extern void ExecMaterialRestrPos(Material *node);
#endif
#endif							/* NODEMATERIAL_H */
