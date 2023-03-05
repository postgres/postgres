/*-------------------------------------------------------------------------
 *
 *	parallel_slot.h
 *		Parallel support for bin/scripts/
 *
 *	Copyright (c) 2003-2023, PostgreSQL Global Development Group
 *
 *	src/include/fe_utils/parallel_slot.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARALLEL_SLOT_H
#define PARALLEL_SLOT_H

#include "fe_utils/connect_utils.h"
#include "libpq-fe.h"

typedef bool (*ParallelSlotResultHandler) (PGresult *res, PGconn *conn,
										   void *context);

typedef struct ParallelSlot
{
	PGconn	   *connection;		/* One connection */
	bool		inUse;			/* Is the slot being used? */

	/*
	 * Prior to issuing a command or query on 'connection', a handler callback
	 * function may optionally be registered to be invoked to process the
	 * results, and context information may optionally be registered for use
	 * by the handler.  If unset, these fields should be NULL.
	 */
	ParallelSlotResultHandler handler;
	void	   *handler_context;
} ParallelSlot;

typedef struct ParallelSlotArray
{
	int			numslots;
	ConnParams *cparams;
	const char *progname;
	bool		echo;
	const char *initcmd;
	ParallelSlot slots[FLEXIBLE_ARRAY_MEMBER];
} ParallelSlotArray;

static inline void
ParallelSlotSetHandler(ParallelSlot *slot, ParallelSlotResultHandler handler,
					   void *context)
{
	slot->handler = handler;
	slot->handler_context = context;
}

static inline void
ParallelSlotClearHandler(ParallelSlot *slot)
{
	slot->handler = NULL;
	slot->handler_context = NULL;
}

extern ParallelSlot *ParallelSlotsGetIdle(ParallelSlotArray *sa,
										  const char *dbname);

extern ParallelSlotArray *ParallelSlotsSetup(int numslots, ConnParams *cparams,
											 const char *progname, bool echo,
											 const char *initcmd);

extern void ParallelSlotsAdoptConn(ParallelSlotArray *sa, PGconn *conn);

extern void ParallelSlotsTerminate(ParallelSlotArray *sa);

extern bool ParallelSlotsWaitCompletion(ParallelSlotArray *sa);

extern bool TableCommandResultHandler(PGresult *res, PGconn *conn,
									  void *context);

#endif							/* PARALLEL_SLOT_H */
