/*-------------------------------------------------------------------------
 *
 * schemacmds.h
 *	  prototypes for schemacmds.c.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: schemacmds.h,v 1.3 2002/09/04 20:31:42 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef SCHEMACMDS_H
#define SCHEMACMDS_H

#include "nodes/parsenodes.h"

extern void CreateSchemaCommand(CreateSchemaStmt *parsetree);

extern void RemoveSchema(List *names, DropBehavior behavior);
extern void RemoveSchemaById(Oid schemaOid);

#endif   /* SCHEMACMDS_H */
