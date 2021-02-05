/*-------------------------------------------------------------------------
 *
 *	parallel_slot.h
 *		Parallel support for bin/scripts/
 *
 *	Copyright (c) 2003-2021, PostgreSQL Global Development Group
 *
 *	src/include/fe_utils/parallel_slot.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARALLEL_SLOT_H
#define PARALLEL_SLOT_H

#include "fe_utils/connect_utils.h"
#include "libpq-fe.h"

typedef struct ParallelSlot
{
	PGconn	   *connection;		/* One connection */
	bool		isFree;			/* Is it known to be idle? */
} ParallelSlot;

extern ParallelSlot *ParallelSlotsGetIdle(ParallelSlot *slots, int numslots);

extern ParallelSlot *ParallelSlotsSetup(const ConnParams *cparams,
										const char *progname, bool echo,
										PGconn *conn, int numslots);

extern void ParallelSlotsTerminate(ParallelSlot *slots, int numslots);

extern bool ParallelSlotsWaitCompletion(ParallelSlot *slots, int numslots);


#endif							/* PARALLEL_SLOT_H */
