/*-------------------------------------------------------------------------
 *
 * creatinh.h--
 *    prototypes for creatinh.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: creatinh.h,v 1.1.1.1 1996/07/09 06:21:20 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CREATINH_H
#define CREATINH_H

extern void DefineRelation(CreateStmt *stmt);
extern void RemoveRelation(char *name);
extern char* MakeArchiveName(Oid relid);

#endif	/* CREATINH_H */
