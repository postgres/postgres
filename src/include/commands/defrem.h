/*-------------------------------------------------------------------------
 *
 * defrem.h--
 *    POSTGRES define and remove utility definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: defrem.h,v 1.2 1996/10/31 09:48:22 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	DEFREM_H
#define DEFREM_H

#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "nodes/parsenodes.h"
#include "tcop/dest.h"

/*
 * prototypes in defind.c
 */
extern void DefineIndex(char *heapRelationName, 
			char *indexRelationName,
			char *accessMethodName,
			List *attributeList,
			List *parameterList, Expr *predicate,
			List *rangetable);
extern void ExtendIndex(char *indexRelationName,
			Expr *predicate,
			List *rangetable);
extern void RemoveIndex(char *name);

/*
 * prototypes in define.c
 */
extern void DefineFunction(ProcedureStmt *nameargsexe, CommandDest dest);
extern void DefineOperator(char *name, List *parameters);
extern void DefineAggregate(char *name, List *parameters);
extern void DefineType(char *name, List *parameters);

/*
 * prototypes in remove.c
 */
extern void RemoveFunction(char *functionName, int nargs, List *argNameList);
extern void RemoveOperator(char *operatorName, 
			   char *typeName1, char *typeName2);
extern void RemoveType(char *typeName);
extern void RemoveAggregate(char *aggName);

#endif	/* DEFREM_H */
