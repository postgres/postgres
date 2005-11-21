/*-------------------------------------------------------------------------
 *
 * conversioncmds.h
 *	  prototypes for conversioncmds.c.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/commands/conversioncmds.h,v 1.12 2005/11/21 12:49:32 alvherre Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef CONVERSIONCMDS_H
#define CONVERSIONCMDS_H

#include "nodes/parsenodes.h"

extern void CreateConversionCommand(CreateConversionStmt *parsetree);
extern void DropConversionCommand(List *conversion_name, 
								  DropBehavior behavior, bool missing_ok);
extern void RenameConversion(List *name, const char *newname);
extern void AlterConversionOwner(List *name, Oid newOwnerId);
extern void AlterConversionOwner_oid(Oid conversionOid, Oid newOwnerId);

#endif   /* CONVERSIONCMDS_H */
