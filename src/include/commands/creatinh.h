/*-------------------------------------------------------------------------
 *
 * creatinh.h--
 *	  prototypes for creatinh.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: creatinh.h,v 1.6 1997/11/26 01:12:12 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CREATINH_H
#define CREATINH_H

#include "nodes/parsenodes.h"

extern void DefineRelation(CreateStmt *stmt);
extern void RemoveRelation(char *name);

#endif							/* CREATINH_H */
