/*-------------------------------------------------------------------------
 *
 * creatinh.h
 *	  prototypes for creatinh.c.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: creatinh.h,v 1.19 2002/03/22 02:56:36 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CREATINH_H
#define CREATINH_H

#include "nodes/parsenodes.h"

extern Oid	DefineRelation(CreateStmt *stmt, char relkind);
extern void RemoveRelation(const char *name);
extern void TruncateRelation(const char *name);

#endif   /* CREATINH_H */
