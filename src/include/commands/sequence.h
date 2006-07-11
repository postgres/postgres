/*-------------------------------------------------------------------------
 *
 * sequence.h
 *	  prototypes for sequence.c.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/commands/sequence.h,v 1.37 2006/07/11 13:54:24 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SEQUENCE_H
#define SEQUENCE_H

#include "nodes/parsenodes.h"
#include "storage/relfilenode.h"
#include "access/xlog.h"
#include "fmgr.h"


/*
 * On a machine with no 64-bit-int C datatype, sizeof(int64) will not be 8,
 * but we need this struct type to line up with the way that a sequence
 * table is defined --- and pg_type will say that int8 is 8 bytes anyway.
 * So, we need padding.  Ugly but necessary.
 */
typedef struct FormData_pg_sequence
{
	NameData	sequence_name;
#ifndef INT64_IS_BUSTED
	int64		last_value;
	int64		increment_by;
	int64		max_value;
	int64		min_value;
	int64		cache_value;
	int64		log_cnt;
#else
	int32		last_value;
	int32		pad1;
	int32		increment_by;
	int32		pad2;
	int32		max_value;
	int32		pad3;
	int32		min_value;
	int32		pad4;
	int32		cache_value;
	int32		pad5;
	int32		log_cnt;
	int32		pad6;
#endif
	bool		is_cycled;
	bool		is_called;
} FormData_pg_sequence;

typedef FormData_pg_sequence *Form_pg_sequence;

/*
 * Columns of a sequence relation
 */

#define SEQ_COL_NAME			1
#define SEQ_COL_LASTVAL			2
#define SEQ_COL_INCBY			3
#define SEQ_COL_MAXVALUE		4
#define SEQ_COL_MINVALUE		5
#define SEQ_COL_CACHE			6
#define SEQ_COL_LOG				7
#define SEQ_COL_CYCLE			8
#define SEQ_COL_CALLED			9

#define SEQ_COL_FIRSTCOL		SEQ_COL_NAME
#define SEQ_COL_LASTCOL			SEQ_COL_CALLED

/* XLOG stuff */
#define XLOG_SEQ_LOG			0x00

typedef struct xl_seq_rec
{
	RelFileNode node;
	/* SEQUENCE TUPLE DATA FOLLOWS AT THE END */
} xl_seq_rec;

extern Datum nextval(PG_FUNCTION_ARGS);
extern Datum nextval_oid(PG_FUNCTION_ARGS);
extern Datum currval_oid(PG_FUNCTION_ARGS);
extern Datum setval_oid(PG_FUNCTION_ARGS);
extern Datum setval3_oid(PG_FUNCTION_ARGS);
extern Datum lastval(PG_FUNCTION_ARGS);

extern void DefineSequence(CreateSeqStmt *stmt);
extern void AlterSequence(AlterSeqStmt *stmt);

extern void seq_redo(XLogRecPtr lsn, XLogRecord *rptr);
extern void seq_desc(StringInfo buf, uint8 xl_info, char *rec);

/* Set the upper and lower bounds of a sequence */
#ifndef INT64_IS_BUSTED
#define SEQ_MAXVALUE	INT64CONST(0x7FFFFFFFFFFFFFFF)
#else							/* INT64_IS_BUSTED */
#define SEQ_MAXVALUE	((int64) 0x7FFFFFFF)
#endif   /* INT64_IS_BUSTED */

#define SEQ_MINVALUE	(-SEQ_MAXVALUE)

#endif   /* SEQUENCE_H */
