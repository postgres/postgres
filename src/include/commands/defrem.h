/*-------------------------------------------------------------------------
 *
 * defrem.h
 *	  POSTGRES define and remove utility definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: defrem.h,v 1.36 2002/04/16 23:08:12 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef DEFREM_H
#define DEFREM_H

#include "nodes/parsenodes.h"

#define DEFAULT_TYPDELIM		','

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
 * DefineFoo and RemoveFoo are now both in foocmds.c
 */

extern void CreateFunction(ProcedureStmt *stmt);
extern void RemoveFunction(List *functionName, List *argTypes);

extern void DefineOperator(List *names, List *parameters);
extern void RemoveOperator(List *operatorName,
						   TypeName *typeName1, TypeName *typeName2);

extern void DefineAggregate(List *names, List *parameters);
extern void RemoveAggregate(List *aggName, TypeName *aggType);

extern void DefineType(List *names, List *parameters);
extern void RemoveType(List *names);
extern void DefineDomain(CreateDomainStmt *stmt);
extern void RemoveDomain(List *names, int behavior);


/* support routines in define.c */

extern void case_translate_language_name(const char *input, char *output);

extern char *defGetString(DefElem *def);
extern double defGetNumeric(DefElem *def);
extern List *defGetQualifiedName(DefElem *def);
extern TypeName *defGetTypeName(DefElem *def);
extern int	defGetTypeLength(DefElem *def);

#endif   /* DEFREM_H */
