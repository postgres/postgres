/*-------------------------------------------------------------------------
 *
 * datachecksum_state.h
 *	  header file for data checksum helper background worker and data
 *	  checksum state manipulation
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/postmaster/datachecksum_state.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DATACHECKSUM_STATE_H
#define DATACHECKSUM_STATE_H

#include "storage/procsignal.h"

/* Possible operations the DataChecksumsWorker can perform */
typedef enum DataChecksumsWorkerOperation
{
	ENABLE_DATACHECKSUMS,
	DISABLE_DATACHECKSUMS,
} DataChecksumsWorkerOperation;

/*
 * Possible states for a database entry which has been processed. Exported
 * here since we want to be able to reference this from injection point tests.
 */
typedef enum
{
	DATACHECKSUMSWORKER_SUCCESSFUL = 0,
	DATACHECKSUMSWORKER_ABORTED,
	DATACHECKSUMSWORKER_FAILED,
	DATACHECKSUMSWORKER_DROPDB,
} DataChecksumsWorkerResult;

/* Prototypes for data checksum state manipulation */
bool		AbsorbDataChecksumsBarrier(ProcSignalBarrierType barrier);
void		EmitAndWaitDataChecksumsBarrier(uint32 state);

/* Prototypes for data checksum background worker */

/* Start the background processes for enabling or disabling checksums */
void		StartDataChecksumsWorkerLauncher(DataChecksumsWorkerOperation op,
											 int cost_delay,
											 int cost_limit);

/* Background worker entrypoints */
void		DataChecksumsWorkerLauncherMain(Datum arg);
void		DataChecksumsWorkerMain(Datum arg);

#endif							/* DATACHECKSUM_STATE_H */
