/*-------------------------------------------------------------------------
 *
 * prepare.h
 *	  PREPARE, EXECUTE and DEALLOCATE command prototypes
 *
 *
 * Copyright (c) 2002, PostgreSQL Global Development Group
 *
 * $Id: prepare.h,v 1.3 2003/02/02 23:46:38 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PREPARE_H
#define PREPARE_H

#include "executor/executor.h"
#include "nodes/parsenodes.h"
#include "tcop/dest.h"


extern void PrepareQuery(PrepareStmt *stmt);
extern void ExecuteQuery(ExecuteStmt *stmt, CommandDest outputDest);
extern void DeallocateQuery(DeallocateStmt *stmt);
extern List *FetchQueryParams(const char *plan_name);
extern void ExplainExecuteQuery(ExplainStmt *stmt, TupOutputState *tstate);

#endif   /* PREPARE_H */
