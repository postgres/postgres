/*-------------------------------------------------------------------------
 *
 * defrem.h
 *	  POSTGRES define and remove utility definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: defrem.h,v 1.32 2002/03/26 19:16:47 tgl Exp $
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
extern void DefineOperator(char *name, List *parameters);
extern void DefineAggregate(char *name, List *parameters);
extern void DefineType(char *name, List *parameters);
extern void DefineDomain(CreateDomainStmt *stmt);

/*
 * prototypes in remove.c
 */
extern void RemoveDomain(char *domainName, int behavior);
extern void RemoveFunction(char *functionName, List *argTypes);
extern void RemoveOperator(char *operatorName,
			   char *typeName1, char *typeName2);
extern void RemoveType(char *typeName);
extern void RemoveAggregate(char *aggName, char *aggType);

#endif   /* DEFREM_H */
