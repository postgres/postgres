/*-------------------------------------------------------------------------
 *
 * conversioncmds.h
 *	  prototypes for conversioncmds.c.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: conversioncmds.h,v 1.1 2002/07/11 07:39:27 ishii Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef CONVERSIONCMDS_H
#define CONVERSIONCMDS_H

#include "nodes/parsenodes.h"

extern void CreateConversionCommand(CreateConversionStmt *parsetree);
extern void DropConversionCommand(List *conversion_name);

#endif  /* CONVERSIONCMDS_H */
