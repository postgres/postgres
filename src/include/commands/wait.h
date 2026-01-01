/*-------------------------------------------------------------------------
 *
 * wait.h
 *	  prototypes for commands/wait.c
 *
 * Portions Copyright (c) 2025-2026, PostgreSQL Global Development Group
 *
 * src/include/commands/wait.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WAIT_H
#define WAIT_H

#include "nodes/parsenodes.h"
#include "parser/parse_node.h"
#include "tcop/dest.h"

extern void ExecWaitStmt(ParseState *pstate, WaitStmt *stmt, DestReceiver *dest);
extern TupleDesc WaitStmtResultDesc(WaitStmt *stmt);

#endif							/* WAIT_H */
