/*-------------------------------------------------------------------------
 *
 * creatinh.h--
 *	  prototypes for creatinh.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: creatinh.h,v 1.4 1997/09/08 21:51:35 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CREATINH_H
#define CREATINH_H

extern void DefineRelation(CreateStmt *stmt);
extern void RemoveRelation(char *name);
extern char *MakeArchiveName(Oid relid);

#endif							/* CREATINH_H */
