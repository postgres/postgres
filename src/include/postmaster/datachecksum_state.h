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

/* Prototypes for data checksum state manipulation */
bool		AbsorbDataChecksumsBarrier(ProcSignalBarrierType barrier);
void		EmitAndWaitDataChecksumsBarrier(uint32 state);

/* Background worker entrypoints */
void		DataChecksumsWorkerLauncherMain(Datum arg);
void		DataChecksumsWorkerMain(Datum arg);

#endif							/* DATACHECKSUM_STATE_H */
