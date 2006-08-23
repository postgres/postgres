/* $PostgreSQL: pgsql/src/interfaces/ecpg/include/pgtypes_interval.h,v 1.10 2006/08/23 12:01:52 meskes Exp $ */

#ifndef PGTYPES_INTERVAL
#define PGTYPES_INTERVAL

#include <ecpg_config.h>

#if defined(USE_INTEGER_DATETIMES) && (defined(HAVE_LONG_INT_64) || defined(HAVE_LONG_LONG_INT_64))
#define HAVE_INT64_TIMESTAMP
#endif

typedef struct
{
#ifdef HAVE_INT64_TIMESTAMP
	int64		time;			/* all time units other than months and years */
#else
	double		time;			/* all time units other than months and years */
#endif
	long		month;			/* months and years, after time for alignment */
}	interval;

#ifdef __cplusplus
extern		"C"
{
#endif

extern interval *PGTYPESinterval_from_asc(char *, char **);
extern char *PGTYPESinterval_to_asc(interval *);
extern int	PGTYPESinterval_copy(interval *, interval *);

#ifdef __cplusplus
}
#endif

#endif   /* PGTYPES_INTERVAL */
