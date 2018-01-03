/*-------------------------------------------------------------------------
 *
 * pg_aggregate_fn.h
 *	  prototypes for functions in catalog/pg_aggregate.c
 *
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_aggregate_fn.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_AGGREGATE_FN_H
#define PG_AGGREGATE_FN_H

#include "catalog/objectaddress.h"
#include "nodes/pg_list.h"

extern ObjectAddress AggregateCreate(const char *aggName,
				Oid aggNamespace,
				char aggKind,
				int numArgs,
				int numDirectArgs,
				oidvector *parameterTypes,
				Datum allParameterTypes,
				Datum parameterModes,
				Datum parameterNames,
				List *parameterDefaults,
				Oid variadicArgType,
				List *aggtransfnName,
				List *aggfinalfnName,
				List *aggcombinefnName,
				List *aggserialfnName,
				List *aggdeserialfnName,
				List *aggmtransfnName,
				List *aggminvtransfnName,
				List *aggmfinalfnName,
				bool finalfnExtraArgs,
				bool mfinalfnExtraArgs,
				char finalfnModify,
				char mfinalfnModify,
				List *aggsortopName,
				Oid aggTransType,
				int32 aggTransSpace,
				Oid aggmTransType,
				int32 aggmTransSpace,
				const char *agginitval,
				const char *aggminitval,
				char proparallel);

#endif							/* PG_AGGREGATE_FN_H */
