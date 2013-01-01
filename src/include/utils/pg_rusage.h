/*-------------------------------------------------------------------------
 *
 * pg_rusage.h
 *	  header file for resource usage measurement support routines
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/pg_rusage.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_RUSAGE_H
#define PG_RUSAGE_H

#include <sys/time.h>

#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#else
#include "rusagestub.h"
#endif


/* State structure for pg_rusage_init/pg_rusage_show */
typedef struct PGRUsage
{
	struct timeval tv;
	struct rusage ru;
} PGRUsage;


extern void pg_rusage_init(PGRUsage *ru0);
extern const char *pg_rusage_show(const PGRUsage *ru0);

#endif   /* PG_RUSAGE_H */
