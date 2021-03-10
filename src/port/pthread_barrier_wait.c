/*-------------------------------------------------------------------------
 *
 * pthread_barrier_wait.c
 *    Implementation of pthread_barrier_t support for platforms lacking it.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/port/pthread_barrier_wait.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include "port/pg_pthread.h"

int
pthread_barrier_init(pthread_barrier_t *barrier, const void *attr, int count)
{
	barrier->sense = false;
	barrier->count = count;
	barrier->arrived = 0;
	if (pthread_cond_init(&barrier->cond, NULL) < 0)
		return -1;
	if (pthread_mutex_init(&barrier->mutex, NULL) < 0)
	{
		int			save_errno = errno;

		pthread_cond_destroy(&barrier->cond);
		errno = save_errno;

		return -1;
	}

	return 0;
}

int
pthread_barrier_wait(pthread_barrier_t *barrier)
{
	bool		initial_sense;

	pthread_mutex_lock(&barrier->mutex);

	/* We have arrived at the barrier. */
	barrier->arrived++;
	Assert(barrier->arrived <= barrier->count);

	/* If we were the last to arrive, release the others and return. */
	if (barrier->arrived == barrier->count)
	{
		barrier->arrived = 0;
		barrier->sense = !barrier->sense;
		pthread_mutex_unlock(&barrier->mutex);
		pthread_cond_broadcast(&barrier->cond);

		return PTHREAD_BARRIER_SERIAL_THREAD;
	}

	/* Wait for someone else to flip the sense. */
	initial_sense = barrier->sense;
	do
	{
		pthread_cond_wait(&barrier->cond, &barrier->mutex);
	} while (barrier->sense == initial_sense);

	pthread_mutex_unlock(&barrier->mutex);

	return 0;
}

int
pthread_barrier_destroy(pthread_barrier_t *barrier)
{
	pthread_cond_destroy(&barrier->cond);
	pthread_mutex_destroy(&barrier->mutex);
	return 0;
}
