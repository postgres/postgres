/*-------------------------------------------------------------------------
 *
 * creatinh.h--
 *	  prototypes for creatinh.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: creatinh.h,v 1.5 1997/11/21 18:12:16 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CREATINH_H
#define CREATINH_H

extern void DefineRelation(CreateStmt *stmt);
extern void RemoveRelation(char *name);

#endif							/* CREATINH_H */
