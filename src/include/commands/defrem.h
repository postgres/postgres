/*-------------------------------------------------------------------------
 *
 * defrem.h
 *	  POSTGRES define and remove utility definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: defrem.h,v 1.22 2001/01/24 19:43:23 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef DEFREM_H
#define DEFREM_H

#include "nodes/parsenodes.h"
#include "tcop/dest.h"

/*
 * prototypes in defind.c
 */
extern void DefineIndex(char *heapRelationName,
			char *indexRelationName,
			char *accessMethodName,
			List *attributeList,
			List *parameterList,
			bool unique,
			bool primary,
			Expr *predicate,
			List *rangetable);
extern void ExtendIndex(char *indexRelationName,
			Expr *predicate,
			List *rangetable);
extern void RemoveIndex(char *name);
extern void ReindexIndex(const char *indexRelationName, bool force);
extern void ReindexTable(const char *relationName, bool force);
extern void ReindexDatabase(const char *databaseName, bool force, bool all);

/*
 * prototypes in define.c
 */
extern void CreateFunction(ProcedureStmt *stmt, CommandDest dest);
extern void DefineOperator(char *name, List *parameters);
extern void DefineAggregate(char *name, List *parameters);
extern void DefineType(char *name, List *parameters);

/*
 * prototypes in remove.c
 */
extern void RemoveFunction(char *functionName, List *argTypes);
extern void RemoveOperator(char *operatorName,
			   char *typeName1, char *typeName2);
extern void RemoveType(char *typeName);
extern void RemoveAggregate(char *aggName, char *aggType);

#endif	 /* DEFREM_H */
