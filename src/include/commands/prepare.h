/*-------------------------------------------------------------------------
 *
 * prepare.h
 *	  PREPARE, EXECUTE and DEALLOCATE command prototypes
 *
 *
 * Copyright (c) 2002, PostgreSQL Global Development Group
 *
 * $Id: prepare.h,v 1.1 2002/08/27 04:55:11 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef PREPARE_H
#define PREPARE_H

#include "nodes/parsenodes.h"
#include "tcop/dest.h"


extern void PrepareQuery(PrepareStmt *stmt);

extern void ExecuteQuery(ExecuteStmt *stmt, CommandDest outputDest);

extern void DeallocateQuery(DeallocateStmt *stmt);

extern List *FetchQueryParams(const char *plan_name);

#endif /* PREPARE_H */
