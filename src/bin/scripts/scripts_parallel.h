/*-------------------------------------------------------------------------
 *
 *	scripts_parallel.h
 *		Parallel support for bin/scripts/
 *
 *	Copyright (c) 2003-2020, PostgreSQL Global Development Group
 *
 *	src/bin/scripts/scripts_parallel.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SCRIPTS_PARALLEL_H
#define SCRIPTS_PARALLEL_H

#include "libpq-fe.h"


typedef struct ParallelSlot
{
	PGconn	   *connection;		/* One connection */
	bool		isFree;			/* Is it known to be idle? */
} ParallelSlot;

extern ParallelSlot *ParallelSlotsGetIdle(ParallelSlot *slots, int numslots);

extern ParallelSlot *ParallelSlotsSetup(const char *dbname, const char *host,
										const char *port,
										const char *username,
										bool prompt_password,
										const char *progname, bool echo,
										PGconn *conn, int numslots);

extern void ParallelSlotsTerminate(ParallelSlot *slots, int numslots);

extern bool ParallelSlotsWaitCompletion(ParallelSlot *slots, int numslots);


#endif							/* SCRIPTS_PARALLEL_H */
