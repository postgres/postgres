/*-------------------------------------------------------------------------
 *
 * nodeMaterial.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeMaterial.h,v 1.1.1.1 1996/07/09 06:21:27 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	NODEMATERIAL_H
#define	NODEMATERIAL_H

extern TupleTableSlot *ExecMaterial(Material *node);
extern bool ExecInitMaterial(Material *node, EState *estate, Plan *parent);
extern int ExecCountSlotsMaterial(Material *node);
extern void ExecEndMaterial(Material *node);
extern List ExecMaterialMarkPos(Material *node);
extern void ExecMaterialRestrPos(Material *node);

#endif	/* NODEMATERIAL_H */
