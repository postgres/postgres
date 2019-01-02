/*-------------------------------------------------------------------------
 *
 * barrier.h
 *	  Barriers for synchronizing cooperating processes.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/barrier.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BARRIER_H
#define BARRIER_H

/*
 * For the header previously known as "barrier.h", please include
 * "port/atomics.h", which deals with atomics, compiler barriers and memory
 * barriers.
 */

#include "storage/condition_variable.h"
#include "storage/spin.h"

typedef struct Barrier
{
	slock_t		mutex;
	int			phase;			/* phase counter */
	int			participants;	/* the number of participants attached */
	int			arrived;		/* the number of participants that have
								 * arrived */
	int			elected;		/* highest phase elected */
	bool		static_party;	/* used only for assertions */
	ConditionVariable condition_variable;
} Barrier;

extern void BarrierInit(Barrier *barrier, int num_workers);
extern bool BarrierArriveAndWait(Barrier *barrier, uint32 wait_event_info);
extern bool BarrierArriveAndDetach(Barrier *barrier);
extern int	BarrierAttach(Barrier *barrier);
extern bool BarrierDetach(Barrier *barrier);
extern int	BarrierPhase(Barrier *barrier);
extern int	BarrierParticipants(Barrier *barrier);

#endif							/* BARRIER_H */
