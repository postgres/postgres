/*-------------------------------------------------------------------------
 *
 * sequence.h
 *	  prototypes for sequence.c.
 *
 *
 *-------------------------------------------------------------------------
 */
#ifndef SEQUENCE_H
#define SEQUENCE_H

#include "nodes/parsenodes.h"
#include "access/xlog.h"

typedef struct FormData_pg_sequence
{
	NameData	sequence_name;
	int4		last_value;
	int4		increment_by;
	int4		max_value;
	int4		min_value;
	int4		cache_value;
	int4		log_cnt;
	char		is_cycled;
	char		is_called;
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
#define	SEQ_COL_LOG				7
#define SEQ_COL_CYCLE			8
#define SEQ_COL_CALLED			9

#define SEQ_COL_FIRSTCOL		SEQ_COL_NAME
#define SEQ_COL_LASTCOL			SEQ_COL_CALLED

/* XLOG stuff */
#define	XLOG_SEQ_LOG			0x00

typedef struct xl_seq_rec
{
	RelFileNode				node;
	/* SEQUENCE TUPLE DATA FOLLOWS AT THE END */
} xl_seq_rec;

extern Datum nextval(PG_FUNCTION_ARGS);
extern Datum currval(PG_FUNCTION_ARGS);
extern Datum setval(PG_FUNCTION_ARGS);
extern Datum setval_and_iscalled(PG_FUNCTION_ARGS);

extern void DefineSequence(CreateSeqStmt *stmt);
extern void CloseSequences(void);

extern void seq_redo(XLogRecPtr lsn, XLogRecord *rptr);
extern void seq_undo(XLogRecPtr lsn, XLogRecord *rptr);
extern void seq_desc(char *buf, uint8 xl_info, char* rec);

#endif	 /* SEQUENCE_H */
