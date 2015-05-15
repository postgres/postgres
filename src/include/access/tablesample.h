/*-------------------------------------------------------------------------
 *
 * tablesample.h
 *        Public header file for TABLESAMPLE clause interface
 *
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/tablesample.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TABLESAMPLE_H
#define TABLESAMPLE_H

#include "access/relscan.h"
#include "executor/executor.h"

typedef struct TableSampleDesc {
	HeapScanDesc	heapScan;
	TupleDesc		tupDesc;	/* Mostly useful for tsmexaminetuple */

	void		   *tsmdata;	/* private method data */

	/* These point to he function of the TABLESAMPLE Method. */
	FmgrInfo		tsminit;
	FmgrInfo		tsmnextblock;
	FmgrInfo		tsmnexttuple;
	FmgrInfo		tsmexaminetuple;
	FmgrInfo		tsmreset;
	FmgrInfo		tsmend;
} TableSampleDesc;


extern TableSampleDesc *tablesample_init(SampleScanState *scanstate,
										 TableSampleClause *tablesample);
extern HeapTuple tablesample_getnext(TableSampleDesc *desc);
extern void tablesample_reset(TableSampleDesc *desc);
extern void tablesample_end(TableSampleDesc *desc);
extern HeapTuple tablesample_source_getnext(TableSampleDesc *desc);
extern HeapTuple tablesample_source_gettup(TableSampleDesc *desc, ItemPointer tid,
										   bool *visible);

extern Datum tsm_system_init(PG_FUNCTION_ARGS);
extern Datum tsm_system_nextblock(PG_FUNCTION_ARGS);
extern Datum tsm_system_nexttuple(PG_FUNCTION_ARGS);
extern Datum tsm_system_end(PG_FUNCTION_ARGS);
extern Datum tsm_system_reset(PG_FUNCTION_ARGS);
extern Datum tsm_system_cost(PG_FUNCTION_ARGS);

extern Datum tsm_bernoulli_init(PG_FUNCTION_ARGS);
extern Datum tsm_bernoulli_nextblock(PG_FUNCTION_ARGS);
extern Datum tsm_bernoulli_nexttuple(PG_FUNCTION_ARGS);
extern Datum tsm_bernoulli_end(PG_FUNCTION_ARGS);
extern Datum tsm_bernoulli_reset(PG_FUNCTION_ARGS);
extern Datum tsm_bernoulli_cost(PG_FUNCTION_ARGS);


#endif
