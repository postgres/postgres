/*-------------------------------------------------------------------------
 *
 * creatinh.h--
 *	  prototypes for creatinh.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: creatinh.h,v 1.2 1997/09/07 04:57:24 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CREATINH_H
#define CREATINH_H

extern void		DefineRelation(CreateStmt * stmt);
extern void		RemoveRelation(char *name);
extern char    *MakeArchiveName(Oid relid);

#endif							/* CREATINH_H */
