/*-------------------------------------------------------------------------
 *
 * creatinh.h
 *	  prototypes for creatinh.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: creatinh.h,v 1.11 1999/10/15 01:49:46 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CREATINH_H
#define CREATINH_H

#include "nodes/parsenodes.h"

extern void DefineRelation(CreateStmt *stmt, char relkind);
extern void RemoveRelation(char *name);
extern void TruncateRelation(char *name);
extern void CommentRelation(char *name, char *attr, char *comment);

#endif	 /* CREATINH_H */
