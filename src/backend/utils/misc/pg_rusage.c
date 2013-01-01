/*-------------------------------------------------------------------------
 *
 * pg_rusage.c
 *	  Resource usage measurement support routines.
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/pg_rusage.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "utils/pg_rusage.h"


/*
 * Initialize usage snapshot.
 */
void
pg_rusage_init(PGRUsage *ru0)
{
	getrusage(RUSAGE_SELF, &ru0->ru);
	gettimeofday(&ru0->tv, NULL);
}

/*
 * Compute elapsed time since ru0 usage snapshot, and format into
 * a displayable string.  Result is in a static string, which is
 * tacky, but no one ever claimed that the Postgres backend is
 * threadable...
 */
const char *
pg_rusage_show(const PGRUsage *ru0)
{
	static char result[100];
	PGRUsage	ru1;

	pg_rusage_init(&ru1);

	if (ru1.tv.tv_usec < ru0->tv.tv_usec)
	{
		ru1.tv.tv_sec--;
		ru1.tv.tv_usec += 1000000;
	}
	if (ru1.ru.ru_stime.tv_usec < ru0->ru.ru_stime.tv_usec)
	{
		ru1.ru.ru_stime.tv_sec--;
		ru1.ru.ru_stime.tv_usec += 1000000;
	}
	if (ru1.ru.ru_utime.tv_usec < ru0->ru.ru_utime.tv_usec)
	{
		ru1.ru.ru_utime.tv_sec--;
		ru1.ru.ru_utime.tv_usec += 1000000;
	}

	snprintf(result, sizeof(result),
			 "CPU %d.%02ds/%d.%02du sec elapsed %d.%02d sec",
			 (int) (ru1.ru.ru_stime.tv_sec - ru0->ru.ru_stime.tv_sec),
		  (int) (ru1.ru.ru_stime.tv_usec - ru0->ru.ru_stime.tv_usec) / 10000,
			 (int) (ru1.ru.ru_utime.tv_sec - ru0->ru.ru_utime.tv_sec),
		  (int) (ru1.ru.ru_utime.tv_usec - ru0->ru.ru_utime.tv_usec) / 10000,
			 (int) (ru1.tv.tv_sec - ru0->tv.tv_sec),
			 (int) (ru1.tv.tv_usec - ru0->tv.tv_usec) / 10000);

	return result;
}
