/*-------------------------------------------------------------------------
 *
 * pg_operator_fn.h
*	 prototypes for functions in catalog/pg_operator.c
 *
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_operator_fn.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_OPERATOR_FN_H
#define PG_OPERATOR_FN_H

#include "catalog/objectaddress.h"
#include "nodes/pg_list.h"

extern ObjectAddress OperatorCreate(const char *operatorName,
			   Oid operatorNamespace,
			   Oid leftTypeId,
			   Oid rightTypeId,
			   Oid procedureId,
			   List *commutatorName,
			   List *negatorName,
			   Oid restrictionId,
			   Oid joinId,
			   bool canMerge,
			   bool canHash);

#endif   /* PG_OPERATOR_FN_H */
