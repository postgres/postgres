/*-------------------------------------------------------------------------
 *
 * Declarations for missing POSIX thread components.
 *
 *	  Currently this supplies an implementation of pthread_barrier_t for the
 *	  benefit of macOS, which lacks it.  These declarations are not in port.h,
 *	  because that'd require <pthread.h> to be included by every translation
 *	  unit.
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_PTHREAD_H
#define PG_PTHREAD_H

#include <pthread.h>

#ifndef HAVE_PTHREAD_BARRIER_WAIT

#ifndef PTHREAD_BARRIER_SERIAL_THREAD
#define PTHREAD_BARRIER_SERIAL_THREAD (-1)
#endif

typedef struct pg_pthread_barrier
{
	bool		sense;			/* we only need a one bit phase */
	int			count;			/* number of threads expected */
	int			arrived;		/* number of threads that have arrived */
	pthread_mutex_t mutex;
	pthread_cond_t cond;
} pthread_barrier_t;

extern int	pthread_barrier_init(pthread_barrier_t *barrier,
								 const void *attr,
								 int count);
extern int	pthread_barrier_wait(pthread_barrier_t *barrier);
extern int	pthread_barrier_destroy(pthread_barrier_t *barrier);

#endif

#endif
