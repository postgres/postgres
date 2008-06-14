/*-------------------------------------------------------------------------
 *
 * conversioncmds.h
 *	  prototypes for conversioncmds.c.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/commands/conversioncmds.h,v 1.17 2008/06/14 18:04:34 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef CONVERSIONCMDS_H
#define CONVERSIONCMDS_H

#include "nodes/parsenodes.h"

extern void CreateConversionCommand(CreateConversionStmt *parsetree);
extern void DropConversionsCommand(DropStmt *drop);
extern void RenameConversion(List *name, const char *newname);
extern void AlterConversionOwner(List *name, Oid newOwnerId);
extern void AlterConversionOwner_oid(Oid conversionOid, Oid newOwnerId);

#endif   /* CONVERSIONCMDS_H */
