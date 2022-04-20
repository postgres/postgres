/*-------------------------------------------------------------------------
 *
 * createas.h
 *	  prototypes for createas.c.
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/createas.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CREATEAS_H
#define CREATEAS_H

#include "catalog/objectaddress.h"
#include "nodes/params.h"
#include "parser/parse_node.h"
#include "tcop/dest.h"
#include "utils/queryenvironment.h"


extern ObjectAddress ExecCreateTableAs(ParseState *pstate, CreateTableAsStmt *stmt,
									   ParamListInfo params, QueryEnvironment *queryEnv,
									   QueryCompletion *qc);

extern int	GetIntoRelEFlags(IntoClause *intoClause);

extern DestReceiver *CreateIntoRelDestReceiver(IntoClause *intoClause);

extern bool CreateTableAsRelExists(CreateTableAsStmt *ctas);

#endif							/* CREATEAS_H */
