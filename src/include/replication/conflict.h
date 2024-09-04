/*-------------------------------------------------------------------------
 * conflict.h
 *	   Exports for conflicts logging.
 *
 * Copyright (c) 2024, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#ifndef CONFLICT_H
#define CONFLICT_H

#include "nodes/execnodes.h"
#include "utils/timestamp.h"

/*
 * Conflict types that could occur while applying remote changes.
 *
 * This enum is used in statistics collection (see
 * PgStat_StatSubEntry::conflict_count and
 * PgStat_BackendSubEntry::conflict_count) as well, therefore, when adding new
 * values or reordering existing ones, ensure to review and potentially adjust
 * the corresponding statistics collection codes.
 */
typedef enum
{
	/* The row to be inserted violates unique constraint */
	CT_INSERT_EXISTS,

	/* The row to be updated was modified by a different origin */
	CT_UPDATE_ORIGIN_DIFFERS,

	/* The updated row value violates unique constraint */
	CT_UPDATE_EXISTS,

	/* The row to be updated is missing */
	CT_UPDATE_MISSING,

	/* The row to be deleted was modified by a different origin */
	CT_DELETE_ORIGIN_DIFFERS,

	/* The row to be deleted is missing */
	CT_DELETE_MISSING,

	/*
	 * Other conflicts, such as exclusion constraint violations, involve more
	 * complex rules than simple equality checks. These conflicts are left for
	 * future improvements.
	 */
} ConflictType;

#define CONFLICT_NUM_TYPES (CT_DELETE_MISSING + 1)

extern bool GetTupleTransactionInfo(TupleTableSlot *localslot,
									TransactionId *xmin,
									RepOriginId *localorigin,
									TimestampTz *localts);
extern void ReportApplyConflict(EState *estate, ResultRelInfo *relinfo,
								int elevel, ConflictType type,
								TupleTableSlot *searchslot,
								TupleTableSlot *localslot,
								TupleTableSlot *remoteslot,
								Oid indexoid, TransactionId localxmin,
								RepOriginId localorigin, TimestampTz localts);
extern void InitConflictIndexes(ResultRelInfo *relInfo);

#endif
