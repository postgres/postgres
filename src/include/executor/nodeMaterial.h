/*-------------------------------------------------------------------------
 *
 * nodeMaterial.h
 *
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeMaterial.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEMATERIAL_H
#define NODEMATERIAL_H

#include "nodes/execnodes.h"

extern MaterialState *ExecInitMaterial(Material *node, EState *estate, int eflags);
extern void ExecEndMaterial(MaterialState *node);
extern void ExecMaterialMarkPos(MaterialState *node);
extern void ExecMaterialRestrPos(MaterialState *node);
extern void ExecReScanMaterial(MaterialState *node);

#endif							/* NODEMATERIAL_H */
