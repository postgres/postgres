/*-------------------------------------------------------------------------
 *
 * defrem.h--
 *	  POSTGRES define and remove utility definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: defrem.h,v 1.10 1997/09/08 21:51:36 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef DEFREM_H
#define DEFREM_H

#include <tcop/dest.h>
#include <nodes/parsenodes.h>

/*
 * prototypes in defind.c
 */
extern void
DefineIndex(char *heapRelationName,
			char *indexRelationName,
			char *accessMethodName,
			List *attributeList,
			List *parameterList,
			bool unique,
			Expr *predicate,
			List *rangetable);
extern void
ExtendIndex(char *indexRelationName,
			Expr *predicate,
			List *rangetable);
extern void RemoveIndex(char *name);

/*
 * prototypes in define.c
 */
extern void CreateFunction(ProcedureStmt *stmt, CommandDest dest);
extern void DefineOperator(char *name, List *parameters);
extern void DefineAggregate(char *name, List *parameters);
extern void DefineType(char *name, List *parameters);
extern void CreateFunction(ProcedureStmt *stmt, CommandDest dest);

/*
 * prototypes in remove.c
 */
extern void RemoveFunction(char *functionName, int nargs, List *argNameList);
extern void
RemoveOperator(char *operatorName,
			   char *typeName1, char *typeName2);
extern void RemoveType(char *typeName);
extern void RemoveAggregate(char *aggName, char *aggType);

#endif							/* DEFREM_H */
