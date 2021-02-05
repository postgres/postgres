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

typedef bool (*ParallelSlotResultHandler) (PGresult *res, PGconn *conn,
										   void *context);

typedef struct ParallelSlot
{
	PGconn	   *connection;		/* One connection */
	bool		isFree;			/* Is it known to be idle? */

	/*
	 * Prior to issuing a command or query on 'connection', a handler callback
	 * function may optionally be registered to be invoked to process the
	 * results, and context information may optionally be registered for use
	 * by the handler.  If unset, these fields should be NULL.
	 */
	ParallelSlotResultHandler handler;
	void	   *handler_context;
} ParallelSlot;

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

extern ParallelSlot *ParallelSlotsGetIdle(ParallelSlot *slots, int numslots);

extern ParallelSlot *ParallelSlotsSetup(const ConnParams *cparams,
										const char *progname, bool echo,
										PGconn *conn, int numslots);

extern void ParallelSlotsTerminate(ParallelSlot *slots, int numslots);

extern bool ParallelSlotsWaitCompletion(ParallelSlot *slots, int numslots);

extern bool TableCommandResultHandler(PGresult *res, PGconn *conn,
									  void *context);

#endif							/* PARALLEL_SLOT_H */
