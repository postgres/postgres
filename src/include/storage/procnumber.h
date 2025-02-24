/*-------------------------------------------------------------------------
 *
 * procnumber.h
 *	  definition of process number
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/procnumber.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROCNUMBER_H
#define PROCNUMBER_H

/*
 * ProcNumber uniquely identifies an active backend or auxiliary process.
 * It's assigned at backend startup after authentication, when the process
 * adds itself to the proc array.  It is an index into the proc array,
 * starting from 0. Note that a ProcNumber can be reused for a different
 * backend immediately after a backend exits.
 */
typedef int ProcNumber;

#define INVALID_PROC_NUMBER		(-1)

/*
 * Note: MAX_BACKENDS_BITS is 18 as that is the space available for buffer
 * refcounts in buf_internals.h.  This limitation could be lifted by using a
 * 64bit state; but it's unlikely to be worthwhile as 2^18-1 backends exceed
 * currently realistic configurations. Even if that limitation were removed,
 * we still could not a) exceed 2^23-1 because inval.c stores the ProcNumber
 * as a 3-byte signed integer, b) INT_MAX/4 because some places compute
 * 4*MaxBackends without any overflow check.  We check that the configured
 * number of backends does not exceed MAX_BACKENDS in InitializeMaxBackends().
 */
#define MAX_BACKENDS_BITS		18
#define MAX_BACKENDS			((1U << MAX_BACKENDS_BITS)-1)

/*
 * Proc number of this backend (same as GetNumberFromPGProc(MyProc))
 */
extern PGDLLIMPORT ProcNumber MyProcNumber;

/* proc number of our parallel session leader, or INVALID_PROC_NUMBER if none */
extern PGDLLIMPORT ProcNumber ParallelLeaderProcNumber;

/*
 * The ProcNumber to use for our session's temp relations is normally our own,
 * but parallel workers should use their leader's proc number.
 */
#define ProcNumberForTempRelations() \
	(ParallelLeaderProcNumber == INVALID_PROC_NUMBER ? MyProcNumber : ParallelLeaderProcNumber)

#endif							/* PROCNUMBER_H */
