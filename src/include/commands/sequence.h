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

#include "fmgr.h"
#include "nodes/parsenodes.h"

/*
 * Columns of a sequence relation
 */

#define SEQ_COL_NAME			1
#define SEQ_COL_LASTVAL			2
#define SEQ_COL_INCBY			3
#define SEQ_COL_MAXVALUE		4
#define SEQ_COL_MINVALUE		5
#define SEQ_COL_CACHE			6
#define SEQ_COL_CYCLE			7
#define SEQ_COL_CALLED			8

#define SEQ_COL_FIRSTCOL		SEQ_COL_NAME
#define SEQ_COL_LASTCOL			SEQ_COL_CALLED

extern Datum nextval(PG_FUNCTION_ARGS);
extern Datum currval(PG_FUNCTION_ARGS);
extern Datum setval(PG_FUNCTION_ARGS);

extern void DefineSequence(CreateSeqStmt *stmt);
extern void CloseSequences(void);

#endif	 /* SEQUENCE_H */
