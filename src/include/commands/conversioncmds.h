/*-------------------------------------------------------------------------
 *
 * conversioncmds.h
 *	  prototypes for conversioncmds.c.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/commands/conversioncmds.h,v 1.11 2005/11/19 17:39:45 adunstan Exp $
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

#endif   /* CONVERSIONCMDS_H */
