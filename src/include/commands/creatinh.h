/*-------------------------------------------------------------------------
 *
 * creatinh.h--
 *	  prototypes for creatinh.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: creatinh.h,v 1.8 1998/09/01 04:35:27 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CREATINH_H
#define CREATINH_H

#include "nodes/parsenodes.h"

extern void DefineRelation(CreateStmt *stmt, char relkind);
extern void RemoveRelation(char *name);

#endif	 /* CREATINH_H */
