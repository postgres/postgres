/*-------------------------------------------------------------------------
 *
 * creatinh.h--
 *    prototypes for creatinh.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: creatinh.h,v 1.1 1996/08/28 07:21:45 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CREATINH_H
#define CREATINH_H

extern void DefineRelation(CreateStmt *stmt);
extern void RemoveRelation(char *name);
extern char* MakeArchiveName(Oid relid);

#endif	/* CREATINH_H */
