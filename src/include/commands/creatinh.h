/*-------------------------------------------------------------------------
 *
 * creatinh.h
 *	  prototypes for creatinh.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: creatinh.h,v 1.12 1999/10/26 03:12:38 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CREATINH_H
#define CREATINH_H

#include "nodes/parsenodes.h"

extern void DefineRelation(CreateStmt *stmt, char relkind);
extern void RemoveRelation(char *name);
extern void TruncateRelation(char *name);

#endif	 /* CREATINH_H */
