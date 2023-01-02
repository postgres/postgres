/*-------------------------------------------------------------------------
 *
 * tsmapi.h
 *	  API for tablesample methods
 *
 * Copyright (c) 2015-2023, PostgreSQL Global Development Group
 *
 * src/include/access/tsmapi.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TSMAPI_H
#define TSMAPI_H

#include "nodes/execnodes.h"
#include "nodes/pathnodes.h"


/*
 * Callback function signatures --- see tablesample-method.sgml for more info.
 */

typedef void (*SampleScanGetSampleSize_function) (PlannerInfo *root,
												  RelOptInfo *baserel,
												  List *paramexprs,
												  BlockNumber *pages,
												  double *tuples);

typedef void (*InitSampleScan_function) (SampleScanState *node,
										 int eflags);

typedef void (*BeginSampleScan_function) (SampleScanState *node,
										  Datum *params,
										  int nparams,
										  uint32 seed);

typedef BlockNumber (*NextSampleBlock_function) (SampleScanState *node,
												 BlockNumber nblocks);

typedef OffsetNumber (*NextSampleTuple_function) (SampleScanState *node,
												  BlockNumber blockno,
												  OffsetNumber maxoffset);

typedef void (*EndSampleScan_function) (SampleScanState *node);

/*
 * TsmRoutine is the struct returned by a tablesample method's handler
 * function.  It provides pointers to the callback functions needed by the
 * planner and executor, as well as additional information about the method.
 *
 * More function pointers are likely to be added in the future.
 * Therefore it's recommended that the handler initialize the struct with
 * makeNode(TsmRoutine) so that all fields are set to NULL.  This will
 * ensure that no fields are accidentally left undefined.
 */
typedef struct TsmRoutine
{
	NodeTag		type;

	/* List of datatype OIDs for the arguments of the TABLESAMPLE clause */
	List	   *parameterTypes;

	/* Can method produce repeatable samples across, or even within, queries? */
	bool		repeatable_across_queries;
	bool		repeatable_across_scans;

	/* Functions for planning a SampleScan on a physical table */
	SampleScanGetSampleSize_function SampleScanGetSampleSize;

	/* Functions for executing a SampleScan on a physical table */
	InitSampleScan_function InitSampleScan; /* can be NULL */
	BeginSampleScan_function BeginSampleScan;
	NextSampleBlock_function NextSampleBlock;	/* can be NULL */
	NextSampleTuple_function NextSampleTuple;
	EndSampleScan_function EndSampleScan;	/* can be NULL */
} TsmRoutine;


/* Functions in access/tablesample/tablesample.c */
extern TsmRoutine *GetTsmRoutine(Oid tsmhandler);

#endif							/* TSMAPI_H */
