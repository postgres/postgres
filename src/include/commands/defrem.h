/*-------------------------------------------------------------------------
 *
 * defrem.h
 *	  POSTGRES define and remove utility definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: defrem.h,v 1.34 2002/04/09 20:35:54 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef DEFREM_H
#define DEFREM_H

#include "nodes/parsenodes.h"

/*
 * prototypes in indexcmds.c
 */
extern void DefineIndex(RangeVar *heapRelation,
			char *indexRelationName,
			char *accessMethodName,
			List *attributeList,
			bool unique,
			bool primary,
			Expr *predicate,
			List *rangetable);
extern void RemoveIndex(RangeVar *relation);
extern void ReindexIndex(RangeVar *indexRelation, bool force);
extern void ReindexTable(RangeVar *relation, bool force);
extern void ReindexDatabase(const char *databaseName, bool force, bool all);

/*
 * prototypes in define.c
 */
extern void CreateFunction(ProcedureStmt *stmt);
extern void DefineOperator(List *names, List *parameters);
extern void DefineAggregate(List *names, List *parameters);
extern void DefineType(List *names, List *parameters);
extern void DefineDomain(CreateDomainStmt *stmt);

/*
 * prototypes in remove.c
 */
extern void RemoveDomain(List *names, int behavior);
extern void RemoveFunction(List *functionName, List *argTypes);
extern void RemoveOperator(char *operatorName,
			   TypeName *typeName1, TypeName *typeName2);
extern void RemoveType(List *names);
extern void RemoveAggregate(List *aggName, TypeName *aggType);

#endif   /* DEFREM_H */
