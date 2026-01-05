/*-------------------------------------------------------------------------
 *
 * sequence.h
 *	  prototypes for sequence.c.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/sequence.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SEQUENCE_H
#define SEQUENCE_H

#include "catalog/objectaddress.h"
#include "fmgr.h"
#include "nodes/parsenodes.h"
#include "parser/parse_node.h"

typedef struct FormData_pg_sequence_data
{
	int64		last_value;
	int64		log_cnt;
	bool		is_called;
} FormData_pg_sequence_data;

typedef FormData_pg_sequence_data *Form_pg_sequence_data;

/*
 * Columns of a sequence relation
 */

#define SEQ_COL_LASTVAL			1
#define SEQ_COL_LOG				2
#define SEQ_COL_CALLED			3

#define SEQ_COL_FIRSTCOL		SEQ_COL_LASTVAL
#define SEQ_COL_LASTCOL			SEQ_COL_CALLED

extern int64 nextval_internal(Oid relid, bool check_permissions);
extern Datum nextval(PG_FUNCTION_ARGS);
extern List *sequence_options(Oid relid);

extern ObjectAddress DefineSequence(ParseState *pstate, CreateSeqStmt *seq);
extern ObjectAddress AlterSequence(ParseState *pstate, AlterSeqStmt *stmt);
extern void SequenceChangePersistence(Oid relid, char newrelpersistence);
extern void DeleteSequenceTuple(Oid relid);
extern void ResetSequence(Oid seq_relid);
extern void SetSequence(Oid relid, int64 next, bool iscalled);
extern void ResetSequenceCaches(void);

#endif							/* SEQUENCE_H */
